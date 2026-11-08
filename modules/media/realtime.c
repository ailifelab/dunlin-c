#define _POSIX_C_SOURCE 200809L
#include "dunlin/realtime.h"
#include "dunlin/websocket.h"
#include "dunlin/compact/json.h"
#include <json-c/json.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
typedef struct json_object J;
struct dl_realtime {dl_websocket *ws;dl_realtime_hooks hooks;J *settings,*finals,*history_pending;int phase,images;double started;};
static double now(void){
#ifdef _WIN32
return (double)GetTickCount64()/1000;
#else
struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (double)t.tv_sec+(double)t.tv_nsec/1e9;
#endif
}
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);if(!json_object_is_type(v,json_type_string))return NULL;const char *s=json_object_get_string(v);return strlen(s)==(size_t)json_object_get_string_len(v)?s:NULL;}
static void text(J *o,const char *k,const char *v){json_object_object_add(o,k,json_object_new_string(v?v:""));}
static void emit(dl_realtime *s,const char *kind,const void *bytes,size_t n){if(s->hooks.event)s->hooks.event(s->hooks.user,kind,bytes,n);}
static int fail(dl_realtime *s,const char *code){s->phase=-1;J *j=json_object_new_object();text(j,"code",code);const char *raw=json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN);emit(s,"error",raw,strlen(raw));json_object_put(j);return 0;}
static int send_event(dl_realtime *s,J *j){const char *raw=json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN);int ok=dl_ws_send(s->ws,raw,strlen(raw));json_object_put(j);return ok?1:fail(s,"transport_send_failed");}
static J *event(const char *kind){J *j=json_object_new_object();text(j,"type",kind);return j;}
static char *encoded(const void *bytes,size_t n){if(!bytes||!n||n>1048576)return NULL;char *out=malloc(4*((n+2)/3)+1);if(out)EVP_EncodeBlock((unsigned char*)out,bytes,(int)n);return out;}
dl_realtime *dl_realtime_open(const char *raw,const char *secret,const dl_realtime_hooks *hooks){
 if(!raw||strlen(raw)>262144)return NULL;J *cfg=dunlin_json_parse(raw,strlen(raw));const char *url=str(cfg,"url"),*model=str(cfg,"model"),*voice=str(cfg,"voice"),*instructions=str(cfg,"instructions"),*transcription=str(cfg,"transcriptionModel");
 if(!url||strlen(url)>4096||strchr(url,'?')||strchr(url,'#')||!model||!*model||strlen(model)>256||!voice||!*voice||strlen(voice)>64||(instructions&&strlen(instructions)>131072)||(transcription&&strlen(transcription)>256)){json_object_put(cfg);return NULL;}
 J *history=get(cfg,"history");size_t history_bytes=0;
 if(history&&(!json_object_is_type(history,json_type_array)||json_object_array_length(history)>256)){json_object_put(cfg);return NULL;}
 for(size_t i=0;history&&i<json_object_array_length(history);i++){
  J *m=json_object_array_get_idx(history,i);const char *role=str(m,"role"),*body=str(m,"text");
  if(!role||(strcmp(role,"user")&&strcmp(role,"assistant")&&strcmp(role,"system"))||!body||!*body||strlen(body)>65536||(history_bytes+=strlen(body))>131072){json_object_put(cfg);return NULL;}
 }
 CURL *encoder=curl_easy_init();char *escaped=encoder?curl_easy_escape(encoder,model,0):NULL;size_t n=strlen(url)+(escaped?strlen(escaped):0)+16;char *target=escaped?malloc(n):NULL;
 if(target)snprintf(target,n,"%s%cmodel=%s",url,strchr(url,'?')?'&':'?',escaped);curl_free(escaped);curl_easy_cleanup(encoder);
 dl_realtime *s=target?calloc(1,sizeof *s):NULL;if(!s){free(target);json_object_put(cfg);return NULL;}if(hooks)s->hooks=*hooks;s->settings=cfg;s->finals=json_object_new_object();s->history_pending=json_object_new_object();s->images=json_object_get_boolean(get(cfg,"images"));
 s->ws=dl_ws_open(target,secret,json_object_get_boolean(get(cfg,"local")),2097152,s->hooks.user,s->hooks.cancelled);free(target);if(!s->ws){dl_realtime_close(s);return NULL;}s->started=now();return s;
}
static int configure(dl_realtime *s){
 J *j=event("session.update"),*session=json_object_new_object(),*audio=json_object_new_object();
 text(session,"type","realtime");text(session,"model",str(s->settings,"model"));text(session,"instructions",str(s->settings,"instructions"));
 J *modalities=json_object_new_array();json_object_array_add(modalities,json_object_new_string("audio"));json_object_object_add(session,"output_modalities",modalities);
 J *input=json_tokener_parse("{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},\"turn_detection\":{\"type\":\"server_vad\",\"create_response\":true,\"interrupt_response\":true}}");
 const char *transcription=str(s->settings,"transcriptionModel");if(transcription&&*transcription){J *t=json_object_new_object();text(t,"model",transcription);json_object_object_add(input,"transcription",t);}
 J *output=json_tokener_parse("{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000}}");text(output,"voice",str(s->settings,"voice"));json_object_object_add(audio,"input",input);json_object_object_add(audio,"output",output);json_object_object_add(session,"audio",audio);json_object_object_add(session,"tools",json_object_new_array());json_object_object_add(j,"session",session);s->phase=1;return send_event(s,j);
}
static int load_history(dl_realtime *s){
 J *history=get(s->settings,"history");size_t count=history?json_object_array_length(history):0;
 if(!count){s->phase=2;emit(s,"ready","{}",2);return 1;}
 s->phase=3;
 for(size_t i=0;i<count;i++){
  J *row=json_object_array_get_idx(history,i),*j=event("conversation.item.create"),*item=json_object_new_object(),*content=json_object_new_array(),*part=json_object_new_object();
  char id[64];snprintf(id,sizeof id,"dunlin_history_%zu",i);text(item,"id",id);text(item,"type","message");text(item,"role",str(row,"role"));
  text(part,"type",!strcmp(str(row,"role"),"assistant")?"output_text":"input_text");text(part,"text",str(row,"text"));json_object_array_add(content,part);json_object_object_add(item,"content",content);json_object_object_add(j,"item",item);
  json_object_object_add(s->history_pending,id,json_object_new_boolean(1));if(!send_event(s,j))return 0;
 }
 return 1;
}
static int transcript(dl_realtime *s,const char *id,const char *role,const char *source,const char *body,int gap){
 if(s->phase!=2||!id||!*id||strlen(id)>256||(!gap&&(!body||strlen(body)>65536)))return fail(s,"invalid_transcript");
 unsigned char digest[32];char hash[65]="";if(body){SHA256((const unsigned char*)body,strlen(body),digest);for(size_t i=0;i<32;i++)snprintf(hash+2*i,3,"%02x",digest[i]);}
 J *prior=get(s->finals,id);
 if(prior){if(gap)return 1;return !strcmp(json_object_get_string(prior),hash)?1:fail(s,"conflicting_transcript");}
 if(json_object_object_length(s->finals)>=2048)return fail(s,"transcript_item_limit");
 J *value=json_object_new_object();text(value,"item",id);text(value,"role",role);text(value,"source",source);if(!gap)text(value,"text",body);
 /* Missing transcripts are explicit but may later receive a final ASR receipt. */
 if(!gap)json_object_object_add(s->finals,id,json_object_new_string(hash));
 const char *raw=json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);emit(s,gap?"gap":"final",raw,strlen(raw));json_object_put(value);return 1;
}
int dl_realtime_poll(dl_realtime *s){
 if(!s||s->phase<0)return 0;if(s->hooks.cancelled&&s->hooks.cancelled(s->hooks.user))return fail(s,"cancelled");double elapsed=now()-s->started;if(s->phase!=2&&elapsed>30)return fail(s,"negotiation_timeout");if(elapsed>=3600)return fail(s,"session_time_limit");char *raw=NULL;size_t n=0;int ready=dl_ws_receive(s->ws,&raw,&n);if(ready<0)return fail(s,"transport_closed");if(!ready)return 1;
 J *j=dunlin_json_parse(raw,n);free(raw);const char *type=str(j,"type");int ok=1;
 if(!type){json_object_put(j);return fail(s,"invalid_service_event");}
 if(!strcmp(type,"session.created")){if(s->phase!=0)ok=fail(s,"unexpected_session");else ok=configure(s);}
 else if(!strcmp(type,"session.updated")){
  J *session=get(j,"session"),*audio=get(session,"audio"),*input=get(get(audio,"input"),"format"),*output=get(get(audio,"output"),"format");
  const char *it=str(input,"type"),*ot=str(output,"type");
  if(s->phase!=1||!it||!ot||strcmp(it,"audio/pcm")||strcmp(ot,"audio/pcm")||json_object_get_int(get(input,"rate"))!=24000||json_object_get_int(get(output,"rate"))!=24000)ok=fail(s,"unsupported_negotiated_format");
  else ok=load_history(s);
 }
 else if(!strcmp(type,"conversation.item.done")&&s->phase==3){
  const char *id=str(get(j,"item"),"id");if(id&&get(s->history_pending,id)){
   json_object_object_del(s->history_pending,id);
   if(!json_object_object_length(s->history_pending)){s->phase=2;emit(s,"ready","{}",2);}
  }
 }
 else if(!strcmp(type,"error"))ok=fail(s,"service_error");
 else if(!strcmp(type,"input_audio_buffer.speech_started")){emit(s,"interrupted","{}",2);}
 else if(!strcmp(type,"response.output_audio.delta")){
  const char *b64=str(j,"delta");size_t length=b64?strlen(b64):0;unsigned char *bytes=length&&length<=1048576&&length%4==0?malloc(length):NULL;
  int decoded=bytes?EVP_DecodeBlock(bytes,(const unsigned char*)b64,(int)length):-1;
  if(decoded>=0){if(length&&b64[length-1]=='=')decoded--;if(length>1&&b64[length-2]=='=')decoded--;}
  char *canonical=decoded>0?encoded(bytes,(size_t)decoded):NULL;int valid=canonical&&b64&&!strcmp(canonical,b64);free(canonical);
  if(s->phase!=2||!valid||decoded<0||(decoded&1))ok=fail(s,"invalid_audio_payload");else {J *item=json_object_new_object();text(item,"item",str(j,"item_id"));const char *value=json_object_to_json_string_ext(item,JSON_C_TO_STRING_PLAIN);emit(s,"audio.item",value,strlen(value));json_object_put(item);emit(s,"audio",bytes,(size_t)decoded);}free(bytes);
 }
 else if(!strcmp(type,"response.output_audio_transcript.done")||!strcmp(type,"response.output_text.done")||!strcmp(type,"conversation.item.input_audio_transcription.completed")){
  const char *body=str(j,!strcmp(type,"response.output_text.done")?"text":"transcript"),*id=str(j,"item_id");
  ok=transcript(s,id,!strncmp(type,"conversation.",13)?"user":"assistant",!strncmp(type,"conversation.",13)?"provider_asr":"provider_text",body,0);
 }
 else if(!strcmp(type,"input_audio_buffer.committed")&&(!str(s->settings,"transcriptionModel")||!*str(s->settings,"transcriptionModel"))){
  const char *id=str(j,"item_id");if(s->phase!=2||!id||!*id||strlen(id)>256)ok=fail(s,"invalid_audio_item");
  else{J *gap=json_object_new_object();text(gap,"source","provider_asr");text(gap,"item",id);text(gap,"role","user");const char *value=json_object_to_json_string_ext(gap,JSON_C_TO_STRING_PLAIN);emit(s,"gap",value,strlen(value));json_object_put(gap);}
 }
 else if(!strcmp(type,"conversation.item.input_audio_transcription.failed"))ok=transcript(s,str(j,"item_id"),"user","provider_asr",NULL,1);
 else if(!strcmp(type,"response.done")){
  J *response=get(j,"response"),*items=get(response,"output");const char *status=str(response,"status");
  if(!status||!json_object_is_type(items,json_type_array)||json_object_array_length(items)>64)ok=fail(s,"invalid_response_end");
  for(size_t i=0;ok&&i<json_object_array_length(items);i++){
   J *item=json_object_array_get_idx(items,i);const char *kind=str(item,"type"),*role=str(item,"role"),*id=str(item,"id");
   if(!kind||strcmp(kind,"message")||!role||strcmp(role,"assistant"))continue;
   J *parts=get(item,"content");if(!json_object_is_type(parts,json_type_array)||json_object_array_length(parts)>1){ok=fail(s,"unsupported_output_parts");break;}
   J *part=json_object_array_get_idx(parts,0);const char *body=str(part,"transcript");if(!body)body=str(part,"text");
   ok=transcript(s,id,"assistant","provider_text",body,!body||!*body);
  }
  if(ok&&(!strcmp(status,"failed")||!strcmp(status,"incomplete")))emit(s,"response.failed","{}",2);
 }

 json_object_put(j);return ok;
}
int dl_realtime_audio(dl_realtime *s,const void *pcm,size_t bytes){if(!s||s->phase!=2||!bytes||(bytes&1)||bytes>65536)return 0;char *b64=encoded(pcm,bytes);if(!b64)return 0;J *j=event("input_audio_buffer.append");text(j,"audio",b64);free(b64);return send_event(s,j);}
int dl_realtime_image(dl_realtime *s,const void *jpeg,size_t bytes){if(!s||s->phase!=2||!s->images||!jpeg||bytes<4||bytes>524288||((const unsigned char*)jpeg)[0]!=255||((const unsigned char*)jpeg)[1]!=216)return 0;char *b64=encoded(jpeg,bytes);if(!b64)return 0;char *url=malloc(strlen(b64)+32);if(!url){free(b64);return 0;}sprintf(url,"data:image/jpeg;base64,%s",b64);free(b64);J *j=event("conversation.item.create"),*item=json_object_new_object(),*content=json_object_new_array(),*part=json_object_new_object();text(item,"type","message");text(item,"role","user");text(part,"type","input_image");text(part,"image_url",url);free(url);json_object_array_add(content,part);json_object_object_add(item,"content",content);json_object_object_add(j,"item",item);return send_event(s,j);}
int dl_realtime_interrupt(dl_realtime *s,const char *item,unsigned ms){if(!s||s->phase!=2||!item||!*item||strlen(item)>256||ms>3600000)return 0;J *j=event("conversation.item.truncate");text(j,"item_id",item);json_object_object_add(j,"content_index",json_object_new_int(0));json_object_object_add(j,"audio_end_ms",json_object_new_int64(ms));return send_event(s,j);}
int dl_realtime_ready(const dl_realtime *s){return s&&s->phase==2;}
void dl_realtime_close(dl_realtime *s){if(s){dl_ws_close(s->ws);json_object_put(s->settings);json_object_put(s->finals);json_object_put(s->history_pending);free(s);}}
