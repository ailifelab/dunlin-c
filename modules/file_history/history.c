#define _POSIX_C_SOURCE 200809L
#include "../../desktop/file_history.h"
#include "../../desktop/project_files.h"
#include "../../desktop/trajectory.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);if(!json_object_is_type(v,json_type_string))return NULL;const char *p=json_object_get_string(v);return strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int valid(const char *p,size_t cap){return p&&*p&&strlen(p)<=cap&&ah_utf8_text(p,strlen(p));}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *query){return sqlite3_exec(s->db,query,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *v=json_object_new_object();json_object_object_add(v,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(v,"error",json_object_new_string(error));if(data)json_object_object_add(v,"data",data);return v;}
static J *fail(const char *e){return reply(0,e,NULL);}
static void nonce(char out[65]){unsigned char random[32];sqlite3_randomness(sizeof random,random);dc_sha256(random,sizeof random,out);}
int dc_file_history_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS file_history_blobs(hash TEXT PRIMARY KEY,text TEXT NOT NULL,bytes INTEGER NOT NULL);"
 "CREATE TABLE IF NOT EXISTS file_operations(id TEXT PRIMARY KEY,chat TEXT NOT NULL,run TEXT NOT NULL,call_id TEXT NOT NULL,proposal INTEGER NOT NULL,grant_id INTEGER NOT NULL,project TEXT NOT NULL,root TEXT NOT NULL,device TEXT NOT NULL,inode TEXT NOT NULL,path TEXT NOT NULL,operation TEXT NOT NULL,before_hash TEXT NOT NULL,after_hash TEXT NOT NULL,existed INTEGER NOT NULL,after_exists INTEGER NOT NULL,args_hash TEXT NOT NULL,parent TEXT NOT NULL DEFAULT '',state TEXT NOT NULL DEFAULT 'prepared',detail TEXT NOT NULL DEFAULT '',revision INTEGER NOT NULL DEFAULT 1,created INTEGER NOT NULL DEFAULT(unixepoch()),finished INTEGER,UNIQUE(run,proposal,call_id));"
 "CREATE INDEX IF NOT EXISTS file_operation_chat ON file_operations(chat,created DESC,id);"
 "CREATE TABLE IF NOT EXISTS file_restore_plans(id TEXT PRIMARY KEY,chat TEXT NOT NULL,receipt TEXT NOT NULL,revision INTEGER NOT NULL,current_hash TEXT NOT NULL,expires INTEGER NOT NULL,consumed INTEGER NOT NULL DEFAULT 0);"
 "CREATE TABLE IF NOT EXISTS file_stage_cleanup(id TEXT PRIMARY KEY,root TEXT NOT NULL,device TEXT NOT NULL,inode TEXT NOT NULL,path TEXT NOT NULL,after_hash TEXT NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()));"
 "UPDATE file_operations SET state='unknown',detail='Process interrupted after durable preparation; external effect is not established.',revision=revision+1 WHERE state='prepared';");}
