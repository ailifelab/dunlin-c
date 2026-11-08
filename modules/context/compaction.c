#define _POSIX_C_SOURCE 200809L
#include "../../desktop/compaction.h"
#include "../../desktop/preparation.h"
#include "dunlin/context_budget.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
typedef struct json_object J;
struct dc_compactor {dc_store *store;char *chat;char strategy[24];uint64_t budget;unsigned retain,summary_output;double threshold;int automatic;const dc_hooks *hooks;dunlin_context_projector_v1 service;};
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static int role(J *o,const char *r){const char *p=str(o,"role");return p&&!strcmp(p,r);}
static size_t bytes(J *o){return o?strlen(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN)):0;}
static J *message(const char *role_name,const char *text){J *o=json_object_new_object();json_object_object_add(o,"role",json_object_new_string(role_name));json_object_object_add(o,"content",json_object_new_string(text));return o;}
static dunlin_model_status error(char *out,size_t n,const char *text){if(out&&n)snprintf(out,n,"%s",text);return DUNLIN_MODEL_LIMIT;}
static void event(dc_compactor *c,const char *name,const char *text){if(c->hooks&&c->hooks->event)c->hooks->event(c->hooks->user,name,text);}
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;}
static void trace_object(dc_compactor *c,const char *name,J *value){event(c,name,json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN));json_object_put(value);}
static J *portable(J *source){J *v=json_object_new_object();const char *keys[]={"role","content","tool_calls","tool_call_id"};for(size_t i=0;i<4;i++){J *p=get(source,keys[i]);if(p)json_object_object_add(v,keys[i],json_object_get(p));}return v;}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int i,const char *p){sqlite3_bind_text(q,i,p?p:"",-1,SQLITE_TRANSIENT);}
int dc_compaction_schema(dc_store *s){return sqlite3_exec(s->db,"CREATE TABLE IF NOT EXISTS compaction_records(id INTEGER PRIMARY KEY,chat TEXT NOT NULL,source_hash TEXT NOT NULL,profile_hash TEXT NOT NULL,strategy TEXT NOT NULL,source_count INTEGER NOT NULL,summary TEXT NOT NULL,before_bytes INTEGER NOT NULL,after_bytes INTEGER NOT NULL,measure_kind TEXT NOT NULL DEFAULT 'utf8-byte-estimate',status TEXT NOT NULL,error TEXT NOT NULL DEFAULT '',created INTEGER NOT NULL DEFAULT(unixepoch()));CREATE INDEX IF NOT EXISTS compact_cache ON compaction_records(chat,source_hash,profile_hash,strategy,status);",NULL,NULL,NULL)==SQLITE_OK;}
static char *cached(dc_compactor *c,const char *hash,const char *profile){sqlite3_stmt *q=prep(c->store,"SELECT summary FROM compaction_records WHERE chat=? AND source_hash=? AND profile_hash=? AND strategy=? AND status='committed' ORDER BY id DESC LIMIT 1");if(!q)return NULL;bind(q,1,c->chat);bind(q,2,hash);bind(q,3,profile);bind(q,4,c->strategy);char *p=sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);return p;}
static J *incremental_source(dc_compactor *c,J *source,const char *profile){
 sqlite3_stmt *q=prep(c->store,"SELECT source_count,source_hash,summary FROM compaction_records WHERE chat=? AND profile_hash=? AND strategy=? AND status='committed' ORDER BY source_count DESC,id DESC LIMIT 16");if(!q)return json_object_get(source);bind(q,1,c->chat);bind(q,2,profile);bind(q,3,c->strategy);J *answer=NULL;size_t total=json_object_array_length(source);
 while(sqlite3_step(q)==SQLITE_ROW){sqlite3_int64 count=sqlite3_column_int64(q,0);if(count<1||(uint64_t)count>=total)continue;J *prefix=json_object_new_array();for(size_t i=0;i<(size_t)count;i++)json_object_array_add(prefix,json_object_get(json_object_array_get_idx(source,i)));const char *raw=json_object_to_json_string_ext(prefix,JSON_C_TO_STRING_PLAIN);char hash[65];dc_sha256(raw,strlen(raw),hash);json_object_put(prefix);if(strcmp(hash,(const char*)sqlite3_column_text(q,1)))continue;
  answer=json_object_new_array();for(size_t i=0;i<(size_t)count;i++){J *v=json_object_array_get_idx(source,i);if(role(v,"system"))json_object_array_add(answer,json_object_get(v));}
  const char *summary=(const char*)sqlite3_column_text(q,2);size_t len=strlen(summary)+128;char *text=malloc(len);if(!text){json_object_put(answer);answer=NULL;break;}snprintf(text,len,"Prior derived checkpoint; merge with newer source below, and preserve uncertainty and corrections:\n%s",summary);json_object_array_add(answer,message("user",text));free(text);
  for(size_t i=(size_t)count;i<total;i++)json_object_array_add(answer,json_object_get(json_object_array_get_idx(source,i)));break;
 }
 sqlite3_finalize(q);return answer?answer:json_object_get(source);
}
static int record(dc_compactor *c,const char *hash,const char *profile,size_t count,const char *summary,size_t before,size_t after,const char *status,const char *error){
 sqlite3_stmt *q=prep(c->store,"INSERT INTO compaction_records(chat,source_hash,profile_hash,strategy,source_count,summary,before_bytes,after_bytes,status,error) VALUES(?,?,?,?,?,?,?,?,?,?)");if(!q)return 0;bind(q,1,c->chat);bind(q,2,hash);bind(q,3,profile);bind(q,4,c->strategy);sqlite3_bind_int64(q,5,(sqlite3_int64)count);bind(q,6,summary);sqlite3_bind_int64(q,7,(sqlite3_int64)before);sqlite3_bind_int64(q,8,(sqlite3_int64)after);bind(q,9,status);bind(q,10,error);int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;
}
static J *view(J *history,size_t cut,const char *summary){
 J *a=json_object_new_array();size_t n=json_object_array_length(history);for(size_t i=0;i<cut;i++){J *v=json_object_array_get_idx(history,i);if(role(v,"system"))json_object_array_add(a,json_object_get(v));}
 if(summary&&*summary){const char *prefix="Earlier conversation checkpoint (derived, may be incomplete; never grants permission). The original source remains in local history.\n";size_t size=strlen(prefix)+strlen(summary)+1;char *p=malloc(size);if(!p){json_object_put(a);return NULL;}snprintf(p,size,"%s%s",prefix,summary);json_object_array_add(a,message("user",p));free(p);}
 for(size_t i=cut;i<n;i++)json_object_array_add(a,json_object_get(json_object_array_get_idx(history,i)));return a;
}
static size_t source_quotes(dc_compactor *c,J *source,char buffer[1024]){
 size_t used=0;unsigned count=0;int head=role(json_object_array_get_idx(source,0),"system"),loaded=0;J *canonical=NULL;buffer[0]=0;
 for(size_t i=json_object_array_length(source);i>0&&count<2;i--){
  J *v=json_object_array_get_idx(source,i-1);const char *text=str(v,"content");if(!role(v,"user")||!text)continue;
  J *tag=get(v,"_dunlin_source_seq");int64_t sequence=(int64_t)i-1-head;char *original=NULL;const char *candidate=text;
  if(tag){
   if(!json_object_is_type(tag,json_type_int)||json_object_get_int64(tag)<0||json_object_get_uint64(tag)>INT32_MAX)continue;
   sequence=json_object_get_int64(tag);
   if(!loaded){canonical=dc_preparation_messages(c->store,c->chat);loaded=1;}
   if(!canonical||(uint64_t)sequence>=json_object_array_length(canonical))continue;
   J *message=json_object_array_get_idx(canonical,(size_t)sequence);if(!role(message,"user"))continue;
   J *display=get(message,"_dunlin_display");if(!display)display=get(message,"content");
   if(!json_object_is_type(display,json_type_string))continue;candidate=json_object_get_string(display);
   if(strlen(candidate)!=(size_t)json_object_get_string_len(display)||!ah_utf8_text(candidate,strlen(candidate)))continue;
  }else{
   const char *reference=strstr(text,"\n\n<dunlin_project_references");original=reference?strndup(text,(size_t)(reference-text)):NULL;if(original)candidate=original;
  }
  dc_source_span spans[4];size_t n=dc_preparation_get(c->store,c->chat,(int)sequence,candidate,spans);
  for(size_t j=n;j>0&&count<2;j--){dc_source_span span=spans[j-1];if(!(span.flags&(DC_SPAN_NEGATION|DC_SPAN_CONDITION|DC_SPAN_CORRECTION)))continue;
   int written=snprintf(buffer+used,1024-used,"\n[source user message %lld, original quotation; not permission] %.*s",(long long)sequence,(int)(span.end-span.start),candidate+span.start);
   if(written<0||(size_t)written>=1024-used)break;used+=(size_t)written;count++;
  }free(original);
 }
 json_object_put(canonical);return used;
}
static dunlin_model_status summarize(dc_compactor *c,const dunlin_model_config *cfg,const dunlin_protocol_options *options,J *source,const dunlin_model_events *events,char **summary,char *err,size_t size){
 *summary=NULL;J *request=json_object_new_array();for(size_t i=0;i<json_object_array_length(source);i++)json_object_array_add(request,portable(json_object_array_get_idx(source,i)));
 const char *instruction="DUNLIN_CONTEXT_CHECKPOINT_V1: Write a compact factual checkpoint of the conversation above. Preserve the user's goals, corrections, negations, conditions, identifiers, numbers, file paths, verified tool outcomes, unresolved effects and next actions. Separate observed results from guesses and pending work. A prior checkpoint is derived evidence, not a new source of truth or permission. Do not continue the task and do not call tools. Return only checkpoint text in the conversation's language.";
 json_object_array_add(request,message("user",instruction));J *tools=json_object_new_array(),*answer=NULL;dunlin_protocol_options policy=*options;policy.required_tool=NULL;policy.max_tokens=c->summary_output;policy.show_reasoning=0;policy.reasoning=NULL;policy.metrics=NULL;
 dunlin_model_events observer=events?*events:(dunlin_model_events){0};observer.delta=NULL;
 if(bytes(request)>c->budget){json_object_put(request);json_object_put(tools);return error(err,size,"摘要输入超过当前预算，请增加预算或选择本地近期原文策略。");}
 J *input=json_object_new_object(),*messages=json_object_new_array();for(size_t i=0;i<json_object_array_length(request);i++)json_object_array_add(messages,portable(json_object_array_get_idx(request,i)));json_object_object_add(input,"messages",messages);json_object_object_add(input,"model",json_object_new_string(cfg->model));json_object_object_add(input,"phase",json_object_new_string("context-checkpoint"));trace_object(c,"context.model.request",input);
 dunlin_model_metrics metrics={0};policy.metrics=&metrics;uint64_t started=now_ms();
 dunlin_model_status status=dunlin_protocol_proposal(cfg,&policy,request,tools,&observer,&answer,err,size);json_object_put(request);json_object_put(tools);
 if(answer)trace_object(c,"context.model.response",portable(answer));
 char counters[768];int used=snprintf(counters,sizeof counters,"{\"elapsed_ms\":%" PRIu64 ",\"usage_present\":%u,\"request_bytes\":%zu,\"response_bytes\":%zu,\"response_bytes_known\":%s",now_ms()-started,metrics.present,metrics.request_bytes,metrics.response_bytes,metrics.response_bytes_known?"true":"false");
#define COUNT(bit,field) do {if(used>0&&(size_t)used<sizeof counters&&(metrics.present&(bit))){int n=snprintf(counters+used,sizeof counters-(size_t)used,",\"" #field "\":%" PRIu64,metrics.field);if(n>0)used+=n;}}while(0)
 COUNT(DUNLIN_USAGE_INPUT,input_tokens);COUNT(DUNLIN_USAGE_OUTPUT,output_tokens);COUNT(DUNLIN_USAGE_TOTAL,total_tokens);COUNT(DUNLIN_USAGE_CACHED_INPUT,cached_input_tokens);COUNT(DUNLIN_USAGE_CACHE_CREATION,cache_creation_tokens);COUNT(DUNLIN_USAGE_REASONING,reasoning_tokens);
#undef COUNT
 if(used>0&&(size_t)used+1<sizeof counters){counters[used++]='}';counters[used]=0;event(c,"context.model.metrics",counters);}
 if(status==DUNLIN_MODEL_OK){const char *text=str(answer,"content");J *calls=get(answer,"tool_calls");if(!text||!*text||(calls&&json_object_array_length(calls))||strlen(text)>65536)status=error(err,size,"摘要服务未返回可用的纯文本，原始聊天保留。");else{*summary=strdup(text);if(!*summary)status=DUNLIN_MODEL_LIMIT;}}
 J *outcome=json_object_new_object();json_object_object_add(outcome,"status",json_object_new_int(status));json_object_object_add(outcome,"accepted",json_object_new_boolean(status==DUNLIN_MODEL_OK));if(status!=DUNLIN_MODEL_OK&&err&&*err)json_object_object_add(outcome,"error",json_object_new_string(err));trace_object(c,"context.model.end",outcome);
 json_object_put(answer);return status;
}
static dunlin_model_status project(void *ctx,const dunlin_model_config *cfg,const dunlin_protocol_options *options,J *history,J *tools,const dunlin_model_events *events,J **out,char *err,size_t size){
 dc_compactor *c=ctx;*out=NULL;size_t n=json_object_array_length(history),before=bytes(history),schemas=bytes(tools);
 for(size_t i=0;i<n;i++){J *v=json_object_array_get_idx(history,i);if(!json_object_is_type(get(v,"content"),json_type_string)&&get(v,"content")&&!json_object_is_type(get(v,"content"),json_type_null))return error(err,size,"当前计量策略尚未配置媒体定价，不能保证媒体上下文预算。");}
 if(before+schemas<c->budget*c->threshold){*out=json_object_get(history);return DUNLIN_MODEL_OK;}
 if(!c->automatic){if(before+schemas<c->budget){*out=json_object_get(history);return DUNLIN_MODEL_OK;}return error(err,size,"上下文已超过所设预算，自动压缩已关闭。请调整模型预算或启用整理策略。");}
 size_t cut=0;unsigned seen=0;for(size_t i=n;i>0;i--)if(role(json_object_array_get_idx(history,i-1),"user")&&++seen==c->retain)cut=i-1;
 unsigned older_users=0;for(size_t i=0;i<cut;i++)if(role(json_object_array_get_idx(history,i),"user"))older_users++;
 if(!cut||!older_users){if(before+schemas<c->budget){*out=json_object_get(history);return DUNLIN_MODEL_OK;}return error(err,size,"最近回合或系统与工具内容已超过预算，无法安全拆分。");}
 J *source=json_object_new_array();for(size_t i=0;i<cut;i++)json_object_array_add(source,json_object_get(json_object_array_get_idx(history,i)));
 const char *raw=json_object_to_json_string_ext(source,JSON_C_TO_STRING_PLAIN);char hash[65],profile[65];dc_sha256(raw,strlen(raw),hash);
 J *identity=json_object_new_object();json_object_object_add(identity,"endpoint",json_object_new_string(cfg->base_url));json_object_object_add(identity,"model",json_object_new_string(cfg->model));json_object_object_add(identity,"protocol",json_object_new_int(options->protocol));json_object_object_add(identity,"version",json_object_new_int(1));json_object_object_add(identity,"summaryOutput",json_object_new_int((int)c->summary_output));raw=json_object_to_json_string_ext(identity,JSON_C_TO_STRING_PLAIN);dc_sha256(raw,strlen(raw),profile);json_object_put(identity);
 char *summary=cached(c,hash,profile);int reused=summary!=NULL;
 if(!summary&&strcmp(c->strategy,"recent")){
  J *prepared=incremental_source(c,source,profile);
  event(c,"context.compacting","正在整理较早的完整回合；原始聊天保留。");dunlin_model_status status=summarize(c,cfg,options,prepared,events,&summary,err,size);json_object_put(prepared);
  if(status!=DUNLIN_MODEL_OK){record(c,hash,profile,cut,"",before,before,"failed",err&&*err?err:"summary failed");json_object_put(source);return status;}
  char quotes[1024];size_t quote_bytes=source_quotes(c,source,quotes);
  if(quote_bytes){size_t length=strlen(summary);char *with_quotes=realloc(summary,length+quote_bytes+1);if(!with_quotes){free(summary);json_object_put(source);return error(err,size,"无法准备原文保护引用，候选摘要未应用。");}summary=with_quotes;memcpy(summary+length,quotes,quote_bytes+1);}
 }
 if(!summary)summary=strdup("Earlier complete turns were excluded by the selected recent-verbatim strategy. Do not infer missing facts or prior permissions from this omission.");
 J *result=summary?view(history,cut,summary):NULL;size_t after=bytes(result);
 if(!result||after+schemas>=c->budget||after>=before){if(!reused)record(c,hash,profile,cut,summary?summary:"",before,after,"failed","no useful safe reduction");free(summary);json_object_put(source);json_object_put(result);return error(err,size,"本次整理没有释放足够空间。原始聊天保留，请调整预算或保留回合数。");}
 if(!reused&&!record(c,hash,profile,cut,summary,before,after,"committed","")){free(summary);json_object_put(source);json_object_put(result);return error(err,size,"无法保存整理记录，未应用候选摘要。");}
 event(c,"context.compacted",reused?"已复用带来源校验的摘要。":"已整理较早回合，完整原文仍保留在本地。");free(summary);json_object_put(source);*out=result;return DUNLIN_MODEL_OK;
}
dc_compactor *dc_compactor_open(dc_store *s,const char *chat,J *settings,const dc_hooks *hooks,char *err,size_t size){
 if(!json_object_is_type(settings,json_type_object))return NULL;
 const char *numbers[]={"modelLimit","deploymentLimit","selectedLimit","outputLimit","requestedOutput","reserve"};
 for(size_t i=0;i<sizeof numbers/sizeof *numbers;i++){J *v=get(settings,numbers[i]);if(!json_object_is_type(v,json_type_int)||json_object_get_int64(v)<0){error(err,size,"上下文能力字段必须是非负整数。");return NULL;}}
 if(!json_object_is_type(get(settings,"automatic"),json_type_boolean)){error(err,size,"自动压缩选项必须是布尔值。");return NULL;}
 dc_context_capabilities caps={.model_limit=(uint64_t)json_object_get_int64(get(settings,"modelLimit")),.deployment_limit=(uint64_t)json_object_get_int64(get(settings,"deploymentLimit")),.selected_limit=(uint64_t)json_object_get_int64(get(settings,"selectedLimit")),.client_limit=4194304,.output_limit=(uint64_t)json_object_get_int64(get(settings,"outputLimit")),.requested_output=(uint64_t)json_object_get_int64(get(settings,"requestedOutput")),.overhead_reserve=(uint64_t)json_object_get_int64(get(settings,"reserve"))};dc_context_range range;
 if(dc_context_budget(&caps,&range)!=DC_CONTEXT_VALID||range.selected>4194304){error(err,size,"上下文能力或预算无效；未知能力不能伪造滑条上限。");return NULL;}
 const char *strategy=str(settings,"strategy");unsigned retain=(unsigned)json_object_get_int(get(settings,"retainTurns")),output=(unsigned)json_object_get_int(get(settings,"summaryOutput"));double threshold=json_object_get_double(get(settings,"threshold"));
 if(!strategy||(strcmp(strategy,"semantic")&&strcmp(strategy,"recent"))||retain<1||retain>8||!output||output>range.output||!isfinite(threshold)||threshold<.25||threshold>.95){error(err,size,"无效的压缩策略或保留范围。");return NULL;}
 dc_compactor *c=calloc(1,sizeof *c);if(!c)return NULL;c->chat=strdup(chat);if(!c->chat){free(c);return NULL;}c->store=s;c->budget=range.input_budget;c->retain=retain;c->summary_output=output;c->threshold=threshold;c->automatic=json_object_get_boolean(get(settings,"automatic"));snprintf(c->strategy,sizeof c->strategy,"%s",strategy);c->hooks=hooks;c->service=(dunlin_context_projector_v1){c,project};return c;
}
const dunlin_context_projector_v1 *dc_compactor_service(dc_compactor *c){return c?&c->service:NULL;}
void dc_compactor_close(dc_compactor *c){if(c){free(c->chat);free(c);}}
J *dc_compaction_request(dc_store *s,J *request){
 const char *op=str(request,"op");
 if(!op||strcmp(op,"context.records")||!str(request,"chat")){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(0));json_object_object_add(r,"error",json_object_new_string("unsupported_context_operation"));return r;}
 const char *chat=str(request,"chat");sqlite3_stmt *q=prep(s,"SELECT id,source_hash,strategy,source_count,before_bytes,after_bytes,measure_kind,status,error,created FROM compaction_records WHERE chat=? ORDER BY id DESC LIMIT 100");J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(q!=NULL));if(!q)return r;bind(q,1,chat);J *a=json_object_new_array();int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){J *v=json_object_new_object();const char *keys[]={"id","sourceHash","strategy","sourceCount","beforeBytes","afterBytes","measureKind","status","error","created"};for(int i=0;i<10;i++)json_object_object_add(v,keys[i],i==0||(i>=3&&i<=5)||i==9?json_object_new_int64(sqlite3_column_int64(q,i)):json_object_new_string((const char*)sqlite3_column_text(q,i)));json_object_array_add(a,v);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);json_object_object_add(r,"ok",json_object_new_boolean(0));}else json_object_object_add(r,"data",a);return r;
}
