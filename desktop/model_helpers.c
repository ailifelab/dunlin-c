#define _POSIX_C_SOURCE 200809L
#include "model_helpers.h"
#include "config_registry.h"
#include "secret_transport.h"
#include "dunlin/compact/protocol.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/agent.h"
#include "dunlin/content_hash.h"
#include "dunlin/context_budget.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(json_object_is_type(o,json_type_object))json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);const char *s=json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;return s&&strlen(s)==(size_t)json_object_get_string_len(v)?s:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static void text(J *o,const char *k,const char *s){json_object_object_add(o,k,json_object_new_string(s?s:""));}
static J *reply(J *data,const char *error){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(!error));if(data)json_object_object_add(r,"data",data);if(error)text(r,"error",error);return r;}
static sqlite3_stmt *prep(dc_store *s,const char *v){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,v,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *v){return sqlite3_exec(s->db,v,NULL,NULL,NULL)==SQLITE_OK;}
static uint64_t now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static const char title_system[]="你是任务标题生成器。把用户给的任务说明总结成一条简短标题：不超过 40 个汉字（英文按词），只输出标题本身，不要引号、不要句末句号、不要解释、不要 Markdown。";
int dc_helpers_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS model_helper_plans(call TEXT PRIMARY KEY,model TEXT NOT NULL,revision INTEGER NOT NULL,config TEXT NOT NULL,input TEXT NOT NULL,session TEXT NOT NULL,effort TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'planned');"
 "CREATE TABLE IF NOT EXISTS model_helper_jobs(id TEXT PRIMARY KEY,call TEXT NOT NULL,sequence INTEGER NOT NULL,attempt INTEGER NOT NULL,tokens INTEGER NOT NULL,effort TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'queued',text TEXT NOT NULL DEFAULT '',error TEXT NOT NULL DEFAULT '',created INTEGER NOT NULL DEFAULT(unixepoch()),finished INTEGER,UNIQUE(call,attempt));"
 "UPDATE model_helper_jobs SET state='unknown',error='interrupted_helper_not_replayed',finished=unixepoch() WHERE state='running';"
 "UPDATE model_helper_jobs SET state='cancelled',error='interrupted_helper_not_admitted',finished=unixepoch() WHERE state='queued';UPDATE model_helper_plans SET state='closed';");}

/* JS trim() whitespace and UTF-16 slice(0,12000), retaining valid UTF-8.
 * A split surrogate boundary is refused rather than replacing task meaning. */
