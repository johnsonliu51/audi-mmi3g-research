/* SPDX-License-Identifier: GPL-3.0-only */
#include "ncm_ntb16.h"
#include <string.h>
static unsigned int u16(const uint8_t *p)
{ return p[0] | ((unsigned int)p[1] << 8); }
static void put16(uint8_t *p, unsigned int n)
{ p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }

size_t mmi_ntb16_encode(uint8_t *out, size_t capacity, const uint8_t *frame,
                        size_t length, uint16_t sequence, size_t packet_size)
{
    size_t block, transfer;
    if (!out || !frame || !length || length > 65535u - 28u || !packet_size)
        return 0;
    block = 28u + length;
    transfer = block + (block % packet_size == 0 ? 1u : 0u);
    if (capacity < transfer) return 0;
    memset(out, 0, 28);
    memcpy(out, "NCMH", 4);
    put16(out + 4, 12); put16(out + 6, sequence);
    put16(out + 8, (unsigned int)block); put16(out + 10, 12);
    memcpy(out + 12, "NCM0", 4);
    put16(out + 16, 16); put16(out + 20, 28);
    put16(out + 22, (unsigned int)length);
    memcpy(out + 28, frame, length);
    if (transfer > block) out[block] = 0;
    return transfer;
}

int mmi_ntb16_decode(const uint8_t *data, size_t length,
                     mmi_ncm_datagram callback, void *context)
{
    size_t block, table, table_length, entry, index, count, next, hops;
    int pass, total = 0, terminated;
    /* Bitmap covers all 16-bit offsets to reject pointer cycles. */
    uint8_t visited[8192];
    if (!data || !callback || length < 12 || memcmp(data, "NCMH", 4) ||
        u16(data + 4) != 12) return -1;
    block = u16(data + 8);
    if (block < 12 || block > length || length - block > 1 ||
        (length > block && data[block] != 0)) return -1;
    for (pass = 0; pass < 2; ++pass) {
        memset(visited, 0, sizeof(visited));
        table = u16(data + 10);
        hops = 0;
        while (table) {
            if (table < 12 || table > block || block - table < 12 ||
                ++hops > block / 12) return -1;
            if (visited[table / 8] & (1u << (table % 8))) return -1;
            visited[table / 8] |= (uint8_t)(1u << (table % 8));
            if (memcmp(data + table, "NCM0", 4)) return -1;
            table_length = u16(data + table + 4);
            next = u16(data + table + 6);
            if (table_length < 12 || table_length % 4 ||
                table_length > block - table) return -1;
            terminated = 0;
            for (entry = table + 8; entry < table + table_length; entry += 4) {
                index = u16(data + entry); count = u16(data + entry + 2);
                if (!index && !count) { terminated = 1; break; }
                if (!index || !count || index < 12 || index > block ||
                    count > block - index) return -1;
                if (pass == 0) ++total;
                else callback(context, data + index, count);
            }
            if (!terminated) return -1;
            table = next;
        }
    }
    return total;
}
