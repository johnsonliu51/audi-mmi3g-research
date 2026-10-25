/* SPDX-License-Identifier: GPL-3.0-only
 * Reference: DiPlay MediaCodecSupport.toAnnexB,
 * commit 858a7ee89c188f5c7d825fadbceb514f0bc73f7a.
 */
#include "avc_nal.h"
#include <string.h>
int mmi_avc_config_parse(const uint8_t *input, size_t length,
                         mmi_avc_config *config, uint8_t *out,
                         size_t capacity, size_t *written)
{
    mmi_avc_config parsed;
    size_t offset, count, total = 0, dest = 0;
    unsigned int pass, group, i, sets;
    if (!written) return -1;
    *written = 0;
    if (!input || !config || length < 7 || input[0] != 1 ||
        (input[4] & 0xfc) != 0xfc || (input[5] & 0xe0) != 0xe0 ||
        (input[4] & 3) == 2) return -1;
    memset(&parsed, 0, sizeof(parsed));
    parsed.profile = input[1]; parsed.compatibility = input[2];
    parsed.level = input[3]; parsed.prefix_bytes = (input[4] & 3) + 1;
    parsed.sps_count = input[5] & 31;
    if (!parsed.sps_count) return -1;
    for (pass = 0; pass < 2; ++pass) {
        offset = 6;
        for (group = 0; group < 2; ++group) {
            if (!group) sets = parsed.sps_count;
            else {
                if (offset >= length) return -1;
                sets = input[offset++];
                if (!sets) return -1;
                parsed.pps_count = sets;
            }
            for (i = 0; i < sets; ++i) {
                if (length - offset < 2) return -1;
                count = ((size_t)input[offset] << 8) | input[offset + 1];
                offset += 2;
                if (!count || count > length - offset ||
                    (input[offset] & 0x80) ||
                    (input[offset] & 31u) != (group ? 8u : 7u)) return -1;
                if (!pass) {
                    if (total > (size_t)-1 - 4 || count > (size_t)-1 - total - 4)
                        return -1;
                    total += count + 4;
                } else if (out) {
                    out[dest++] = 0; out[dest++] = 0;
                    out[dest++] = 0; out[dest++] = 1;
                    memcpy(out + dest, input + offset, count); dest += count;
                }
                offset += count;
            }
        }
        if (!pass) {
            *written = total;
            if (out && capacity < total) return -2;
        }
    }
    parsed.parsed_bytes = offset;
    *config = parsed;
    return 0;
}

int mmi_avc_annexb(const uint8_t *input, size_t length, unsigned int prefix,
                    uint8_t *out, size_t capacity, size_t *written)
{
    size_t offset, total = 0, count, dest = 0;
    unsigned int i;
    if (!written) return -1;
    *written = 0;
    if (!input || !length || (prefix != 1 && prefix != 2 && prefix != 4)) return -1;
    for (offset = 0; offset < length;) {
        if (length - offset < prefix) return -1;
        count = 0;
        for (i = 0; i < prefix; ++i) count = (count << 8) | input[offset++];
        if (!count || count > length - offset || total > (size_t)-1 - 4 ||
            count > (size_t)-1 - total - 4) return -1;
        total += 4 + count;
        offset += count;
    }
    *written = total;
    if (!out) return 0;
    if (capacity < total) return -2;
    for (offset = 0; offset < length;) {
        count = 0;
        for (i = 0; i < prefix; ++i) count = (count << 8) | input[offset++];
        out[dest++] = 0; out[dest++] = 0; out[dest++] = 0; out[dest++] = 1;
        memcpy(out + dest, input + offset, count);
        dest += count; offset += count;
    }
    return 0;
}
