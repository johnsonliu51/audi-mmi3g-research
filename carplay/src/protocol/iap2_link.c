/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from shihabal3amri/DiPlay, commit
 * 24ae1fa52edbe27392a668d4c9e71a32d5691986,
 * shared/.../transport/Iap2LinkEngine.kt. See IAP2_LINK_PORT.md.
 */
#include "iap2_link.h"
#include <stdlib.h>
#include <string.h>
#define LIMIT 65535u
#define SYN 0x80
#define ACK 0x40
#define EAK 0x20
#define RST 0x10
struct iap2_link {
    enum iap2_link_state state;
    void *context; iap2_link_output output; iap2_link_delivery delivery;
    uint8_t input[LIMIT], pending[LIMIT], scratch[LIMIT];
    size_t used, pending_length;
    uint8_t tx, rx, pending_sequence, retries;
    uint8_t peer_window, peer_retries, peer_acks, sessions[256];
    uint16_t peer_length, peer_timeout, peer_ack_timeout;
    int peer_seen;
    uint64_t handshake_due, ack_due, retry_due;
    const char *error;
};
static const uint8_t marker[]={255,85,2,0,238,16};
static unsigned u16(const uint8_t *p) { return ((unsigned)p[0]<<8)|p[1]; }
static uint8_t checksum(const uint8_t *p,size_t n) {
    unsigned sum=0; while(n--) sum+=*p++; return (uint8_t)(0-sum);
}
static int fail(struct iap2_link *l,const char *reason) {
    l->state=IAP2_DEAD; l->error=reason;
    l->handshake_due=l->ack_due=l->retry_due=0; return -1;
}
static int emit(struct iap2_link *l,uint8_t control,uint8_t sequence,
                uint8_t session,const uint8_t *p,size_t n,int payload) {
    size_t length=9+(payload?n+1:0); uint8_t *b=l->scratch;
    b[0]=255;b[1]=90;b[2]=(uint8_t)(length>>8);b[3]=(uint8_t)length;
    b[4]=control;b[5]=sequence;b[6]=l->rx;b[7]=session;b[8]=checksum(b,8);
    if(payload) { if(n) memcpy(b+9,p,n); b[9+n]=checksum(p,n); }
    if(l->output(l->context,b,length)!=0) return fail(l,"output rejected frame");
    return 0;
}
static int sync_frame(struct iap2_link *l) {
    /* One outstanding packet, session 10 only. Do not advertise unsupported EA/file sessions. */
    static const uint8_t sync[]={1,1,255,255,15,160,1,244,4,1,10,0,1};
    return emit(l,SYN,l->tx,0,sync,sizeof(sync),1);
}
static int ack_frame(struct iap2_link *l) {
    l->ack_due=0;return emit(l,ACK,l->tx,0,0,0,0);
}
struct iap2_link *iap2_link_create(void *ctx,iap2_link_output out,iap2_link_delivery in) {
    struct iap2_link *l;
    if(!out||!in) return 0;
    l=(struct iap2_link *)calloc(1,sizeof(*l));
    if(l) { l->context=ctx;l->output=out;l->delivery=in;l->tx=99; }
    return l;
}
void iap2_link_destroy(struct iap2_link *l) { if(l) { memset(l,0,sizeof(*l));free(l); } }
int iap2_link_start(struct iap2_link *l,int wired,uint64_t now) {
    if(!l||l->state!=IAP2_IDLE) return -1;
    l->state=IAP2_DETECTING;
    if(l->output(l->context,marker,sizeof(marker))) return fail(l,"marker output failed");
    l->handshake_due=now+1000;
    if(wired) { l->state=IAP2_NEGOTIATING;l->handshake_due=now+500;return sync_frame(l); }
    return 0;
}
static int process(struct iap2_link *l,const uint8_t *b,size_t n,uint64_t now) {
    unsigned control=b[4],seq=b[5],ack=b[6],session=b[7];
    const uint8_t *p=b+9;size_t length=n>9?n-10:0;
    if(control&RST) return fail(l,"peer reset");
    if(control&SYN) {
        size_t i;
        if(l->state!=IAP2_NEGOTIATING) return 0;
        if(length<10||p[0]!=1||(length-10)%3) return fail(l,"invalid synchronization");
        l->peer_window=p[1];l->peer_length=(uint16_t)u16(p+2);
        l->peer_timeout=(uint16_t)u16(p+4);l->peer_ack_timeout=(uint16_t)u16(p+6);
        l->peer_retries=p[8];l->peer_acks=p[9];memset(l->sessions,0,256);
        for(i=10;i<length;i+=3) if(p[i+1]==0&&p[i+2]==1) l->sessions[p[i]]=1;
        if(!l->peer_window||l->peer_length<=10||!l->sessions[10]||
           (l->peer_retries&&!l->peer_timeout)) return fail(l,"unsupported peer limits/session");
        l->peer_seen=1;l->rx=(uint8_t)seq;
        if(ack_frame(l)) return -1;
    }
    if(control&ACK) {
        if(l->state==IAP2_NEGOTIATING&&l->peer_seen&&ack==l->tx) {
            l->state=IAP2_NORMAL;l->handshake_due=0;
        }
        if(l->pending_length&&ack==l->pending_sequence) {
            l->pending_length=0;l->retry_due=0;
        }
    }
    if(l->state!=IAP2_NORMAL) return 0;
    if(control&EAK) {
        size_t i;
        for(i=0;i<length;i++) if(l->pending_length&&p[i]==l->pending_sequence) {
            l->retry_due=now;break;
        }
        return 0;
    }
    if((control&~ACK)==0&&n>9) {
        uint8_t distance=(uint8_t)(seq-l->rx);
        if(distance==1) {
            if(session!=10) return fail(l,"unadvertised session");
            if(l->delivery(l->context,(uint8_t)session,p,length)) return fail(l,"delivery rejected payload");
            l->rx=(uint8_t)seq;
            /* Immediate ACK also handles duplicates without redelivery. */
            return ack_frame(l);
        }
        if(distance==0||distance>128) return ack_frame(l);
        /* Stop-and-wait local window is one: explicitly reject a peer violating it. */
        return fail(l,"peer exceeded receive window");
    }
    return 0;
}
static int parse(struct iap2_link *l,uint64_t now) {
    while(l->used) {
        size_t n;
        if(l->used>=sizeof(marker)&&memcmp(l->input,marker,sizeof(marker))==0) {
            memmove(l->input,l->input+sizeof(marker),l->used-sizeof(marker));l->used-=sizeof(marker);
            if(l->state==IAP2_DETECTING) {
                l->state=IAP2_NEGOTIATING;l->handshake_due=now+500;
                if(sync_frame(l)) return -1;
            }
            continue;
        }
        if(l->state==IAP2_DETECTING) {
            if(l->used<sizeof(marker)) return 0;
            return fail(l,"invalid detection marker");
        }
        /* Keep a fragmented marker intact while negotiating. */
        if(l->used<sizeof(marker)&&memcmp(l->input,marker,l->used)==0) return 0;
        if(l->used<2) return 0;
        if(l->input[0]!=255||l->input[1]!=90) {
            memmove(l->input,l->input+1,--l->used);continue;
        }
        if(l->used<9) return 0;
        n=u16(l->input+2);
        if(n<9||checksum(l->input,9)!=0) {
            memmove(l->input,l->input+1,--l->used);continue;
        }
        if(l->used<n) return 0;
        if(n==9||checksum(l->input+9,n-9)==0) {
            if(process(l,l->input,n,now)) return -1;
        }
        memmove(l->input,l->input+n,l->used-n);l->used-=n;
    }
    return 0;
}
int iap2_link_feed(struct iap2_link *l,const uint8_t *p,size_t n,uint64_t now) {
    if(!l||l->state==IAP2_IDLE||l->state==IAP2_DEAD||(!p&&n)) return -1;
    while(n) {
        size_t count=LIMIT-l->used;
        if(!count) return fail(l,"receive buffer full");
        if(count>n) count=n;
        memcpy(l->input+l->used,p,count);l->used+=count;p+=count;n-=count;
        if(parse(l,now)) return -1;
    }
    return 0;
}
int iap2_link_send(struct iap2_link *l,uint8_t session,const uint8_t *p,size_t n,uint64_t now) {
    if(!l||l->state!=IAP2_NORMAL||session!=10||(!p&&n)||n>LIMIT-10||n+10>l->peer_length) return -1;
    if(l->pending_length) return 1;
    l->tx++;l->pending_sequence=l->tx;l->retries=0;
    if(n) memcpy(l->pending,p,n);
    /* Length includes checksum so zero-length payload is still pending. */
    l->pending_length=n+1;
    if(emit(l,ACK,l->tx,session,p,n,1)) return -1;
    if(l->peer_retries) l->retry_due=now+l->peer_timeout;
    else l->pending_length=0;
    return 0;
}
int iap2_link_tick(struct iap2_link *l,uint64_t now) {
    if(!l||l->state==IAP2_DEAD) return -1;
    if(l->handshake_due&&now>=l->handshake_due) {
        if(l->state==IAP2_DETECTING) {
            if(l->output(l->context,marker,sizeof(marker))) return fail(l,"marker output failed");
            l->handshake_due=now+1000;
        } else if(l->state==IAP2_NEGOTIATING) {
            if(sync_frame(l)) return -1;
            l->handshake_due=now+500;
        }
    }
    if(l->retry_due&&now>=l->retry_due&&l->pending_length) {
        if(++l->retries>=l->peer_retries) return fail(l,"acknowledgement retry limit");
        if(emit(l,ACK,l->pending_sequence,10,l->pending,l->pending_length-1,1)) return -1;
        l->retry_due=now+l->peer_timeout;
    }
    return 0;
}
void iap2_link_eof(struct iap2_link *l) { if(l) fail(l,"transport EOF"); }
enum iap2_link_state iap2_link_state(const struct iap2_link *l) { return l?l->state:IAP2_DEAD; }
const char *iap2_link_error(const struct iap2_link *l) { return l?l->error:"null engine"; }
uint64_t iap2_link_deadline(const struct iap2_link *l) {
    uint64_t a,b;if(!l) return 0;a=l->handshake_due;b=l->retry_due;
    return !a?b:(!b||a<b?a:b);
}
