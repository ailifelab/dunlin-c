#define _POSIX_C_SOURCE 200809L
#include "../../desktop/tool_records.h"
#include "dunlin/compact/json.h"
#include "dunlin/content_hash.h"
#include "../../dunlin-compact/platform/clock.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

typedef struct json_object J;
static J *get(J *o,const char *key){J *v=NULL;if(o)json_object_object_get_ex(o,key,&v);return v;}
static const char *str(J *o,const char *key){J *v=get(o,key);if(!json_object_is_type(v,json_type_string))return NULL;const char *p=json_object_get_string(v);return strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int valid(const char *s,size_t n){return s&&*s&&strlen(s)<=n&&ah_utf8_text(s,strlen(s));}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v?v:"",-1,SQLITE_TRANSIENT);}
static int done(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *v){return sqlite3_exec(s->db,v,NULL,NULL,NULL)==SQLITE_OK;}
static void add(J *v,const char *key,const char *text){json_object_object_add(v,key,json_object_new_string(text?text:""));}
static J *reply(int ok,const char *error,J *data){J *v=json_object_new_object();json_object_object_add(v,"ok",json_object_new_boolean(ok));if(error)add(v,"error",error);if(data)json_object_object_add(v,"data",data);return v;}
static J *fail(const char *error){return reply(0,error,NULL);}
int dc_tool_records_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS tool_records(id INTEGER PRIMARY KEY,chat TEXT NOT NULL,run TEXT NOT NULL,proposal INTEGER NOT NULL,call_id TEXT NOT NULL,name TEXT NOT NULL,arguments TEXT NOT NULL,result TEXT,result_sequence INTEGER,stage TEXT NOT NULL DEFAULT 'proposed',outcome TEXT,grant_id INTEGER,wait_ms INTEGER,started INTEGER,execution_ms INTEGER,committed_ms INTEGER,created INTEGER NOT NULL DEFAULT(unixepoch()),historical INTEGER NOT NULL DEFAULT 0,UNIQUE(chat,run,proposal,call_id));"
 "CREATE INDEX IF NOT EXISTS tool_records_chat ON tool_records(chat,id DESC);"
 "CREATE INDEX IF NOT EXISTS tool_records_run ON tool_records(chat,run,proposal,call_id);"
 "CREATE TABLE IF NOT EXISTS tool_record_migrations(chat TEXT PRIMARY KEY);"
 "CREATE TABLE IF NOT EXISTS tool_record_schemas(record INTEGER PRIMARY KEY,schema TEXT NOT NULL);"
 "UPDATE tool_records SET stage='interrupted' WHERE stage IN('running','returned','validating','awaiting');");}
