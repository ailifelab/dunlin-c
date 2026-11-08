#define _POSIX_C_SOURCE 200809L
#include "dunlin/desktop.h"
#include "dunlin/compact/model.h"
#include "dunlin/compact/json.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
typedef struct {char *body;size_t used;int limited;const dc_hooks *hooks;} transfer;
static size_t receive(char *bytes,size_t size,size_t count,void *ctx){transfer *t=ctx;if(size&&count>SIZE_MAX/size)return 0;size_t n=size*count;if(n>1048576-t->used){t->limited=1;return 0;}char *next=realloc(t->body,t->used+n+1);if(!next)return 0;t->body=next;memcpy(next+t->used,bytes,n);t->used+=n;next[t->used]=0;return n;}
static int progress(void *ctx,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){(void)a;(void)b;(void)c;(void)d;transfer *t=ctx;return t->hooks&&t->hooks->cancelled?t->hooks->cancelled(t->hooks->user):0;}
static int header(struct curl_slist **list,const char *text){struct curl_slist *next=curl_slist_append(*list,text);if(!next)return 0;*list=next;return 1;}
static int64_t number(J *o,const char *const *keys){for(size_t i=0;keys[i];i++){J *v=get(o,keys[i]);if(json_object_is_type(v,json_type_int)&&json_object_get_int64(v)>0)return json_object_get_int64(v);}return 0;}
char *dc_probe_model(const char *raw,const char *secret,const dc_hooks *hooks){
 if(!raw||strlen(raw)>262144)return NULL;J *cfg=dunlin_json_parse(raw,strlen(raw));const char *endpoint=str(cfg,"base_url"),*model=str(cfg,"model"),*protocol=str(cfg,"protocol"),*auth=str(cfg,"desktop_auth");
 J *out=json_object_new_object();int ok=0,known=0,found=0;long status=0;const char *error="invalid_model_configuration";int64_t window=0,output=0;
 CURL *curl=NULL;struct curl_slist *headers=NULL;transfer t={.hooks=hooks};char sensitive[16500]={0};
 if((protocol&&strcmp(protocol,"chat")&&strcmp(protocol,"responses")&&strcmp(protocol,"anthropic")&&strcmp(protocol,"ollama")&&strcmp(protocol,"gemini"))||!endpoint||!model||!dunlin_model_url_valid(endpoint)||strlen(endpoint)>2000||(secret&&(strlen(secret)>16384||strpbrk(secret,"\r\n")))||(auth&&strcmp(auth,"auto")&&strcmp(auth,"bearer")&&strcmp(auth,"none"))||(auth&&!strcmp(auth,"none")&&secret&&*secret))goto done;
 char url[2080];size_t length=strlen(endpoint);while(length&&endpoint[length-1]=='/')length--;int ollama=protocol&&!strcmp(protocol,"ollama"),gemini=protocol&&!strcmp(protocol,"gemini"),anthropic=protocol&&!strcmp(protocol,"anthropic");snprintf(url,sizeof url,"%.*s%s",(int)length,endpoint,ollama?"/tags":"/models");
 if(!header(&headers,"Accept: application/json"))goto done;
 if(anthropic&&!header(&headers,"anthropic-version: 2026-10-21"))goto done;
 if(secret&&*secret){int native_header=!auth||strcmp(auth,"bearer");snprintf(sensitive,sizeof sensitive,"%s%s",gemini&&native_header?"x-goog-api-key: ":anthropic&&native_header?"x-api-key: ":"Authorization: Bearer ",secret);if(!header(&headers,sensitive))goto done;}
 curl=curl_easy_init();if(!curl)goto done;
#define SET(k,v) do {if(curl_easy_setopt(curl,k,v)!=CURLE_OK)goto done;}while(0)
 SET(CURLOPT_URL,url);SET(CURLOPT_HTTPHEADER,headers);SET(CURLOPT_FOLLOWLOCATION,0L);SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_SSL_VERIFYHOST,2L);SET(CURLOPT_NOSIGNAL,1L);SET(CURLOPT_CONNECTTIMEOUT_MS,5000L);SET(CURLOPT_TIMEOUT_MS,15000L);SET(CURLOPT_WRITEFUNCTION,receive);SET(CURLOPT_WRITEDATA,&t);SET(CURLOPT_NOPROGRESS,0L);SET(CURLOPT_XFERINFOFUNCTION,progress);SET(CURLOPT_XFERINFODATA,&t);
#if LIBCURL_VERSION_NUM >= 0x075500
 SET(CURLOPT_PROTOCOLS_STR,"http,https");
#else
 SET(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#endif
 CURLcode result=curl_easy_perform(curl);curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
 if(result!=CURLE_OK){error=t.limited?"model_inventory_exceeds_limit":result==CURLE_OPERATION_TIMEDOUT?"model_inventory_timeout":result==CURLE_ABORTED_BY_CALLBACK?"cancelled":"model_inventory_transport_failed";goto done;}
 if(status<200||status>=300){error="model_inventory_http_error";goto done;}
 ok=1;error=NULL;J *body=t.body?dunlin_json_parse(t.body,t.used):NULL,*models=get(body,ollama||gemini?"models":"data");known=json_object_is_type(models,json_type_array);
 if(known)for(size_t i=0;i<json_object_array_length(models);i++){
  J *row=json_object_array_get_idx(models,i);const char *id=str(row,ollama||gemini?"name":"id");if(gemini&&id&&!strncmp(id,"models/",7))id+=7;if(!id||strcmp(id,model))continue;found=1;
  const char *windows[]={"context_window","max_model_len","context_length","inputTokenLimit",NULL};const char *outputs[]={"max_output_tokens","output_token_limit","outputTokenLimit",NULL};window=number(row,windows);output=number(row,outputs);break;
 }
 json_object_put(body);
done:
 if(curl)curl_easy_cleanup(curl);curl_slist_free_all(headers);free(t.body);for(volatile char *p=sensitive;p<sensitive+sizeof sensitive;p++)*p=0;
 json_object_object_add(out,"ok",json_object_new_boolean(ok));json_object_object_add(out,"httpStatus",json_object_new_int64(status));json_object_object_add(out,"listKnown",json_object_new_boolean(known));json_object_object_add(out,"modelFound",json_object_new_boolean(found));json_object_object_add(out,"contextLimit",json_object_new_int64(window));json_object_object_add(out,"outputLimit",json_object_new_int64(output));json_object_object_add(out,"source",json_object_new_string(protocol&&!strcmp(protocol,"gemini")?"provider-input-limit-conservative":"provider-model-inventory"));if(error)json_object_object_add(out,"error",json_object_new_string(error));char *text=strdup(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN));json_object_put(out);json_object_put(cfg);return text;
}
