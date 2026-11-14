/* Structural browser JobRegistry projection; all ownership and effects are C.
 * Roster pages are immutable by descending scheduler sequence. Observations are
 * non-consuming views of committed source, not a replay or a model request. */
static const char *client_job_state(const char *state){
 if(eq(state,"queued")||eq(state,"running")||eq(state,"waiting"))return "running";
 if(eq(state,"cancel_requested"))return "stopping";
 if(eq(state,"cancelled"))return "killed";
 return eq(state,"succeeded")?"completed":"failed";
}
static J *client_jobs(dc_store *s,J *r){
 const char *method=str(r,"method");J *args=get(r,"arguments");const char *caller=str(args,"sessionId"),*job=str(args,"id");
 if(!id(caller)||(!eq(method,"jobs.list")&&!id(job)))return fail("invalid_job_owner");
 sqlite3_stmt *q=prep(s,"SELECT 1 FROM chats WHERE id=?");if(q)bind(q,1,caller);int exists=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);if(!exists)return fail("job_owner_unavailable");
 const char *owned="COALESCE((SELECT parent.chat FROM agent_jobs parent WHERE parent.id=j.parent),j.chat)";
 char query[2048];J *value=NULL;
 if(eq(method,"jobs.list")){
  J *before=get(args,"before");if(before&&(!json_object_is_type(before,json_type_int)||json_object_get_int64(before)<0))return fail("invalid_job_cursor");
  snprintf(query,sizeof query,"SELECT j.id,j.task,j.state,v.registered,v.finished,j.seq,j.parent FROM agent_jobs j JOIN agent_job_views v ON v.job=j.id WHERE %s=?1 AND (?2=0 OR j.seq<?2) ORDER BY j.seq DESC LIMIT 100",owned);
  q=prep(s,query);if(q){bind(q,1,caller);sqlite3_bind_int64(q,2,json_object_get_int64(before));}value=json_object_new_object();J *rows=json_object_new_array();json_object_object_add(value,"rows",rows);int rc=SQLITE_ERROR;sqlite3_int64 next=0;
  while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){
   J *row=json_object_new_object();const char *task=(const char*)sqlite3_column_text(q,1),*state=(const char*)sqlite3_column_text(q,2),*parent=(const char*)sqlite3_column_text(q,6);size_t length=strlen(task);if(length>1024){length=1024;while(length&&((unsigned char)task[length]&0xc0)==0x80)length--;}
   json_object_object_add(row,"id",json_object_new_string((const char*)sqlite3_column_text(q,0)));json_object_object_add(row,"kind",json_object_new_string(parent&&*parent?"subagent":"dunlin-agent"));json_object_object_add(row,"owner",json_object_new_string(caller));json_object_object_add(row,"label",json_object_new_string_len(task,(int)length));json_object_object_add(row,"status",json_object_new_string(client_job_state(state)));json_object_object_add(row,"detail",json_object_new_string(eq(state,"queued")?"排队等待执行":eq(state,"waiting")?"等待协作任务":eq(state,"interrupted")?"执行已中断，请核对原始会话":eq(state,"running")?"输出在 C 步骤提交后更新":state));
   json_object_object_add(row,"startedAt",json_object_new_int64(sqlite3_column_int64(q,3)*1000));if(sqlite3_column_type(q,4)!=SQLITE_NULL)json_object_object_add(row,"finishedAt",json_object_new_int64(sqlite3_column_int64(q,4)*1000));next=sqlite3_column_int64(q,5);json_object_array_add(rows,row);
  }sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(value);return fail("job_roster_unavailable");}if(json_object_array_length(rows)==100)json_object_object_add(value,"next",json_object_new_int64(next));
 }else{
  snprintf(query,sizeof query,"SELECT j.state,v.output,v.gap,COALESCE(result.status,'') FROM agent_jobs j JOIN agent_job_views v ON v.job=j.id LEFT JOIN agent_job_results result ON result.job=j.id WHERE j.id=?1 AND %s=?2",owned);
  q=prep(s,query);if(q){bind(q,1,job);bind(q,2,caller);}if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("job_scope_invalid");}
  const char *state=(const char*)sqlite3_column_text(q,0);int live=eq(state,"queued")||eq(state,"running")||eq(state,"waiting")||eq(state,"cancel_requested");
  if(eq(method,"jobs.observe")){value=json_object_new_object();json_object_object_add(value,"text",json_object_new_string((const char*)sqlite3_column_text(q,1)));json_object_object_add(value,"gapBefore",json_object_new_boolean(sqlite3_column_int(q,2)));json_object_object_add(value,"streaming",json_object_new_boolean(live));if(eq(state,"failed")||eq(state,"interrupted"))json_object_object_add(value,"error",json_object_new_string(eq(state,"interrupted")?"执行已中断，请核对原始会话":(const char*)sqlite3_column_text(q,3)));sqlite3_finalize(q);}
  else if(eq(method,"jobs.kill")){
   sqlite3_finalize(q);char call[65]="";if(!terminal_audit_begin(s,r,method,call))return fail("invocation_audit_unavailable");dl_agent_scheduler *queue=dl_agent_scheduler_open(s->db,"local",2);int ok=queue&&dl_agent_cancel(queue,job)==DL_SCHED_OK;dl_agent_scheduler_close(queue);
   q=prep(s,"UPDATE extension_calls SET status=?,finished=unixepoch() WHERE id=? AND status='running'");if(q){bind(q,1,ok?"reported":"reported_failure");bind(q,2,call);}if(!step(q))return fail("invocation_finalization_failed");if(!ok)return fail("job_cancel_failed");value=json_object_new_boolean(1);
  }else {sqlite3_finalize(q);return fail("native_action_unsupported");}
 }
 J *out=json_object_new_object();json_object_object_add(out,"value",value);return reply(1,NULL,out);
}
