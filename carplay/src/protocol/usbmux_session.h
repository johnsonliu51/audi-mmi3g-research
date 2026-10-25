/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MMI_USBMUX_SESSION_H
#define MMI_USBMUX_SESSION_H

#include <stddef.h>
#include <stdint.h>

#define MMI_MUX_SESSION_FRAME_CAPACITY 65540u
#define MMI_MUX_SESSION_STREAM_CAPACITY 16384u
#define MMI_MUX_SESSION_TX_CAPACITY (16u + 20u + 16384u)
#define MMI_MUX_LOCKDOWN_PORT 62078u

typedef enum {
    MMI_MUX_IDLE, MMI_MUX_WAIT_VERSION, MMI_MUX_READY, MMI_MUX_SYN_SENT,
    MMI_MUX_OPEN, MMI_MUX_FIN_WAIT_1, MMI_MUX_FIN_WAIT_2,
    MMI_MUX_CLOSING, MMI_MUX_LAST_ACK, MMI_MUX_CLOSED, MMI_MUX_FAILED
} mmi_mux_session_state;

typedef enum {
    MMI_MUX_OK = 0,
    MMI_MUX_AGAIN = 1,
    MMI_MUX_EOF = 2,
    MMI_MUX_BAD_ARGUMENT = -1,
    MMI_MUX_BAD_STATE = -2,
    MMI_MUX_IO_ERROR = -3,
    MMI_MUX_PROTOCOL_ERROR = -4,
    MMI_MUX_TIMEOUT = -5,
    MMI_MUX_RX_OVERFLOW = -6,
    MMI_MUX_PEER_RESET = -7,
    MMI_MUX_ABORTED = -8
} mmi_mux_session_result;

/* These callbacks must not call back into the same session.
 * write_all: 0 only after ALL bytes have been transferred; nonzero is fatal.
 * It must enforce timeout_ms internally, including short USB writes.
 * close_transport: called once at final close/failure, not at local FIN.
 * diagnostic: event and integer metadata only, never stream/identity payload.
 * USB reads are owned by the caller and delivered through feed().
 */
typedef struct {
    void *context;
    int (*write_all)(void *, const uint8_t *, size_t, uint32_t timeout_ms);
    void (*close_transport)(void *);
    void (*diagnostic)(void *, const char *event, uint32_t a, uint32_t b);
} mmi_mux_session_transport;

/* Allocate outside a small QNX thread stack (approximately 97 KiB).
 * Treat the fields as private; access is serialized by the caller.
 * No malloc, thread, USB API, socket, TLS, or platform clock dependency.
 */
typedef struct {
    mmi_mux_session_transport transport;
    mmi_mux_session_state state;
    int failure;
    uint64_t last_now_ms, deadline_ms;
    uint32_t write_timeout_ms, acknowledgement_timeout_ms, close_timeout_ms;
    uint32_t send_unacknowledged, send_next, receive_next;
    uint32_t initial_send_sequence, initial_peer_sequence;
    uint16_t source_port, destination_port, mux_sequence, mux_acknowledgement;
    uint16_t peer_window;
    unsigned stale_frames, padding_reports;
    int transport_closed, optional_padding, peer_eof;
    size_t frame_length, stream_length;
    uint8_t frame_bytes[MMI_MUX_SESSION_FRAME_CAPACITY];
    uint8_t stream_bytes[MMI_MUX_SESSION_STREAM_CAPACITY];
    uint8_t tx_bytes[MMI_MUX_SESSION_TX_CAPACITY];
} mmi_mux_session;

/* All time arguments are milliseconds from one monotonic clock. Calling tick()
 * at least once per read timeout makes all handshake/ACK/close deadlines work.
 * A regressing clock is rejected without changing state.
 */
int mmi_mux_session_init(mmi_mux_session *, const mmi_mux_session_transport *);
int mmi_mux_session_start(mmi_mux_session *, uint64_t now_ms, uint32_t timeout_ms);
int mmi_mux_session_connect(mmi_mux_session *, uint16_t source_port,
                            uint16_t destination_port, uint32_t initial_sequence,
                            uint64_t now_ms, uint32_t timeout_ms);
int mmi_mux_session_feed(mmi_mux_session *, const uint8_t *, size_t,
                         uint64_t now_ms);
int mmi_mux_session_tick(mmi_mux_session *, uint64_t now_ms);
/* One segment per call, max 16 KiB, limited by the peer window. accepted can be
 * shorter than length. No pending user data is copied on AGAIN.
 * Only one data segment may be unacknowledged; no retransmissions.
 */
int mmi_mux_session_send(mmi_mux_session *, const uint8_t *, size_t,
                         size_t *accepted, uint64_t now_ms);
/* Drain accepted stream data. AGAIN means no data yet; EOF means no more data.
 * When OPEN, draining also advertises newly available receive window.
 * received remains valid if sending that window update subsequently fails.
 */
int mmi_mux_session_receive(mmi_mux_session *, uint8_t *, size_t,
                            size_t *received, uint64_t now_ms);
int mmi_mux_session_close(mmi_mux_session *, uint64_t now_ms);
int mmi_mux_session_abort(mmi_mux_session *, uint64_t now_ms);
/* Final cleanup is mandatory even on failure. Closes once and erases buffers.
 * After clear(), only init() is legal; reinitialization needs a NEW transport.
 */
void mmi_mux_session_clear(mmi_mux_session *);

#endif
