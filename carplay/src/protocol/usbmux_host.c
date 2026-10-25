/* SPDX-License-Identifier: GPL-3.0-only
 * Reference: DiPlay Iap2UsbMuxHost.kt / UsbMuxFrameBuffer.kt,
 * commit 858a7ee89c188f5c7d825fadbceb514f0bc73f7a.
 * Shared USBMUX v2 host with bounded independent TCP connections.
 * No USB driver, TLS, pairing, iAP2, NCM, AirPlay, or media implementation.
 */
#include "usbmux_host.h"
#include "usbmux_codec.h"
#include <string.h>

#define FIN 0x01u
#define SYN 0x02u
#define RST 0x04u
#define ACK 0x10u
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

static void erase(void *memory, size_t length)
{
    volatile uint8_t *p = (volatile uint8_t *)memory;
    while (length--) *p++ = 0;
}

static uint64_t deadline(uint64_t now, uint32_t interval)
{ return UINT64_MAX - now < interval ? UINT64_MAX : now + interval; }

static void report(mmi_mux_host *h, const char *event, uint32_t a, uint32_t b)
{
    if (h->transport.diagnostic)
        h->transport.diagnostic(h->transport.context, event, a, b);
}

static void host_state(mmi_mux_host *h, mmi_mux_host_state state)
{
    mmi_mux_host_state previous = h->state;
    h->state = state;
    report(h, "mux.host.state", (uint32_t)previous, (uint32_t)state);
}

static void conn_state(mmi_mux_host *h, mmi_mux_host_connection *c,
                        mmi_mux_connection_state state)
{
    c->state = state;
    report(h, "mux.conn.state", c->source_port, (uint32_t)state);
}

static void release_transport(mmi_mux_host *h)
{
    if (!h->transport_closed) {
        h->transport_closed = 1;
        if (h->transport.close_transport)
            h->transport.close_transport(h->transport.context);
    }
}

static int active(const mmi_mux_host_connection *c)
{ return c->state >= MMI_MUX_CONN_SYN_SENT && c->state <= MMI_MUX_CONN_LAST_ACK; }

static int host_fail(mmi_mux_host *h, int error)
{
    size_t i;
    if (h->state == MMI_MUX_HOST_FAILED) return h->failure;
    h->failure = error;
    h->deadline_ms = 0;
    host_state(h, MMI_MUX_HOST_FAILED);
    report(h, "mux.host.error", (uint32_t)(-error), 0);
    for (i = 0; i < MMI_MUX_HOST_CONNECTIONS; ++i) {
        mmi_mux_host_connection *c = &h->connections[i];
        if (active(c)) {
            c->failure = error;
            c->deadline_ms = 0;
            conn_state(h, c, MMI_MUX_CONN_FAILED);
        }
    }
    release_transport(h);
    return error;
}

static int use_time(mmi_mux_host *h, uint64_t now)
{
    if (!h || now < h->last_now_ms) return MMI_MUX_HOST_BAD_ARGUMENT;
    h->last_now_ms = now;
    return MMI_MUX_HOST_OK;
}

static mmi_mux_host_connection *connection(mmi_mux_host *h,
                                           mmi_mux_connection_handle handle)
{
    mmi_mux_host_connection *c;
    if (!h || handle.slot >= MMI_MUX_HOST_CONNECTIONS || !handle.generation)
        return NULL;
    c = &h->connections[handle.slot];
    return c->state != MMI_MUX_CONN_FREE && c->generation == handle.generation ? c : NULL;
}

static int write_bytes(mmi_mux_host *h, size_t length)
{
    if (h->transport_closed || !h->transport.write_all)
        return host_fail(h, MMI_MUX_HOST_IO_ERROR);
    if (h->transport.write_all(h->transport.context, h->tx_bytes, length,
                               h->write_timeout_ms) != 0)
        return host_fail(h, MMI_MUX_HOST_IO_ERROR);
    return MMI_MUX_HOST_OK;
}

