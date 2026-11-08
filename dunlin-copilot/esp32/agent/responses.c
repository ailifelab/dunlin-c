#include "responses.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static const char *text(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:NULL;}
static int string(cJSON *j,const char *k,const char *v){return v && cJSON_AddStringToObject(j,k,v)!=NULL;}
static int append(cJSON *array,cJSON *v){if(!v)return 0;if(!cJSON_AddItemToArray(array,v)){cJSON_Delete(v);return 0;}return 1;}
static int attach(cJSON *j,const char *k,cJSON *v){if(!v)return 0;if(!cJSON_AddItemToObject(j,k,v)){cJSON_Delete(v);return 0;}return 1;}
void ae_responses_reset(ae_responses *s){memset(s,0,sizeof *s);s->usage_known=7;}
char *ae_responses_request(ae_responses *s,const char *chat,const char *effort,unsigned limit){
 if(!s || !chat)return NULL;
 cJSON *src=ae_json_parse_bounded(chat,16384),*dst=NULL;char *wire=NULL;
 if(!src || !effort || (strcmp(effort,"low") && strcmp(effort,"xhigh")) || !limit)goto done;
 cJSON *messages=cJSON_GetObjectItemCaseSensitive(src,"messages"),*defs=cJSON_GetObjectItemCaseSensitive(src,"tools");
 int count=cJSON_GetArraySize(messages);
 if(!cJSON_IsArray(messages) || !cJSON_IsArray(defs) || s->sent_messages>count)goto done;
 dst=cJSON_CreateObject();if(!dst || !string(dst,"model",text(src,"model")) || !cJSON_AddBoolToObject(dst,"store",1) || !cJSON_AddBoolToObject(dst,"parallel_tool_calls",0) || !cJSON_AddNumberToObject(dst,"max_output_tokens",limit))goto done;
 cJSON *reason=cJSON_AddObjectToObject(dst,"reasoning");if(!reason || !string(reason,"effort",effort))goto done;
 if(s->previous_id[0] && !string(dst,"previous_response_id",s->previous_id))goto done;
 cJSON *input=cJSON_AddArrayToObject(dst,"input"),*tools=cJSON_AddArrayToObject(dst,"tools");if(!input || !tools)goto done;
 for(int i=s->previous_id[0]?s->sent_messages:0;i<count;i++){
  cJSON *m=cJSON_GetArrayItem(messages,i);const char *role=text(m,"role"),*content=text(m,"content");if(!role || !content)goto done;
  if(s->previous_id[0] && !strcmp(role,"assistant"))continue; /* Already retained by Responses. */
  cJSON *v=cJSON_CreateObject();if(!v)goto done;
  if(!strcmp(role,"tool")){
   const char *call=text(m,"tool_call_id");
   if(!s->previous_id[0] || !s->pending_call[0] || !call || strcmp(call,s->pending_call) || !string(v,"type","function_call_output") || !string(v,"call_id",call) || !string(v,"output",content)){cJSON_Delete(v);goto done;}
  }else if(strcmp(role,"user") && strcmp(role,"assistant") && strcmp(role,"system")){cJSON_Delete(v);goto done;}
  else if(!string(v,"role",role) || !string(v,"content",content)){cJSON_Delete(v);goto done;}
  if(!append(input,v))goto done;
 }
 if(!cJSON_GetArraySize(input))goto done;
 for(int i=0;i<cJSON_GetArraySize(defs);i++){
  cJSON *f=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(defs,i),"function");
  cJSON *v=cJSON_CreateObject();if(!v)goto done;
  if(!string(v,"type","function") || !string(v,"name",text(f,"name")) || !string(v,"description",text(f,"description")) || !cJSON_AddBoolToObject(v,"strict",0)){cJSON_Delete(v);goto done;}
  cJSON *params=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(f,"parameters"),1);
  if(!attach(v,"parameters",params)){cJSON_Delete(v);goto done;}
  if(!append(tools,v))goto done;
 }
 wire=cJSON_PrintUnformatted(dst);if(wire && strlen(wire)>=32768){cJSON_free(wire);wire=NULL;}
 if(wire)s->pending_messages=count;
