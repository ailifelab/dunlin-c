#define _XOPEN_SOURCE 700
#include "dunlin/live_stream.h"
#include "mutex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
typedef struct preview {struct preview *next;char chat[65],run[65],text[65537];size_t used;uint64_t attempt;int active,omitted;int64_t time;long long through;} preview;
struct dl_live_stream {extension_mutex mutex;unsigned users,count;uint64_t sequence;preview *head;};
static int key(const char *s){return s&&*s&&strlen(s)<=64;}
static preview *find(dl_live_stream *b,const char *chat){for(preview *p=b->head;p;p=p->next)if(!strcmp(p->chat,chat))return p;return NULL;}
dl_live_stream *dl_live_stream_open(void){dl_live_stream *b=calloc(1,sizeof *b);if(b&&!extension_mutex_init(&b->mutex)){free(b);b=NULL;}if(b)b->users=1;return b;}
dl_live_stream *dl_live_stream_retain(dl_live_stream *b){if(b){extension_mutex_lock(&b->mutex);if(b->users==UINT_MAX){extension_mutex_unlock(&b->mutex);return NULL;}b->users++;extension_mutex_unlock(&b->mutex);}return b;}
void dl_live_stream_close(dl_live_stream *b){if(!b)return;extension_mutex_lock(&b->mutex);if(--b->users){extension_mutex_unlock(&b->mutex);return;}preview *p=b->head;while(p){preview *next=p->next;free(p);p=next;}extension_mutex_unlock(&b->mutex);extension_mutex_destroy(&b->mutex);free(b);}
void dl_live_stream_begin(dl_live_stream *b,const char *chat,const char *run,long long through){
 if(!b||!key(chat)||!key(run))return;extension_mutex_lock(&b->mutex);preview *p=find(b,chat);
 if(!p&&b->count<32){p=calloc(1,sizeof *p);if(p){snprintf(p->chat,sizeof p->chat,"%s",chat);p->next=b->head;b->head=p;b->count++;}}
 if(p){snprintf(p->run,sizeof p->run,"%s",run);p->omitted=b->sequence==UINT64_MAX;if(!p->omitted)p->attempt=++b->sequence;p->through=through;p->active=1;p->used=0;p->text[0]=0;struct timespec t;p->time=timespec_get(&t,TIME_UTC)==TIME_UTC?(int64_t)t.tv_sec*1000+t.tv_nsec/1000000:0;}
 extension_mutex_unlock(&b->mutex);
}
void dl_live_stream_append(dl_live_stream *b,const char *chat,const char *run,const char *text,size_t length){
 if(!b||!key(chat)||!key(run)||(!text&&length))return;extension_mutex_lock(&b->mutex);preview *p=find(b,chat);
 if(p&&p->active&&!strcmp(p->run,run)&&!p->omitted){if(length>65536-p->used||(length&&memchr(text,0,length))){p->omitted=1;p->used=0;p->text[0]=0;}else if(length){memcpy(p->text+p->used,text,length);p->used+=length;p->text[p->used]=0;}}
 extension_mutex_unlock(&b->mutex);
}
/* Terminal attempts release their storage. The bus sequence prevents reuse of
 * an attempt identity even when a run issues another model request. */
void dl_live_stream_end(dl_live_stream *b,const char *chat,const char *run){
 if(!b||!key(chat)||!key(run))return;extension_mutex_lock(&b->mutex);preview **link=&b->head;
 while(*link){preview *p=*link;if(!strcmp(p->chat,chat)&&!strcmp(p->run,run)){*link=p->next;b->count--;free(p);break;}link=&p->next;}
 extension_mutex_unlock(&b->mutex);
}
static int utf8(const unsigned char *s,size_t n){for(size_t i=0;i<n;){unsigned c=s[i++];if(c<0x80)continue;unsigned k=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;if(!k||n-i<k)return 0;unsigned d=s[i];if((c==0xe0&&d<0xa0)||(c==0xed&&d>=0xa0)||(c==0xf0&&d<0x90)||(c==0xf4&&d>=0x90))return 0;for(unsigned j=0;j<k;j++)if((s[i++]&0xc0)!=0x80)return 0;}return 1;}
struct json_object *dl_live_stream_snapshot(dl_live_stream *b,const char *chat){
 struct json_object *o=json_object_new_object();if(!o)return NULL;json_object_object_add(o,"available",json_object_new_boolean(b!=NULL));json_object_object_add(o,"active",json_object_new_boolean(0));if(!b||!key(chat))return o;
 extension_mutex_lock(&b->mutex);preview *p=find(b,chat);
 if(p&&p->active){if(p->omitted||!utf8((const unsigned char*)p->text,p->used)){json_object_object_add(o,"omitted",json_object_new_boolean(1));}
 else{char attempt[90];snprintf(attempt,sizeof attempt,"%s-%llu",p->run,(unsigned long long)p->attempt);json_object_object_add(o,"active",json_object_new_boolean(1));json_object_object_add(o,"text",json_object_new_string_len(p->text,(int)p->used));json_object_object_add(o,"attemptId",json_object_new_string(attempt));json_object_object_add(o,"time",json_object_new_int64(p->time));json_object_object_add(o,"through",json_object_new_int64(p->through));}}
 extension_mutex_unlock(&b->mutex);return o;
}
