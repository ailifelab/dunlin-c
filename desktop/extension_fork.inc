#include "session_outcomes.h"
/* Copy a committed C history prefix, never a pending tool or execution lease.
 * Attachment resources stay content-addressed; the new source owns new refs. */
static J *client_fork_as(dc_store *s,J *r,const char *requested,const char *expected,int64_t through){
 J *args=get(r,"arguments");const char *parent=str(args,"sessionId");if(!parent)parent=str(args,"id");if(!id(parent)||(requested&&(!*requested||strlen(requested)>64||strspn(requested,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=strlen(requested))))return fail("invalid_native_parent");
 sqlite3_stmt *q=prep(s,"SELECT state,title,workspace FROM chats WHERE id=?");if(q)bind(q,1,parent);J *original=NULL;int stored=0;char *title=NULL,*path=NULL;
 if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);stored=raw!=NULL;if(expected){const char *snapshot=raw?raw:"{\"version\":1,\"messages\":[],\"active\":false,\"rounds\":0}";char observed[65];dc_sha256(snapshot,strlen(snapshot),observed);if(!eq(expected,observed)){sqlite3_finalize(q);return fail("native_fork_source_changed");}}if(raw&&strlen(raw)<=8388608)original=dunlin_json_parse(raw,strlen(raw));title=strdup((const char*)sqlite3_column_text(q,1));path=strdup((const char*)sqlite3_column_text(q,2));}sqlite3_finalize(q);

 J *messages=get(original,"messages");if((stored&&!original)||(original&&(!json_object_is_type(messages,json_type_array)||(json_object_get_int(get(original,"version"))!=1&&json_object_get_int(get(original,"version"))!=2)||!json_object_is_type(get(original,"active"),json_type_boolean)||!json_object_is_type(get(original,"rounds"),json_type_int)||json_object_get_int64(get(original,"rounds"))<0||!json_object_is_type(get(original,"version"),json_type_int)))){json_object_put(original);free(title);free(path);return fail("native_fork_history_invalid");}
 J *binding=dc_chat_workspace_view(s,parent);int available=json_object_get_boolean(get(binding,"available"));json_object_put(binding);if(!path||!title||!available){json_object_put(original);free(title);free(path);return fail("native_fork_workspace_unavailable");}
 size_t cut=messages?json_object_array_length(messages):0,requested_cut=cut;
 if(through != -2){
  if(through < -1 || (through>=0 && (uint64_t)through>=cut)){json_object_put(original);free(title);free(path);return fail("native_replay_boundary_invalid");}
  size_t boundary=(size_t)(through+1);
  if((boundary<cut && boundary && !eq(str(json_object_array_get_idx(messages,boundary),"role"),"user")) || (boundary==cut && cut && json_object_get_boolean(get(original,"active")))){json_object_put(original);free(title);free(path);return fail("native_replay_boundary_invalid");}
  cut=requested_cut=boundary;
 }
 for(size_t i=0;i<cut;i++){
  J *message=json_object_array_get_idx(messages,i),*calls=get(message,"tool_calls");
  if(!json_object_is_type(message,json_type_object)||!str(message,"role")){cut=0;available=0;break;}
  if(calls){if(!json_object_is_type(calls,json_type_array)){available=0;break;}size_t n=json_object_array_length(calls);for(size_t k=0;k<n;k++){J *result=i+1+k<cut?json_object_array_get_idx(messages,i+1+k):NULL;if(!eq(str(result,"role"),"tool")||!eq(str(result,"tool_call_id"),str(json_object_array_get_idx(calls,k),"id"))){if(!json_object_get_boolean(get(original,"active")))available=0;cut=i;break;}}}
 }
 if(!available || (through != -2 && cut != requested_cut)){json_object_put(original);free(title);free(path);return fail("native_fork_history_invalid");}
 J *next=json_object_new_object(),*prefix=json_object_new_array();json_object_object_add(next,"version",json_object_new_int(1));json_object_object_add(next,"messages",prefix);json_object_object_add(next,"active",json_object_new_boolean(0));json_object_object_add(next,"rounds",json_object_new_int(0));
 for(size_t i=0;i<cut;i++)json_object_array_add(prefix,json_object_get(json_object_array_get_idx(messages,i)));
 J *outcomes=dc_outcomes_copy(original,cut);if(!outcomes){json_object_put(original);json_object_put(next);free(title);free(path);return fail("native_fork_outcomes_invalid");}if(json_object_array_length(outcomes))json_object_object_add(next,"_dunlin_turn_outcomes",outcomes);else json_object_put(outcomes);
 char hash[65];const char *raw=json_object_to_json_string_ext(prefix,JSON_C_TO_STRING_PLAIN);dc_sha256(raw,strlen(raw),hash);
 char child[65]="";if(requested)snprintf(child,sizeof child,"%s",requested);else{q=prep(s,"SELECT lower(hex(randomblob(16)))");if(q&&sqlite3_step(q)==SQLITE_ROW)snprintf(child,sizeof child,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);}
 char label[512];snprintf(label,sizeof label,"%s · 分支",title);int made=*child&&dc_add_chat(s,child,label,path);free(title);free(path);json_object_put(original);if(!made){json_object_put(next);return fail("native_fork_create_failed");}
 int ok=sql(s,"BEGIN IMMEDIATE");if(ok){q=prep(s,"UPDATE chats SET state=? WHERE id=?");if(q){bind(q,1,json_object_to_json_string_ext(next,JSON_C_TO_STRING_PLAIN));bind(q,2,child);}ok=step(q);}
 if(ok){q=prep(s,"INSERT INTO extension_forks(chat,parent,through,source_hash) VALUES(?,?,?,?)");if(q){bind(q,1,child);bind(q,2,parent);sqlite3_bind_int64(q,3,(sqlite3_int64)cut-1);bind(q,4,hash);}ok=step(q);}
 if(ok){q=prep(s,"INSERT INTO chat_properties(chat,model) SELECT ?,COALESCE(model,'') FROM chat_properties WHERE chat=? ON CONFLICT(chat) DO UPDATE SET model=excluded.model");if(q){bind(q,1,child);bind(q,2,parent);}ok=step(q);}
 if(ok&&s->attachments){q=prep(s,"INSERT INTO attachment_messages(scope,owner,seq,id,sha,name,kind,source_hash) SELECT scope,?,seq,id,sha,name,kind,source_hash FROM attachment_messages WHERE scope=1 AND owner=? AND seq<?");if(q){bind(q,1,child);bind(q,2,parent);sqlite3_bind_int64(q,3,(sqlite3_int64)cut);}ok=step(q);}
 if(ok){
  char *display=dc_messages(s,child);J *rows=display?dunlin_json_parse(display,strlen(display)):NULL;free(display);if(!json_object_is_type(rows,json_type_array))ok=0;
  for(size_t i=0;ok&&i<json_object_array_length(rows);i++){J *m=json_object_array_get_idx(rows,i);const char *text=str(m,"content");if(!text)continue;q=prep(s,"INSERT INTO index_jobs(chat,seq,text) VALUES(?,?,?)");if(q){bind(q,1,child);sqlite3_bind_int64(q,2,json_object_get_int64(get(m,"sequence")));bind(q,3,text);}ok=step(q);}json_object_put(rows);
 }
 if(ok)ok=sql(s,"COMMIT");if(!ok){sql(s,"ROLLBACK");dc_delete_chat(s,child);json_object_put(next);return fail("native_fork_storage_failed");}json_object_put(next);
 J *out=json_object_new_object();json_object_object_add(out,"id",json_object_new_string(child));json_object_object_add(out,"parentId",json_object_new_string(parent));json_object_object_add(out,"through",json_object_new_int64((sqlite3_int64)cut-1));json_object_object_add(out,"sourceHash",json_object_new_string(hash));return reply(1,NULL,out);
}

static J *client_fork(dc_store *s,J *r){return client_fork_as(s,r,NULL,NULL,-2);}