static int send_mux(mmi_mux_host *h, uint32_t protocol,
                    const uint8_t *payload, size_t length)
{
    size_t n = mmi_mux_encode(h->tx_bytes, sizeof(h->tx_bytes), protocol,
                              h->mux_sequence, h->mux_acknowledgement,
                              payload, length);
    int rc;
    if (!n) return host_fail(h, MMI_MUX_HOST_PROTOCOL_ERROR);
    rc = write_bytes(h, n);
    if (rc == MMI_MUX_HOST_OK) h->mux_sequence = (uint16_t)(h->mux_sequence + 1u);
    return rc;
}

static int send_tcp(mmi_mux_host *h, mmi_mux_host_connection *c, uint8_t flags,
                    const uint8_t *payload, size_t length)
{
    uint8_t *tcp = h->tx_bytes + 16;
    uint16_t window = (uint16_t)(sizeof(c->stream_bytes) - c->stream_length);
    int rc;
    if (length > MMI_MUX_HOST_STREAM_CAPACITY)
        return host_fail(h, MMI_MUX_HOST_PROTOCOL_ERROR);
    /* Build in place without overlapping memcpy in the standalone codec. */
    memset(h->tx_bytes, 0, 16u + TCP_HEADER);
    w32(h->tx_bytes, 6u);
    w32(h->tx_bytes + 4, (uint32_t)(16u + TCP_HEADER + length));
    w32(h->tx_bytes + 8, UINT32_C(0xfeedface));
    w16(h->tx_bytes + 12, h->mux_sequence);
    w16(h->tx_bytes + 14, h->mux_acknowledgement);
    w16(tcp, c->source_port);
    w16(tcp + 2, c->destination_port);
    w32(tcp + 4, c->send_next);
    w32(tcp + 8, c->receive_next);
    tcp[12] = 0x50u;
    tcp[13] = flags;
    w16(tcp + 14, window);
    if (length) memcpy(tcp + TCP_HEADER, payload, length);
    rc = write_bytes(h, 16u + TCP_HEADER + length);
    if (rc == MMI_MUX_HOST_OK) {
        h->mux_sequence = (uint16_t)(h->mux_sequence + 1u);
        report(h, "mux.tcp.tx", ((uint32_t)c->source_port << 16) | c->destination_port,
               ((uint32_t)flags << 24) | (uint32_t)length);
    }
    return rc;
}

static int conn_fail(mmi_mux_host *h, mmi_mux_host_connection *c, int error,
                      int reset_peer)
{
    int rc;
    if (c->state == MMI_MUX_CONN_FAILED) return c->failure;
    if (reset_peer && active(c) && h->state == MMI_MUX_HOST_READY) {
        rc = send_tcp(h, c, RST | ACK, NULL, 0);
        if (rc != MMI_MUX_HOST_OK) return rc;
    }
    c->failure = error;
    c->deadline_ms = 0;
    conn_state(h, c, MMI_MUX_CONN_FAILED);
    report(h, "mux.conn.error", c->source_port, (uint32_t)(-error));
    return error;
}

static void conn_finished(mmi_mux_host *h, mmi_mux_host_connection *c)
{
    c->deadline_ms = 0;
    conn_state(h, c, MMI_MUX_CONN_CLOSED);
    /* Deliberately does NOT close the shared transport. */
}

static void advance_close_ack(mmi_mux_host *h, mmi_mux_host_connection *c)
{
    if (c->send_unacknowledged != c->send_next) return;
    if (c->state == MMI_MUX_CONN_FIN_WAIT_1)
        conn_state(h, c, MMI_MUX_CONN_FIN_WAIT_2);
    else if (c->state == MMI_MUX_CONN_CLOSING || c->state == MMI_MUX_CONN_LAST_ACK)
        conn_finished(h, c);
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
    size_t header;
    if (f->protocol == 0u && f->word8 == 2u && f->payload_length == 4u) return 1;
    if (f->word8 != REPLY_MAGIC) return 0;
    if (f->protocol == 1u) return valid_diagnostic(f->payload, f->payload_length);
    if (f->protocol != 6u || f->payload_length < TCP_HEADER) return 0;
    header = (size_t)(f->payload[12] >> 4) * 4u;
    return header >= TCP_HEADER && header <= f->payload_length &&
           r16(f->payload) != 0u && r16(f->payload + 2) != 0u;
}

