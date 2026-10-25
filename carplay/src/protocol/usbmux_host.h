/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MMI_USBMUX_HOST_H
#define MMI_USBMUX_HOST_H

#include <stddef.h>
#include <stdint.h>

#define MMI_MUX_HOST_CONNECTIONS 4u
#define MMI_MUX_HOST_FRAME_CAPACITY 65540u
#define MMI_MUX_HOST_STREAM_CAPACITY 16384u
#define MMI_MUX_HOST_TX_CAPACITY (16u + 20u + 16384u)
#define MMI_MUX_HOST_LOCKDOWN_PORT 62078u

typedef enum {
    MMI_MUX_HOST_IDLE, MMI_MUX_HOST_WAIT_VERSION, MMI_MUX_HOST_READY,
    MMI_MUX_HOST_CLOSED, MMI_MUX_HOST_FAILED
} mmi_mux_host_state;

typedef enum {
    MMI_MUX_CONN_FREE, MMI_MUX_CONN_SYN_SENT, MMI_MUX_CONN_OPEN,
    MMI_MUX_CONN_FIN_WAIT_1, MMI_MUX_CONN_FIN_WAIT_2, MMI_MUX_CONN_CLOSING,
    MMI_MUX_CONN_LAST_ACK, MMI_MUX_CONN_CLOSED, MMI_MUX_CONN_FAILED
} mmi_mux_connection_state;

typedef enum {
    MMI_MUX_HOST_OK = 0, MMI_MUX_HOST_AGAIN = 1, MMI_MUX_HOST_EOF = 2,
    MMI_MUX_HOST_BAD_ARGUMENT = -1, MMI_MUX_HOST_BAD_STATE = -2,
    MMI_MUX_HOST_IO_ERROR = -3, MMI_MUX_HOST_PROTOCOL_ERROR = -4,
    MMI_MUX_HOST_TIMEOUT = -5, MMI_MUX_HOST_RX_OVERFLOW = -6,
    MMI_MUX_HOST_PEER_RESET = -7, MMI_MUX_HOST_ABORTED = -8,
    MMI_MUX_HOST_BAD_HANDLE = -9, MMI_MUX_HOST_NO_SLOT = -10
} mmi_mux_host_result;

typedef struct { uint32_t slot, generation; } mmi_mux_connection_handle;

/* All calls are serialized; callbacks must not reenter this host.
 * write_all: return 0 only after the WHOLE buffer was written within timeout.
 * close_transport: owned by the HOST; called exactly once at host termination.
 * diagnostic: metadata only; no TCP payload or phone-generated text.
 * USB reads are managed by the caller and submitted through host_feed().
 */
typedef struct {
    void *context;
    int (*write_all)(void *, const uint8_t *, size_t, uint32_t timeout_ms);
    void (*close_transport)(void *);
    void (*diagnostic)(void *, const char *event, uint32_t a, uint32_t b);
} mmi_mux_host_transport;

/* Internal fields; use handles and the APIs below. */
typedef struct {
    mmi_mux_connection_state state;
    int failure, peer_eof;
    uint32_t generation;
    uint64_t deadline_ms;
    uint16_t source_port, destination_port, peer_window;
    uint32_t send_unacknowledged, send_next, receive_next;
    uint32_t initial_send_sequence, initial_peer_sequence;
    size_t stream_length;
    uint8_t stream_bytes[MMI_MUX_HOST_STREAM_CAPACITY];
} mmi_mux_host_connection;

/* Approximately 145 KiB: allocate on heap or static storage, not a small QNX
 * thread stack. Host contains the one shared framer and transport owner.
 */
typedef struct {
    mmi_mux_host_transport transport;
    mmi_mux_host_state state;
    int failure, transport_closed, optional_padding;
    uint64_t last_now_ms, deadline_ms;
    uint32_t write_timeout_ms, acknowledgement_timeout_ms, close_timeout_ms;
    uint16_t mux_sequence, mux_acknowledgement, next_source_port;
    unsigned stale_frames, padding_reports;
    size_t frame_length;
    mmi_mux_host_connection connections[MMI_MUX_HOST_CONNECTIONS];
    uint8_t frame_bytes[MMI_MUX_HOST_FRAME_CAPACITY];
    uint8_t tx_bytes[MMI_MUX_HOST_TX_CAPACITY];
} mmi_mux_host;

int mmi_mux_host_init(mmi_mux_host *, const mmi_mux_host_transport *);
/* VERSION/SETUP occur ONCE per host. A live host cannot be started twice. */
int mmi_mux_host_start(mmi_mux_host *, uint64_t now_ms, uint32_t timeout_ms);
int mmi_mux_host_feed(mmi_mux_host *, const uint8_t *, size_t, uint64_t now_ms);
/* Invoke on every transport read timeout as well as incoming data.
 * Per-connection timeout terminates just that connection, not the host.
 * Returns a host-level error only; inspect connection_status for local errors.
 */
int mmi_mux_host_tick(mmi_mux_host *, uint64_t now_ms);
/* A real shared USB read/disconnect error terminates ALL active connections.
 * A normal read timeout is NOT a transport failure: call tick() instead.
 * No reset writes are attempted after this notification.
 */
int mmi_mux_host_transport_failed(mmi_mux_host *, uint64_t now_ms);

/* Allocates an unused nonzero source port and slot; sends only a TCP SYN.
 * Separate handles allow secure Lockdown and the returned carkit port to live
 * together over one host. The actual TLS/Lockdown/service startup is external.
 * Store the returned handle; released handles are rejected on slot reuse.
 */
int mmi_mux_host_connect(mmi_mux_host *, uint16_t destination_port,
                         uint32_t initial_sequence, uint64_t now_ms,
                         uint32_t timeout_ms, mmi_mux_connection_handle *);
int mmi_mux_host_connection_status(const mmi_mux_host *, mmi_mux_connection_handle,
                                   mmi_mux_connection_state *, int *failure);

/* One outstanding data segment per connection, max 16 KiB and peer window.
 * accepted may be short. AGAIN queues nothing; caller retains unsent bytes.
 */
int mmi_mux_host_send(mmi_mux_host *, mmi_mux_connection_handle,
                      const uint8_t *, size_t, size_t *accepted, uint64_t now_ms);
/* received remains valid even if its subsequent window-update write fails. */
int mmi_mux_host_receive(mmi_mux_host *, mmi_mux_connection_handle,
                         uint8_t *, size_t, size_t *received, uint64_t now_ms);
/* These close ONLY the selected TCP connection. Continue feeding/ticking until
 * CLOSED/FAILED, then release. Other connections and the USB host stay alive.
 */
int mmi_mux_host_connection_close(mmi_mux_host *, mmi_mux_connection_handle,
                                  uint64_t now_ms);
int mmi_mux_host_connection_abort(mmi_mux_host *, mmi_mux_connection_handle,
                                  uint64_t now_ms);
/* Requires CLOSED/FAILED; discards unread data, erases that slot, invalidates
 * the handle, and keeps its generation counter. No transport close/SETUP.
 */
int mmi_mux_host_connection_release(mmi_mux_host *, mmi_mux_connection_handle);

/* Immediate host shutdown. It attempts RST on all active connections, then
 * closes the transport once. For graceful shutdown, close connections first.
 */
int mmi_mux_host_close(mmi_mux_host *, uint64_t now_ms);
/* Mandatory final erase after shutdown/failure. After clear(), init() with a
 * new transport is required. Do not reinitialize an uncleared live host.
 */
void mmi_mux_host_clear(mmi_mux_host *);

#endif
