#include "dunlin/compact/http2.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

typedef struct { CURL *easy; const dunlin_http_request *request; } http2_transfer;
static size_t receive(char *data,size_t size,size_t count,void *ptr) {
    http2_transfer *t=ptr; long code=0;
    if(size && count>SIZE_MAX/size) return 0;
    if(curl_easy_getinfo(t->easy,CURLINFO_RESPONSE_CODE,&code)!=CURLE_OK || code<200 || code>=300) return 0;
    size_t n=size*count;
    return t->request->body(t->request->user,data,n)?n:0;
}
static int progress(void *ptr,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d) {
    (void)a;(void)b;(void)c;(void)d;
    const dunlin_http_request *r=ptr;
    return r->cancelled && r->cancelled(r->user);
}
static int safe_value(const char *s) {
    if(!s || strlen(s)>16384) return 0;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++) if(*p<32 || *p==127) return 0;
    return 1;
}
static int add(struct curl_slist **list,const char *name,const char *value) {
    if(!name || !*name || !safe_value(value)) return 0;
    for(const unsigned char *p=(const unsigned char *)name;*p;p++)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-')) return 0;
    size_t n=strlen(name)+strlen(value)+3;
    char *s=malloc(n); if(!s) return 0;
    memcpy(s,name,strlen(name));s[strlen(name)]=':';s[strlen(name)+1]=' ';
    memcpy(s+strlen(name)+2,value,strlen(value)+1);
    struct curl_slist *next=curl_slist_append(*list,s);free(s);
    if(!next) return 0;
    *list=next;return 1;
}
static dunlin_http_result post(void *unused,const dunlin_http_request_v2 *request,long *status) {
    (void)unused;
    if(!status) return DUNLIN_HTTP_FAILED;
    *status=0;
    if(!request || !request->request.url || !request->request.json || !request->request.body ||
       request->header_count>16 || (request->header_count && !request->headers)) return DUNLIN_HTTP_FAILED;
    const dunlin_http_request *r=&request->request;
    http2_transfer t={curl_easy_init(),r};
    if(!t.easy) return DUNLIN_HTTP_FAILED;
    dunlin_http_result result=DUNLIN_HTTP_FAILED;
    struct curl_slist *headers=NULL;
    char *auth=NULL;
    if(!add(&headers,"Expect","") || !add(&headers,"Content-Type","application/json") ||
       !add(&headers,"Accept",request->accept?request->accept:"application/json")) goto done;
    if(r->bearer) {
        if(!safe_value(r->bearer)) goto done;
        size_t n=strlen(r->bearer)+8;auth=malloc(n);if(!auth) goto done;
        memcpy(auth,"Bearer ",7);memcpy(auth+7,r->bearer,n-7);
        if(!add(&headers,"Authorization",auth)) goto done;
    }
    for(size_t i=0;i<request->header_count;i++) {
        const dunlin_http_header *h=&request->headers[i];
        /* The extension cannot replace the transport's framing or authority. */
        if(!h->name || !strcasecmp(h->name,"Host") || !strcasecmp(h->name,"Authorization") ||
           !strcasecmp(h->name,"Content-Type") || !strcasecmp(h->name,"Content-Length") ||
           !strcasecmp(h->name,"Transfer-Encoding") || !strcasecmp(h->name,"Accept") ||
           !add(&headers,h->name,h->value)) goto done;
    }
#define SET(o,v) do {if(curl_easy_setopt(t.easy,o,v)!=CURLE_OK) goto done;}while(0)
    SET(CURLOPT_URL,r->url);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR,"http,https");
#else
    SET(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#endif
    SET(CURLOPT_FOLLOWLOCATION,0L);SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_SSL_VERIFYHOST,2L);
    SET(CURLOPT_NOSIGNAL,1L);SET(CURLOPT_TIMEOUT_MS,r->timeout_ms);
    SET(CURLOPT_HTTPHEADER,headers);SET(CURLOPT_POSTFIELDS,r->json);
    SET(CURLOPT_WRITEFUNCTION,receive);SET(CURLOPT_WRITEDATA,&t);
    SET(CURLOPT_XFERINFOFUNCTION,progress);SET(CURLOPT_XFERINFODATA,(void *)r);SET(CURLOPT_NOPROGRESS,0L);
#undef SET
    CURLcode rc=curl_easy_perform(t.easy);curl_easy_getinfo(t.easy,CURLINFO_RESPONSE_CODE,status);
    if(rc==CURLE_OK) result=DUNLIN_HTTP_OK;
    else if(rc==CURLE_OPERATION_TIMEDOUT) result=DUNLIN_HTTP_EXPIRED;
    else if(rc==CURLE_ABORTED_BY_CALLBACK) result=DUNLIN_HTTP_ABORTED;
done:
    free(auth);curl_slist_free_all(headers);curl_easy_cleanup(t.easy);return result;
}
const dunlin_http_v2 dunlin_curl_http_v2={post,NULL};