/* +4 only after a captured compatible previous frame; never scan for magic. */
static int padding_candidate(const mmi_mux_host *h)
{
    const uint8_t *p = h->frame_bytes;
    uint32_t protocol, n;
    size_t header;
    if (!h->optional_padding || r32(p) == 6u) return -1;
    if (h->frame_length < 20u) return 0;
    protocol = r32(p + 4);
    n = r32(p + 8);
    if (r32(p + 12) != REPLY_MAGIC) return -1;
    if (protocol == 6u && n >= 36u && n <= 65536u) {
        if (h->frame_length < 40u) return 0;
        header = (size_t)(p[32] >> 4) * 4u;
        return header >= TCP_HEADER && header <= n - 16u &&
               r16(p + 20) != 0u && r16(p + 22) != 0u ? 1 : -1;
    }
    if (protocol == 1u && n >= 18u && n <= 1040u) {
        if (h->frame_length < 4u + n) return 0;
        return valid_diagnostic(p + 20, n - 16u) ? 1 : -1;
    }
    return -1;
}

static int acknowledge(mmi_mux_host *h, mmi_mux_host_connection *c, uint32_t ack)
{
    uint32_t outstanding = c->send_next - c->send_unacknowledged;
    uint32_t advance = ack - c->send_unacknowledged;
    if (advance > outstanding) {
        if (advance > UINT32_C(0x80000000)) return MMI_MUX_HOST_OK;
        return conn_fail(h, c, MMI_MUX_HOST_PROTOCOL_ERROR, 1);
    }
    c->send_unacknowledged = ack;
    if (c->state == MMI_MUX_CONN_OPEN && ack == c->send_next) c->deadline_ms = 0;
    return MMI_MUX_HOST_OK;
}

static mmi_mux_host_connection *route_tcp(mmi_mux_host *h,
                                           uint16_t peer_port, uint16_t host_port)
{
    size_t i;
    for (i = 0; i < MMI_MUX_HOST_CONNECTIONS; ++i) {
        mmi_mux_host_connection *c = &h->connections[i];
        if (c->state != MMI_MUX_CONN_FREE && c->source_port == host_port &&
            c->destination_port == peer_port) return c;
    }
    return NULL;
}

