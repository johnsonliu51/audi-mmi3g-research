/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MMI_AVC_NAL_H
#define MMI_AVC_NAL_H
#include <stddef.h>
#include <stdint.h>
/* Explicitly length-prefixed AVC input, not Annex B autodetection.
 * prefix_bytes: 1, 2 or 4, derived from decoder configuration.
 * out==NULL computes required size. No overlapping buffers.
 * Returns 0 on success, -1 malformed/invalid, -2 insufficient capacity.
 * Output untouched on malformed input or insufficient capacity.
 */
int mmi_avc_annexb(const uint8_t *, size_t, unsigned int prefix_bytes,
                    uint8_t *out, size_t capacity, size_t *written);
typedef struct {
    unsigned int profile, compatibility, level, prefix_bytes;
    unsigned int sps_count, pps_count;
    size_t parsed_bytes; /* Extensions, if present, remain unparsed. */
} mmi_avc_config;
/* Parse base AVCDecoderConfigurationRecord, output all SPS/PPS in Annex B.
 * Reject missing/truncated/mistyped parameter sets and reserved length=3.
 * Trailing profile extensions are not interpreted; inspect parsed_bytes.
 * out==NULL sizes output. Buffers must not overlap.
 * 0 success, -1 malformed, -2 capacity. config only assigned on success.
 */
int mmi_avc_config_parse(const uint8_t *, size_t, mmi_avc_config *,
                         uint8_t *out, size_t capacity, size_t *written);
#endif
