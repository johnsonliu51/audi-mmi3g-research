/* SPDX-License-Identifier: GPL-3.0-only
 * C port reference: DiPlay Iap2UsbMuxHost.kt and UsbMuxFrameBuffer.kt,
 * commit 858a7ee89c188f5c7d825fadbceb514f0bc73f7a.
 * Scope: one serialized USBMUX v2 TCP byte stream, not a CarPlay receiver.
 */
#include "usbmux_session.h"
#include "usbmux_codec.h"
#include <string.h>

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_ACK 0x10u
#define TCP_HEADER 20u
#define REPLY_MAGIC UINT32_C(0xfaceface)
#define MAX_STALE_FRAMES 32u

static uint16_t r16(const uint8_t *p)
{ return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t r32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3]; }
static void w16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void w32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void report(mmi_mux_session *s, const char *event, uint32_t a, uint32_t b)
{
    if (s->transport.diagnostic)
        s->transport.diagnostic(s->transport.context, event, a, b);
}

static void set_state(mmi_mux_session *s, mmi_mux_session_state state)
{
    mmi_mux_session_state previous = s->state;
    s->state = state;
    report(s, "mux.state", (uint32_t)previous, (uint32_t)state);
}

static void release_transport(mmi_mux_session *s)
{
    if (!s->transport_closed) {
        s->transport_closed = 1;
        if (s->transport.close_transport)
            s->transport.close_transport(s->transport.context);
    }
}

static int fail(mmi_mux_session *s, int error)
{
    if (s->state == MMI_MUX_FAILED) return s->failure;
    s->failure = error;
    s->deadline_ms = 0;
    set_state(s, MMI_MUX_FAILED);
    report(s, "mux.error", (uint32_t)(-error), 0);
    release_transport(s);
    return error;
}

static void finished(mmi_mux_session *s)
{
    s->deadline_ms = 0;
    set_state(s, MMI_MUX_CLOSED);
    release_transport(s);
}

static uint64_t deadline(uint64_t now, uint32_t interval)
{ return UINT64_MAX - now < interval ? UINT64_MAX : now + interval; }

static int use_time(mmi_mux_session *s, uint64_t now)
{
    if (!s || now < s->last_now_ms) return MMI_MUX_BAD_ARGUMENT;
    s->last_now_ms = now;
    return MMI_MUX_OK;
}

static int write_bytes(mmi_mux_session *s, size_t length)
{
    if (s->transport_closed || !s->transport.write_all)
        return fail(s, MMI_MUX_IO_ERROR);
    if (s->transport.write_all(s->transport.context, s->tx_bytes, length,
                               s->write_timeout_ms) != 0)
        return fail(s, MMI_MUX_IO_ERROR);
    return MMI_MUX_OK;
}

static int send_mux(mmi_mux_session *s, uint32_t protocol,
                    const uint8_t *payload, size_t length)
{
    size_t encoded = mmi_mux_encode(s->tx_bytes, sizeof(s->tx_bytes), protocol,
                                    s->mux_sequence, s->mux_acknowledgement,
                                    payload, length);
    int rc;
    if (!encoded) return fail(s, MMI_MUX_PROTOCOL_ERROR);
    rc = write_bytes(s, encoded);
    if (rc != MMI_MUX_OK) return rc;
    s->mux_sequence = (uint16_t)(s->mux_sequence + 1u);
    return MMI_MUX_OK;
}

