/* SPDX-License-Identifier: GPL-3.0-only */
#include "lockdown_channel.h"
#include <string.h>
#define MAX_PLIST (16u * 1024u * 1024u)

int mmi_ld_init(mmi_ld_channel *channel, const mmi_ld_stream *stream, size_t maximum)
{
    if (!channel || !stream || !stream->read || !stream->write ||
        !stream->now_ms || !stream->close || !maximum || maximum > MAX_PLIST)
        return MMI_LD_ARGUMENT;
    channel->stream = *stream;
    channel->maximum = maximum;
    channel->closed = 0;
    return MMI_LD_OK;
}

void mmi_ld_close(mmi_ld_channel *channel)
{
    if (!channel || channel->closed) return;
    channel->closed = 1;
    channel->stream.close(channel->stream.context);
}

int mmi_ld_detach(mmi_ld_channel *channel, mmi_ld_stream *stream)
{
    if (!channel || !stream) return MMI_LD_ARGUMENT;
    if (channel->closed) return MMI_LD_CLOSED;
    *stream = channel->stream;
    channel->closed = 1;
    return MMI_LD_OK;
}

static int deadline(mmi_ld_channel *channel, uint32_t timeout, uint64_t *until)
{
    uint64_t now;
    if (!timeout) return MMI_LD_ARGUMENT;
    now = channel->stream.now_ms(channel->stream.context);
    if (now > UINT64_MAX - timeout) return MMI_LD_ARGUMENT;
    *until = now + timeout;
    return MMI_LD_OK;
}

static int transfer(mmi_ld_channel *channel, uint8_t *input,
                     const uint8_t *output, size_t length, uint64_t until)
{
    size_t offset = 0;
    while (offset < length) {
        uint64_t now = channel->stream.now_ms(channel->stream.context);
        uint64_t remaining;
        ptrdiff_t result;
        if (now >= until) return MMI_LD_TIMEOUT;
        remaining = until - now;
        if (remaining > UINT32_MAX) return MMI_LD_IO; /* invalid/backwards clock */
        if (input) result = channel->stream.read(channel->stream.context,
                              input + offset, length - offset, (uint32_t)remaining);
        else result = channel->stream.write(channel->stream.context,
                              output + offset, length - offset, (uint32_t)remaining);
        if (result == -2) return MMI_LD_TIMEOUT;
        if (result < 0 || (size_t)result > length - offset) return MMI_LD_IO;
        if (!result) return input ? MMI_LD_EOF : MMI_LD_IO;
        offset += (size_t)result;
    }
    /* A blocking adapter which exceeded the deadline is not accepted. */
    return channel->stream.now_ms(channel->stream.context) >= until
           ? MMI_LD_TIMEOUT : MMI_LD_OK;
}

int mmi_ld_send(mmi_ld_channel *channel, const uint8_t *plist, size_t length,
                 uint32_t timeout_ms)
{
    uint8_t header[4];
    uint64_t until;
    int result;
    if (!channel || !plist) return MMI_LD_ARGUMENT;
    if (channel->closed) return MMI_LD_CLOSED;
    if (!length || length > channel->maximum) return MMI_LD_LENGTH;
    result = deadline(channel, timeout_ms, &until);
    if (result != MMI_LD_OK) return result;
    header[0] = (uint8_t)(length >> 24); header[1] = (uint8_t)(length >> 16);
    header[2] = (uint8_t)(length >> 8); header[3] = (uint8_t)length;
    result = transfer(channel, NULL, header, sizeof(header), until);
    if (result == MMI_LD_OK) result = transfer(channel, NULL, plist, length, until);
    if (result != MMI_LD_OK) mmi_ld_close(channel); /* partial framing cannot be reused */
    return result;
}

int mmi_ld_receive(mmi_ld_channel *channel, uint8_t *plist, size_t capacity,
                    size_t *received, uint32_t timeout_ms)
{
    uint8_t header[4];
    uint64_t until;
    size_t length;
    int result;
    if (!received) return MMI_LD_ARGUMENT;
    *received = 0;
    if (!channel || !plist || !capacity) return MMI_LD_ARGUMENT;
    if (channel->closed) return MMI_LD_CLOSED;
    result = deadline(channel, timeout_ms, &until);
    if (result != MMI_LD_OK) return result;
    result = transfer(channel, header, NULL, sizeof(header), until);
    if (result == MMI_LD_OK) {
        length = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
                 ((size_t)header[2] << 8) | header[3];
        if (!length || length > channel->maximum) result = MMI_LD_LENGTH;
        else if (length > capacity) result = MMI_LD_CAPACITY;
        else {
            result = transfer(channel, plist, NULL, length, until);
            if (result == MMI_LD_OK) *received = length;
        }
    }
    if (result != MMI_LD_OK) mmi_ld_close(channel);
    return result;
}
