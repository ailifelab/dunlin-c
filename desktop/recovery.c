#define _POSIX_C_SOURCE 200809L
#include "recovery.h"
#include "trajectory.h"
#include "tool_records.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/agent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int valid(const char *p,size_t cap){return p&&*p&&strlen(p)<=cap&&ah_utf8_text(p,strlen(p));}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *query){return sqlite3_exec(s->db,query,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(o,"error",json_object_new_string(error));if(data)json_object_object_add(o,"data",data);return o;}
static J *fail(const char *e){return reply(0,e,NULL);}
int dc_recovery_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS abandoned_effects(run TEXT NOT NULL,call_id TEXT NOT NULL,chat TEXT NOT NULL,args_hash TEXT NOT NULL,state TEXT NOT NULL CHECK(state='unknown'),created INTEGER NOT NULL DEFAULT(unixepoch()),PRIMARY KEY(run,call_id));"
 "CREATE TABLE IF NOT EXISTS conversation_events(id INTEGER PRIMARY KEY,chat TEXT NOT NULL,run TEXT NOT NULL,type TEXT NOT NULL,body TEXT NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()));CREATE INDEX IF NOT EXISTS conversation_event_chat ON conversation_events(chat,id);");}
static J *state(dc_store *s,const char *id,int *found){
 sqlite3_stmt *q=prep(s,"SELECT state FROM chats WHERE id=?");if(q)bind(q,1,id);J *out=NULL;*found=0;if(q&&sqlite3_step(q)==SQLITE_ROW){*found=1;const char *raw=(const char*)sqlite3_column_text(q,0);if(raw&&strlen(raw)<=8388608)out=dunlin_json_parse(raw,strlen(raw));if(raw&&!out)*found=2;}sqlite3_finalize(q);return out;
}
static J *inspect(dc_store *s,const char *id){
 int found=0;J *checkpoint=state(s,id,&found);if(!found){json_object_put(checkpoint);return fail("chat_not_found");}if(found==2){json_object_put(checkpoint);return fail("invalid_recovery_checkpoint");}J *o=json_object_new_object();const char *raw=checkpoint?json_object_to_json_string_ext(checkpoint,JSON_C_TO_STRING_PLAIN):"null";char hash[65];dc_sha256(raw,strlen(raw),hash);json_object_object_add(o,"stateHash",json_object_new_string(hash));json_object_object_add(o,"active",json_object_new_boolean(json_object_get_boolean(get(checkpoint,"active"))));json_object_object_add(o,"run",json_object_new_string(str(checkpoint,"_dunlin_run")?str(checkpoint,"_dunlin_run"):""));json_object_object_add(o,"profile",json_object_new_string(str(checkpoint,"_dunlin_profile")?str(checkpoint,"_dunlin_profile"):""));json_object_object_add(o,"pending",json_object_new_string(str(checkpoint,"pending")?str(checkpoint,"pending"):""));json_object_object_add(o,"arguments",json_object_new_string(str(checkpoint,"pending_arguments")?str(checkpoint,"pending_arguments"):""));json_object_object_add(o,"detail",json_object_new_string(str(checkpoint,"detail")?str(checkpoint,"detail"):""));sqlite3_stmt *q=prep(s,"SELECT count(*) FROM abandoned_effects WHERE chat=?");if(q)bind(q,1,id);int64_t unknown=q&&sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int64(q,0):-1;sqlite3_finalize(q);json_object_object_add(o,"unknownCount",json_object_new_int64(unknown));json_object_put(checkpoint);return reply(1,NULL,o);
}
static int close_batch(J *checkpoint,J *messages,const char *pending){
 size_t n=json_object_array_length(messages),batch_at=SIZE_MAX;J *calls=NULL;
 for(size_t i=n;i>0;i--){J *m=json_object_array_get_idx(messages,i-1);if(eq(str(m,"role"),"assistant")){calls=get(m,"tool_calls");if(calls)batch_at=i-1;break;}if(eq(str(m,"role"),"user"))break;}
 if(!calls)return pending?0:1;if(!json_object_is_type(calls,json_type_array)||json_object_array_length(calls)>64)return 0;
 size_t count=json_object_array_length(calls),done=n-batch_at-1;if(done>count)return 0;
 for(size_t i=0;i<done;i++){J *m=json_object_array_get_idx(messages,batch_at+1+i),*call=json_object_array_get_idx(calls,i);if(!eq(str(m,"role"),"tool")||!eq(str(m,"tool_call_id"),str(call,"id")))return 0;}
 int pending_found=!pending;for(size_t i=done;i<count;i++){J *call=json_object_array_get_idx(calls,i);const char *id=str(call,"id");if(!valid(id,256))return 0;int unknown=pending&&eq(pending,id);if(pending&&i==done&&!unknown)return 0;if(unknown)pending_found=1;J *m=json_object_new_object();json_object_object_add(m,"role",json_object_new_string("tool"));json_object_object_add(m,"tool_call_id",json_object_new_string(id));json_object_object_add(m,"content",json_object_new_string(unknown?"UNKNOWN external effect. The user ended this run; success, failure and rollback are not established. Do not replay this old invocation.":"NOT EXECUTED: the user ended this run before this invocation."));json_object_object_add(m,"_dunlin_observation",json_object_new_string(unknown?"unknown":"not_executed"));json_object_array_add(messages,m);}
 (void)checkpoint;return pending_found;
}
static J *abandon(dc_store *s,J *r,const char *id){
 if(!sql(s,"BEGIN IMMEDIATE"))return fail("storage_error");int found=0;J *checkpoint=state(s,id,&found),*messages=get(checkpoint,"messages");const char *error="invalid_recovery_checkpoint";
 if(!found||!json_object_is_type(get(checkpoint,"active"),json_type_boolean)||!json_object_is_type(checkpoint,json_type_object)||!json_object_is_type(messages,json_type_array)||json_object_array_length(messages)>8192)goto failed;
 if(!json_object_get_boolean(get(checkpoint,"active"))&&!get(checkpoint,"pending")){sql(s,"ROLLBACK");json_object_put(checkpoint);return inspect(s,id);}
 const char *raw=json_object_to_json_string_ext(checkpoint,JSON_C_TO_STRING_PLAIN);char hash[65];dc_sha256(raw,strlen(raw),hash);error="recovery_state_changed";if(!eq(hash,str(r,"stateHash")))goto failed;
 const char *pending=str(checkpoint,"pending"),*args=str(checkpoint,"pending_arguments"),*run=str(checkpoint,"_dunlin_run");int has_unknown=pending!=NULL;char run_id[65];if(run&&strlen(run)==64)snprintf(run_id,sizeof run_id,"%s",run);else snprintf(run_id,sizeof run_id,"%s",hash);
 error="unknown_acknowledgement_required";if(pending&&!json_object_get_boolean(get(r,"acknowledgeUnknown")))goto failed;
 error="invalid_recovery_pairing";size_t before=json_object_array_length(messages);if(!close_batch(checkpoint,messages,pending))goto failed;
 if(pending){if(!args){error="missing_pending_arguments";goto failed;}char args_hash[65];dc_sha256(args,strlen(args),args_hash);sqlite3_stmt *q=prep(s,"INSERT INTO abandoned_effects(run,call_id,chat,args_hash,state) VALUES(?,?,?,?,'unknown') ON CONFLICT(run,call_id) DO UPDATE SET args_hash=excluded.args_hash WHERE abandoned_effects.args_hash=excluded.args_hash");if(q){bind(q,1,run_id);bind(q,2,pending);bind(q,3,id);bind(q,4,args_hash);}if(!step(q)||sqlite3_changes(s->db)!=1){error="unknown_audit_unavailable";goto failed;}}
 json_object_object_add(checkpoint,"active",json_object_new_boolean(0));json_object_object_add(checkpoint,"status",json_object_new_int(has_unknown?AH_UNKNOWN:AH_CANCELLED));json_object_object_add(checkpoint,"detail",json_object_new_string(has_unknown?"User ended the run; unknown external effect retained.":"User ended the run before further execution."));json_object_object_add(checkpoint,"_dunlin_abandoned",json_object_new_boolean(1));json_object_object_del(checkpoint,"pending");json_object_object_del(checkpoint,"pending_arguments");json_object_object_del(checkpoint,"sealed");
 sqlite3_stmt *q=prep(s,"UPDATE chats SET state=?,updated=unixepoch() WHERE id=?");if(q){bind(q,1,json_object_to_json_string_ext(checkpoint,JSON_C_TO_STRING_PLAIN));bind(q,2,id);}if(!step(q)){error="storage_error";goto failed;}
 for(size_t i=before;i<json_object_array_length(messages);i++){J *m=json_object_array_get_idx(messages,i);q=prep(s,"INSERT OR REPLACE INTO index_jobs(chat,seq,text) VALUES(?,?,?)");if(q){bind(q,1,id);sqlite3_bind_int64(q,2,(int64_t)i);bind(q,3,str(m,"content"));}if(!step(q)){error="storage_error";goto failed;}}
 if(!dc_tool_records_append(s,id,run_id,checkpoint,before,NULL)){error="storage_error";goto failed;}
 q=prep(s,"UPDATE run_records SET status=?,finished=unixepoch() WHERE id=? AND chat=?");if(q){bind(q,1,has_unknown?"abandoned_unknown":"abandoned");bind(q,2,run_id);bind(q,3,id);}if(!step(q)){error="storage_error";goto failed;}
 q=prep(s,"INSERT INTO conversation_events(chat,run,type,body) VALUES(?,?,'run.abandoned',?)");if(q){bind(q,1,id);bind(q,2,run_id);bind(q,3,has_unknown?"unknown_external_effect_retained":"no_pending_effect");}if(!step(q)||!sql(s,"COMMIT")){error="storage_error";goto failed;}
 dl_trace_span *span=dc_trajectory_begin(s,id,run_id,"宿主恢复","host",(long long)before,NULL);const char *observation=has_unknown?"{\"unknown_effect_retained\":true,\"old_invocation_replayed\":false}":"{\"unknown_effect_retained\":false,\"old_invocation_replayed\":false}";dl_trajectory_event(span,"recovery.abandoned",observation,strlen(observation));dl_trajectory_finish(span,has_unknown?"abandoned_unknown":"abandoned",(long long)json_object_array_length(messages));
 json_object_put(checkpoint);return inspect(s,id);
 failed:sql(s,"ROLLBACK");json_object_put(checkpoint);return fail(error);
}
J *dc_recovery_request(dc_store *s,J *r){const char *op=str(r,"op"),*id=str(r,"chat");if(!valid(id,128))return fail("invalid_chat_id");if(eq(op,"recovery.inspect"))return inspect(s,id);if(eq(op,"recovery.abandon"))return abandon(s,r,id);return fail("unsupported_recovery_operation");}