static unsigned codepoint(const unsigned char *p,size_t *n){unsigned c=*p;*n=c<128?1:c<224?2:c<240?3:4;if(*n==1)return c;unsigned v=c&((1u<<(7-*n))-1);for(size_t i=1;i<*n;i++)v=(v<<6)|(p[i]&63);return v;}
static int whitespace(unsigned c){return c==9||c==10||c==11||c==12||c==13||c==32||c==160||c==0x1680||(c>=0x2000&&c<=0x200a)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000||c==0xfeff;}
static char *task_prefix(const char *s){
 if(!s||strlen(s)>131072||!ah_utf8_text(s,strlen(s)))return NULL;
 const unsigned char *p=(const unsigned char*)s,*first=NULL,*end=NULL;
 while(*p){size_t n;unsigned c=codepoint(p,&n);if(!whitespace(c)){if(!first)first=p;end=p+n;}p+=n;}
 if(!first)return NULL;p=first;unsigned units=0;while(p<end){size_t n;unsigned c=codepoint(p,&n);unsigned add=c>0xffff?2:1;if(units+add>12000){if(units==11999&&add==2)return NULL;break;}units+=add;p+=n;}
 size_t length=(size_t)(p-first);char *out=malloc(length+1);if(out){memcpy(out,first,length);out[length]=0;}return out;
}
static J *settings(dc_store *s,const char *model,int64_t revision){J *q=json_object_new_object();text(q,"op","config.model");text(q,"id",model);json_object_object_add(q,"revision",json_object_new_int64(revision));J *r=dc_config_request(s,q);json_object_put(q);J *v=json_object_get_boolean(get(r,"ok"))?json_object_get(get(r,"data")):NULL;json_object_put(r);return v;}
static unsigned capabilities(J *cfg){unsigned bits=0;J *a=get(cfg,"reasoning_capabilities");if(!json_object_is_type(a,json_type_array))return 0;for(size_t i=0;i<json_object_array_length(a);i++){const char *v=json_object_get_string(json_object_array_get_idx(a,i));bits|=eq(v,"none")?1:eq(v,"levels")?2:eq(v,"xhigh")?4:0;}return bits;}
static int effort_valid(J *cfg,const char *effort){unsigned bits=capabilities(cfg);return !effort||!*effort||eq(effort,"default")||(eq(effort,"none")&&(bits&1))||((eq(effort,"low")||eq(effort,"medium")||eq(effort,"high"))&&(bits&2))||(eq(effort,"xhigh")&&(bits&4)&&(eq(str(cfg,"protocol"),"chat")||eq(str(cfg,"protocol"),"responses")));}
static int context_fits(dc_store *s,J *cfg,const char *id,int64_t revision,const char *input){
 sqlite3_stmt *q=prep(s,"SELECT models FROM configuration_registry WHERE singleton=1 AND revision=?");if(q)sqlite3_bind_int64(q,1,revision);J *models=NULL;if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);if(raw)models=dunlin_json_parse(raw,strlen(raw));}sqlite3_finalize(q);if(!json_object_is_type(models,json_type_array)){json_object_put(models);return 0;}
 J *profile=NULL;for(size_t i=0;i<json_object_array_length(models);i++)if(eq(str(json_object_array_get_idx(models,i),"id"),id)){profile=json_object_array_get_idx(models,i);break;}if(!profile){json_object_put(models);return 0;}
 dc_context_capabilities caps={.model_limit=(uint64_t)json_object_get_int64(get(profile,"contextLimit")),.deployment_limit=(uint64_t)json_object_get_int64(get(profile,"deploymentLimit")),.selected_limit=(uint64_t)json_object_get_int64(get(profile,"contextBudget")),.client_limit=4194304,.output_limit=65536,.requested_output=(uint64_t)json_object_get_int64(get(cfg,"max_tokens")),.overhead_reserve=get(profile,"contextReserve")?(uint64_t)json_object_get_int64(get(profile,"contextReserve")):512};
 const char *limits[]={"outputLimit","advertisedOutputLimit"};for(size_t i=0;i<2;i++)if(get(profile,limits[i])&&(uint64_t)json_object_get_int64(get(profile,limits[i]))<caps.output_limit)caps.output_limit=(uint64_t)json_object_get_int64(get(profile,limits[i]));
 dc_context_range range;dc_context_status status=dc_context_budget(&caps,&range);int fits=status!=DC_CONTEXT_INVALID;
 /* This is a conservative byte estimate, not a tokenizer/provider guarantee. */
 size_t estimate=strlen(title_system)+strlen(input)+16+32;if(status==DC_CONTEXT_VALID){fits=estimate<=range.input_budget;json_object_object_add(cfg,"helper_input_budget",json_object_new_int64((int64_t)range.input_budget));}json_object_put(models);return fits;
}
J *dc_helpers_plan(dc_store *s,const char *call,J *request){
 J *args=get(request,"arguments");const char *url=str(args,"url");
 if(!eq(str(request,"package"),"dsh-project-manager")||!eq(str(request,"command"),"http")||!eq(str(args,"method"),"POST")||!eq(url,"/_dsh/dsh-project-manager/api/goal/title"))return reply(json_object_new_boolean(0),NULL);
 J *body=get(args,"body"),*route=get(body,"route");const char *model=str(route,"model"),*effort=str(route,"effort"),*session=str(body,"session"),*chat=str(request,"chat");char *input=task_prefix(str(body,"text"));
 if(!input||!chat||!*chat||!eq(str(route,"provider"),"dunlin-native")||!model||!*model||strlen(model)>64||(session&&*session&&!eq(session,chat))){free(input);return reply(NULL,"native_helper_request_invalid");}
 sqlite3_stmt *q=prep(s,"SELECT revision FROM configuration_registry WHERE singleton=1");int64_t revision=q&&sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int64(q,0):-1;sqlite3_finalize(q);J *cfg=settings(s,model,revision);
 if(!cfg||!effort_valid(cfg,effort)){free(input);json_object_put(cfg);return reply(NULL,"native_helper_model_or_effort_invalid");}
 if(!context_fits(s,cfg,model,revision,input)){free(input);json_object_put(cfg);return reply(NULL,"native_helper_context_budget_exceeded");}
 /* The original helper supplies its own system prompt. Never borrow a user's
  * coding prompt, conversation, tools or knowledge projection. */
 json_object_object_del(cfg,"system_prompt");
 q=prep(s,"INSERT INTO model_helper_plans(call,model,revision,config,input,session,effort) VALUES(?,?,?,?,?,?,?)");if(q){bind(q,1,call);bind(q,2,model);sqlite3_bind_int64(q,3,revision);bind(q,4,json_object_to_json_string_ext(cfg,JSON_C_TO_STRING_PLAIN));bind(q,5,input);bind(q,6,session);bind(q,7,effort);}int ok=step(q);free(input);json_object_put(cfg);return reply(ok?json_object_new_boolean(1):NULL,ok?NULL:"native_helper_plan_audit_unavailable");
}
int dc_helpers_handoff(dc_store *s,const char *call){sqlite3_stmt *q=prep(s,"UPDATE model_helper_plans SET state='handed_off' WHERE call=? AND state='planned' AND EXISTS(SELECT 1 FROM extension_requests r JOIN extension_calls c ON c.id=r.call WHERE r.call=model_helper_plans.call AND r.state='running' AND c.status='running')");if(q)bind(q,1,call);return step(q);}
int dc_helpers_cancel_call(dc_store *s,const char *call){sqlite3_stmt *q=prep(s,"UPDATE model_helper_jobs SET state=CASE WHEN state='queued' THEN 'cancelled' ELSE 'cancel_requested' END,error='native_helper_lease_closed' WHERE call=? AND state IN('queued','running')");if(q)bind(q,1,call);return step(q);}