static int send_tcp(mmi_mux_session *s, uint8_t flags,
                    const uint8_t *payload, size_t length)
{
    /* Encode TCP directly after the MUX header; codec memcpy must not receive
     * overlapping source/destination. Assemble MUX without calling the
     * overlapping encoder.
     */
    uint8_t *tcp = s->tx_bytes + 16u;
    uint16_t window = (uint16_t)(sizeof(s->stream_bytes) - s->stream_length);
    int rc;
    if (length > MMI_MUX_SESSION_STREAM_CAPACITY)
        return fail(s, MMI_MUX_PROTOCOL_ERROR);
    memset(s->tx_bytes, 0, 16u + TCP_HEADER);
    w32(s->tx_bytes, 6u);
    w32(s->tx_bytes + 4, (uint32_t)(16u + TCP_HEADER + length));
    w32(s->tx_bytes + 8, UINT32_C(0xfeedface));
    w16(s->tx_bytes + 12, s->mux_sequence);
    w16(s->tx_bytes + 14, s->mux_acknowledgement);
    w16(tcp, s->source_port);
    w16(tcp + 2, s->destination_port);
    w32(tcp + 4, s->send_next);
    w32(tcp + 8, s->receive_next);
    tcp[12] = 0x50u;
    tcp[13] = flags;
    w16(tcp + 14, window);
    if (length) memcpy(tcp + TCP_HEADER, payload, length);
    rc = write_bytes(s, 16u + TCP_HEADER + length);
    if (rc == MMI_MUX_OK) {
        s->mux_sequence = (uint16_t)(s->mux_sequence + 1u);
        report(s, "mux.tcp.tx", flags, (uint32_t)length);
    }
    return rc;
}

static int valid_diagnostic(const uint8_t *p, size_t n)
{
    size_t i;
    if (n < 2u || n > 1024u || p[0] != 4u) return 0;
    for (i = 1; i < n; ++i) if (p[i] < 0x20u || p[i] > 0x7eu) return 0;
    return 1;
}

static int can_have_padding(const mmi_mux_frame *f)
{
    size_t tcp_header;
    if (f->protocol == 0u && f->word8 == 2u && f->payload_length == 4u)
        return 1;
    if (f->word8 != REPLY_MAGIC) return 0;
    if (f->protocol == 1u)
        return valid_diagnostic(f->payload, f->payload_length);
    if (f->protocol != 6u || f->payload_length < TCP_HEADER) return 0;
    tcp_header = (size_t)(f->payload[12] >> 4) * 4u;
    return tcp_header >= TCP_HEADER && tcp_header <= f->payload_length &&
           r16(f->payload) != 0u && r16(f->payload + 2) != 0u;
}

/* 1: discard exactly four bytes; 0: need more; -1: invalid boundary.
 * This deliberately does not scan for a header or discard a USB completion.
 */
static int padding_candidate(const mmi_mux_session *s)
{
    const uint8_t *p = s->frame_bytes;
    uint32_t protocol, n;
    size_t header;
    if (!s->optional_padding || r32(p) == 6u) return -1;
    if (s->frame_length < 20u) return 0;
    protocol = r32(p + 4);
    n = r32(p + 8);
    if (r32(p + 12) != REPLY_MAGIC) return -1;
    if (protocol == 6u && n >= 36u && n <= 65536u) {
        if (s->frame_length < 40u) return 0;
        header = (size_t)(p[32] >> 4) * 4u;
        return header >= TCP_HEADER && header <= n - 16u &&
               r16(p + 20) != 0u && r16(p + 22) != 0u ? 1 : -1;
    }
    if (protocol == 1u && n >= 18u && n <= 1040u) {
        if (s->frame_length < 4u + n) return 0;
        return valid_diagnostic(p + 20, n - 16u) ? 1 : -1;
    }
    return -1;
}

static int acknowledge(mmi_mux_session *s, uint32_t ack)
{
    uint32_t outstanding = s->send_next - s->send_unacknowledged;
    uint32_t advance = ack - s->send_unacknowledged;
    if (advance > outstanding) {
        /* Older duplicate ACKs are allowed; future/ambiguous ACKs are not. */
        if (advance > UINT32_C(0x80000000)) return MMI_MUX_OK;
        return fail(s, MMI_MUX_PROTOCOL_ERROR);
    }
    s->send_unacknowledged = ack;
    if (s->state == MMI_MUX_OPEN && ack == s->send_next) s->deadline_ms = 0;
    return MMI_MUX_OK;
}

static void advance_close_ack(mmi_mux_session *s)
{
    if (s->send_unacknowledged != s->send_next) return;
    if (s->state == MMI_MUX_FIN_WAIT_1) set_state(s, MMI_MUX_FIN_WAIT_2);
    else if (s->state == MMI_MUX_CLOSING || s->state == MMI_MUX_LAST_ACK)
        finished(s);
}

