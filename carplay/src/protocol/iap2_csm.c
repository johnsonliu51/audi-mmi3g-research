/* SPDX-License-Identifier: GPL-3.0-only */
#include "iap2_csm.h"
#include <string.h>

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

int mmi_csm_init(mmi_csm_parser *parser, uint8_t *storage, size_t capacity)
{
    if (!parser || !storage || capacity < MMI_CSM_MAX_FRAME) return -1;
    parser->bytes = storage;
    parser->used = 0;
    return 0;
}

int mmi_csm_feed(mmi_csm_parser *parser, const uint8_t *data, size_t length,
                 mmi_csm_callback callback, void *context)
{
    size_t i, frame_length;
    if (!parser || !parser->bytes || parser->used >= MMI_CSM_MAX_FRAME ||
        (!data && length) || !callback) return -1;
    for (i = 0; i < length; ++i) {
        parser->bytes[parser->used++] = data[i];
        /* Preserve a single trailing 0x40 when seeking a marker. */
        while (parser->used >= 2 &&
               (parser->bytes[0] != 0x40 || parser->bytes[1] != 0x40)) {
            --parser->used;
            memmove(parser->bytes, parser->bytes + 1, parser->used);
        }
        if (parser->used < MMI_CSM_HEADER) continue;
        frame_length = read16(parser->bytes + 2);
        if (frame_length < MMI_CSM_HEADER) {
            --parser->used;
            memmove(parser->bytes, parser->bytes + 1, parser->used);
            continue;
        }
        if (parser->used == frame_length) {
            callback(context, read16(parser->bytes + 4),
                     parser->bytes + MMI_CSM_HEADER,
                     frame_length - MMI_CSM_HEADER);
            parser->used = 0;
        }
    }
    return 0;
}

size_t mmi_csm_pending(const mmi_csm_parser *parser)
{
    return parser ? parser->used : 0;
}

size_t mmi_csm_encode(uint8_t *out, size_t capacity, uint16_t message,
                      const uint8_t *body, size_t length)
{
    size_t total;
    if (!out || (!body && length) || length > MMI_CSM_MAX_FRAME - MMI_CSM_HEADER)
        return 0;
    total = MMI_CSM_HEADER + length;
    if (capacity < total) return 0;
    write16(out, 0x4040);
    write16(out + 2, (uint16_t)total);
    write16(out + 4, message);
    if (length) memcpy(out + MMI_CSM_HEADER, body, length);
    return total;
}

size_t mmi_csm_param_encode(uint8_t *out, size_t capacity, uint16_t id,
                            const uint8_t *value, size_t length)
{
    size_t total;
    if (!out || (!value && length) || length > 65535u - 4u) return 0;
    total = length + 4u;
    if (capacity < total) return 0;
    write16(out, (uint16_t)total);
    write16(out + 2, id);
    if (length) memcpy(out + 4, value, length);
    return total;
}

int mmi_csm_param_next(const uint8_t *body, size_t length, size_t *offset,
                       uint16_t *id, const uint8_t **value, size_t *value_length)
{
    size_t count, remaining;
    if ((!body && length) || !offset || !id || !value || !value_length ||
        *offset > length) return -1;
    remaining = length - *offset;
    if (!remaining) return 0;
    if (remaining < 4) return -1;
    count = read16(body + *offset);
    if (count < 4 || count > remaining) return -1;
    *id = read16(body + *offset + 2);
    *value = body + *offset + 4;
    *value_length = count - 4;
    *offset += count;
    return 1;
}
