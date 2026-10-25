/* SPDX-License-Identifier: GPL-3.0-only
 * C port informed by DiPlay's iAP2 CSM framing. See PROVENANCE.md.
 */
#ifndef MMI_IAP2_CSM_H
#define MMI_IAP2_CSM_H
#include <stddef.h>
#include <stdint.h>

#define MMI_CSM_HEADER 6u
#define MMI_CSM_MAX_FRAME 65535u

typedef struct {
    uint8_t *bytes;
    size_t used;
} mmi_csm_parser;

/* Body borrows parser storage, valid only during callback. Do not reenter
 * or reset this parser from the callback. No allocation or I/O in this library.
 */
typedef void (*mmi_csm_callback)(void *context, uint16_t message,
                                const uint8_t *body, size_t body_length);

/* storage must remain alive and provide MMI_CSM_MAX_FRAME bytes. */
int mmi_csm_init(mmi_csm_parser *parser, uint8_t *storage, size_t capacity);
int mmi_csm_feed(mmi_csm_parser *parser, const uint8_t *data, size_t length,
                 mmi_csm_callback callback, void *context);
size_t mmi_csm_pending(const mmi_csm_parser *parser);

/* Complete frame/parameter encoding. Input/output buffers must not overlap.
 * Return encoded length, or zero on invalid arguments/insufficient capacity.
 */
size_t mmi_csm_encode(uint8_t *out, size_t capacity, uint16_t message,
                      const uint8_t *body, size_t length);
size_t mmi_csm_param_encode(uint8_t *out, size_t capacity, uint16_t id,
                            const uint8_t *value, size_t length);

/* Iterate ordered, repeated parameters. Return 1 for a parameter, 0 at end,
 * -1 for malformed input. Offset changes only for a valid parameter.
 */
int mmi_csm_param_next(const uint8_t *body, size_t length, size_t *offset,
                       uint16_t *id, const uint8_t **value, size_t *value_length);
#endif