static int dispatch_tcp(mmi_mux_session *s, const uint8_t *tcp, size_t length,
                         uint64_t now)
{
    size_t header = (size_t)(tcp[12] >> 4) * 4u;
    const uint8_t *payload;
    size_t n, skip = 0;
    uint8_t flags = tcp[13];
    uint32_t seq = r32(tcp + 4), ack = r32(tcp + 8), behind;
    int has_fin = (flags & TCP_FIN) != 0, rc;
    if (r16(tcp) != s->destination_port || r16(tcp + 2) != s->source_port) {
        report(s, "mux.tcp.foreign_ports", r16(tcp), r16(tcp + 2));
        return MMI_MUX_OK;
    }
    if (s->state == MMI_MUX_READY || s->state == MMI_MUX_WAIT_VERSION)
        return MMI_MUX_OK;
    if ((flags & TCP_RST) != 0) return fail(s, MMI_MUX_PEER_RESET);
    payload = tcp + header;
    n = length - header;
    report(s, "mux.tcp.rx", flags, (uint32_t)n);
    if (s->state == MMI_MUX_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK) ||
            has_fin || ack != s->send_next)
            return fail(s, MMI_MUX_PROTOCOL_ERROR);
        s->initial_peer_sequence = seq;
        s->send_unacknowledged = ack;
        s->receive_next = seq + 1u;
        s->peer_window = r16(tcp + 14);
        s->deadline_ms = 0;
        set_state(s, MMI_MUX_OPEN);
        ++seq; /* SYN consumes one sequence number, including wrap at UINT32_MAX. */
        rc = send_tcp(s, TCP_ACK, NULL, 0);
        if (rc != MMI_MUX_OK) return rc;
    } else {
        if ((flags & TCP_SYN) != 0) {
            if ((flags & TCP_ACK) == 0 || seq != s->initial_peer_sequence ||
                ack != s->initial_send_sequence + 1u || has_fin)
                return fail(s, MMI_MUX_PROTOCOL_ERROR);
            ++seq; /* Duplicate SYNACK: do not advance our sequence twice. */
        } else if ((flags & TCP_ACK) == 0) {
            return fail(s, MMI_MUX_PROTOCOL_ERROR);
        }
        rc = acknowledge(s, ack);
        if (rc != MMI_MUX_OK) return rc;
        s->peer_window = r16(tcp + 14);
    }
    /* Only contiguous or overlapping duplicate data can enter the stream. */
    if (n || has_fin) {
        if (s->peer_eof) {
            report(s, "mux.tcp.after_eof", seq, (uint32_t)n);
            rc = send_tcp(s, TCP_ACK, NULL, 0);
            if (rc == MMI_MUX_OK) advance_close_ack(s);
            return rc;
        }
        behind = s->receive_next - seq;
        if (behind > UINT32_C(0x7fffffff)) {
            report(s, "mux.tcp.out_of_order", seq, s->receive_next);
            rc = send_tcp(s, TCP_ACK, NULL, 0);
            if (rc == MMI_MUX_OK) advance_close_ack(s);
            return rc;
        }
        if (behind > n) {
            report(s, "mux.tcp.duplicate", seq, (uint32_t)n);
            rc = send_tcp(s, TCP_ACK, NULL, 0);
            if (rc == MMI_MUX_OK) advance_close_ack(s);
            return rc;
        }
        skip = behind;
        if (n - skip > sizeof(s->stream_bytes) - s->stream_length)
            return fail(s, MMI_MUX_RX_OVERFLOW);
        if (n > skip) {
            memcpy(s->stream_bytes + s->stream_length, payload + skip, n - skip);
            s->stream_length += n - skip;
            s->receive_next += (uint32_t)(n - skip);
        }
        if (has_fin) {
            ++s->receive_next;
            s->peer_eof = 1;
        }
        rc = send_tcp(s, TCP_ACK, NULL, 0);
        if (rc != MMI_MUX_OK) return rc;
    } else if ((flags & TCP_SYN) != 0) {
        rc = send_tcp(s, TCP_ACK, NULL, 0);
        if (rc != MMI_MUX_OK) return rc;
    }
    if (s->state == MMI_MUX_OPEN && has_fin) {
        /* Passive close is automatic. Valid queued data remains readable. */
        rc = send_tcp(s, TCP_FIN | TCP_ACK, NULL, 0);
        if (rc != MMI_MUX_OK) return rc;
        ++s->send_next;
        s->deadline_ms = deadline(now, s->close_timeout_ms);
        set_state(s, MMI_MUX_LAST_ACK);
    } else if (s->state == MMI_MUX_FIN_WAIT_1) {
        if (s->send_unacknowledged == s->send_next) {
            if (has_fin) finished(s);
            else set_state(s, MMI_MUX_FIN_WAIT_2);
        } else if (has_fin) set_state(s, MMI_MUX_CLOSING);
    } else if (s->state == MMI_MUX_FIN_WAIT_2 && has_fin) {
        finished(s);
    } else if ((s->state == MMI_MUX_CLOSING || s->state == MMI_MUX_LAST_ACK) &&
               s->send_unacknowledged == s->send_next) {
        finished(s);
    }
    return MMI_MUX_OK;
}