static int dispatch_tcp(mmi_mux_host *h, const uint8_t *tcp, size_t length,
                         uint64_t now)
{
    mmi_mux_host_connection *c = route_tcp(h, r16(tcp), r16(tcp + 2));
    size_t header = (size_t)(tcp[12] >> 4) * 4u, n, skip;
    const uint8_t *payload;
    uint8_t flags = tcp[13];
    uint32_t seq = r32(tcp + 4), ack = r32(tcp + 8), behind;
    int has_fin = (flags & FIN) != 0, rc;
    if (!c) {
        report(h, "mux.tcp.unknown_ports", r16(tcp), r16(tcp + 2));
        return MMI_MUX_HOST_OK;
    }
    if (!active(c)) {
        report(h, "mux.tcp.closed_port", c->source_port, flags);
        return MMI_MUX_HOST_OK;
    }
    if ((flags & RST) != 0) return conn_fail(h, c, MMI_MUX_HOST_PEER_RESET, 0);
    payload = tcp + header;
    n = length - header;
    report(h, "mux.tcp.rx", ((uint32_t)c->source_port << 16) | c->destination_port,
           ((uint32_t)flags << 24) | (uint32_t)n);
    if (c->state == MMI_MUX_CONN_SYN_SENT) {
        if ((flags & (SYN | ACK)) != (SYN | ACK) || has_fin || ack != c->send_next)
            return conn_fail(h, c, MMI_MUX_HOST_PROTOCOL_ERROR, 1);
        c->initial_peer_sequence = seq;
        c->send_unacknowledged = ack;
        c->receive_next = seq + 1u;
        c->peer_window = r16(tcp + 14);
        c->deadline_ms = 0;
        conn_state(h, c, MMI_MUX_CONN_OPEN);
        ++seq;
        rc = send_tcp(h, c, ACK, NULL, 0);
        if (rc != MMI_MUX_HOST_OK) return rc;
    } else {
        if ((flags & SYN) != 0) {
            if ((flags & ACK) == 0 || seq != c->initial_peer_sequence ||
                ack != c->initial_send_sequence + 1u || has_fin)
                return conn_fail(h, c, MMI_MUX_HOST_PROTOCOL_ERROR, 1);
            ++seq;
        } else if ((flags & ACK) == 0) {
            return conn_fail(h, c, MMI_MUX_HOST_PROTOCOL_ERROR, 1);
        }
        rc = acknowledge(h, c, ack);
        if (rc != MMI_MUX_HOST_OK) return rc;
        c->peer_window = r16(tcp + 14);
    }
    if (n || has_fin) {
        if (c->peer_eof) {
            /* DiPlay removes a connection from dispatch after peer FIN. Keep
             * only enough state to finish our FIN acknowledgement: never
             * deliver post-FIN bytes or consume a second FIN sequence number.
             * Late/repeated payload/FIN gets the current cumulative ACK.
             */
            report(h, "mux.tcp.after_eof", c->source_port, (uint32_t)n);
            rc = send_tcp(h, c, ACK, NULL, 0);
            if (rc == MMI_MUX_HOST_OK) advance_close_ack(h, c);
            return rc;
        }
        behind = c->receive_next - seq;
        if (behind > UINT32_C(0x7fffffff) || behind > n) {
            report(h, behind > UINT32_C(0x7fffffff) ? "mux.tcp.out_of_order" :
                   "mux.tcp.duplicate", seq, c->receive_next);
            rc = send_tcp(h, c, ACK, NULL, 0);
            if (rc == MMI_MUX_HOST_OK) advance_close_ack(h, c);
            return rc;
        }
        skip = behind;
        if (n - skip > sizeof(c->stream_bytes) - c->stream_length)
            return conn_fail(h, c, MMI_MUX_HOST_RX_OVERFLOW, 1);
        if (n > skip) {
            memcpy(c->stream_bytes + c->stream_length, payload + skip, n - skip);
            c->stream_length += n - skip;
            c->receive_next += (uint32_t)(n - skip);
        }
        if (has_fin) { ++c->receive_next; c->peer_eof = 1; }
        rc = send_tcp(h, c, ACK, NULL, 0);
        if (rc != MMI_MUX_HOST_OK) return rc;
    } else if ((flags & SYN) != 0) {
        rc = send_tcp(h, c, ACK, NULL, 0);
        if (rc != MMI_MUX_HOST_OK) return rc;
    }
    if (c->state == MMI_MUX_CONN_OPEN && has_fin) {
        rc = send_tcp(h, c, FIN | ACK, NULL, 0);
        if (rc != MMI_MUX_HOST_OK) return rc;
        ++c->send_next;
        c->deadline_ms = deadline(now, h->close_timeout_ms);
        conn_state(h, c, MMI_MUX_CONN_LAST_ACK);
    } else if (c->state == MMI_MUX_CONN_FIN_WAIT_1) {
        if (c->send_unacknowledged == c->send_next) {
            if (has_fin) conn_finished(h, c);
            else conn_state(h, c, MMI_MUX_CONN_FIN_WAIT_2);
        } else if (has_fin) conn_state(h, c, MMI_MUX_CONN_CLOSING);
    } else if (c->state == MMI_MUX_CONN_FIN_WAIT_2 && has_fin) {
        conn_finished(h, c);
    } else if ((c->state == MMI_MUX_CONN_CLOSING || c->state == MMI_MUX_CONN_LAST_ACK) &&
               c->send_unacknowledged == c->send_next) {
        conn_finished(h, c);
    }
    return MMI_MUX_HOST_OK;
}

