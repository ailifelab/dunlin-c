/* HTTP work can outlive a bridge call. Keep the original intent running until
 * an owner-bound terminal receipt or a confirmed disposal; never replay it. */
static int pending_finish(dc_store *s,const char *handle,const char *status){
 if(!handle||!sqlite3_get_autocommit(s->db)||!sql(s,"BEGIN IMMEDIATE"))return 0;
 sqlite3_stmt *q=prep(s,"UPDATE extension_calls SET status=?,finished=unixepoch() WHERE status='running' AND id=(SELECT call FROM extension_requests WHERE id=? AND state='running')");
 if(q){bind(q,1,status);bind(q,2,handle);}int ok=step(q);
 if(ok){q=prep(s,"UPDATE extension_requests SET state=? WHERE id=? AND state='running'");if(q){bind(q,1,status);bind(q,2,handle);}ok=step(q);}
 if(ok){q=prep(s,"SELECT call FROM extension_requests WHERE id=?");if(q)bind(q,1,handle);char original[65]="";int rc=q?sqlite3_step(q):SQLITE_ERROR;if(rc==SQLITE_ROW)snprintf(original,sizeof original,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);ok=rc==SQLITE_ROW&&dc_helpers_cancel_call(s,original);}
 if(!sql(s,ok?"COMMIT":"ROLLBACK")){sql(s,"ROLLBACK");ok=0;}return ok;
}
static int pending_close(dc_store *s,const char *source,const char *package,const char *surface,const char *status){
 sqlite3_stmt *q=prep(s,"SELECT id FROM extension_requests WHERE source=? AND package=? AND state='running' AND (? IS NULL OR surface=?)");
 if(q){bind(q,1,source);bind(q,2,package);if(surface){bind(q,3,surface);bind(q,4,surface);}else{sqlite3_bind_null(q,3);sqlite3_bind_null(q,4);}}
 J *ids=json_object_new_array();int rc=SQLITE_ERROR;
 while(q&&(rc=sqlite3_step(q))==SQLITE_ROW)json_object_array_add(ids,json_object_new_string((const char*)sqlite3_column_text(q,0)));
 sqlite3_finalize(q);int ok=rc==SQLITE_DONE;
 for(size_t i=0;ok&&i<json_object_array_length(ids);i++)ok=pending_finish(s,json_object_get_string(json_object_array_get_idx(ids,i)),status);
 json_object_put(ids);return ok;
}
static int pending_begin(dc_store *s,J *r,J *active,const char *version,const char *call,const char *handle){
 J *args=get(r,"arguments");const char *surface=str(args,"_surface");
 if(!id(handle)||(surface&&strlen(surface)>128))return 0;
 const char *url=str(args,"url"),*cid=NULL;
 if(eq(str(r,"package"),"dsh-project-manager")&&(eq(url,"/_dsh/dsh-project-manager/api/session/bind")||eq(url,"/_dsh/dsh-project-manager/session/bind")))cid=str(get(args,"body"),"sessionId");
 sqlite3_stmt *q=prep(s,"INSERT INTO extension_requests(id,call,source,package,version,instance,surface,chat,native_session) VALUES(?,?,?,?,?,?,?,?,?)");
 if(q){const char *values[]={handle,call,str(r,"source"),str(r,"package"),version,str(active,"instance"),surface,str(r,"chat"),cid};for(int i=0;i<9;i++)bind(q,i+1,values[i]);}return step(q);
}
static int pending_receipt(dc_store *s,const char *handle,J *response){
 sqlite3_stmt *receipt=prep(s,"SELECT call FROM extension_requests WHERE id=?");if(receipt)bind(receipt,1,handle);char original[65]="";int row=receipt?sqlite3_step(receipt):SQLITE_ERROR;if(row==SQLITE_ROW)snprintf(original,sizeof original,"%s",sqlite3_column_text(receipt,0));sqlite3_finalize(receipt);if(row!=SQLITE_ROW||!native_jobs_receipt(s,original,response))return 0;
 const char *title=str(get(response,"body"),"title");if(!title||!json_object_get_boolean(get(get(response,"body"),"ok")))return 1;
 sqlite3_stmt *q=prep(s,"SELECT native_session FROM extension_requests WHERE id=? AND state='running'");if(q)bind(q,1,handle);char cid[257]="";int rc=q?sqlite3_step(q):SQLITE_ERROR;if(rc==SQLITE_ROW)snprintf(cid,sizeof cid,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);return rc==SQLITE_DONE||(rc==SQLITE_ROW&&(!*cid||dc_rename_chat(s,cid,title)));
}
static int pending_owned(dc_store *s,J *r,J *active){
 J *args=get(r,"arguments");if(!id(str(args,"id")))return 0;
 sqlite3_stmt *q=prep(s,"SELECT 1 FROM extension_requests WHERE id=? AND source=? AND package=? AND version=? AND instance=? AND surface=? AND chat=? AND state<>'unknown'");
 if(q){const char *v[]={str(args,"id"),str(r,"source"),str(r,"package"),str(active,"version"),str(active,"instance"),str(args,"_surface"),str(r,"chat")};for(int i=0;i<7;i++)bind(q,i+1,v[i]);}
 int ok=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);return ok;
}
static int chat_busy_unlocked(dc_store *s,const char *chat){
 if(dc_helpers_chat_busy(s,chat))return 1;
 extension_state *e=s->extensions;if(e&&dl_terminal_chat_busy(e->terminals,chat))return 1;
 sqlite3_stmt *q=prep(s,e?"SELECT 1 FROM extension_requests WHERE chat=?1 AND state='running' LIMIT 1":"SELECT 1 FROM extension_requests WHERE chat=?1 AND state='running' UNION ALL SELECT 1 FROM extension_terminal_owners WHERE chat=?1 AND state='running' LIMIT 1");if(q)bind(q,1,chat);int rc=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);return rc!=SQLITE_DONE;
}

int dc_extensions_chat_busy(dc_store *s,const char *chat){if(s&&s->lock<0)return chat_busy_unlocked(s,chat);void *scope=dc_extensions_scope_enter(s);if(!scope)return 1;int out=chat_busy_unlocked(s,chat);dc_extensions_scope_leave(scope);return out;}

int dc_extensions_forget_chat(dc_store *s,const char *chat){void *scope=dc_extensions_scope_enter(s);if(!scope)return 0;extension_state *e=scope;int ok=dl_terminal_forget_chat(e->terminals,chat);dc_extensions_scope_leave(scope);return ok;}