static int dispatch_frame(mmi_mux_session *s, const mmi_mux_frame *f,
                           uint64_t now)
{
    int rc;
    s->mux_acknowledgement = f->sequence;
    report(s, "mux.frame.rx", f->protocol, (uint32_t)f->payload_length);
    if (s->state == MMI_MUX_WAIT_VERSION) {
        if (f->protocol == 6u && ++s->stale_frames <= MAX_STALE_FRAMES) {
            report(s, "mux.handshake.stale_tcp", s->stale_frames, 0);
            return MMI_MUX_OK;
        }
        if (f->protocol != 0u || f->word8 != 2u || f->payload_length != 4u)
            return fail(s, MMI_MUX_PROTOCOL_ERROR);
        {
            const uint8_t setup = 7u;
            rc = send_mux(s, 2u, &setup, 1);
        }
        if (rc != MMI_MUX_OK) return rc;
        s->deadline_ms = 0;
        set_state(s, MMI_MUX_READY);
        return MMI_MUX_OK;
    }
    if (f->protocol == 6u)
        return dispatch_tcp(s, f->payload, f->payload_length, now);
    if (f->protocol == 1u && f->word8 == REPLY_MAGIC &&
        valid_diagnostic(f->payload, f->payload_length)) {
        /* Its text is deliberately never logged: phone-generated content. */
        report(s, "mux.peer.diagnostic", (uint32_t)f->payload_length, 0);
        return MMI_MUX_OK;
    }
    return fail(s, MMI_MUX_PROTOCOL_ERROR);
}

static int process_frames(mmi_mux_session *s, uint64_t now)
{
    mmi_mux_frame frame;
    size_t consumed;
    int rc, padding;
    while (s->frame_length) {
        rc = mmi_mux_decode(s->frame_bytes, s->frame_length, &frame, &consumed);
        if (rc < 0) {
            padding = padding_candidate(s);
            if (padding == 0) return MMI_MUX_OK;
            if (padding < 0) return fail(s, MMI_MUX_PROTOCOL_ERROR);
            memmove(s->frame_bytes, s->frame_bytes + 4, s->frame_length - 4);
            s->frame_length -= 4;
            s->optional_padding = 0;
            if (s->padding_reports++ < 4u) report(s, "mux.reply.padding", 4, 0);
            continue;
        }
        if (rc == 0) return MMI_MUX_OK;
        s->optional_padding = can_have_padding(&frame);
        rc = dispatch_frame(s, &frame, now);
        if (rc != MMI_MUX_OK) return rc;
        s->frame_length -= consumed;
        memmove(s->frame_bytes, s->frame_bytes + consumed, s->frame_length);
        if (s->state == MMI_MUX_CLOSED) {
            /* Late transport bytes do not re-open a completed connection. */
            s->frame_length = 0;
            break;
        }
    }
    return MMI_MUX_OK;
}

