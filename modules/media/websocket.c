#define _POSIX_C_SOURCE 200809L
#include "dunlin/websocket.h"
#include <curl/curl.h>
#include <curl/websockets.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/select.h>
#endif
struct dl_websocket {CURL *curl;struct curl_slist *headers;void *user;int (*cancelled)(void *);char *message;size_t used,limit;int failed,connected;};
static uint64_t now_ms(void){
#ifdef _WIN32
 return GetTickCount64();
#else
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
#endif
}
static int cancelled(dl_websocket *s){return s->cancelled&&s->cancelled(s->user);}
static int progress(void *raw,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){(void)a;(void)b;(void)c;(void)d;return cancelled(raw);}
static int clean(const char *s,size_t cap){return s&&strlen(s)<=cap&&!strchr(s,'\r')&&!strchr(s,'\n');}
dl_websocket *dl_ws_open(const char *url,const char *key,int local,size_t limit,void *user,int (*stop)(void *)){
 if(!clean(url,4096)||!clean(key?key:"",16384)||limit<4096||limit>8388608)return NULL;
 CURLU *u=curl_url();char *scheme=NULL,*host=NULL,*username=NULL,*password=NULL,*fragment=NULL;
 int valid=u&&curl_url_set(u,CURLUPART_URL,url,CURLU_NON_SUPPORT_SCHEME)==CURLUE_OK&&curl_url_get(u,CURLUPART_SCHEME,&scheme,0)==CURLUE_OK&&curl_url_get(u,CURLUPART_HOST,&host,0)==CURLUE_OK;
 if(valid){curl_url_get(u,CURLUPART_USER,&username,0);curl_url_get(u,CURLUPART_PASSWORD,&password,0);curl_url_get(u,CURLUPART_FRAGMENT,&fragment,0);valid=!username&&!password&&!fragment&&(!strcmp(scheme,"wss")||(local&&!strcmp(scheme,"ws")&&(!strcmp(host,"127.0.0.1")||!strcmp(host,"[::1]"))));}
 curl_free(scheme);curl_free(host);curl_free(username);curl_free(password);curl_free(fragment);curl_url_cleanup(u);if(!valid)return NULL;
 dl_websocket *s=calloc(1,sizeof *s);if(!s)return NULL;s->user=user;s->cancelled=stop;s->limit=limit;s->curl=curl_easy_init();s->message=malloc(limit+1);
 if(!s->curl||!s->message){dl_ws_close(s);return NULL;}
 if(key&&*key){size_t n=strlen(key)+32;char *header=malloc(n);if(!header){dl_ws_close(s);return NULL;}snprintf(header,n,"Authorization: Bearer %s",key);s->headers=curl_slist_append(NULL,header);memset(header,0,n);free(header);if(!s->headers){dl_ws_close(s);return NULL;}}
 curl_easy_setopt(s->curl,CURLOPT_URL,url);curl_easy_setopt(s->curl,CURLOPT_CONNECT_ONLY,2L);
 curl_easy_setopt(s->curl,CURLOPT_HTTPHEADER,s->headers);curl_easy_setopt(s->curl,CURLOPT_CONNECTTIMEOUT_MS,15000L);curl_easy_setopt(s->curl,CURLOPT_TIMEOUT_MS,30000L);
 curl_easy_setopt(s->curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(s->curl,CURLOPT_FOLLOWLOCATION,0L);
 curl_easy_setopt(s->curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(s->curl,CURLOPT_SSL_VERIFYHOST,2L);
 curl_easy_setopt(s->curl,CURLOPT_PROTOCOLS_STR,"ws,wss");curl_easy_setopt(s->curl,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(s->curl,CURLOPT_XFERINFOFUNCTION,progress);curl_easy_setopt(s->curl,CURLOPT_XFERINFODATA,s);
 curl_easy_setopt(s->curl,CURLOPT_NETRC,CURL_NETRC_IGNORED);
 curl_easy_setopt(s->curl,CURLOPT_PROXY,"");
 if(cancelled(s)||curl_easy_perform(s->curl)!=CURLE_OK){s->failed=1;dl_ws_close(s);return NULL;}s->connected=1;return s;
}
int dl_ws_send(dl_websocket *s,const char *text,size_t n){
 if(!s||s->failed||!text||!n||n>s->limit||memchr(text,0,n))return 0;
 size_t used=0;uint64_t end=now_ms()+5000;
 while(used<n){if(cancelled(s)||now_ms()>=end){s->failed=1;return 0;}size_t sent=0;CURLcode rc=curl_ws_send(s->curl,text+used,n-used,&sent,0,CURLWS_TEXT);used+=sent;
  if(rc!=CURLE_OK&&rc!=CURLE_AGAIN){s->failed=1;return 0;}
  if(rc==CURLE_AGAIN||!sent){curl_socket_t sock=CURL_SOCKET_BAD;if(curl_easy_getinfo(s->curl,CURLINFO_ACTIVESOCKET,&sock)!=CURLE_OK||sock==CURL_SOCKET_BAD){s->failed=1;return 0;}
#ifndef _WIN32
   if(sock>=FD_SETSIZE){s->failed=1;return 0;}
#endif
   fd_set write;FD_ZERO(&write);FD_SET(sock,&write);struct timeval wait={0,20000};select((int)sock+1,NULL,&write,NULL,&wait);
  }
 }return 1;
}
int dl_ws_receive(dl_websocket *s,char **text,size_t *bytes){
 if(text)*text=NULL;if(bytes)*bytes=0;if(!s||!text||!bytes||s->failed)return -1;
 if(cancelled(s)){s->failed=1;return -1;}
 for(unsigned i=0;i<32;i++){
  unsigned char chunk[16384];size_t n=0;const struct curl_ws_frame *meta=NULL;CURLcode rc=curl_ws_recv(s->curl,chunk,sizeof chunk,&n,&meta);
  if(rc==CURLE_AGAIN)return 0;if(rc!=CURLE_OK||!meta){s->failed=1;return -1;}
  int flags=meta->flags;curl_off_t remaining=meta->bytesleft;
  if(flags&CURLWS_CLOSE){s->failed=1;return -1;}
  if(flags&(CURLWS_PING|CURLWS_PONG))continue;
  if(!(flags&CURLWS_TEXT)||n>s->limit-s->used||memchr(chunk,0,n)){s->failed=1;return -1;}
  memcpy(s->message+s->used,chunk,n);s->used+=n;
  if(!remaining&&!(flags&CURLWS_CONT)){s->message[s->used]=0;char *out=malloc(s->used+1);if(!out){s->failed=1;return -1;}memcpy(out,s->message,s->used+1);*text=out;*bytes=s->used;s->used=0;return 1;}
 }return 0;
}
void dl_ws_close(dl_websocket *s){if(!s)return;if(s->curl){if(s->connected&&!s->failed){size_t sent=0;curl_ws_send(s->curl,"",0,&sent,0,CURLWS_CLOSE);}curl_easy_cleanup(s->curl);}curl_slist_free_all(s->headers);free(s->message);free(s);}