static int exists(dc_store *s,const char *chat){sqlite3_stmt *q=prep(s,"SELECT 1 FROM chats WHERE id=?");if(q)bind(q,1,chat);int ok=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);return ok;}
static int inactive(dc_store *s,const char *chat){
 sqlite3_stmt *q=prep(s,"SELECT state FROM chats WHERE id=?");if(q)bind(q,1,chat);int ok=0;if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);if(!raw)ok=1;else{J *v=dunlin_json_parse(raw,strlen(raw));ok=json_object_is_type(v,json_type_object)&&json_object_is_type(get(v,"active"),json_type_boolean)&&!json_object_get_boolean(get(v,"active"))&&!get(v,"pending");json_object_put(v);}}sqlite3_finalize(q);return ok;
}
static char *workspace(dc_store *s,const char *chat){sqlite3_stmt *q=prep(s,"SELECT workspace FROM chats WHERE id=?");if(q)bind(q,1,chat);char *v=q&&sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);return v;}
static int root_matches(const char *path,const char *device,const char *inode){struct stat st;char dev[32],ino[32];if(!path||stat(path,&st)||!S_ISDIR(st.st_mode))return 0;snprintf(dev,sizeof dev,"%ju",(uintmax_t)st.st_dev);snprintf(ino,sizeof ino,"%ju",(uintmax_t)st.st_ino);return eq(dev,device)&&eq(ino,inode);}
static int save_blob(dc_store *s,const char *text,char hash[65]){
 if(!text||strlen(text)>1048576||!ah_utf8_text(text,strlen(text)))return 0;size_t size=strlen(text);dc_sha256(text,size,hash);
 sqlite3_stmt *q=prep(s,"INSERT OR IGNORE INTO file_history_blobs(hash,text,bytes) VALUES(?,?,?)");if(q){bind(q,1,hash);bind(q,2,text);sqlite3_bind_int64(q,3,(int64_t)size);}if(!step(q))return 0;
 q=prep(s,"SELECT text,bytes FROM file_history_blobs WHERE hash=?");if(q)bind(q,1,hash);int ok=q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_int64(q,1)==(int64_t)size&&sqlite3_column_bytes(q,0)==(int)size&&!memcmp(sqlite3_column_text(q,0),text,size);sqlite3_finalize(q);return ok;
}
static char *load_blob(dc_store *s,const char *hash){
 sqlite3_stmt *q=prep(s,"SELECT text,bytes FROM file_history_blobs WHERE hash=?");if(q)bind(q,1,hash);char *text=NULL;
 if(q&&sqlite3_step(q)==SQLITE_ROW){int n=sqlite3_column_bytes(q,0);const char *raw=(const char*)sqlite3_column_text(q,0);if(raw&&n>=0&&n<=1048576&&sqlite3_column_int64(q,1)==n&&strlen(raw)==(size_t)n&&ah_utf8_text(raw,(size_t)n)){char actual[65];dc_sha256(raw,(size_t)n,actual);if(eq(actual,hash))text=strdup(raw);}}
 sqlite3_finalize(q);return text;
}
static int prepare(void *user,const char *operation,const char *path,J *arguments,int existed,const char *before,const char *after,const char *device,const char *inode,char id[65]){
 dc_file_history_run *r=user;dc_store *s=r?r->store:NULL;
 if(!s||!valid(r->chat,64)||!valid(r->run,64)||!valid(r->call,256)||!valid(path,1024)||!valid(operation,32)||!before||!device||!inode||(!r->parent&&r->proposal<0))return 0;
 char *root=workspace(s,r->chat);J *project=dc_chat_project(s,r->chat);const char *project_id=str(project,"id");
 if(!root||!root_matches(root,device,inode)||(project&&(!json_object_get_boolean(get(project,"available"))||!eq(device,str(project,"device"))||!eq(inode,str(project,"inode"))))){free(root);json_object_put(project);return 0;}
 const char *raw=json_object_to_json_string_ext(arguments,JSON_C_TO_STRING_PLAIN);
 if(!r->parent){J *approved=r->authorized_arguments?dunlin_json_parse(r->authorized_arguments,strlen(r->authorized_arguments)):NULL;int equal=approved&&json_object_equal(approved,arguments);json_object_put(approved);if(!equal){free(root);json_object_put(project);return 0;}raw=r->authorized_arguments;}
 char args_hash[65],before_hash[65],after_hash[65];dc_sha256(raw,strlen(raw),args_hash);
 int ok=sql(s,"BEGIN IMMEDIATE");if(!ok){free(root);json_object_put(project);return 0;}
 if(r->parent&&r->plan){
  sqlite3_stmt *q=prep(s,"UPDATE file_restore_plans SET consumed=2 WHERE id=? AND chat=? AND receipt=? AND consumed=1 AND expires>=unixepoch()");if(q){bind(q,1,r->plan);bind(q,2,r->chat);bind(q,3,r->parent);}ok=step(q)&&sqlite3_changes(s->db)==1;
 }else{
  sqlite3_stmt *q=prep(s,"SELECT 1 FROM grant_records WHERE id=? AND chat=? AND tool=? AND args_hash=? AND allowed=1 AND status='issued_once'");if(q){sqlite3_bind_int64(q,1,r->grant);bind(q,2,r->chat);bind(q,3,operation);bind(q,4,args_hash);}ok=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);
 }
 ok=ok&&save_blob(s,before,before_hash);if(after)ok=ok&&save_blob(s,after,after_hash);else snprintf(after_hash,sizeof after_hash,"absent");nonce(id);
 sqlite3_stmt *q=ok?prep(s,"INSERT INTO file_operations(id,chat,run,call_id,proposal,grant_id,project,root,device,inode,path,operation,before_hash,after_hash,existed,after_exists,args_hash,parent) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"):NULL;
 if(q){bind(q,1,id);bind(q,2,r->chat);bind(q,3,r->run);bind(q,4,r->call);sqlite3_bind_int64(q,5,r->proposal);sqlite3_bind_int64(q,6,r->grant);bind(q,7,project_id);bind(q,8,root);bind(q,9,device);bind(q,10,inode);bind(q,11,path);bind(q,12,operation);bind(q,13,before_hash);bind(q,14,after_hash);sqlite3_bind_int(q,15,existed);sqlite3_bind_int(q,16,after!=NULL);bind(q,17,args_hash);bind(q,18,r->parent);}ok=step(q)&&ok;
 if(ok&&after){q=prep(s,"INSERT INTO file_stage_cleanup(id,root,device,inode,path,after_hash) VALUES(?,?,?,?,?,?)");if(q){bind(q,1,id);bind(q,2,root);bind(q,3,device);bind(q,4,inode);bind(q,5,path);bind(q,6,after_hash);}ok=step(q);}
 if(!ok||!sql(s,"COMMIT")){sql(s,"ROLLBACK");ok=0;}if(ok)snprintf(r->last_operation_id,sizeof r->last_operation_id,"%s",id);free(root);json_object_put(project);return ok;
}
static int settle(void *user,const char *id,const char *state,const char *detail){
 dc_file_history_run *r=user;if(!r||(!eq(state,"applied")&&!eq(state,"failed")&&!eq(state,"unknown")))return 0;
 dc_store *s=r->store;if(!sql(s,"BEGIN IMMEDIATE"))return 0;
 sqlite3_stmt *q=prep(s,"UPDATE file_operations SET state=?,detail=?,finished=unixepoch(),revision=revision+1 WHERE id=? AND chat=? AND run=? AND state='prepared'");if(q){bind(q,1,state);bind(q,2,detail);bind(q,3,id);bind(q,4,r->chat);bind(q,5,r->run);}int ok=step(q)&&sqlite3_changes(s->db)==1;
 if(ok&&eq(state,"applied")){q=prep(s,"DELETE FROM file_stage_cleanup WHERE id=?");if(q)bind(q,1,id);ok=step(q);}
 if(ok){
  J *observation=json_object_new_object();json_object_object_add(observation,"operationId",json_object_new_string(id));json_object_object_add(observation,"state",json_object_new_string(state));json_object_object_add(observation,"actor",json_object_new_string(r->parent?"user_restore":"model_tool"));if(r->parent)json_object_object_add(observation,"parent",json_object_new_string(r->parent));
  q=prep(s,"INSERT INTO conversation_events(chat,run,type,body) VALUES(?,?,'file.operation',?)");if(q){bind(q,1,r->chat);bind(q,2,r->run);bind(q,3,json_object_to_json_string_ext(observation,JSON_C_TO_STRING_PLAIN));}ok=step(q);json_object_put(observation);
 }
 if(!ok||!sql(s,"COMMIT")){sql(s,"ROLLBACK");return 0;}return 1;
}
dc_file_journal_v1 dc_file_history_journal(dc_file_history_run *run){return (dc_file_journal_v1){run,prepare,settle};}
int dc_file_history_drop_chat(dc_store *s,const char *chat){
 const char *queries[]={"DELETE FROM file_restore_plans WHERE chat=?","DELETE FROM file_operations WHERE chat=?"};for(size_t i=0;i<2;i++){sqlite3_stmt *q=prep(s,queries[i]);if(q)bind(q,1,chat);if(!step(q))return 0;}
 return sql(s,"DELETE FROM file_history_blobs WHERE NOT EXISTS(SELECT 1 FROM file_operations WHERE before_hash=file_history_blobs.hash OR after_hash=file_history_blobs.hash) AND NOT EXISTS(SELECT 1 FROM file_stage_cleanup WHERE after_hash=file_history_blobs.hash)");
}
int dc_file_history_cleanup(dc_store *s,unsigned limit){
 if(!s||!limit||limit>64)return -1;sqlite3_stmt *q=prep(s,"SELECT id,root,device,inode,path,after_hash FROM file_stage_cleanup WHERE id>? ORDER BY id LIMIT ?");if(!q)return -1;bind(q,1,s->file_cleanup_cursor);sqlite3_bind_int(q,2,(int)limit);J *jobs=json_object_new_array();int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){J *v=json_object_new_object();const char *keys[]={"id","root","device","inode","path","afterHash"};for(int i=0;i<6;i++)json_object_object_add(v,keys[i],json_object_new_string((const char*)sqlite3_column_text(q,i)));json_object_array_add(jobs,v);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(jobs);return -1;}if(!json_object_array_length(jobs)&&*s->file_cleanup_cursor){s->file_cleanup_cursor[0]=0;json_object_put(jobs);return dc_file_history_cleanup(s,limit);}int cleaned=0;
 for(size_t i=0;i<json_object_array_length(jobs);i++){J *job=json_object_array_get_idx(jobs,i);if(!valid(str(job,"id"),64))continue;snprintf(s->file_cleanup_cursor,sizeof s->file_cleanup_cursor,"%s",str(job,"id"));if(!valid(str(job,"root"),4095)||!valid(str(job,"path"),1024)||!valid(str(job,"afterHash"),64))continue;char *root=strdup(str(job,"root")),*text=load_blob(s,str(job,"afterHash"));if(!root||!text){free(root);free(text);continue;}
  if(!root_matches(root,str(job,"device"),str(job,"inode"))){free(root);root=NULL;q=prep(s,"SELECT path FROM projects WHERE device=? AND inode=? LIMIT 1");if(q){bind(q,1,str(job,"device"));bind(q,2,str(job,"inode"));}if(q&&sqlite3_step(q)==SQLITE_ROW)root=strdup((const char*)sqlite3_column_text(q,0));sqlite3_finalize(q);}
  int clean=root&&root_matches(root,str(job,"device"),str(job,"inode"))&&dc_project_file_discard_stage(root,str(job,"path"),str(job,"id"),text);free(root);free(text);if(!clean)continue;
  q=prep(s,"DELETE FROM file_stage_cleanup WHERE id=?");if(q)bind(q,1,str(job,"id"));if(!step(q)){json_object_put(jobs);return -1;}cleaned++;
 }
 json_object_put(jobs);if(!sql(s,"DELETE FROM file_history_blobs WHERE NOT EXISTS(SELECT 1 FROM file_operations WHERE before_hash=file_history_blobs.hash OR after_hash=file_history_blobs.hash) AND NOT EXISTS(SELECT 1 FROM file_stage_cleanup WHERE after_hash=file_history_blobs.hash)"))return -1;return cleaned;
}
static const char *select_record="SELECT rowid,id,run,call_id,proposal,grant_id,project,root,device,inode,path,operation,before_hash,after_hash,existed,after_exists,parent,state,detail,revision,created,COALESCE(finished,0) FROM file_operations ";
static J *record_row(sqlite3_stmt *q){
 J *v=json_object_new_object();const char *keys[]={"cursor","id","run","call","proposal","grant","project","root","device","inode","path","operation","beforeHash","afterHash","existed","afterExists","parent","state","detail","revision","created","finished"};
 for(int i=0;i<22;i++)json_object_object_add(v,keys[i],i==14||i==15?json_object_new_boolean(sqlite3_column_int(q,i)):i==0||i==4||i==5||i>=19?json_object_new_int64(sqlite3_column_int64(q,i)):json_object_new_string((const char*)sqlite3_column_text(q,i)));return v;
}
static J *record(dc_store *s,const char *chat,const char *id){char query[1024];snprintf(query,sizeof query,"%s WHERE chat=? AND id=?",select_record);sqlite3_stmt *q=prep(s,query);if(q){bind(q,1,chat);bind(q,2,id);}J *v=q&&sqlite3_step(q)==SQLITE_ROW?record_row(q):NULL;sqlite3_finalize(q);return v;}
static J *list(dc_store *s,J *r,const char *chat){
 int64_t before=json_object_get_int64(get(r,"before"));if(!before)before=INT64_MAX;char query[1024];snprintf(query,sizeof query,"%s WHERE chat=? AND rowid<? ORDER BY rowid DESC LIMIT 51",select_record);sqlite3_stmt *q=prep(s,query);if(!q)return fail("file_history_storage_error");bind(q,1,chat);sqlite3_bind_int64(q,2,before);J *items=json_object_new_array();int rc,count=0,more=0;int64_t next=0;
 while((rc=sqlite3_step(q))==SQLITE_ROW){if(count++==50){more=1;break;}next=sqlite3_column_int64(q,0);json_object_array_add(items,record_row(q));}sqlite3_finalize(q);if(rc!=SQLITE_DONE&&rc!=SQLITE_ROW){json_object_put(items);return fail("file_history_storage_error");}J *v=json_object_new_object();json_object_object_add(v,"items",items);json_object_object_add(v,"more",json_object_new_boolean(more));json_object_object_add(v,"nextCursor",json_object_new_int64(next));return reply(1,NULL,v);
}
static char *restore_root(dc_store *s,const char *chat,J *entry){
 J *project=dc_chat_project(s,chat);const char *path=str(project,"path");char *root=project&&json_object_get_boolean(get(project,"available"))&&eq(str(entry,"device"),str(project,"device"))&&eq(str(entry,"inode"),str(project,"inode"))&&root_matches(path,str(entry,"device"),str(entry,"inode"))?strdup(path):NULL;json_object_put(project);return root;
}
static J *detail(dc_store *s,const char *chat,const char *id,int preview,int acknowledge){
 J *entry=record(s,chat,id);if(!entry)return fail("file_receipt_not_found");char *before=load_blob(s,str(entry,"beforeHash")),*after=json_object_get_boolean(get(entry,"afterExists"))?load_blob(s,str(entry,"afterHash")):strdup("");
 if(!before||!after){free(before);free(after);json_object_put(entry);return fail("file_history_snapshot_invalid");}
 json_object_object_add(entry,"before",json_object_new_string(before));json_object_object_add(entry,"after",json_object_new_string(after));free(before);free(after);
 char *root=restore_root(s,chat,entry),current_hash[65]="absent";int current=root?dc_project_file_digest(root,str(entry,"path"),current_hash):-1;
 json_object_object_add(entry,"currentRoot",json_object_new_string(root?root:""));
 const char *expected=str(entry,"afterHash");int matches=current>=0&&eq(expected,current_hash);int unknown=eq(str(entry,"state"),"unknown")||eq(str(entry,"state"),"prepared");
 json_object_object_add(entry,"currentState",json_object_new_string(!root?"project_unavailable":current<0?"unreadable":matches?"matches_after":"conflict"));if(current>=0)json_object_object_add(entry,"currentHash",json_object_new_string(current_hash));
 int can=matches&&inactive(s,chat)&&(eq(str(entry,"state"),"applied")||(unknown&&acknowledge));json_object_object_add(entry,"canRestore",json_object_new_boolean(can));json_object_object_add(entry,"unknownAcknowledgementRequired",json_object_new_boolean(unknown));
 if(preview){
  if(!can){free(root);json_object_put(entry);return fail(unknown&&!acknowledge?"file_unknown_acknowledgement_required":"file_restore_conflict_or_active");}
  if(!sql(s,"DELETE FROM file_restore_plans WHERE consumed<>0 OR expires<unixepoch()")){free(root);json_object_put(entry);return fail("file_history_storage_error");}
  char plan[65];nonce(plan);sqlite3_stmt *q=prep(s,"INSERT INTO file_restore_plans(id,chat,receipt,revision,current_hash,expires) VALUES(?,?,?,?,?,unixepoch()+300)");if(q){bind(q,1,plan);bind(q,2,chat);bind(q,3,id);sqlite3_bind_int64(q,4,json_object_get_int64(get(entry,"revision")));bind(q,5,expected);}if(!step(q)){free(root);json_object_put(entry);return fail("file_history_storage_error");}
  json_object_object_add(entry,"plan",json_object_new_string(plan));json_object_object_add(entry,"planLifetimeSeconds",json_object_new_int(300));
 }
 free(root);return reply(1,NULL,entry);
}
static J *restore(dc_store *s,J *r,const char *chat){
 const char *plan=str(r,"plan");
 if(!valid(plan,64)||!json_object_is_type(get(r,"confirmed"),json_type_boolean)||!json_object_get_boolean(get(r,"confirmed")))return fail("file_restore_confirmation_required");
 if(!inactive(s,chat))return fail("active_checkpoint");
 sqlite3_stmt *q=prep(s,"SELECT receipt,revision,current_hash FROM file_restore_plans WHERE id=? AND chat=? AND consumed=0 AND expires>=unixepoch()");
 if(q){bind(q,1,plan);bind(q,2,chat);}int found=q&&sqlite3_step(q)==SQLITE_ROW;
 char *id=found?strdup((const char*)sqlite3_column_text(q,0)):NULL,*hash=found?strdup((const char*)sqlite3_column_text(q,2)):NULL;
 int64_t revision=found?sqlite3_column_int64(q,1):0;sqlite3_finalize(q);
 if(!id||!hash){free(id);free(hash);return fail("file_restore_plan_stale");}
 J *entry=record(s,chat,id),*intent=json_object_new_object();char run[65];nonce(run);
 json_object_object_add(intent,"receipt",json_object_new_string(id));json_object_object_add(intent,"plan",json_object_new_string(plan));json_object_object_add(intent,"expectedHash",json_object_new_string(hash));
 if(entry){json_object_object_add(intent,"path",json_object_get(get(entry,"path")));int target_exists=json_object_get_boolean(get(entry,"existed"));json_object_object_add(intent,"targetExists",json_object_new_boolean(target_exists));json_object_object_add(intent,"targetHash",target_exists?json_object_get(get(entry,"beforeHash")):json_object_new_string("absent"));}
 /* Consume at durable admission, not at publication. A confirmed conflict may
  * not become an authorization to retry the same plan later. State 1 reserves
  * this invocation; only its C journal can advance it to prepared state 2. */
 int begun=sql(s,"BEGIN IMMEDIATE"),ok=0,write_ok=0,reserved=0;
 if(begun){
  q=prep(s,"UPDATE file_restore_plans SET consumed=1 WHERE id=? AND chat=? AND receipt=? AND revision=? AND current_hash=? AND consumed=0 AND expires>=unixepoch()");
  if(q){bind(q,1,plan);bind(q,2,chat);bind(q,3,id);sqlite3_bind_int64(q,4,revision);bind(q,5,hash);}write_ok=step(q);reserved=write_ok&&sqlite3_changes(s->db)==1;ok=reserved;
  if(ok){q=prep(s,"INSERT INTO conversation_events(chat,run,type,body) VALUES(?,?,'file.restore_confirmed',?)");if(q){bind(q,1,chat);bind(q,2,run);bind(q,3,json_object_to_json_string_ext(intent,JSON_C_TO_STRING_PLAIN));}ok=step(q);}
 }
 if(!ok||!sql(s,"COMMIT")){
  if(begun)sql(s,"ROLLBACK");free(id);free(hash);json_object_put(entry);json_object_put(intent);return fail(begun&&write_ok&&!reserved?"file_restore_plan_stale":"file_history_storage_error");
 }
 dl_trace_span *trace=dc_trajectory_begin(s,chat,run,"手动文件回退","host",-1,NULL);
 const char *raw=json_object_to_json_string_ext(intent,JSON_C_TO_STRING_PLAIN);dl_trajectory_event(trace,"file.operation.request",raw,strlen(raw));json_object_put(intent);
 char *root=entry?restore_root(s,chat,entry):NULL,*before=entry?load_blob(s,str(entry,"beforeHash")):NULL;
 if(!entry||!root||!before||revision!=json_object_get_int64(get(entry,"revision"))||!eq(hash,str(entry,"afterHash"))){
  const char *error="source or project changed after preview";dl_trajectory_event(trace,"file.operation.rejected",error,strlen(error));dl_trajectory_finish(trace,"failed",-1);free(id);free(hash);free(root);free(before);json_object_put(entry);return fail("file_restore_plan_stale");
 }
 J *arguments=json_object_new_object();json_object_object_add(arguments,"path",json_object_get(get(entry,"path")));json_object_object_add(arguments,"expected_sha256",json_object_new_string(eq(hash,"absent")?"new":hash));json_object_object_add(arguments,"remove",json_object_new_boolean(!json_object_get_boolean(get(entry,"existed"))));json_object_object_add(arguments,"text",json_object_new_string(before));
 dc_file_history_run context={.store=s,.chat=chat,.run=run,.proposal=-1,.parent=id,.plan=plan};snprintf(context.call,sizeof context.call,"%s",plan);dc_file_journal_v1 journal=dc_file_history_journal(&context);
 char *output=NULL;ah_status status=dc_project_file_restore(root,arguments,&journal,&output);
 J *data=json_object_new_object();json_object_object_add(data,"status",json_object_new_string(ah_status_name(status)));json_object_object_add(data,"result",json_object_new_string(output?output:"restore returned no result"));if(*context.last_operation_id)json_object_object_add(data,"operationId",json_object_new_string(context.last_operation_id));
 raw=json_object_to_json_string_ext(data,JSON_C_TO_STRING_PLAIN);dl_trajectory_event(trace,"file.operation.result",raw,strlen(raw));dl_trajectory_finish(trace,ah_status_name(status),-1);
 free(output);free(id);free(hash);free(root);free(before);json_object_put(entry);json_object_put(arguments);
 return reply(status==AH_DONE,status==AH_DONE?NULL:status==AH_UNKNOWN?"file_restore_outcome_unknown":"file_restore_conflict_or_failed",data);
}
static J *history_status(dc_store *s,int cleaned){
 sqlite3_stmt *q=prep(s,"SELECT (SELECT count(*) FROM file_operations),(SELECT coalesce(sum(bytes),0) FROM file_history_blobs),(SELECT count(*) FROM file_stage_cleanup)");
 if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("file_history_storage_error");}J *v=json_object_new_object();const char *keys[]={"operations","blobBytes","pendingStages"};for(int i=0;i<3;i++)json_object_object_add(v,keys[i],json_object_new_int64(sqlite3_column_int64(q,i)));sqlite3_finalize(q);if(cleaned>=0)json_object_object_add(v,"cleaned",json_object_new_int(cleaned));return reply(1,NULL,v);
}
J *dc_file_history_request(dc_store *s,J *r){
 const char *op=str(r,"op"),*chat=str(r,"chat");
 if(eq(op,"receipts.status"))return history_status(s,-1);
 if(eq(op,"receipts.cleanup")){int n=dc_file_history_cleanup(s,64);return n>=0?history_status(s,n):fail("file_history_storage_error");}
 if(eq(op,"receipts.pending")){
  sqlite3_stmt *q=prep(s,"SELECT id,root,path,created FROM file_stage_cleanup ORDER BY created,id LIMIT 100");if(!q)return fail("file_history_storage_error");J *a=json_object_new_array();int rc;
  while((rc=sqlite3_step(q))==SQLITE_ROW){J *v=json_object_new_object();const char *keys[]={"id","root","path","created"};for(int i=0;i<4;i++)json_object_object_add(v,keys[i],i==3?json_object_new_int64(sqlite3_column_int64(q,i)):json_object_new_string((const char*)sqlite3_column_text(q,i)));json_object_array_add(a,v);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("file_history_storage_error");}return reply(1,NULL,a);
 }
 if(!valid(chat,64)||!exists(s,chat))return fail("chat_not_found");
 if(eq(op,"receipts.list"))return list(s,r,chat);
 if(eq(op,"receipts.detail")||eq(op,"receipts.preview")){
  const char *id=str(r,"id");J *ack=get(r,"acknowledgeUnknown");if(!valid(id,64))return fail("invalid_file_receipt");if(ack&&!json_object_is_type(ack,json_type_boolean))return fail("invalid_file_history_acknowledgement");
  return detail(s,chat,id,eq(op,"receipts.preview"),json_object_get_boolean(ack));
 }
 if(eq(op,"receipts.restore"))return restore(s,r,chat);
 return fail("unsupported_file_history_operation");
}