static int dispatch_frame(mmi_mux_host *h, const mmi_mux_frame *f, uint64_t now)
{
    int rc;
    h->mux_acknowledgement = f->sequence;
    report(h, "mux.frame.rx", f->protocol, (uint32_t)f->payload_length);
    /* DiPlay does not require 0xFEEDFACE/0xFACEFACE on ordinary received TCP
     * frames. The captured FACEFACE value is only an optional-padding guard.
     */
    report(h, "mux.frame.word8", f->word8, f->sequence);
    if (h->state == MMI_MUX_HOST_WAIT_VERSION) {
        if (f->protocol == 6u && ++h->stale_frames <= MAX_STALE_FRAMES) {
            report(h, "mux.handshake.stale_tcp", h->stale_frames, 0);
            return MMI_MUX_HOST_OK;
        }
        if (f->protocol != 0u || f->word8 != 2u || f->payload_length != 4u)
            return host_fail(h, MMI_MUX_HOST_PROTOCOL_ERROR);
        {
            const uint8_t setup = 7u;
            rc = send_mux(h, 2u, &setup, 1);
        }
        if (rc != MMI_MUX_HOST_OK) return rc;
        h->deadline_ms = 0;
        host_state(h, MMI_MUX_HOST_READY);
        return MMI_MUX_HOST_OK;
    }
    if (f->protocol == 6u) {
        (void)dispatch_tcp(h, f->payload, f->payload_length, now);
        /* A bad/reset TCP connection is isolated. Failure of USB I/O from its
         * reset/ACK escalates to host failure and must escape the feed loop.
         */
        return h->state == MMI_MUX_HOST_FAILED ? h->failure : MMI_MUX_HOST_OK;
    }
    if (f->protocol == 1u && f->word8 == REPLY_MAGIC &&
        valid_diagnostic(f->payload, f->payload_length)) {
        report(h, "mux.peer.diagnostic", (uint32_t)f->payload_length, 0);
        return MMI_MUX_HOST_OK;
    }
    return host_fail(h, MMI_MUX_HOST_PROTOCOL_ERROR);
}

