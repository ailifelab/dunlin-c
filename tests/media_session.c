#include "dunlin/media.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {int connects,closes,sends,accept;} fixture;
static int connect_service(void *raw,unsigned caps,unsigned rate){fixture *f=raw;f->connects++;assert(caps&DL_MEDIA_AUDIO_INPUT);assert(rate==24000);return 1;}
static int send_packet(void *raw,int frame,const void *bytes,size_t size,uint64_t stamp){fixture *f=raw;assert(!frame&&size==4&&stamp==10&&!memcmp(bytes,"abcd",4));f->sends++;return f->accept;}
static void close_service(void *raw){((fixture*)raw)->closes++;}
int main(void){
    fixture f={0};dl_media_options o={DL_MEDIA_AUDIO_INPUT|DL_MEDIA_AUDIO_OUTPUT,31,0,0,1,24000,4096};dl_media_transport t={&f,connect_service,send_packet,close_service};
    dl_media *s=dl_media_new(&o,&t);assert(s);assert(dl_media_start(s)==DL_MEDIA_DENIED);assert(!f.connects);dl_media_free(s);assert(!f.closes);
    o.microphone_allowed=1;o.available=DL_MEDIA_AUDIO_INPUT;s=dl_media_new(&o,&t);assert(dl_media_start(s)==DL_MEDIA_UNSUPPORTED);dl_media_free(s);assert(!f.connects);
    o.available=31;s=dl_media_new(&o,&t);assert(dl_media_start(s)==DL_MEDIA_OK);assert(dl_media_status(s)==DL_MEDIA_CONNECTING);
    assert(dl_media_push(s,0,"abcd",4,10)==DL_MEDIA_CLOSED);
    assert(dl_media_negotiated(s,31)==DL_MEDIA_OK);assert(dl_media_capabilities(s)==o.requested);
    assert(dl_media_push(s,1,"abcd",4,10)==DL_MEDIA_UNSUPPORTED); /* Server cannot expand camera consent. */
    assert(dl_media_push(s,0,"abc",3,10)==DL_MEDIA_INVALID);
    assert(dl_media_push(s,0,"abcd",4,10)==DL_MEDIA_OK);assert(dl_media_push(s,0,"abcd",4,9)==DL_MEDIA_INVALID);
    assert(dl_media_queued(s)==4);assert(dl_media_drain(s,1)==DL_MEDIA_BACKPRESSURE);assert(dl_media_queued(s)==4);
    f.accept=1;assert(dl_media_drain(s,1)==DL_MEDIA_OK);assert(dl_media_queued(s)==0);
    for(int i=0;i<128;i++)assert(dl_media_push(s,0,"abcd",4,10)==DL_MEDIA_OK);
    assert(dl_media_push(s,0,"abcd",4,10)==DL_MEDIA_BACKPRESSURE);
    dl_media_end(s,0);assert(dl_media_status(s)==DL_MEDIA_ENDED&&dl_media_queued(s)==0&&f.closes==1);
    assert(dl_media_push(s,0,"abcd",4,10)==DL_MEDIA_CLOSED);dl_media_end(s,1);dl_media_free(s);assert(f.closes==1);
    s=dl_media_new(&o,&t);assert(dl_media_start(s)==DL_MEDIA_OK);assert(dl_media_negotiated(s,DL_MEDIA_AUDIO_INPUT)==DL_MEDIA_UNSUPPORTED);assert(dl_media_status(s)==DL_MEDIA_FAILED);dl_media_free(s);assert(f.closes==2);
    s=dl_media_new(&o,&t);assert(dl_media_start(s)==DL_MEDIA_OK);assert(dl_media_negotiated(s,o.requested)==DL_MEDIA_OK);assert(dl_media_push(s,0,"abcd",4,10)==DL_MEDIA_OK);f.accept=-1;assert(dl_media_drain(s,1)==DL_MEDIA_IO);assert(dl_media_queued(s)==0);dl_media_free(s);assert(f.closes==3);
    puts("PASS media admission, capability intersection, ordered bounded backpressure, disconnect and idempotent hangup; fixture transport only");
}