int mmi_mux_session_init(mmi_mux_session *s,
                          const mmi_mux_session_transport *transport)
{
    if (!s || !transport || !transport->write_all) return MMI_MUX_BAD_ARGUMENT;
    memset(s, 0, sizeof(*s));
    s->transport = *transport;
    s->state = MMI_MUX_IDLE;
    s->write_timeout_ms = 2000u;
    s->acknowledgement_timeout_ms = 5000u;
    s->close_timeout_ms = 5000u;
    return MMI_MUX_OK;
}

int mmi_mux_session_tick(mmi_mux_session *s, uint64_t now)
{
    if (use_time(s, now) != MMI_MUX_OK) return MMI_MUX_BAD_ARGUMENT;
    if (s->state == MMI_MUX_FAILED) return s->failure;
    if (s->deadline_ms && now >= s->deadline_ms) return fail(s, MMI_MUX_TIMEOUT);
    return MMI_MUX_OK;
}

int mmi_mux_session_start(mmi_mux_session *s, uint64_t now, uint32_t timeout)
{
    size_t n;
    int rc;
    if (!timeout || use_time(s, now) != MMI_MUX_OK) return MMI_MUX_BAD_ARGUMENT;
    if (s->state != MMI_MUX_IDLE || s->transport_closed) return MMI_MUX_BAD_STATE;
    n = mmi_mux_version(s->tx_bytes, sizeof(s->tx_bytes));
    rc = write_bytes(s, n);
    if (rc != MMI_MUX_OK) return rc;
    s->deadline_ms = deadline(now, timeout);
    set_state(s, MMI_MUX_WAIT_VERSION);
    return MMI_MUX_OK;
}

int mmi_mux_session_connect(mmi_mux_session *s, uint16_t source,
                            uint16_t destination, uint32_t sequence,
                            uint64_t now, uint32_t timeout)
{
    int rc;
    if (!source || !destination || !timeout) return MMI_MUX_BAD_ARGUMENT;
    rc = mmi_mux_session_tick(s, now);
    if (rc != MMI_MUX_OK) return rc;
    if (s->state != MMI_MUX_READY) return MMI_MUX_BAD_STATE;
    s->source_port = source;
    s->destination_port = destination;
    s->initial_send_sequence = sequence;
    s->send_unacknowledged = s->send_next = sequence;
    rc = send_tcp(s, TCP_SYN, NULL, 0);
    if (rc != MMI_MUX_OK) return rc;
    ++s->send_next;
    s->deadline_ms = deadline(now, timeout);
    set_state(s, MMI_MUX_SYN_SENT);
    return MMI_MUX_OK;
}

int mmi_mux_session_feed(mmi_mux_session *s, const uint8_t *bytes, size_t length,
                         uint64_t now)
{
    int rc;
    size_t n;
    if (!bytes && length) return MMI_MUX_BAD_ARGUMENT;
    rc = mmi_mux_session_tick(s, now);
    if (rc != MMI_MUX_OK) return rc;
    if (s->state == MMI_MUX_IDLE || s->state == MMI_MUX_CLOSED)
        return MMI_MUX_BAD_STATE;
    while (length) {
        n = sizeof(s->frame_bytes) - s->frame_length;
        if (n > length) n = length;
        if (!n) return fail(s, MMI_MUX_RX_OVERFLOW);
        memcpy(s->frame_bytes + s->frame_length, bytes, n);
        s->frame_length += n;
        bytes += n;
        length -= n;
        rc = process_frames(s, now);
        if (rc != MMI_MUX_OK) return rc;
        if (s->state == MMI_MUX_CLOSED) break;
    }
    return MMI_MUX_OK;
}

