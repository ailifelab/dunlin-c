#include "profile_hash.h"
#include <stdint.h>
#include <string.h>

/* SHA-256 仅用于稳定会话命名，无平台密码库依赖；完整保留 256 位摘要。 */
typedef struct { uint32_t h[8]; uint64_t bytes; unsigned char block[64]; size_t used; } digest;
static uint32_t rotate(uint32_t x,unsigned n) {return (x>>n)|(x<<(32-n));}
static void transform(digest *d,const unsigned char *p) {
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for(unsigned i=0;i<16;i++) w[i]=(uint32_t)p[i*4]<<24|(uint32_t)p[i*4+1]<<16|(uint32_t)p[i*4+2]<<8|p[i*4+3];
    for(unsigned i=16;i<64;i++) {
        uint32_t a=w[i-15],b=w[i-2];
        w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
    }
    uint32_t a=d->h[0],b=d->h[1],c=d->h[2],e=d->h[4],f=d->h[5],g=d->h[6],h=d->h[7],v=d->h[3];
    for(unsigned i=0;i<64;i++) {
        uint32_t t=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=v+t;v=c;c=b;b=a;a=t+u;
    }
    d->h[0]+=a;d->h[1]+=b;d->h[2]+=c;d->h[3]+=v;d->h[4]+=e;d->h[5]+=f;d->h[6]+=g;d->h[7]+=h;
}
static digest start(void) {
    return (digest){.h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
}
static void update(digest *d,const void *data,size_t size) {
    const unsigned char *p=data;
    d->bytes+=(uint64_t)size;
    while(size) {
        size_t n=64-d->used; if(n>size) n=size;
        memcpy(d->block+d->used,p,n); d->used+=n;p+=n;size-=n;
        if(d->used==64) {transform(d,d->block);d->used=0;}
    }
}
static void finish(digest *d,char out[65]) {
    uint64_t bits=d->bytes*8;
    d->block[d->used++]=0x80;
    if(d->used>56) {memset(d->block+d->used,0,64-d->used);transform(d,d->block);d->used=0;}
    memset(d->block+d->used,0,56-d->used);
    for(unsigned i=0;i<8;i++) d->block[63-i]=(unsigned char)(bits>>(8*i));
    transform(d,d->block);
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++) {
        unsigned char byte=(unsigned char)(d->h[i/4]>>(24-8*(i%4)));
        out[i*2]=hex[byte>>4];out[i*2+1]=hex[byte&15];
    }
    out[64]=0;
}
void dunlin_profile_sha256(const void *data,size_t size,char out[65]) {
    digest d=start();update(&d,data,size);finish(&d,out);
}
int dunlin_profile_session_id(const char *conversation,const char *alias,const char *protocol,
        const char *model,const char *endpoint,char out[65]) {
    const char *parts[]={conversation,alias,protocol,model,endpoint};
    if(!out) return 0;
    for(unsigned i=0;i<5;i++) if(!parts[i]) return 0;
    digest d=start();
    static const char domain[]="DUNLIN/model-session/v1";
    update(&d,domain,sizeof domain);
    for(unsigned i=0;i<5;i++) {
        size_t size=strlen(parts[i]); uint64_t length=(uint64_t)size;
        unsigned char prefix[8];
        for(unsigned j=0;j<8;j++) prefix[7-j]=(unsigned char)(length>>(j*8));
        update(&d,prefix,sizeof prefix);update(&d,parts[i],size);
    }
    finish(&d,out);return 1;
}
