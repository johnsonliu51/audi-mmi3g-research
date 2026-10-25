/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MMI_USBMUX_CODEC_H
#define MMI_USBMUX_CODEC_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t protocol, word8;
    uint16_t sequence, acknowledgement;
    const uint8_t *payload;
    size_t payload_length;
} mmi_mux_frame;
/* One frame at buffer start: 1 complete, 0 incomplete, -1 invalid.
 * consumed excludes any optional iOS reply trailer. Output borrows input.
 */
int mmi_mux_decode(const uint8_t *, size_t, mmi_mux_frame *, size_t *consumed);
size_t mmi_mux_version(uint8_t *, size_t);
size_t mmi_mux_encode(uint8_t *, size_t, uint32_t protocol,
                      uint16_t sequence, uint16_t acknowledgement,
                      const uint8_t *, size_t);
#endif