int mmi_mux_session_send(mmi_mux_session *s, const uint8_t *bytes, size_t length,
                         size_t *accepted, uint64_t now)
{
    size_t n;
    int rc;
    if (!accepted || (!bytes && length)) return MMI_MUX_BAD_ARGUMENT;
    *accepted = 0;
    rc = mmi_mux_session_tick(s, now);
    if (rc != MMI_MUX_OK) return rc;
    if (s->state != MMI_MUX_OPEN || s->peer_eof) return MMI_MUX_BAD_STATE;
    if (!length) return MMI_MUX_OK;
    if (s->send_unacknowledged != s->send_next || !s->peer_window)
        return MMI_MUX_AGAIN;
    n = length;
    if (n > MMI_MUX_SESSION_STREAM_CAPACITY) n = MMI_MUX_SESSION_STREAM_CAPACITY;
    if (n > s->peer_window) n = s->peer_window;
    rc = send_tcp(s, TCP_ACK, bytes, n);
    if (rc != MMI_MUX_OK) return rc;
    s->send_next += (uint32_t)n;
    s->deadline_ms = deadline(now, s->acknowledgement_timeout_ms);
    *accepted = n;
    return MMI_MUX_OK;
}

int mmi_mux_session_receive(mmi_mux_session *s, uint8_t *out, size_t capacity,
                            size_t *received, uint64_t now)
{
    size_t n;
    int rc;
    if (!out || !capacity || !received) return MMI_MUX_BAD_ARGUMENT;
    *received = 0;
    rc = mmi_mux_session_tick(s, now);
    if (rc != MMI_MUX_OK) return rc;
    if (s->state < MMI_MUX_OPEN) return MMI_MUX_BAD_STATE;
    if (!s->stream_length)
        return s->peer_eof || s->state == MMI_MUX_CLOSED ? MMI_MUX_EOF : MMI_MUX_AGAIN;
    n = capacity < s->stream_length ? capacity : s->stream_length;
    memcpy(out, s->stream_bytes, n);
    s->stream_length -= n;
    memmove(s->stream_bytes, s->stream_bytes + n, s->stream_length);
    memset(s->stream_bytes + s->stream_length, 0, n);
    *received = n;
    if (s->state == MMI_MUX_OPEN) return send_tcp(s, TCP_ACK, NULL, 0);
    return MMI_MUX_OK;
}

int mmi_mux_session_close(mmi_mux_session *s, uint64_t now)
{
    int rc = mmi_mux_session_tick(s, now);
    if (rc != MMI_MUX_OK) return rc;
    if (s->state == MMI_MUX_CLOSED) return MMI_MUX_OK;
    if (s->state == MMI_MUX_OPEN) {
        rc = send_tcp(s, TCP_FIN | TCP_ACK, NULL, 0);
        if (rc != MMI_MUX_OK) return rc;
        ++s->send_next;
        s->deadline_ms = deadline(now, s->close_timeout_ms);
        set_state(s, MMI_MUX_FIN_WAIT_1);
        return MMI_MUX_OK;
    }
    if (s->state == MMI_MUX_FIN_WAIT_1 || s->state == MMI_MUX_FIN_WAIT_2 ||
        s->state == MMI_MUX_CLOSING || s->state == MMI_MUX_LAST_ACK) return MMI_MUX_OK;
    if (s->state == MMI_MUX_SYN_SENT) {
        rc = send_tcp(s, TCP_RST | TCP_ACK, NULL, 0);
        if (rc != MMI_MUX_OK) return rc;
    }
    finished(s);
    return MMI_MUX_OK;
}

int mmi_mux_session_abort(mmi_mux_session *s, uint64_t now)
{
    int rc;
    if (use_time(s, now) != MMI_MUX_OK) return MMI_MUX_BAD_ARGUMENT;
    if (s->state == MMI_MUX_CLOSED) return MMI_MUX_OK;
    if (s->state == MMI_MUX_FAILED) return s->failure;
    if (s->state >= MMI_MUX_SYN_SENT && s->state <= MMI_MUX_LAST_ACK) {
        rc = send_tcp(s, TCP_RST | TCP_ACK, NULL, 0);
        if (rc != MMI_MUX_OK) return rc;
    }
    return fail(s, MMI_MUX_ABORTED);
}

void mmi_mux_session_clear(mmi_mux_session *s)
{
    volatile uint8_t *p;
    size_t n;
    if (!s) return;
    release_transport(s);
    /* Volatile erase also removes any future pairing/Lockdown bytes retained
     * by the stream. This does not replace the caller's own secret cleanup.
     */
    p = (volatile uint8_t *)s;
    for (n = 0; n < sizeof(*s); ++n) p[n] = 0;
}