static int process_frames(mmi_mux_host *h, uint64_t now)
{
    mmi_mux_frame frame;
    size_t consumed;
    int rc, padding;
    while (h->frame_length) {
        rc = mmi_mux_decode(h->frame_bytes, h->frame_length, &frame, &consumed);
        if (rc < 0) {
            padding = padding_candidate(h);
            if (padding == 0) return MMI_MUX_HOST_OK;
            if (padding < 0) return host_fail(h, MMI_MUX_HOST_PROTOCOL_ERROR);
            memmove(h->frame_bytes, h->frame_bytes + 4, h->frame_length - 4);
            h->frame_length -= 4;
            h->optional_padding = 0;
            if (h->padding_reports++ < 4u) report(h, "mux.reply.padding", 4, 0);
            continue;
        }
        if (rc == 0) return MMI_MUX_HOST_OK;
        h->optional_padding = can_have_padding(&frame);
        rc = dispatch_frame(h, &frame, now);
        if (rc != MMI_MUX_HOST_OK) return rc;
        h->frame_length -= consumed;
        memmove(h->frame_bytes, h->frame_bytes + consumed, h->frame_length);
    }
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_init(mmi_mux_host *h, const mmi_mux_host_transport *transport)
{
    if (!h || !transport || !transport->write_all) return MMI_MUX_HOST_BAD_ARGUMENT;
    memset(h, 0, sizeof(*h));
    h->transport = *transport;
    h->state = MMI_MUX_HOST_IDLE;
    h->write_timeout_ms = 2000u;
    h->acknowledgement_timeout_ms = 5000u;
    h->close_timeout_ms = 5000u;
    h->next_source_port = 1u;
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_tick(mmi_mux_host *h, uint64_t now)
{
    size_t i;
    if (use_time(h, now) != MMI_MUX_HOST_OK) return MMI_MUX_HOST_BAD_ARGUMENT;
    if (h->state == MMI_MUX_HOST_FAILED) return h->failure;
    if (h->state == MMI_MUX_HOST_CLOSED) return MMI_MUX_HOST_OK;
    if (h->deadline_ms && now >= h->deadline_ms)
        return host_fail(h, MMI_MUX_HOST_TIMEOUT);
    for (i = 0; i < MMI_MUX_HOST_CONNECTIONS; ++i) {
        mmi_mux_host_connection *c = &h->connections[i];
        if (active(c) && c->deadline_ms && now >= c->deadline_ms) {
            (void)conn_fail(h, c, MMI_MUX_HOST_TIMEOUT, 1);
            if (h->state == MMI_MUX_HOST_FAILED) return h->failure;
        }
    }
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_start(mmi_mux_host *h, uint64_t now, uint32_t timeout)
{
    int rc;
    size_t n;
    if (!timeout || use_time(h, now) != MMI_MUX_HOST_OK) return MMI_MUX_HOST_BAD_ARGUMENT;
    if (h->state != MMI_MUX_HOST_IDLE || h->transport_closed) return MMI_MUX_HOST_BAD_STATE;
    n = mmi_mux_version(h->tx_bytes, sizeof(h->tx_bytes));
    rc = write_bytes(h, n);
    if (rc != MMI_MUX_HOST_OK) return rc;
    h->deadline_ms = deadline(now, timeout);
    host_state(h, MMI_MUX_HOST_WAIT_VERSION);
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_transport_failed(mmi_mux_host *h, uint64_t now)
{
    if (use_time(h, now) != MMI_MUX_HOST_OK) return MMI_MUX_HOST_BAD_ARGUMENT;
    if (h->state == MMI_MUX_HOST_CLOSED) return MMI_MUX_HOST_BAD_STATE;
    return host_fail(h, MMI_MUX_HOST_IO_ERROR);
}

int mmi_mux_host_feed(mmi_mux_host *h, const uint8_t *bytes, size_t length,
                      uint64_t now)
{
    int rc;
    size_t n;
    if (!bytes && length) return MMI_MUX_HOST_BAD_ARGUMENT;
    rc = mmi_mux_host_tick(h, now);
    if (rc != MMI_MUX_HOST_OK) return rc;
    if (h->state != MMI_MUX_HOST_WAIT_VERSION && h->state != MMI_MUX_HOST_READY)
        return MMI_MUX_HOST_BAD_STATE;
    while (length) {
        n = sizeof(h->frame_bytes) - h->frame_length;
        if (n > length) n = length;
        if (!n) return host_fail(h, MMI_MUX_HOST_RX_OVERFLOW);
        memcpy(h->frame_bytes + h->frame_length, bytes, n);
        h->frame_length += n;
        bytes += n;
        length -= n;
        rc = process_frames(h, now);
        if (rc != MMI_MUX_HOST_OK) return rc;
    }
    return MMI_MUX_HOST_OK;
}

static uint16_t allocate_source_port(mmi_mux_host *h)
{
    uint32_t attempts;
    size_t i;
    for (attempts = 0; attempts < 65535u; ++attempts) {
        uint16_t candidate = h->next_source_port;
        int used = 0;
        h->next_source_port = candidate == 65535u ? 1u : (uint16_t)(candidate + 1u);
        for (i = 0; i < MMI_MUX_HOST_CONNECTIONS; ++i)
            if (h->connections[i].state != MMI_MUX_CONN_FREE &&
                h->connections[i].source_port == candidate) { used = 1; break; }
        if (!used) return candidate;
    }
    return 0;
}

int mmi_mux_host_connect(mmi_mux_host *h, uint16_t destination, uint32_t sequence,
                         uint64_t now, uint32_t timeout,
                         mmi_mux_connection_handle *handle)
{
    size_t i;
    uint16_t source;
    mmi_mux_host_connection *c = NULL;
    int rc;
    if (!handle || !destination || !timeout) return MMI_MUX_HOST_BAD_ARGUMENT;
    handle->slot = UINT32_MAX;
    handle->generation = 0;
    rc = mmi_mux_host_tick(h, now);
    if (rc != MMI_MUX_HOST_OK) return rc;
    if (h->state != MMI_MUX_HOST_READY) return MMI_MUX_HOST_BAD_STATE;
    for (i = 0; i < MMI_MUX_HOST_CONNECTIONS; ++i)
        if (h->connections[i].state == MMI_MUX_CONN_FREE &&
            h->connections[i].generation != UINT32_MAX) { c = &h->connections[i]; break; }
    if (!c) return MMI_MUX_HOST_NO_SLOT;
    source = allocate_source_port(h);
    if (!source) return MMI_MUX_HOST_NO_SLOT;
    ++c->generation; /* Never wrap: generation-exhausted slots stay retired. */
    c->source_port = source;
    c->destination_port = destination;
    c->initial_send_sequence = sequence;
    c->send_unacknowledged = c->send_next = sequence;
    c->deadline_ms = deadline(now, timeout);
    conn_state(h, c, MMI_MUX_CONN_SYN_SENT);
    handle->slot = (uint32_t)i;
    handle->generation = c->generation;
    rc = send_tcp(h, c, SYN, NULL, 0);
    if (rc != MMI_MUX_HOST_OK) return rc;
    ++c->send_next;
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_connection_status(const mmi_mux_host *h,
                                   mmi_mux_connection_handle handle,
                                   mmi_mux_connection_state *state, int *failure)
{
    const mmi_mux_host_connection *c;
    if (!h || !state || !failure) return MMI_MUX_HOST_BAD_ARGUMENT;
    if (handle.slot >= MMI_MUX_HOST_CONNECTIONS || !handle.generation)
        return MMI_MUX_HOST_BAD_HANDLE;
    c = &h->connections[handle.slot];
    if (c->state == MMI_MUX_CONN_FREE || c->generation != handle.generation)
        return MMI_MUX_HOST_BAD_HANDLE;
    *state = c->state;
    *failure = c->failure;
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_send(mmi_mux_host *h, mmi_mux_connection_handle handle,
                      const uint8_t *bytes, size_t length, size_t *accepted,
                      uint64_t now)
{
    mmi_mux_host_connection *c;
    size_t n;
    int rc;
    if (!accepted || (!bytes && length)) return MMI_MUX_HOST_BAD_ARGUMENT;
    *accepted = 0;
    rc = mmi_mux_host_tick(h, now);
    if (rc != MMI_MUX_HOST_OK) return rc;
    c = connection(h, handle);
    if (!c) return MMI_MUX_HOST_BAD_HANDLE;
    if (c->state == MMI_MUX_CONN_FAILED) return c->failure;
    if (h->state != MMI_MUX_HOST_READY || c->state != MMI_MUX_CONN_OPEN || c->peer_eof)
        return MMI_MUX_HOST_BAD_STATE;
    if (!length) return MMI_MUX_HOST_OK;
    if (c->send_unacknowledged != c->send_next || !c->peer_window) return MMI_MUX_HOST_AGAIN;
    n = length;
    if (n > MMI_MUX_HOST_STREAM_CAPACITY) n = MMI_MUX_HOST_STREAM_CAPACITY;
    if (n > c->peer_window) n = c->peer_window;
    rc = send_tcp(h, c, ACK, bytes, n);
    if (rc != MMI_MUX_HOST_OK) return rc;
    c->send_next += (uint32_t)n;
    c->deadline_ms = deadline(now, h->acknowledgement_timeout_ms);
    *accepted = n;
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_receive(mmi_mux_host *h, mmi_mux_connection_handle handle,
                         uint8_t *out, size_t capacity, size_t *received,
                         uint64_t now)
{
    mmi_mux_host_connection *c;
    size_t n;
    int rc;
    if (!out || !capacity || !received) return MMI_MUX_HOST_BAD_ARGUMENT;
    *received = 0;
    rc = mmi_mux_host_tick(h, now);
    if (rc != MMI_MUX_HOST_OK) return rc;
    c = connection(h, handle);
    if (!c) return MMI_MUX_HOST_BAD_HANDLE;
    if (c->state == MMI_MUX_CONN_FAILED) return c->failure;
    if (c->state == MMI_MUX_CONN_SYN_SENT) return MMI_MUX_HOST_BAD_STATE;
    if (!c->stream_length)
        return c->peer_eof || c->state == MMI_MUX_CONN_CLOSED ? MMI_MUX_HOST_EOF : MMI_MUX_HOST_AGAIN;
    n = capacity < c->stream_length ? capacity : c->stream_length;
    memcpy(out, c->stream_bytes, n);
    c->stream_length -= n;
    memmove(c->stream_bytes, c->stream_bytes + n, c->stream_length);
    memset(c->stream_bytes + c->stream_length, 0, n);
    *received = n;
    if (c->state == MMI_MUX_CONN_OPEN && h->state == MMI_MUX_HOST_READY)
        return send_tcp(h, c, ACK, NULL, 0);
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_connection_close(mmi_mux_host *h, mmi_mux_connection_handle handle,
                                  uint64_t now)
{
    mmi_mux_host_connection *c;
    int rc = mmi_mux_host_tick(h, now);
    if (rc != MMI_MUX_HOST_OK) return rc;
    c = connection(h, handle);
    if (!c) return MMI_MUX_HOST_BAD_HANDLE;
    if (c->state == MMI_MUX_CONN_FAILED) return c->failure;
    if (c->state == MMI_MUX_CONN_CLOSED) return MMI_MUX_HOST_OK;
    if (c->state == MMI_MUX_CONN_SYN_SENT) {
        rc = send_tcp(h, c, RST | ACK, NULL, 0);
        if (rc != MMI_MUX_HOST_OK) return rc;
        conn_finished(h, c);
    } else if (c->state == MMI_MUX_CONN_OPEN) {
        rc = send_tcp(h, c, FIN | ACK, NULL, 0);
        if (rc != MMI_MUX_HOST_OK) return rc;
        ++c->send_next;
        c->deadline_ms = deadline(now, h->close_timeout_ms);
        conn_state(h, c, MMI_MUX_CONN_FIN_WAIT_1);
    }
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_connection_abort(mmi_mux_host *h, mmi_mux_connection_handle handle,
                                  uint64_t now)
{
    mmi_mux_host_connection *c;
    int rc = mmi_mux_host_tick(h, now);
    if (rc != MMI_MUX_HOST_OK) return rc;
    c = connection(h, handle);
    if (!c) return MMI_MUX_HOST_BAD_HANDLE;
    if (c->state == MMI_MUX_CONN_CLOSED) return MMI_MUX_HOST_OK;
    return conn_fail(h, c, MMI_MUX_HOST_ABORTED, 1);
}

int mmi_mux_host_connection_release(mmi_mux_host *h, mmi_mux_connection_handle handle)
{
    mmi_mux_host_connection *c = connection(h, handle);
    uint32_t generation;
    if (!c) return MMI_MUX_HOST_BAD_HANDLE;
    if (c->state != MMI_MUX_CONN_CLOSED && c->state != MMI_MUX_CONN_FAILED)
        return MMI_MUX_HOST_BAD_STATE;
    generation = c->generation;
    report(h, "mux.conn.release", c->source_port, generation);
    erase(c, sizeof(*c));
    c->generation = generation;
    return MMI_MUX_HOST_OK;
}

int mmi_mux_host_close(mmi_mux_host *h, uint64_t now)
{
    size_t i;
    int rc;
    if (use_time(h, now) != MMI_MUX_HOST_OK) return MMI_MUX_HOST_BAD_ARGUMENT;
    if (h->state == MMI_MUX_HOST_FAILED) return h->failure;
    if (h->state == MMI_MUX_HOST_CLOSED) return MMI_MUX_HOST_OK;
    for (i = 0; i < MMI_MUX_HOST_CONNECTIONS; ++i) {
        mmi_mux_host_connection *c = &h->connections[i];
        if (active(c)) {
            rc = send_tcp(h, c, RST | ACK, NULL, 0);
            if (rc != MMI_MUX_HOST_OK) return rc;
            conn_finished(h, c);
        }
    }
    h->deadline_ms = 0;
    host_state(h, MMI_MUX_HOST_CLOSED);
    release_transport(h);
    return MMI_MUX_HOST_OK;
}

void mmi_mux_host_clear(mmi_mux_host *h)
{
    if (!h) return;
    release_transport(h);
    erase(h, sizeof(*h));
}
