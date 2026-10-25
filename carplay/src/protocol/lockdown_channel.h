/* SPDX-License-Identifier: GPL-3.0-only
 * Reference: DiPlay LockdownPlistChannel.kt, 858a7ee89c188f5c7d825fadbceb514f0bc73f7a.
 */
#ifndef MMI_LOCKDOWN_CHANNEL_H
#define MMI_LOCKDOWN_CHANNEL_H
#include <stddef.h>
#include <stdint.h>

/* Duplex stream is already connected (USBMUX TCP, or negotiated TLS).
 * All callbacks must return within timeout_ms. read: >0 bytes, 0 EOF,
 * -1 transport error, -2 timeout; write: >0 progress, negatives same errors.
 * No callback may return more than requested or reenter the channel.
 * now_ms is monotonic and valid for the channel lifetime.
 */
typedef struct {
    void *context;
    ptrdiff_t (*read)(void *, uint8_t *, size_t, uint32_t timeout_ms);
    ptrdiff_t (*write)(void *, const uint8_t *, size_t, uint32_t timeout_ms);
    uint64_t (*now_ms)(void *);
    void (*close)(void *);
} mmi_ld_stream;

enum {
    MMI_LD_OK = 0, MMI_LD_ARGUMENT = -1, MMI_LD_IO = -2,
    MMI_LD_TIMEOUT = -3, MMI_LD_EOF = -4, MMI_LD_LENGTH = -5,
    MMI_LD_CAPACITY = -6, MMI_LD_CLOSED = -7
};
typedef struct {
    mmi_ld_stream stream;
    size_t maximum;
    int closed;
} mmi_ld_channel;

/* On successful init the channel owns the stream until close/detach.
 * Unsuccessful init leaves ownership with caller. No threading: serialize
 * request send/receive and TLS upgrade at the caller.
 */
int mmi_ld_init(mmi_ld_channel *, const mmi_ld_stream *, size_t maximum);
void mmi_ld_close(mmi_ld_channel *);
int mmi_ld_detach(mmi_ld_channel *, mmi_ld_stream *);
int mmi_ld_send(mmi_ld_channel *, const uint8_t *plist, size_t,
                 uint32_t timeout_ms);
int mmi_ld_receive(mmi_ld_channel *, uint8_t *plist, size_t capacity,
                    size_t *received, uint32_t timeout_ms);
/* Payload is opaque: framing validates lengths only. Caller must parse the
 * plist with a bounded XML/binary-plist parser; no entity expansion/network.
 */
#endif
