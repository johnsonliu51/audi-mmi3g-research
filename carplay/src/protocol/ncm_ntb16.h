/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from DiPlay Ntb16Codec.kt; see PROVENANCE.md.
 */
#ifndef MMI_NCM_NTB16_H
#define MMI_NCM_NTB16_H
#include <stddef.h>
#include <stdint.h>
typedef void (*mmi_ncm_datagram)(void *, const uint8_t *, size_t);
/* Encode one Ethernet frame. packet_size is the actual endpoint max packet
 * size, not assumed 512. Buffers must not overlap. Zero means invalid input.
 * Returned transfer length includes optional USB pad; NTH length excludes it.
 */
size_t mmi_ntb16_encode(uint8_t *, size_t, const uint8_t *, size_t,
                        uint16_t sequence, size_t packet_size);
/* One complete NTB16, optional one-byte zero transfer pad. No NTB32 or CRC.
 * Strict validation of all tables before callbacks. Returns datagram count,
 * or -1 for malformed/unsupported data. Borrowed data valid during callback.
 */
int mmi_ntb16_decode(const uint8_t *, size_t, mmi_ncm_datagram, void *);
#endif
