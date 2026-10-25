/* Platform contract only: no QNX backend or receiver is implemented here. */
#ifndef MMI_QNX_RECEIVER_PLATFORM_H
#define MMI_QNX_RECEIVER_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
/* All deadlines are absolute monotonic milliseconds. A timeout is not EOF. */
enum mmi_io_result { MMI_IO_OK=0, MMI_IO_TIMEOUT=1, MMI_IO_EOF=2,
                     MMI_IO_UNAVAILABLE=-1, MMI_IO_ERROR=-2 };
struct mmi_receiver_platform {
    void *context;
    uint64_t (*monotonic_ms)(void *context);
    /* Return actual transfer length in count; callers handle partial I/O. */
    int (*read)(void *context, uint8_t *data, size_t capacity,
                size_t *count, uint64_t deadline_ms);
    int (*write)(void *context, const uint8_t *data, size_t length,
                 size_t *count, uint64_t deadline_ms);
    /* Interrupt pending I/O before joining workers; must be idempotent. */
    void (*cancel)(void *context);
    void (*close)(void *context);
    /* Record stage/error only. Do not log keys or authentication payloads. */
    void (*log_stage)(void *context, const char *stage, int error);
};
/* Authentication stays behind a provider. No bundled identity or fake success. */
struct mmi_receiver_identity {
    void *context;
    int (*certificate)(void *context, uint8_t *out, size_t capacity,
                       size_t *length);
    int (*sign_challenge)(void *context, const uint8_t *challenge,
                          size_t challenge_length, uint8_t *out,
                          size_t capacity, size_t *length);
};
#endif