/* Read from the independent execution connection without taking the optional
 * worker mutex. Current durable owner/lease/revision are the authority. */
static int eligible(dc_store *s,const char *call,int handoff){
 sqlite3_stmt *q=prep(s,"SELECT 1 FROM model_helper_plans p JOIN extension_calls c ON c.id=p.call JOIN extension_native_scopes n ON n.call=c.id JOIN extension_bindings b ON b.source=c.source AND b.package=c.package JOIN extension_versions v ON v.source=b.source AND v.package=b.package AND v.version=b.version JOIN configuration_registry cfg ON cfg.singleton=1 JOIN chat_properties chat ON chat.chat=n.chat WHERE p.call=? AND c.status='running' AND b.enabled=1 AND v.quarantined=0 AND b.version=c.version AND b.runtime_instance=n.instance AND cfg.revision=p.revision AND chat.archived=0 AND (?=0 OR (p.state='handed_off' AND EXISTS(SELECT 1 FROM extension_requests r WHERE r.call=p.call AND r.state='running'))) AND (?2=0 OR NOT EXISTS(SELECT 1 FROM extension_native_effects e WHERE e.call=p.call AND e.status='running'))");if(q){bind(q,1,call);sqlite3_bind_int(q,2,handoff);}int valid=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);return valid;
}
static J *catalog(dc_store *s){
 sqlite3_stmt *q=prep(s,"SELECT models FROM configuration_registry WHERE singleton=1");J *models=NULL;if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);if(raw)models=dunlin_json_parse(raw,strlen(raw));}sqlite3_finalize(q);if(!json_object_is_type(models,json_type_array)){json_object_put(models);return reply(NULL,"native_model_catalog_unavailable");}
 J *rows=json_object_new_array();for(size_t i=0;i<json_object_array_length(models);i++){J *m=json_object_array_get_idx(models,i),*v=json_object_new_object();text(v,"id",str(m,"id"));text(v,"name",str(m,"name"));J *declared=get(m,"reasoningCapabilities");if(json_object_is_type(declared,json_type_array)){J *supported=json_object_new_array();for(size_t n=0;n<json_object_array_length(declared);n++){J *cap=json_object_array_get_idx(declared,n);const char *name=json_object_get_string(cap);if(!eq(name,"xhigh")||eq(str(m,"protocolName"),"chat")||eq(str(m,"protocolName"),"responses"))json_object_array_add(supported,json_object_get(cap));}json_object_object_add(v,"capabilities",supported);}if(get(m,"reasoningEffort"))json_object_object_add(v,"defaultEffort",json_object_get(get(m,"reasoningEffort")));json_object_array_add(rows,v);}json_object_put(models);return reply(rows,NULL);
}
J *dc_helpers_native(dc_store *s,const char *method,J *args,const char *call){
 if(eq(method,"helper.models"))return catalog(s);
 if(eq(method,"helper.submit")){
  if(!eligible(s,call,0)||!eq(str(args,"purpose"),"dsh-project-manager:task-title")||!eq(str(args,"system"),title_system)||!eq(str(args,"provider"),"dunlin-native"))return reply(NULL,"native_helper_scope_invalid");
  sqlite3_stmt *q=prep(s,"SELECT model,config,input,session,effort FROM model_helper_plans WHERE call=? AND state IN('planned','handed_off')");if(q)bind(q,1,call);J *cfg=NULL;char *input=NULL,*wanted_effort=NULL;int valid=q&&sqlite3_step(q)==SQLITE_ROW;
  if(valid){const char *raw=(const char*)sqlite3_column_text(q,1),*task=(const char*)sqlite3_column_text(q,2),*session=(const char*)sqlite3_column_text(q,3),*eff=(const char*)sqlite3_column_text(q,4);valid=eq(str(args,"model"),(const char*)sqlite3_column_text(q,0))&&eq(str(args,"session"),session);cfg=raw?dunlin_json_parse(raw,strlen(raw)):NULL;input=task?strdup(task):NULL;wanted_effort=eff?strdup(eff):NULL;}sqlite3_finalize(q);
  const char *provided=str(args,"input"),*effort=str(args,"effort");valid=valid&&cfg&&input&&wanted_effort&&provided&&!strncmp(provided,"任务说明：\n",16)&&eq(provided+16,input)&&effort_valid(cfg,effort);
  q=prep(s,"SELECT count(*),COALESCE(max(attempt),0),COALESCE(sum(state='reported'),0) FROM model_helper_jobs WHERE call=?");if(q)bind(q,1,call);int attempts=0,settled=0;if(q&&sqlite3_step(q)==SQLITE_ROW){attempts=sqlite3_column_int(q,0);settled=sqlite3_column_int(q,2);}else valid=0;sqlite3_finalize(q);
  int tokens=json_object_get_int(get(args,"maxTokens"));valid=valid&&json_object_is_type(get(args,"maxTokens"),json_type_int)&&((attempts==0&&tokens==400&&eq(effort,wanted_effort))||(attempts==1&&settled==1&&tokens==1600&&(eq(effort,wanted_effort)||((capabilities(cfg)&1)&&eq(effort,"none")))));
  free(input);free(wanted_effort);int limit=json_object_get_int(get(cfg,"max_tokens"));if(limit>0&&tokens>limit)tokens=limit;json_object_put(cfg);if(!valid||tokens<1)return reply(NULL,"native_helper_request_mismatch");
  q=prep(s,"SELECT count(*) FROM model_helper_jobs WHERE state IN('queued','running','cancel_requested')");int quota=q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_int64(q,0)<64;sqlite3_finalize(q);if(!quota)return reply(NULL,"native_helper_queue_full");
  q=prep(s,"SELECT lower(hex(randomblob(16)))");char id[33]="";if(q&&sqlite3_step(q)==SQLITE_ROW)snprintf(id,sizeof id,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);if(!*id)return reply(NULL,"native_helper_storage_error");
  q=prep(s,"INSERT INTO model_helper_jobs(id,call,sequence,attempt,tokens,effort) VALUES(?,?,?,?,?,?)");if(q){bind(q,1,id);bind(q,2,call);sqlite3_bind_int64(q,3,json_object_get_int64(get(args,"_sequence")));sqlite3_bind_int(q,4,attempts+1);sqlite3_bind_int(q,5,tokens);bind(q,6,effort);}if(!step(q))return reply(NULL,"native_helper_submit_audit_unavailable");J *v=json_object_new_object();text(v,"id",id);return reply(v,NULL);
 }
 if(eq(method,"helper.poll")||eq(method,"helper.cancel")){
  const char *id=str(args,"id");if(!id||strlen(id)!=32)return reply(NULL,"native_helper_id_invalid");
  sqlite3_stmt *q=prep(s,"SELECT state,text,error FROM model_helper_jobs WHERE id=? AND call=?");if(q){bind(q,1,id);bind(q,2,call);}if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return reply(NULL,"native_helper_owner_invalid");}J *v=json_object_new_object();text(v,"state",(const char*)sqlite3_column_text(q,0));text(v,"text",(const char*)sqlite3_column_text(q,1));text(v,"error",(const char*)sqlite3_column_text(q,2));sqlite3_finalize(q);
  if(eq(method,"helper.cancel")){q=prep(s,"UPDATE model_helper_jobs SET state=CASE state WHEN 'queued' THEN 'cancelled' ELSE 'cancel_requested' END,error='helper_cancelled' WHERE id=? AND call=? AND state IN('queued','running')");if(q){bind(q,1,id);bind(q,2,call);}if(!step(q)){json_object_put(v);return reply(NULL,"native_helper_cancel_audit_unavailable");}text(v,"state","cancelled");}
  return reply(v,NULL);
 }
 return reply(NULL,"native_helper_method_unavailable");
}
unsigned dc_helpers_running(void *raw){dc_store *s=raw;sqlite3_stmt *q=prep(s,"SELECT count(*) FROM model_helper_jobs WHERE state IN('running','cancel_requested')");unsigned n=q&&sqlite3_step(q)==SQLITE_ROW?(unsigned)sqlite3_column_int(q,0):2;sqlite3_finalize(q);return n;}
int dc_helpers_chat_busy(dc_store *s,const char *chat){sqlite3_stmt *q=prep(s,"SELECT 1 FROM model_helper_jobs j JOIN extension_native_scopes n ON n.call=j.call WHERE n.chat=? AND j.state IN('queued','running','cancel_requested') LIMIT 1");if(q)bind(q,1,chat);int rc=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);return rc!=SQLITE_DONE;}
int dc_helpers_drop_chat(dc_store *s,const char *chat){
 if(sqlite3_get_autocommit(s->db)||dc_helpers_chat_busy(s,chat))return 0;
 const char *queries[]={"DELETE FROM model_helper_jobs WHERE call IN(SELECT call FROM extension_native_scopes WHERE chat=?)","DELETE FROM model_helper_plans WHERE call IN(SELECT call FROM extension_native_scopes WHERE chat=?)"};
 for(size_t i=0;i<2;i++){sqlite3_stmt *q=prep(s,queries[i]);if(q)bind(q,1,chat);if(!step(q))return 0;}return 1;
}
int dc_helper_cancel(dc_store *s,const char *id,long long revision){if(!s||!id||strlen(id)!=32)return 0;sqlite3_stmt *q=prep(s,"UPDATE model_helper_jobs SET state=CASE WHEN state='queued' THEN 'cancelled' ELSE 'cancel_requested' END,error='native_helper_cancelled' WHERE id=? AND state IN('queued','running') AND call IN(SELECT call FROM model_helper_plans WHERE revision=?)");if(q){bind(q,1,id);sqlite3_bind_int64(q,2,revision);}return step(q);}
J *dc_helpers_pending(dc_store *s){
 sqlite3_stmt *capacity=prep(s,"SELECT count(*) FROM agent_jobs WHERE state IN('running','cancel_requested')");int running=capacity&&sqlite3_step(capacity)==SQLITE_ROW?sqlite3_column_int(capacity,0):2;sqlite3_finalize(capacity);if(running+(int)dc_helpers_running(s)>=2)return reply(json_object_new_array(),NULL);
 sqlite3_stmt *q=prep(s,"SELECT j.id,p.model,p.revision,j.call FROM model_helper_jobs j JOIN model_helper_plans p ON p.call=j.call JOIN extension_native_effects e ON e.call=j.call AND e.sequence=j.sequence AND e.method='helper.submit' AND e.status='reported' WHERE j.state='queued' ORDER BY j.rowid LIMIT 64");J *rows=json_object_new_array();int rc=SQLITE_ERROR;while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){const char *call=(const char*)sqlite3_column_text(q,3);if(!eligible(s,call,1))continue;J *v=json_object_new_object();text(v,"id",(const char*)sqlite3_column_text(q,0));text(v,"model",(const char*)sqlite3_column_text(q,1));json_object_object_add(v,"revision",json_object_new_int64(sqlite3_column_int64(q,2)));json_object_array_add(rows,v);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(rows);return reply(NULL,"native_helper_pending_unavailable");}return reply(rows,NULL);
}

