#include "dunlin/live_stream.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct json_object *field(struct json_object *j,const char *k){struct json_object *v=NULL;json_object_object_get_ex(j,k,&v);return v;}
static void check(dl_live_stream *b,const char *chat,const char *text,int omitted){struct json_object *j=dl_live_stream_snapshot(b,chat);assert(j);assert(json_object_get_boolean(field(j,"active"))==(text!=NULL));assert(json_object_get_boolean(field(j,"omitted"))==omitted);if(text)assert(!strcmp(json_object_get_string(field(j,"text")),text));json_object_put(j);}
static void *writer(void *raw){dl_live_stream *b=raw;for(int i=0;i<500;i++){dl_live_stream_begin(b,"concurrent","run",1);dl_live_stream_append(b,"concurrent","run","中文🦆",10);dl_live_stream_end(b,"concurrent","run");}dl_live_stream_close(b);return NULL;}
int main(void){
 dl_live_stream *b=dl_live_stream_open(),*other=dl_live_stream_open();assert(b&&other);check(b,"chat",NULL,0);check(NULL,"chat",NULL,0);
 dl_live_stream_begin(b,"chat","run",7);dl_live_stream_append(b,"chat","wrong-run","wrong",5);check(b,"chat","",0);
 dl_live_stream_append(b,"chat","run","中文",6);check(b,"chat","中文",0);check(other,"chat",NULL,0);check(b,"foreign",NULL,0);
 dl_live_stream_append(b,"chat","run","\xf0\x9f",2);check(b,"chat",NULL,1);dl_live_stream_append(b,"chat","run","\xa6\x86",2);check(b,"chat","中文🦆",0);
 struct json_object *j=dl_live_stream_snapshot(b,"chat");assert(json_object_get_int64(field(j,"through"))==7);char *attempt=strdup(json_object_get_string(field(j,"attemptId")));json_object_put(j);assert(attempt);
 dl_live_stream_end(b,"chat","old-run");check(b,"chat","中文🦆",0);dl_live_stream_end(b,"chat","run");check(b,"chat",NULL,0);
 dl_live_stream_begin(b,"chat","run",8);j=dl_live_stream_snapshot(b,"chat");assert(strcmp(attempt,json_object_get_string(field(j,"attemptId"))));json_object_put(j);free(attempt);
 char *large=malloc(65537);assert(large);memset(large,'a',65537);dl_live_stream_append(b,"chat","run",large,65536);j=dl_live_stream_snapshot(b,"chat");assert(json_object_get_string_len(field(j,"text"))==65536);json_object_put(j);dl_live_stream_append(b,"chat","run",large,1);check(b,"chat",NULL,1);free(large);dl_live_stream_end(b,"chat","run");
 for(int i=0;i<64;i++){char name[16];snprintf(name,sizeof name,"serial-%d",i);dl_live_stream_begin(b,name,"run",0);check(b,name,"",0);dl_live_stream_end(b,name,"run");}
 for(int i=0;i<32;i++){char name[16];snprintf(name,sizeof name,"parallel-%d",i);dl_live_stream_begin(b,name,"run",0);check(b,name,"",0);}dl_live_stream_begin(b,"overflow","run",0);check(b,"overflow",NULL,0);dl_live_stream_end(b,"parallel-0","run");dl_live_stream_begin(b,"overflow","run",0);check(b,"overflow","",0);
 dl_live_stream_close(b);b=dl_live_stream_open();assert(b);dl_live_stream_begin(b,"bad","run",0);dl_live_stream_append(b,"bad","run","x\0y",3);check(b,"bad",NULL,1);dl_live_stream_end(b,"bad","run");
 dl_live_stream *worker=dl_live_stream_retain(b);assert(worker);pthread_t thread;assert(!pthread_create(&thread,NULL,writer,worker));for(int i=0;i<500;i++){j=dl_live_stream_snapshot(b,"concurrent");assert(j);json_object_put(j);}assert(!pthread_join(thread,NULL));
 worker=dl_live_stream_retain(b);assert(worker);dl_live_stream_close(b);dl_live_stream_begin(worker,"retained","run",0);check(worker,"retained","",0);dl_live_stream_close(worker);dl_live_stream_close(other);
 puts("PASS independent live text UTF-8/bounds/identity/attempt/lifetime/concurrency");return 0;
}