done:cJSON_Delete(src);cJSON_Delete(dst);return wire;
}
static void usage(ae_responses *s,const cJSON *j,unsigned bit,const char *key,uint64_t *sum){
 const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
 if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<0 || v->valuedouble>4294967295.0 || floor(v->valuedouble)!=v->valuedouble)s->usage_known&=~bit;
 else *sum+=(uint64_t)v->valuedouble;
}
int ae_responses_reply(ae_responses *s,const char *raw,char *out,size_t cap){
 if(!s || !raw || !out || !cap)return 0;
 cJSON *src=ae_json_parse_bounded(raw,65536),*dst=NULL;char *wire=NULL;int ok=0;
 const char *failure="OpenAI Responses invalid/over-limit; no tool frame accepted";
 char pending[81]={0};out[0]=0;
 const char *id=text(src,"id"),*status=text(src,"status");
 if(status && !strcmp(status,"incomplete")){
  const char *reason=text(cJSON_GetObjectItemCaseSensitive(src,"incomplete_details"),"reason");
  failure=reason && !strcmp(reason,"max_output_tokens")?"OpenAI output token budget exhausted; no tool frame accepted":"OpenAI response incomplete; no tool frame accepted";goto done;
 }
 cJSON *items=cJSON_GetObjectItemCaseSensitive(src,"output");
 if(!id || !*id || strlen(id)>=sizeof s->previous_id || !status || strcmp(status,"completed") || !cJSON_IsArray(items))goto done;
 dst=cJSON_CreateObject();cJSON *choices=dst?cJSON_AddArrayToObject(dst,"choices"):NULL,*choice=cJSON_CreateObject();
 if(!choices || !append(choices,choice)){if(!choices)cJSON_Delete(choice);goto done;}
 cJSON *m=cJSON_AddObjectToObject(choice,"message"),*calls=m?cJSON_AddArrayToObject(m,"tool_calls"):NULL;
 if(!m || !calls || !string(m,"content",""))goto done;
 for(int i=0;i<cJSON_GetArraySize(items);i++){
  cJSON *item=cJSON_GetArrayItem(items,i);const char *type=text(item,"type");if(!type)goto done;
  if(!strcmp(type,"reasoning"))continue; /* Server continuation preserves opaque state. */
  if(!strcmp(type,"function_call")){
   const char *call=text(item,"call_id"),*name=text(item,"name"),*args=text(item,"arguments");
   if(!call || !*call || strlen(call)>80 || !name || !*name || !args)goto done;
   cJSON *v=cJSON_CreateObject();if(!v)goto done;
   cJSON *f=cJSON_AddObjectToObject(v,"function");
   if(!f || !string(v,"id",call) || !string(v,"type","function") || !string(f,"name",name) || !string(f,"arguments",args)){cJSON_Delete(v);goto done;}
   if(!append(calls,v))goto done;
   if(cJSON_GetArraySize(calls)>1)goto done;
   snprintf(pending,sizeof pending,"%s",call);
  }else if(!strcmp(type,"message")){
   cJSON *parts=cJSON_GetObjectItemCaseSensitive(item,"content");if(!cJSON_IsArray(parts))goto done;
   for(int k=0;k<cJSON_GetArraySize(parts);k++){
    cJSON *part=cJSON_GetArrayItem(parts,k);const char *kind=text(part,"type"),*value=NULL;
    if(kind && !strcmp(kind,"output_text"))value=text(part,"text");
    else if(kind && !strcmp(kind,"refusal"))value=text(part,"refusal");
    if(!value)goto done;
    /* Multiple text fragments are combined, not silently discarded. */
    cJSON *old=cJSON_GetObjectItemCaseSensitive(m,"content");size_t a=strlen(old->valuestring),b=strlen(value);
    if(a+b>=8192)goto done;
    char *joined=cJSON_malloc(a+b+1);if(!joined)goto done;
    memcpy(joined,old->valuestring,a);memcpy(joined+a,value,b+1);
    cJSON *v=cJSON_CreateString(joined);cJSON_free(joined);if(!v)goto done;
    if(!cJSON_ReplaceItemInObjectCaseSensitive(m,"content",v)){cJSON_Delete(v);goto done;}
   }
  }else goto done;
 }
 wire=cJSON_PrintUnformatted(dst);if(!wire || strlen(wire)>=cap)goto done;
 memcpy(out,wire,strlen(wire)+1);snprintf(s->previous_id,sizeof s->previous_id,"%s",id);s->sent_messages=s->pending_messages;s->calls++;
 snprintf(s->pending_call,sizeof s->pending_call,"%s",pending);
 cJSON *u=cJSON_GetObjectItemCaseSensitive(src,"usage");usage(s,u,1,"input_tokens",&s->input_tokens);usage(s,u,2,"output_tokens",&s->output_tokens);
 usage(s,cJSON_GetObjectItemCaseSensitive(u,"output_tokens_details"),4,"reasoning_tokens",&s->reasoning_tokens);ok=1;
done:if(!ok && out && cap)snprintf(out,cap,"%s",failure);cJSON_free(wire);cJSON_Delete(dst);cJSON_Delete(src);return ok;
}
