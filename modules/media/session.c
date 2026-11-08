#include "dunlin/media.h"
#include <stdlib.h>
#include <string.h>
typedef struct packet {struct packet *next;size_t size;uint64_t stamp;int frame;unsigned char data[];} packet;
struct dl_media {dl_media_options options;dl_media_transport transport;dl_media_state state;unsigned capabilities;size_t queued,count;uint64_t audio_stamp,frame_stamp;int audio_seen,frame_seen,opened;packet *first,*last;};
static void clear(dl_media *s){while(s->first){packet *p=s->first;s->first=p->next;memset(p->data,0,p->size);free(p);}s->last=NULL;s->queued=s->count=0;}
dl_media *dl_media_new(const dl_media_options *o,const dl_media_transport *t){
    if(!o||!t||!t->connect||!t->send||!t->close||!(o->requested&DL_MEDIA_AUDIO_INPUT)||
       (o->requested&~31u)||(o->available&~31u)||o->max_queued_bytes<4096||o->max_queued_bytes>8388608||
       (o->sample_rate!=16000&&o->sample_rate!=24000&&o->sample_rate!=48000))return NULL;
    dl_media *s=calloc(1,sizeof *s);if(s){s->options=*o;s->transport=*t;}return s;
}
void dl_media_end(dl_media *s,int failed){if(!s||s->state==DL_MEDIA_ENDED||s->state==DL_MEDIA_FAILED)return;
    s->state=failed?DL_MEDIA_FAILED:DL_MEDIA_ENDED;clear(s);if(s->opened){s->opened=0;s->transport.close(s->transport.user);}}
void dl_media_free(dl_media *s){if(s){dl_media_end(s,0);clear(s);free(s);}}
dl_media_result dl_media_start(dl_media *s){
    if(!s)return DL_MEDIA_INVALID;if(s->state!=DL_MEDIA_NEW)return DL_MEDIA_CLOSED;
    if(!s->options.microphone_allowed||!s->options.upload_allowed||((s->options.requested&DL_MEDIA_IMAGE_INPUT)&&!s->options.camera_allowed))return DL_MEDIA_DENIED;
    if((s->options.requested&s->options.available)!=s->options.requested)return DL_MEDIA_UNSUPPORTED;
    s->state=DL_MEDIA_CONNECTING;s->opened=1;
    if(!s->transport.connect(s->transport.user,s->options.requested,s->options.sample_rate)){dl_media_end(s,1);return DL_MEDIA_IO;}return DL_MEDIA_OK;
}
dl_media_result dl_media_negotiated(dl_media *s,unsigned caps){
    if(!s||(caps&~31u))return DL_MEDIA_INVALID;if(s->state!=DL_MEDIA_CONNECTING)return DL_MEDIA_CLOSED;
    if((caps&s->options.requested)!=s->options.requested||(caps&~s->options.available)){dl_media_end(s,1);return DL_MEDIA_UNSUPPORTED;}
    s->capabilities=caps&s->options.requested;s->state=DL_MEDIA_LIVE;return DL_MEDIA_OK;
}
dl_media_result dl_media_push(dl_media *s,int frame,const void *bytes,size_t n,uint64_t stamp){
    if(!s||!bytes||!n||(frame!=0&&frame!=1)||(!frame&&(n&1)))return DL_MEDIA_INVALID;
    if(s->state!=DL_MEDIA_LIVE)return DL_MEDIA_CLOSED;
    if(frame&&!(s->capabilities&DL_MEDIA_IMAGE_INPUT))return DL_MEDIA_UNSUPPORTED;
    if((frame&&s->frame_seen&&stamp<s->frame_stamp)||(!frame&&s->audio_seen&&stamp<s->audio_stamp))return DL_MEDIA_INVALID;
    if(n>s->options.max_queued_bytes-s->queued||s->count>=128)return DL_MEDIA_BACKPRESSURE;
    packet *p=malloc(sizeof *p+n);if(!p)return DL_MEDIA_BACKPRESSURE;
    p->next=NULL;p->size=n;p->stamp=stamp;p->frame=frame;memcpy(p->data,bytes,n);
    if(s->last)s->last->next=p;else s->first=p;s->last=p;s->queued+=n;s->count++;
    if(frame){s->frame_stamp=stamp;s->frame_seen=1;}else{s->audio_stamp=stamp;s->audio_seen=1;}return DL_MEDIA_OK;
}
dl_media_result dl_media_drain(dl_media *s,unsigned limit){
    if(!s||!limit||limit>128)return DL_MEDIA_INVALID;if(s->state!=DL_MEDIA_LIVE)return DL_MEDIA_CLOSED;
    while(limit--&&s->first){packet *p=s->first;int result=s->transport.send(s->transport.user,p->frame,p->data,p->size,p->stamp);
        if(result<0){dl_media_end(s,1);return DL_MEDIA_IO;}if(!result)return DL_MEDIA_BACKPRESSURE;
        s->first=p->next;if(!s->first)s->last=NULL;s->queued-=p->size;s->count--;memset(p->data,0,p->size);free(p);
    }return DL_MEDIA_OK;
}
dl_media_state dl_media_status(const dl_media *s){return s?s->state:DL_MEDIA_FAILED;}
size_t dl_media_queued(const dl_media *s){return s?s->queued:0;}
unsigned dl_media_capabilities(const dl_media *s){return s?s->capabilities:0;}
