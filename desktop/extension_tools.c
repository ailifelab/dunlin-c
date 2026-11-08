#define _XOPEN_SOURCE 700
#include "extension_tools.h"
#include "extensions.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
typedef struct json_object J;
typedef struct {dc_store *store;char *chat;const ah_tool_service *base;J *rows;} tools;
static J *get(J *o,const char *key){J *v=NULL;if(o)json_object_object_get_ex(o,key,&v);return v;}
static const char *str(J *o,const char *key){J *v=get(o,key);const char *p=json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;return p&&strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static J *copy(J *j){if(!j)return NULL;const char *raw=json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN);return json_tokener_parse(raw);}
static J *find(tools *t,const char *name){for(size_t i=0;i<json_object_array_length(t->rows);i++){J *row=json_object_array_get_idx(t->rows,i);if(name&&!strcmp(name,str(row,"alias")))return row;}return NULL;}
static J *schema(void *raw){
 tools *t=raw;J *out=t->base->schema(t->base->ctx);if(!json_object_is_type(out,json_type_array)){json_object_put(out);return NULL;}
 for(size_t i=0;i<json_object_array_length(t->rows);i++){J *row=json_object_array_get_idx(t->rows,i),*fn=copy(get(row,"schema")),*entry=json_object_new_object();json_object_object_del(fn,"deferLoading");json_object_object_add(fn,"name",json_object_new_string(str(row,"alias")));json_object_object_add(entry,"type",json_object_new_string("function"));json_object_object_add(entry,"function",fn);json_object_array_add(out,entry);}return out;
}
static ah_status check(void *raw,const char *name,J *args,int *approval,int *effect,char *error,size_t size){
 tools *t=raw;if(!find(t,name))return t->base->check(t->base->ctx,name,args,approval,effect,error,size);
 if(!json_object_is_type(args,json_type_object)||strlen(json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN))>131072){snprintf(error,size,"Invalid extension arguments");return AH_INVALID_ARGUMENTS;}
 /* Unknown plugin behavior is effectful even if its display name says read. */
 *approval=1;*effect=1;return AH_DONE;
}
typedef struct {const ah_hooks *hooks;uint64_t deadline;} call_control;
static uint64_t milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return UINT64_MAX;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int cancelled(void *raw){call_control *c=raw;return milliseconds()>=c->deadline||(c->hooks&&c->hooks->cancelled&&c->hooks->cancelled(c->hooks->user));}
static ah_status call(void *raw,const char *name,J *args,const ah_hooks *hooks,size_t limit,unsigned timeout,char **out){
 tools *t=raw;J *row=find(t,name);if(!row)return t->base->call(t->base->ctx,name,args,hooks,limit,timeout,out);*out=NULL;
 J *request=json_object_new_object();json_object_object_add(request,"op",json_object_new_string("extensions.command"));json_object_object_add(request,"chat",json_object_new_string(t->chat));
 const char *keys[]={"source","package","version","command","configurationHash"};for(size_t i=0;i<5;i++)json_object_object_add(request,keys[i],json_object_get(get(row,keys[i])));json_object_object_add(request,"arguments",json_object_get(args));
 call_control control={hooks,milliseconds()+(timeout?timeout:15000)};dc_hooks bridge={.user=&control,.cancelled=cancelled};J *response=dc_extensions_request(t->store,request,&bridge);json_object_put(request);
 ah_status status=AH_EXEC_FAILED;J *value=get(get(response,"data"),"data");
 if(json_object_get_boolean(get(response,"ok")))status=json_object_get_boolean(get(value,"isError"))?AH_EXEC_FAILED:AH_DONE;
 else if(str(response,"error")&&!strcmp(str(response,"error"),"extension_effect_unknown"))status=AH_UNKNOWN;
 const char *text=json_object_to_json_string_ext(value?value:response,JSON_C_TO_STRING_PLAIN);
 if(text&&strlen(text)<=limit)*out=strdup(text);
 else{*out=strdup("{\"error\":\"extension_result_exceeds_limit\",\"effectsMayHaveOccurred\":true}");if(status==AH_DONE)status=AH_EXEC_FAILED;}
 json_object_put(response);return *out?status:AH_UNKNOWN;
}
static int compare(const void *a,const void *b){J *left=*(J*const*)a,*right=*(J*const*)b;return strcmp(json_object_to_json_string_ext(left,JSON_C_TO_STRING_PLAIN),json_object_to_json_string_ext(right,JSON_C_TO_STRING_PLAIN));}
J *dc_extension_tools_snapshot(dc_store *store){
 J *request=json_tokener_parse("{\"op\":\"extensions.tools\"}"),*response=dc_extensions_request(store,request,NULL);json_object_put(request);
 if(!json_object_get_boolean(get(response,"ok"))){json_object_put(response);return NULL;}J *rows=get(response,"data");size_t n=json_object_array_length(rows);J **items=calloc(n?n:1,sizeof *items);if(!items){json_object_put(response);return NULL;}
 for(size_t i=0;i<n;i++)items[i]=json_object_array_get_idx(rows,i);qsort(items,n,sizeof *items,compare);J *out=json_object_new_array();for(size_t i=0;i<n;i++)json_object_array_add(out,json_object_get(items[i]));free(items);json_object_put(response);return out;
}
int dc_extension_tools_open(dc_store *store,const char *chat,const ah_tool_service *base,J *snapshot,ah_tool_service *out){
 if(!store||!chat||!base||!base->ctx||!out||!json_object_is_type(snapshot,json_type_array))return 0;*out=(ah_tool_service){0};
 tools *t=calloc(1,sizeof *t);if(!t)return 0;t->store=store;t->chat=strdup(chat);t->base=base;t->rows=json_object_new_array();
 J *rows=snapshot;for(size_t i=0;i<json_object_array_length(rows);i++){
  J *row=json_object_array_get_idx(rows,i),*definition=get(row,"schema");const char *name=str(definition,"name"),*digest=str(row,"digest");
  if(!name||!digest||strlen(digest)!=64||strlen(name)>39||strspn(name,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=strlen(name)||!json_object_is_type(get(definition,"parameters"),json_type_object))continue;
  char alias[65];snprintf(alias,sizeof alias,"ext_%.20s_%s",digest,name);J *cloned=copy(row);if(!cloned||find(t,alias)){json_object_put(cloned);dc_extension_tools_close(&(ah_tool_service){.ctx=t});return 0;}json_object_object_add(cloned,"alias",json_object_new_string(alias));json_object_array_add(t->rows,cloned);
 }
 if(!t->chat){dc_extension_tools_close(&(ah_tool_service){.ctx=t});return 0;}
 *out=(ah_tool_service){t,schema,check,call};return 1;
}
void dc_extension_tools_close(ah_tool_service *service){if(!service||!service->ctx)return;tools *t=service->ctx;json_object_put(t->rows);free(t->chat);free(t);*service=(ah_tool_service){0};}