typedef struct {dc_store *store;const char *call,*id;const dc_hooks *hooks;uint64_t checked;int stopped;} observation;
static int cancelled(void *raw){observation *o=raw;uint64_t now=now_ms();if(o->stopped||(o->hooks&&o->hooks->cancelled&&o->hooks->cancelled(o->hooks->user)))return 1;if(!o->checked||now-o->checked>=100){o->checked=now;sqlite3_stmt *q=prep(o->store,"SELECT state FROM model_helper_jobs WHERE id=?");if(q)bind(q,1,o->id);o->stopped=!q||sqlite3_step(q)!=SQLITE_ROW||!eq((const char*)sqlite3_column_text(q,0),"running");sqlite3_finalize(q);if(!eligible(o->store,o->call,1))o->stopped=1;}return o->stopped;}
typedef struct {dc_secret_transport secret;J *config;} helper_transport;
int dc_helpers_uncertain(dc_store *s,const char *source,const char *package,const char *instance){sqlite3_stmt *q=prep(s,"SELECT 1 FROM model_helper_jobs j JOIN extension_calls c ON c.id=j.call JOIN extension_native_scopes n ON n.call=c.id WHERE c.source=? AND c.package=? AND n.instance=? AND j.state='unknown' LIMIT 1");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,instance);}int rc=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);return rc!=SQLITE_DONE;}
static dunlin_http_result helper_post(void *raw,const dunlin_http_request *request,long *status){helper_transport *t=raw;J *body=dunlin_json_parse(request->json,strlen(request->json));if(!json_object_is_type(body,json_type_object)){json_object_put(body);return DUNLIN_HTTP_FAILED;}if(get(t->config,"enable_thinking"))json_object_object_add(body,"enable_thinking",json_object_get(get(t->config,"enable_thinking")));J *template=get(t->config,"omlx_chat_template");if(template){J *kwargs=json_object_new_object();json_object_object_add(kwargs,"chat_template",json_object_get(template));json_object_object_add(body,"chat_template_kwargs",kwargs);}dunlin_http_request fixed=*request;fixed.json=json_object_to_json_string_ext(body,JSON_C_TO_STRING_PLAIN);dunlin_http_result result=t->secret.v1.post(t->secret.v1.user,&fixed,status);json_object_put(body);return result;}
char *dc_helper_execute(dc_store *s,const char *id,long long revision,const char *secret,const dc_hooks *hooks){
 J *result=NULL,*cfg=NULL;char *input=NULL;char call[33]="",effort[16]="";unsigned tokens=0;
 if(s&&id&&hooks&&hooks->cancelled&&hooks->cancelled(hooks->user)){dc_helper_cancel(s,id,revision);return strdup("{\"ok\":false,\"error\":\"native_helper_cancelled\"}");}
 if(!s||!id||strlen(id)!=32||!sqlite3_get_autocommit(s->db)||!sql(s,"BEGIN IMMEDIATE"))return strdup("{\"ok\":false,\"error\":\"native_helper_execution_unavailable\"}");
 sqlite3_stmt *q=prep(s,"SELECT j.call,p.config,p.input,j.tokens,j.effort FROM model_helper_jobs j JOIN model_helper_plans p ON p.call=j.call JOIN extension_native_effects e ON e.call=j.call AND e.sequence=j.sequence AND e.method='helper.submit' AND e.status='reported' WHERE j.id=? AND j.state='queued' AND p.revision=?");if(q){bind(q,1,id);sqlite3_bind_int64(q,2,revision);}int valid=q&&sqlite3_step(q)==SQLITE_ROW;
 if(valid){const char *c=(const char*)sqlite3_column_text(q,0),*raw=(const char*)sqlite3_column_text(q,1),*t=(const char*)sqlite3_column_text(q,2),*e=(const char*)sqlite3_column_text(q,4);valid=c&&strlen(c)==32&&raw&&t&&e&&strlen(e)<sizeof effort;if(valid){snprintf(call,sizeof call,"%s",c);cfg=dunlin_json_parse(raw,strlen(raw));input=strdup(t);tokens=(unsigned)sqlite3_column_int(q,3);snprintf(effort,sizeof effort,"%s",e);}}sqlite3_finalize(q);valid=valid&&cfg&&input&&eligible(s,call,1);
 q=prep(s,"SELECT count(*) FROM agent_jobs WHERE state IN('running','cancel_requested')");int running=q&&sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int(q,0):2;sqlite3_finalize(q);valid=valid&&running+(int)dc_helpers_running(s)<2&&!(hooks&&hooks->cancelled&&hooks->cancelled(hooks->user));
 helper_transport transport={.config=cfg};
 if(valid&&((!eq(str(cfg,"desktop_auth"),"none")&&(!secret||!*secret))||!dc_secret_transport_init(&transport.secret,str(cfg,"base_url"),str(cfg,"protocol"),str(cfg,"desktop_auth"),secret))){q=prep(s,"UPDATE model_helper_jobs SET state='failed',error='native_helper_credentials_unavailable',finished=unixepoch() WHERE id=? AND state='queued'");if(q)bind(q,1,id);int stored=step(q)&&sqlite3_changes(s->db)==1;if(!sql(s,stored?"COMMIT":"ROLLBACK")){sql(s,"ROLLBACK");stored=0;}result=reply(stored?json_object_new_boolean(1):NULL,stored?NULL:"native_helper_credential_failure_receipt_unknown");goto done;}
 if(valid){q=prep(s,"UPDATE model_helper_jobs SET state='running' WHERE id=? AND state='queued'");if(q)bind(q,1,id);valid=step(q)&&sqlite3_changes(s->db)==1;}
 if(!sql(s,valid?"COMMIT":"ROLLBACK")){sql(s,"ROLLBACK");valid=0;}if(!valid){result=reply(NULL,"native_helper_claim_refused");goto done;}
 observation observer={.store=s,.call=call,.id=id,.hooks=hooks};dunlin_model_events events={.user=&observer,.cancelled=cancelled};dunlin_http_v1 v1={helper_post,&transport};
 dunlin_model_config model={.base_url=str(cfg,"base_url"),.model=str(cfg,"model"),.timeout_ms=20000,.stream=1,.max_response_bytes=1048576,.http=&v1};
 const char *protocol=str(cfg,"protocol");dunlin_protocol_options options={.protocol=eq(protocol,"responses")?DUNLIN_PROTOCOL_RESPONSES:eq(protocol,"anthropic")?DUNLIN_PROTOCOL_ANTHROPIC:eq(protocol,"ollama")?DUNLIN_PROTOCOL_OLLAMA:eq(protocol,"gemini")?DUNLIN_PROTOCOL_GEMINI:DUNLIN_PROTOCOL_CHAT,.max_tokens=tokens,.http=&transport.secret.v2,.reasoning_supported=capabilities(cfg),.show_reasoning=0};
 const char *eff=*effort?effort:str(cfg,"reasoning_effort");options.reasoning_effort=eq(eff,"none")?DUNLIN_REASONING_NONE:eq(eff,"low")?DUNLIN_REASONING_LOW:eq(eff,"medium")?DUNLIN_REASONING_MEDIUM:eq(eff,"high")?DUNLIN_REASONING_HIGH:eq(eff,"xhigh")?DUNLIN_REASONING_XHIGH:DUNLIN_REASONING_DEFAULT;if(get(cfg,"temperature")){options.has_temperature=1;options.temperature=json_object_get_double(get(cfg,"temperature"));}
 J *messages=json_object_new_array(),*system=json_object_new_object(),*user=json_object_new_object();text(system,"role","system");text(system,"content",title_system);text(user,"role","user");size_t n=strlen(input)+17;char *task=malloc(n);if(task)snprintf(task,n,"任务说明：\n%s",input);text(user,"content",task);free(task);json_object_array_add(messages,system);json_object_array_add(messages,user);J *answer=NULL;char diagnostic[256]="";
 dunlin_model_status status=dunlin_protocol_proposal(&model,&options,messages,NULL,&events,&answer,diagnostic,sizeof diagnostic);json_object_put(messages);const char *output=str(answer,"content");if(status==DUNLIN_MODEL_OK&&(!output||strlen(output)>65536||get(answer,"tool_calls")))status=DUNLIN_MODEL_PROTOCOL;if(cancelled(&observer))status=DUNLIN_MODEL_CANCELLED;
 const char *state=status==DUNLIN_MODEL_OK?"reported":status==DUNLIN_MODEL_CANCELLED?"cancelled":"failed";const char *error=status==DUNLIN_MODEL_OK?"":status==DUNLIN_MODEL_CANCELLED?"native_helper_cancelled":status==DUNLIN_MODEL_TIMEOUT?"native_helper_timeout":"native_helper_provider_failed";
 q=prep(s,"UPDATE model_helper_jobs SET state=?,text=?,error=?,finished=unixepoch() WHERE id=? AND state IN('running','cancel_requested')");if(q){bind(q,1,state);bind(q,2,status==DUNLIN_MODEL_OK?output:"");bind(q,3,error);bind(q,4,id);}int stored=step(q)&&sqlite3_changes(s->db)==1;if(!stored){q=prep(s,"UPDATE model_helper_jobs SET state='unknown',text='',error='native_helper_result_receipt_unknown',finished=unixepoch() WHERE id=? AND state IN('running','cancel_requested')");if(q)bind(q,1,id);step(q);}json_object_put(answer);result=reply(stored?json_object_new_boolean(1):NULL,stored?NULL:"native_helper_result_receipt_unknown");
done:free(input);json_object_put(cfg);char *out=strdup(json_object_to_json_string_ext(result,JSON_C_TO_STRING_PLAIN));json_object_put(result);return out;
}