static int call_insert(dc_store *s,const char *chat,const char *run,int64_t proposal,J *call,int historical){
 const char *id=str(call,"id"),*name=str(get(call,"function"),"name"),*arguments=str(get(call,"function"),"arguments");
 if(!valid(id,256)||!name||strlen(name)>1048576||!ah_utf8_text(name,strlen(name))||!arguments||strlen(arguments)>1048576||!ah_utf8_text(arguments,strlen(arguments)))return 0;
 if(historical){sqlite3_stmt *old=prep(s,"SELECT 1 FROM tool_records WHERE chat=? AND proposal=? AND call_id=? LIMIT 1");if(old){bind(old,1,chat);sqlite3_bind_int64(old,2,proposal);bind(old,3,id);}int rc=old?sqlite3_step(old):SQLITE_ERROR;sqlite3_finalize(old);if(rc==SQLITE_ROW)return 1;if(rc!=SQLITE_DONE)return 0;}
 sqlite3_stmt *q=prep(s,"INSERT OR IGNORE INTO tool_records(chat,run,proposal,call_id,name,arguments,historical) VALUES(?,?,?,?,?,?,?)");if(q){bind(q,1,chat);bind(q,2,run);sqlite3_bind_int64(q,3,proposal);bind(q,4,id);bind(q,5,name);bind(q,6,arguments);sqlite3_bind_int(q,7,historical);}return done(q);
}
int dc_tool_records_append(dc_store *s,const char *chat,const char *run,J *state,size_t before,dc_tool_record_run *current){
 J *messages=get(state,"messages");if(!json_object_is_type(messages,json_type_array))return 0;size_t count=json_object_array_length(messages);int historical=current==NULL;
 for(size_t i=before;i<count;i++){
  J *message=json_object_array_get_idx(messages,i);const char *role=str(message,"role");J *calls=get(message,"tool_calls");
  if(eq(role,"assistant")&&json_object_is_type(calls,json_type_array)){
   for(size_t j=0;j<json_object_array_length(calls);j++)if(!call_insert(s,chat,run,(int64_t)i,json_object_array_get_idx(calls,j),historical))return 0;
  }else if(eq(role,"tool")){
   const char *id=str(message,"tool_call_id"),*result=str(message,"content");if(!id||!result)return 0;int64_t proposal=-1;
   for(size_t j=i;j>0;j--){J *m=json_object_array_get_idx(messages,j-1);if(eq(str(m,"role"),"assistant")&&get(m,"tool_calls")){proposal=(int64_t)j-1;break;}}
   if(proposal<0)return 0;
   int matching=current&&current->proposal==proposal&&eq(current->call,id);
   const char *observation=str(message,"_dunlin_observation");const char *stage=historical?"recorded":matching&&current->returned?"committed":matching&&current->denied?"denied":"not_executed";
   if(eq(observation,"unknown"))stage="unknown";else if(eq(observation,"not_executed"))stage="not_executed";
   const char *outcome=matching&&current->returned?ah_status_name(current->outcome):matching&&current->denied?"denied":NULL;
   sqlite3_stmt *q=prep(s,"UPDATE tool_records SET result=?,result_sequence=?,stage=?,outcome=?,execution_ms=? WHERE chat=? AND run=? AND proposal=? AND call_id=? AND result_sequence IS NULL");
   if(q){bind(q,1,result);sqlite3_bind_int64(q,2,(int64_t)i);bind(q,3,stage);if(outcome)bind(q,4,outcome);else sqlite3_bind_null(q,4);if(matching&&current->returned&&current->started_ms)sqlite3_bind_int64(q,5,(int64_t)(current->returned_ms-current->started_ms));else sqlite3_bind_null(q,5);bind(q,6,chat);bind(q,7,run);sqlite3_bind_int64(q,8,proposal);bind(q,9,id);}if(!done(q))return 0;
   /* A resume may append a pair to the original run's already persisted call. */
   if(sqlite3_changes(s->db)==0){q=prep(s,"UPDATE tool_records SET result=?,result_sequence=?,stage=?,outcome=? WHERE chat=? AND proposal=? AND call_id=? AND result_sequence IS NULL");if(q){bind(q,1,result);sqlite3_bind_int64(q,2,(int64_t)i);bind(q,3,stage);if(outcome)bind(q,4,outcome);else sqlite3_bind_null(q,4);bind(q,5,chat);sqlite3_bind_int64(q,6,proposal);bind(q,7,id);}if(!done(q))return 0;}
  }
 }
 return 1;
}
int dc_tool_records_import(dc_store *s){
 sqlite3_stmt *q=prep(s,"SELECT id,state FROM chats WHERE state IS NOT NULL AND id NOT IN(SELECT chat FROM tool_record_migrations)");if(!q)return 0;int rc,ok=sql(s,"BEGIN IMMEDIATE");
 while(ok&&(rc=sqlite3_step(q))==SQLITE_ROW){const char *chat=(const char*)sqlite3_column_text(q,0),*raw=(const char*)sqlite3_column_text(q,1);J *state=dunlin_json_parse(raw,(size_t)sqlite3_column_bytes(q,1));if(!state){ok=0;break;}ok=dc_tool_records_append(s,chat,"historical",state,0,NULL);json_object_put(state);if(ok){sqlite3_stmt *mark=prep(s,"INSERT INTO tool_record_migrations(chat) VALUES(?)");if(mark)bind(mark,1,chat);ok=done(mark);}}
 if(ok&&rc!=SQLITE_DONE)ok=0;sqlite3_finalize(q);if(!ok||!sql(s,"COMMIT")){sql(s,"ROLLBACK");ok=0;}return ok;
}
static ah_status pre_execute(void *ctx,const char *id,const char *name,const J *args,J **replacement,char *error,size_t cap){
 dc_tool_record_run *r=ctx;r->record=0;r->returned=0;r->denied=0;r->started_ms=r->returned_ms=0;r->call[0]=0;
 if(!valid(id,256)){snprintf(error,cap,"invalid recorded tool ID");return AH_INVALID_ARGUMENTS;}
 snprintf(r->call,sizeof r->call,"%s",id);
 sqlite3_stmt *q=prep(r->store,"SELECT id,proposal FROM tool_records WHERE chat=? AND call_id=? AND name=? AND result_sequence IS NULL ORDER BY proposal DESC LIMIT 1");if(q){bind(q,1,r->chat);bind(q,2,id);bind(q,3,name);}if(q&&sqlite3_step(q)==SQLITE_ROW){r->record=sqlite3_column_int64(q,0);r->proposal=sqlite3_column_int64(q,1);}sqlite3_finalize(q);
 if(!r->record){snprintf(error,cap,"cannot locate durable tool proposal");return AH_FAILED;}
 if(r->tools&&r->tools->schema){
  J *schemas=r->tools->schema(r->tools->ctx),*schema=NULL;
  if(json_object_is_type(schemas,json_type_array))for(size_t i=0;i<json_object_array_length(schemas);i++){J *v=json_object_array_get_idx(schemas,i);if(eq(str(get(v,"function"),"name"),name)){schema=v;break;}}
  int saved=1;if(schema){const char *raw=json_object_to_json_string_ext(schema,JSON_C_TO_STRING_PLAIN);saved=strlen(raw)<=262144;if(saved){q=prep(r->store,"INSERT OR IGNORE INTO tool_record_schemas(record,schema) VALUES(?,?)");if(q){sqlite3_bind_int64(q,1,r->record);bind(q,2,raw);}saved=done(q);}}
  json_object_put(schemas);if(!saved){snprintf(error,cap,"cannot save recorded tool schema");return AH_FAILED;}
 }
 q=prep(r->store,"UPDATE tool_records SET stage='validating',run=CASE WHEN historical<>0 THEN ? ELSE run END WHERE id=? AND result_sequence IS NULL");if(q){bind(q,1,r->run);sqlite3_bind_int64(q,2,r->record);}if(!done(q)){snprintf(error,cap,"cannot save tool validation record");return AH_FAILED;}
 return r->inner&&r->inner->pre_execute?r->inner->pre_execute(r->inner->ctx,id,name,args,replacement,error,cap):AH_DONE;
}
static ah_status post_execute(void *ctx,const char *id,const char *name,ah_status outcome,const char *output,char **context){
 dc_tool_record_run *r=ctx;r->returned=1;r->outcome=outcome;r->returned_ms=ah_os_now_ms(NULL);
 return r->inner&&r->inner->post_execute?r->inner->post_execute(r->inner->ctx,id,name,outcome,output,context):AH_DONE;
}
static void finalized(void *ctx,const char *id,const char *name,ah_status outcome){
 dc_tool_record_run *r=ctx;if(r->record&&r->started_ms){sqlite3_stmt *q=prep(r->store,"UPDATE tool_records SET committed_ms=? WHERE id=? AND result_sequence IS NOT NULL");if(q){sqlite3_bind_int64(q,1,(int64_t)(ah_os_now_ms(NULL)-r->started_ms));sqlite3_bind_int64(q,2,r->record);}done(q);}
 if(r->inner&&r->inner->finalized)r->inner->finalized(r->inner->ctx,id,name,outcome);
}
ah_tool_pipeline_v1 dc_tool_records_pipeline(dc_tool_record_run *r){return (ah_tool_pipeline_v1){r,pre_execute,post_execute,finalized};}
void dc_tool_records_event(dc_tool_record_run *r,const char *event,const char *id,size_t n){
 if(!r||!r->record||strlen(r->call)!=n||memcmp(r->call,id,n))return;
 if(eq(event,"tool.start")){
  r->started_ms=ah_os_now_ms(NULL);sqlite3_stmt *q=prep(r->store,"UPDATE tool_records SET stage='running',started=unixepoch() WHERE id=? AND result_sequence IS NULL");if(q)sqlite3_bind_int64(q,1,r->record);done(q);
 }else if(eq(event,"tool.outcome_unknown")){sqlite3_stmt *q=prep(r->store,"UPDATE tool_records SET stage='unknown' WHERE id=? AND result_sequence IS NULL");if(q)sqlite3_bind_int64(q,1,r->record);done(q);}
}
int dc_tool_records_approval(dc_tool_record_run *r,long long grant,int decision,unsigned long long wait){
 if(!r||!r->record)return 0;r->denied=decision==0;
 sqlite3_stmt *q=prep(r->store,decision<0?"UPDATE tool_records SET grant_id=?,stage='awaiting' WHERE id=? AND result_sequence IS NULL":"UPDATE tool_records SET grant_id=?,wait_ms=?,stage=? WHERE id=? AND result_sequence IS NULL");
 if(q){sqlite3_bind_int64(q,1,grant);if(decision<0)sqlite3_bind_int64(q,2,r->record);else{sqlite3_bind_int64(q,2,(int64_t)wait);bind(q,3,decision?"validating":"denied");sqlite3_bind_int64(q,4,r->record);}}return done(q)&&sqlite3_changes(r->store->db)==1;
}
void dc_tool_records_finish(dc_tool_record_run *r,ah_status outcome){
 if(!r||!r->record)return;sqlite3_stmt *q=prep(r->store,"UPDATE tool_records SET stage=? WHERE id=? AND result_sequence IS NULL AND stage NOT IN('denied','unknown')");if(q){bind(q,1,outcome==AH_UNKNOWN?"unknown":"interrupted");sqlite3_bind_int64(q,2,r->record);}done(q);
}
static const char *selection="SELECT id,chat,run,proposal,call_id,name,arguments,result,result_sequence,stage,outcome,grant_id,wait_ms,started,execution_ms,committed_ms,created,historical FROM tool_records ";
static J *row(sqlite3_stmt *q,int detail){
 const char *keys[]={"id","chat","run","sourceSequence","callID","name","arguments","result","resultSequence","stage","outcome","grantID","waitMs","started","executionMs","committedMs","created","historical"};J *v=json_object_new_object();
 for(int i=0;i<18;i++){
  if(sqlite3_column_type(q,i)==SQLITE_NULL)continue;
  int is_text=i==1||i==2||i==4||i==5||i==6||i==7||i==9||i==10;
  if(is_text){const char *value=(const char*)sqlite3_column_text(q,i);size_t n=(size_t)sqlite3_column_bytes(q,i);if(!detail&&(i==6||i==7)&&n>200){size_t end=200;while(end&&(value[end]&0xc0)==0x80)end--;json_object_object_add(v,keys[i],json_object_new_string_len(value,(int)end));json_object_object_add(v,i==6?"argumentsPreview":"resultPreview",json_object_new_boolean(1));}else add(v,keys[i],value);}
  else json_object_object_add(v,keys[i],i==17?json_object_new_boolean(sqlite3_column_int(q,i)):json_object_new_int64(sqlite3_column_int64(q,i)));
 }return v;
}
J *dc_tool_records_request(dc_store *s,J *r){
 const char *op=str(r,"op"),*chat=str(r,"chat");if(!valid(chat,64))return fail("invalid_tool_record_query");
 sqlite3_stmt *exists=prep(s,"SELECT 1 FROM chats WHERE id=?");if(exists)bind(exists,1,chat);int found=exists&&sqlite3_step(exists)==SQLITE_ROW;sqlite3_finalize(exists);if(!found)return fail("chat_not_found");
 if(eq(op,"tools.detail")){
  J *id=get(r,"id");if(!json_object_is_type(id,json_type_int)||json_object_get_int64(id)<=0)return fail("invalid_tool_record_query");char query[1024];snprintf(query,sizeof query,"%s WHERE chat=? AND id=?",selection);sqlite3_stmt *q=prep(s,query);if(q){bind(q,1,chat);sqlite3_bind_int64(q,2,json_object_get_int64(id));}J *v=q&&sqlite3_step(q)==SQLITE_ROW?row(q,1):NULL;sqlite3_finalize(q);if(!v)return fail("tool_record_not_found");
  sqlite3_stmt *a=prep(s,"SELECT args,status FROM grant_records WHERE id=?");if(a){sqlite3_bind_int64(a,1,json_object_get_int64(get(v,"grantID")));if(sqlite3_step(a)==SQLITE_ROW){add(v,"approvedArguments",(const char*)sqlite3_column_text(a,0));add(v,"grantStatus",(const char*)sqlite3_column_text(a,1));}}sqlite3_finalize(a);
  a=prep(s,"SELECT schema FROM tool_record_schemas WHERE record=?");if(a){sqlite3_bind_int64(a,1,json_object_get_int64(get(v,"id")));if(sqlite3_step(a)==SQLITE_ROW)add(v,"schema",(const char*)sqlite3_column_text(a,0));}sqlite3_finalize(a);
  a=prep(s,"SELECT json_extract(state,'$.pending') FROM chats WHERE id=? AND json_valid(state)");int pending=0;if(a){bind(a,1,chat);if(sqlite3_step(a)==SQLITE_ROW){const char *call=(const char*)sqlite3_column_text(a,0);pending=call&&eq(call,str(v,"callID"))&&!get(v,"resultSequence");}}sqlite3_finalize(a);json_object_object_add(v,"unknownEffect",json_object_new_boolean(pending||eq(str(v,"stage"),"unknown")));
  a=prep(s,"SELECT id FROM file_operations WHERE chat=? AND run=? AND proposal=? AND call_id=? ORDER BY rowid DESC LIMIT 1");if(a){bind(a,1,chat);bind(a,2,str(v,"run"));sqlite3_bind_int64(a,3,json_object_get_int64(get(v,"sourceSequence")));bind(a,4,str(v,"callID"));if(sqlite3_step(a)==SQLITE_ROW)add(v,"fileReceiptID",(const char*)sqlite3_column_text(a,0));}sqlite3_finalize(a);return reply(1,NULL,v);
 }
 if(eq(op,"tools.list")||eq(op,"tools.locate")){
  const char *query=str(r,"query"),*stage=str(r,"stage"),*run=str(r,"run");if((query&&strlen(query)>256)||(stage&&strlen(stage)>32)||(run&&strlen(run)>64))return fail("invalid_tool_record_query");int64_t before=json_object_get_int64(get(r,"before"));if(before<=0)before=INT64_MAX;
  if((get(r,"query")&&!query)||(get(r,"stage")&&!stage)||(get(r,"run")&&!run)||(get(r,"before")&&(!json_object_is_type(get(r,"before"),json_type_int)||json_object_get_int64(get(r,"before"))<0)))return fail("invalid_tool_record_query");
  int64_t sequence=-1;if(eq(op,"tools.locate")){if(!json_object_is_type(get(r,"sequence"),json_type_int)||json_object_get_int64(get(r,"sequence"))<0)return fail("invalid_tool_record_query");sequence=json_object_get_int64(get(r,"sequence"));}
  char statement[1600];snprintf(statement,sizeof statement,"%s WHERE chat=? AND id<? AND (?='' OR stage=?) AND (?='' OR run=?) AND (?='' OR instr(lower(name||' '||arguments||' '||coalesce(result,'')),lower(?))>0) AND (?<0 OR proposal=? OR result_sequence=?) ORDER BY id DESC LIMIT 51",selection);sqlite3_stmt *q=prep(s,statement);if(!q)return fail("tool_record_storage_error");bind(q,1,chat);sqlite3_bind_int64(q,2,before);bind(q,3,stage);bind(q,4,stage);bind(q,5,run);bind(q,6,run);bind(q,7,query);bind(q,8,query);sqlite3_bind_int64(q,9,sequence);sqlite3_bind_int64(q,10,sequence);sqlite3_bind_int64(q,11,sequence);J *items=json_object_new_array();int rc,count=0,more=0;int64_t cursor=0;
  while((rc=sqlite3_step(q))==SQLITE_ROW){if(count++==50){more=1;break;}cursor=sqlite3_column_int64(q,0);json_object_array_add(items,row(q,0));}sqlite3_finalize(q);if(rc!=SQLITE_DONE&&rc!=SQLITE_ROW){json_object_put(items);return fail("tool_record_storage_error");}
  J *data=json_object_new_object();json_object_object_add(data,"items",items);json_object_object_add(data,"more",json_object_new_boolean(more));json_object_object_add(data,"nextCursor",json_object_new_int64(cursor));return reply(1,NULL,data);
 }
 return fail("unknown_tool_record_operation");
}
