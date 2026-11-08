#include "dunlin/compact/agent.h"

/* Checked before any untrusted bytes enter a durable JSON string or C API. */
int ah_utf8_text(const char *text,size_t n) {
    const unsigned char *p=(const unsigned char*)text;
    if (!p) return 0;
    for (size_t i=0;i<n;) {
        unsigned b=p[i++];
        if (b==0) return 0;
        if (b<128) continue;
        unsigned k=b>=0xc2 && b<=0xdf ? 1 : b>=0xe0 && b<=0xef ? 2 : b>=0xf0 && b<=0xf4 ? 3 : 0;
        if (!k || n-i<k || (b==0xe0 && p[i]<0xa0) || (b==0xed && p[i]>=0xa0) ||
            (b==0xf0 && p[i]<0x90) || (b==0xf4 && p[i]>=0x90)) return 0;
        while(k--) if ((p[i++] & 0xc0)!=0x80) return 0;
    }
    return 1;
}
