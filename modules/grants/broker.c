#define _POSIX_C_SOURCE 200809L
#include "../../desktop/grants.h"
#include "../../desktop/platform_grants.h"
#include "dunlin/content_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int i,const char *v){sqlite3_bind_text(q,i,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static J *reply(int ok,const char *error,J *data){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(o,"error",json_object_new_string(error));if(data)json_object_object_add(o,"data",data);return o;}
int dc_grants_schema(dc_store *s){return sqlite3_exec(s->db,
 "CREATE TABLE IF NOT EXISTS grant_records(id INTEGER PRIMARY KEY,chat TEXT NOT NULL,tool TEXT NOT NULL,args TEXT NOT NULL,args_hash TEXT NOT NULL,allowed INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL DEFAULT 'awaiting',started INTEGER NOT NULL DEFAULT(unixepoch()),decided INTEGER,ended INTEGER,wait_ms INTEGER NOT NULL DEFAULT 0);CREATE INDEX IF NOT EXISTS grant_chat ON grant_records(chat,id DESC);UPDATE grant_records SET status='interrupted',ended=unixepoch() WHERE status IN('awaiting','issued_once');",NULL,NULL,NULL)==SQLITE_OK;}
long long dc_grant_begin(dc_store *s,const char *chat,const char *tool,const char *args){
 if(!s||!chat||!tool||!args)return 0;char digest[65];dc_sha256(args,strlen(args),digest);sqlite3_stmt *q=prep(s,"INSERT INTO grant_records(chat,tool,args,args_hash) VALUES(?,?,?,?)");if(q){bind(q,1,chat);bind(q,2,tool);bind(q,3,args);bind(q,4,digest);}return step(q)?sqlite3_last_insert_rowid(s->db):0;
}
int dc_grant_decide(dc_store *s,long long id,int allow,unsigned long long elapsed){
 sqlite3_stmt *q=prep(s,"UPDATE grant_records SET allowed=?,status=?,decided=unixepoch(),wait_ms=? WHERE id=? AND status='awaiting'");if(q){sqlite3_bind_int(q,1,allow);bind(q,2,allow?"issued_once":"denied");sqlite3_bind_int64(q,3,(sqlite3_int64)elapsed);sqlite3_bind_int64(q,4,id);}return step(q)&&sqlite3_changes(s->db)==1;
}
int dc_grant_end(dc_store *s,long long id){sqlite3_stmt *q=prep(s,"UPDATE grant_records SET status='expired',ended=unixepoch() WHERE id=? AND status='issued_once'");if(q)sqlite3_bind_int64(q,1,id);return step(q);}
void dc_grants_finish(dc_store *s,const char *chat){sqlite3_stmt *q=prep(s,"UPDATE grant_records SET ended=unixepoch(),status=CASE WHEN status='issued_once' THEN 'expired' ELSE status END WHERE chat=? AND ended IS NULL");if(q)bind(q,1,chat);step(q);}
J *dc_grants_request(dc_store *s,J *r){
 const char *op=str(r,"op");if(!op)return reply(0,"invalid_request",NULL);
 if(!strncmp(op,"grants.platform.",16))return dc_platform_grants_request(s,r);
 if(!strcmp(op,"grants.list")){
  const char *chat=str(r,"chat");long long cursor=json_object_get_int64(get(r,"before"));int limit=json_object_get_int(get(r,"limit"));if(limit<=0||limit>200)limit=200;
  const char *scope=str(r,"scope");
  J *record=get(r,"recordID");
  if(record&&(!json_object_is_type(record,json_type_int)||json_object_get_int64(record)<=0))return reply(0,"invalid_grant_record",NULL);
  if(get(r,"scope")&&(!scope||(strcmp(scope,"active")&&strcmp(scope,"history"))))return reply(0,"invalid_grant_scope",NULL);
  sqlite3_stmt *q=prep(s,"SELECT id,chat,tool,args,args_hash,allowed,status,started,COALESCE(decided,0),COALESCE(ended,0),wait_ms FROM grant_records WHERE (?1='' OR chat=?1) AND (?2=0 OR id<?2) AND (?3='' OR (?3='active')=(status IN('awaiting','issued_once'))) AND (?5 IS NULL OR id=?5) ORDER BY id DESC LIMIT ?4");
  if(!q)return reply(0,"storage_error",NULL);
  bind(q,1,chat);sqlite3_bind_int64(q,2,cursor);bind(q,3,scope);sqlite3_bind_int(q,4,limit);if(record)sqlite3_bind_int64(q,5,json_object_get_int64(record));
  J *a=json_object_new_array();int rc;
  while((rc=sqlite3_step(q))==SQLITE_ROW){J *v=json_object_new_object();const char *keys[]={"id","chat","tool","arguments","argsHash","allowed","status","started","decided","ended","waitMs"};for(int i=0;i<11;i++)json_object_object_add(v,keys[i],i==5?json_object_new_boolean(sqlite3_column_int(q,i)):i>=7||i==0?json_object_new_int64(sqlite3_column_int64(q,i)):json_object_new_string((const char*)sqlite3_column_text(q,i)));json_object_array_add(a,v);}
  sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return reply(0,"storage_error",NULL);}return reply(1,NULL,a);
 }
 if(!strcmp(op,"grants.revoke")){
  long long id=json_object_get_int64(get(r,"id"));sqlite3_stmt *q=prep(s,"SELECT status FROM grant_records WHERE id=?");if(!q)return reply(0,"storage_error",NULL);sqlite3_bind_int64(q,1,id);int rc=sqlite3_step(q);const char *status=rc==SQLITE_ROW?(const char*)sqlite3_column_text(q,0):NULL;
  int active=status&&(!strcmp(status,"issued_once")||!strcmp(status,"awaiting"));int found=status!=NULL;sqlite3_finalize(q);J *data=json_object_new_object();json_object_object_add(data,"state",json_object_new_string(active?"cancel_running_call_first":"already_inactive"));json_object_object_add(data,"externalEffects",json_object_new_string("not_reverted"));return reply(found,found?NULL:"grant_not_found",data);
 }
 return reply(0,"unsupported_operation",NULL);
}
