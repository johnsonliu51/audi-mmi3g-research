/* SPDX-License-Identifier: GPL-3.0-only
 * Runtime/ABI diagnostic, not a CarPlay receiver. No device open or ioctl.
 */
#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <unistd.h>
#include "protocol/iap2_link.h"
static int reject_output(void *ctx, const uint8_t *p, size_t n)
{ (void)ctx; (void)p; (void)n; return -1; }
static int reject_delivery(void *ctx, uint8_t session, const uint8_t *p, size_t n)
{ (void)ctx; (void)session; (void)p; (void)n; return -1; }
int main(void)
{
    static const char *paths[]={"/dev/layermanager", "/dev/ipc/ch8",
        "/dev/io-usb/io-usb", "/dev/usb", "/usr/lib/ldqnx.so.2"};
    struct iap2_link *link;
    unsigned i;
    puts("MMI runtime check 0.2 - NOT A CARPLAY RECEIVER");
    printf("pointer_bytes=%u\n",(unsigned)sizeof(void *));
    for(i=0;i<sizeof(paths)/sizeof(paths[0]);i++)
        printf("path=%s accessible=%d\n",paths[i],access(paths[i],F_OK)==0);
    link=iap2_link_create(NULL,reject_output,reject_delivery);
    if(!link) { puts("protocol_allocation=failed");return 1; }
    printf("protocol_state=%d (IDLE; handshake not started)\n",(int)iap2_link_state(link));
    iap2_link_destroy(link);
    puts("complete=1 usb_opened=0 carplay_session=not_attempted");
    return 0;
}
