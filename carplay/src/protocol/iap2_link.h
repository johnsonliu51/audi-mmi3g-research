/* GPL-3.0-only. Derived from DiPlay Iap2LinkEngine; see provenance. */
#ifndef MMI_IAP2_LINK_H
#define MMI_IAP2_LINK_H
#include <stddef.h>
#include <stdint.h>
enum iap2_link_state { IAP2_IDLE, IAP2_DETECTING, IAP2_NEGOTIATING,
                       IAP2_NORMAL, IAP2_DEAD };
/* Single caller thread. Output callback must copy/queue the complete frame;
 * delivery callback must consume/copy bytes before returning. No reentry.
 * Any callback failure terminates the link. No USB or authentication here. */
struct iap2_link;
typedef int (*iap2_link_output)(void *, const uint8_t *, size_t);
typedef int (*iap2_link_delivery)(void *, uint8_t, const uint8_t *, size_t);
struct iap2_link *iap2_link_create(void *, iap2_link_output, iap2_link_delivery);
void iap2_link_destroy(struct iap2_link *);
int iap2_link_start(struct iap2_link *, int wired, uint64_t now_ms);
int iap2_link_feed(struct iap2_link *, const uint8_t *, size_t, uint64_t);
int iap2_link_send(struct iap2_link *, uint8_t session, const uint8_t *, size_t, uint64_t);
int iap2_link_tick(struct iap2_link *, uint64_t);
void iap2_link_eof(struct iap2_link *);
enum iap2_link_state iap2_link_state(const struct iap2_link *);
const char *iap2_link_error(const struct iap2_link *);
/* 0 means no deadline. Send returns 1 for backpressure, -1 for error. */
uint64_t iap2_link_deadline(const struct iap2_link *);
#endif
