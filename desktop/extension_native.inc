/* Narrow native admission for the optional DSH host. The worker is already
 * serialized by extension_state; never re-enter it from this callback. */
static int native_effects_settled(dc_store *s,const char *call){
 sqlite3_stmt *q=prep(s,"SELECT 1 FROM extension_native_effects WHERE call=? AND status='running' LIMIT 1");if(q)bind(q,1,call);int rc=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);return rc==SQLITE_DONE;
}
static int native_owned(dc_store *s,const char *source,const char *package,const char *root,const char *chat){
 if(eq(root,chat))return 1;
 sqlite3_stmt *q=prep(s,"WITH RECURSIVE roots(chat) AS(SELECT ?1 UNION SELECT chat FROM extension_client_sessions WHERE root=?1 AND source=?2 AND package=?3),children(chat,depth) AS(SELECT a.chat,1 FROM extension_agent_sessions a JOIN roots r ON a.parent=r.chat WHERE a.source=?2 AND a.package=?3 UNION ALL SELECT a.chat,c.depth+1 FROM extension_agent_sessions a JOIN children c ON a.parent=c.chat WHERE a.source=?2 AND a.package=?3 AND c.depth<8) SELECT 1 FROM roots WHERE chat=?4 UNION ALL SELECT 1 FROM children WHERE chat=?4 LIMIT 1");if(q){bind(q,1,root);bind(q,2,source);bind(q,3,package);bind(q,4,chat);}int ok=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);return ok;
}
static J *native_source(dc_store *s,J *args,const char *chat){
 const char *requested=str(args,"chat");J *offset=get(args,"offset");
 if(!eq(requested,chat)||!json_object_is_type(offset,json_type_int)||json_object_get_int64(offset)<0)return fail("native_source_scope_invalid");
 sqlite3_stmt *q=prep(s,"SELECT c.state,c.title,c.workspace,COALESCE(p.model,''),c.updated FROM chats c LEFT JOIN chat_properties p ON p.chat=c.id WHERE c.id=?");if(q)bind(q,1,chat);
 if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("native_source_unavailable");}
 const char *raw=(const char*)sqlite3_column_text(q,0);if(!raw)raw="{\"version\":1,\"messages\":[],\"active\":false,\"rounds\":0}";
 size_t length=strlen(raw);int64_t start=json_object_get_int64(offset);char hash[65];dc_sha256(raw,length,hash);
 if(length>8388608||(uint64_t)start>length||(start&&((unsigned char)raw[start]&0xc0)==0x80)||(start&&!eq(str(args,"revision"),hash))){sqlite3_finalize(q);return fail("native_source_changed_or_invalid");}
 size_t end=(size_t)start+196608;if(end>length)end=length;while(end<length&&((unsigned char)raw[end]&0xc0)==0x80)end--;
 J *data=json_object_new_object();json_object_object_add(data,"json",json_object_new_string_len(raw+start,(int)(end-(size_t)start)));json_object_object_add(data,"next",json_object_new_int64((int64_t)end));json_object_object_add(data,"total",json_object_new_int64((int64_t)length));json_object_object_add(data,"revision",json_object_new_string(hash));
 const char *keys[]={"title","cwd","model"};for(int i=0;i<3;i++)json_object_object_add(data,keys[i],json_object_new_string((const char*)sqlite3_column_text(q,i+1)));json_object_object_add(data,"updated",json_object_new_int64(sqlite3_column_int64(q,4)*1000));sqlite3_finalize(q);
 J *workspace=dc_chat_workspace_view(s,chat);const char *workspace_keys[]={"device","inode","available"};for(size_t i=0;i<3;i++)if(get(workspace,workspace_keys[i]))json_object_object_add(data,workspace_keys[i],json_object_get(get(workspace,workspace_keys[i])));json_object_put(workspace);
 q=prep(s,"SELECT a.metadata,f.through FROM extension_agent_sessions a JOIN extension_forks f ON f.chat=a.chat WHERE a.chat=?");if(q)bind(q,1,chat);if(q&&sqlite3_step(q)==SQLITE_ROW){const char *text=(const char*)sqlite3_column_text(q,0);J *metadata=text?dunlin_json_parse(text,strlen(text)):NULL;if(!json_object_is_type(metadata,json_type_object)){json_object_put(metadata);sqlite3_finalize(q);json_object_put(data);return fail("native_agent_metadata_invalid");}json_object_object_add(data,"agent",metadata);json_object_object_add(data,"inheritedThrough",json_object_new_int64(sqlite3_column_int64(q,1)));}sqlite3_finalize(q);return reply(1,NULL,data);
}
#include "extension_replay_notice.inc"
#include "extension_native_settings.inc"
static J *native_agent_create(dc_store *s,J *args,const char *source,const char *package,const char *root,const char *call,int replay){
 const char *parent=str(args,"parent"),*child=str(args,"chat"),*revision=str(args,"revision");J *metadata=get(args,"metadata");
 if(!parent||!child||!revision||strlen(revision)!=64||!native_owned(s,source,package,root,parent)||!json_object_is_type(metadata,json_type_object)||strlen(json_object_to_json_string_ext(metadata,JSON_C_TO_STRING_PLAIN))>32768)return fail("native_agent_creation_invalid");
 if(!eq(str(metadata,"parentSession"),parent)||(replay ? (!json_object_get_boolean(get(metadata,"isSeeded"))||get(metadata,"origin")||!json_object_get_boolean(get(metadata,"checkpointReplay"))||(!json_object_is_type(get(args,"through"),json_type_int)||json_object_get_int64(get(args,"through"))< -1)):!eq(str(metadata,"origin"),"subagent")))return fail("native_agent_metadata_invalid");
 J *request=json_object_new_object(),*arguments=json_object_new_object();json_object_object_add(request,"arguments",arguments);json_object_object_add(arguments,"sessionId",json_object_new_string(parent));J *result=client_fork_as(s,request,child,revision,replay?json_object_get_int64(get(args,"through")):-2);json_object_put(request);if(!json_object_get_boolean(get(result,"ok")))return result;
 sqlite3_stmt *q=prep(s,"INSERT INTO extension_agent_sessions(chat,source,package,parent,metadata,created_call) VALUES(?,?,?,?,?,?)");if(q){const char *v[]={child,source,package,parent,json_object_to_json_string_ext(metadata,JSON_C_TO_STRING_PLAIN),call};for(int i=0;i<6;i++)bind(q,i+1,v[i]);}
 if(!step(q)){json_object_put(result);int removed=dc_delete_chat(s,child);return fail(removed?"native_agent_metadata_unavailable":"native_agent_cleanup_required");}return result;
}
static J *native_agent_inbox(dc_store *s,const char *chat){
 sqlite3_stmt *q=prep(s,"SELECT kind,body FROM extension_agent_inputs WHERE chat=? AND status='queued' ORDER BY rowid");if(q)bind(q,1,chat);J *data=json_object_new_object(),*step_rows=json_object_new_array(),*turn_rows=json_object_new_array();json_object_object_add(data,"nextStep",step_rows);json_object_object_add(data,"nextTurn",turn_rows);int rc=SQLITE_ERROR;
 while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,1);J *message=raw?dunlin_json_parse(raw,strlen(raw)):NULL;if(!json_object_is_type(message,json_type_object)){json_object_put(message);rc=SQLITE_ERROR;break;}json_object_array_add(eq((const char*)sqlite3_column_text(q,0),"inject")?step_rows:turn_rows,message);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(data);return fail("native_agent_inbox_unavailable");}return reply(1,NULL,data);
}
static J *native_agent_control(dc_store *s,const char *method,J *args,const char *chat,const char *call){
 sqlite3_stmt *q=NULL;
 if(eq(method,"agent.model")){
  J *selection=get(args,"selection");const char *model=str(selection,"model");if(!eq(str(selection,"provider"),"dunlin-native")||!model||!*model||strlen(model)>64||get(selection,"reasoningEffort"))return fail("native_model_selection_invalid");
  q=prep(s,"SELECT revision FROM configuration_registry WHERE singleton=1");int64_t revision=q&&sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int64(q,0):-1;sqlite3_finalize(q);J *check=json_object_new_object();json_object_object_add(check,"op",json_object_new_string("config.model"));json_object_object_add(check,"id",json_object_new_string(model));json_object_object_add(check,"revision",json_object_new_int64(revision));J *result=dc_config_request(s,check);json_object_put(check);int valid=json_object_get_boolean(get(result,"ok"));json_object_put(result);if(!valid)return fail("native_model_configuration_changed");
  J *update=json_object_new_object();json_object_object_add(update,"op",json_object_new_string("chats.update"));json_object_object_add(update,"id",json_object_new_string(chat));json_object_object_add(update,"field",json_object_new_string("model"));json_object_object_add(update,"value",json_object_new_string(model));result=ui_workspace_call(s,update);json_object_put(update);if(!json_object_get_boolean(get(result,"ok")))return result;json_object_put(result);J *data=json_object_new_object();json_object_object_add(data,"selection",json_object_get(selection));return reply(1,NULL,data);
 }
 if(eq(method,"agent.status")){
  q=prep(s,"SELECT 1 FROM agent_jobs WHERE chat=? AND state IN('running','waiting','cancel_requested') LIMIT 1");if(q)bind(q,1,chat);int rc=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)return fail("native_agent_status_unavailable");J *data=json_object_new_object();json_object_object_add(data,"status",json_object_new_string(rc==SQLITE_ROW?"running":"idle"));return reply(1,NULL,data);
 }
 if(eq(method,"agent.inject")){
  J *message=get(args,"message"),*content=get(message,"content");const char *mid=str(message,"id"),*kind=str(get(message,"source"),"kind");
  if(!mid||!*mid||strlen(mid)>128||!eq(str(message,"role"),"user")||!kind||strncmp(kind,"plugin:",7)||!json_object_is_type(content,json_type_array))return fail("native_agent_injection_invalid");
  const char *raw=json_object_to_json_string_ext(message,JSON_C_TO_STRING_PLAIN);if(strlen(raw)>65536)return fail("native_agent_input_exceeds_limit");char *plain=native_input_text(message,65536);if(!plain)return fail("native_agent_injection_content_invalid");free(plain);
  q=prep(s,"SELECT count(*),COALESCE(sum(length(CAST(body AS BLOB))),0) FROM extension_agent_inputs WHERE chat=? AND status='queued'");if(q)bind(q,1,chat);int available=q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_int64(q,0)<64&&sqlite3_column_int64(q,1)+(sqlite3_int64)strlen(raw)<=524288;sqlite3_finalize(q);if(!available)return fail("native_agent_inbox_limit");
  q=prep(s,"INSERT INTO extension_agent_inputs(chat,id,kind,body,call) VALUES(?,?,'inject',?,?)");if(q){bind(q,1,chat);bind(q,2,mid);bind(q,3,raw);bind(q,4,call);}return step(q)?reply(1,NULL,json_object_new_object()):fail("native_agent_input_conflict");
 }
 if(eq(method,"agent.dequeue")){
  const char *mid=str(args,"id");if(get(args,"id")&&(!mid||!*mid||strlen(mid)>128))return fail("native_agent_input_invalid");
  q=prep(s,"SELECT id,job,kind FROM extension_agent_inputs WHERE chat=?1 AND status='queued' AND (?2='' OR id=?2) ORDER BY rowid");if(q){bind(q,1,chat);bind(q,2,mid);}J *rows=json_object_new_array();int rc=SQLITE_ERROR;while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){J *row=json_object_new_object();const char *keys[]={"id","job","kind"};for(int i=0;i<3;i++)json_object_object_add(row,keys[i],json_object_new_string((const char*)sqlite3_column_text(q,i)));json_object_array_add(rows,row);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(rows);return fail("native_agent_inbox_unavailable");}
  int removed=0,ok=1;for(size_t i=0;ok&&i<json_object_array_length(rows);i++){
   J *row=json_object_array_get_idx(rows,i);const char *job=str(row,"job"),*input=str(row,"id");if(*job&&eq(str(row,"kind"),"inject")){if(mid){ok=0;break;}continue;}
   if(*job){dl_agent_scheduler *queue=dl_agent_scheduler_open(s->db,"local",2);dl_sched_status status=queue?dl_agent_cancel_queued(queue,job):DL_SCHED_STORAGE;
    if(status==DL_SCHED_STALE){dl_agent_job value={0};status=dl_agent_job_get(queue,job,&value);int settled=status==DL_SCHED_OK&&(!strcmp(value.state,"cancelled")||!strcmp(value.state,"interrupted")||!strcmp(value.state,"failed")||!strcmp(value.state,"succeeded"));dl_agent_job_clear(&value);if(!settled){dl_agent_scheduler_close(queue);continue;}status=DL_SCHED_OK;}
    dl_agent_scheduler_close(queue);if(status!=DL_SCHED_OK&&status!=DL_SCHED_EMPTY){ok=0;break;}
    q=prep(s,"UPDATE extension_agent_inputs SET job='' WHERE chat=? AND job=? AND kind='inject' AND status='queued'");if(q){bind(q,1,chat);bind(q,2,job);}if(!step(q)){ok=0;break;}
   }
   q=prep(s,"UPDATE extension_agent_inputs SET status='cancelled' WHERE chat=? AND id=? AND status='queued'");if(q){bind(q,1,chat);bind(q,2,input);}ok=step(q);if(ok)removed+=sqlite3_changes(s->db);
  }
  json_object_put(rows);if(!ok)return fail("native_agent_dequeue_failed_or_context_frozen");
  if(!mid){q=prep(s,"UPDATE extension_agent_inputs SET status='cancelled' WHERE chat=? AND job='' AND status='queued'");if(q)bind(q,1,chat);ok=step(q);if(!ok)return fail("native_agent_dequeue_failed");removed+=sqlite3_changes(s->db);}
  q=prep(s,"SELECT count(*) FROM extension_agent_inputs WHERE chat=? AND status='queued'");if(q)bind(q,1,chat);int remaining=q&&sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int(q,0):-1;sqlite3_finalize(q);if(remaining<0)return fail("native_agent_inbox_unavailable");J *data=json_object_new_object();json_object_object_add(data,"removed",json_object_new_int(removed));json_object_object_add(data,"remaining",json_object_new_int(remaining));return reply(1,NULL,data);
 }
 if(eq(method,"agent.cancel")){
  q=prep(s,"SELECT id FROM agent_jobs WHERE chat=? AND state IN('queued','running','waiting')");if(q)bind(q,1,chat);J *ids=json_object_new_array();int rc=SQLITE_ERROR;while(q&&(rc=sqlite3_step(q))==SQLITE_ROW)json_object_array_add(ids,json_object_new_string((const char*)sqlite3_column_text(q,0)));sqlite3_finalize(q);int ok=rc==SQLITE_DONE;dl_agent_scheduler *queue=dl_agent_scheduler_open(s->db,"local",2);if(!queue)ok=0;for(size_t i=0;ok&&i<json_object_array_length(ids);i++)ok=dl_agent_cancel(queue,json_object_get_string(json_object_array_get_idx(ids,i)))==DL_SCHED_OK;dl_agent_scheduler_close(queue);json_object_put(ids);return ok?reply(1,NULL,json_object_new_object()):fail("native_agent_cancel_failed");
 }
 return fail("native_method_unavailable");
}
static char *native_admission(void *raw_store,const char *raw,const dc_hooks *hooks){
 dc_store *s=raw_store;J *request=dunlin_json_parse(raw,strlen(raw)),*result=NULL;const char *call=str(request,"nativeCall");
 if(hooks&&hooks->cancelled&&hooks->cancelled(hooks->user)){result=fail("cancelled");goto done;}
 if(!call||strlen(call)!=32){result=fail("native_admission_unavailable");goto done;}
 sqlite3_stmt *q=prep(s,"SELECT c.source,c.package,c.version,n.instance,n.chat FROM extension_calls c JOIN extension_native_scopes n ON n.call=c.id WHERE c.id=? AND c.status='running'");if(q)bind(q,1,call);
 char source[257]="",package[257]="",version[257]="",instance[65]="",chat[65]="";
 int valid=q&&sqlite3_step(q)==SQLITE_ROW;if(valid){char *out[]={source,package,version,instance,chat};size_t sizes[]={sizeof source,sizeof package,sizeof version,sizeof instance,sizeof chat};for(int i=0;i<5;i++){const char *v=(const char*)sqlite3_column_text(q,i);if(!v||strlen(v)>=sizes[i]){valid=0;break;}snprintf(out[i],sizes[i],"%s",v);}}sqlite3_finalize(q);
 char identity[65];package_identity(source,package,identity);J *active=get(state(s)->active,identity);
 if(!valid||!*chat||!eq(str(active,"instance"),instance)||!eq(str(active,"version"),version)){result=fail("native_admission_owner_changed");goto done;}
 if(!native_effects_settled(s,call)){result=fail("native_effect_receipt_unknown");goto done;}
 const char *method=str(request,"method");
 J *args=get(request,"arguments");const char *requested=str(args,"chat");
 if(!eq(method,"settings.applied")&&!native_settings_settled(s,call)){result=fail("native_settings_acknowledgement_required");goto done;}
 if(eq(method,"session.workspace")){
  J *view=requested&&native_owned(s,source,package,chat,requested)?dc_chat_workspace_view(s,requested):NULL;result=view?reply(1,NULL,view):fail("native_workspace_scope_invalid");
 }
 else if(eq(method,"helper.models")||eq(method,"helper.poll"))result=eq(package,"dsh-project-manager")?dc_helpers_native(s,method,args,call):fail("native_helper_owner_invalid");
 else if(eq(method,"settings.hash"))result=eq(package,"dsh-checkpoint-rewind")?native_settings_hash(s,source,package,version):fail("native_settings_owner_invalid");
 else if(eq(method,"session.source"))result=requested&&native_owned(s,source,package,chat,requested)?native_source(s,args,requested):fail("native_source_scope_invalid");
 else if(eq(method,"agent.progress"))result=requested&&native_owned(s,source,package,chat,requested)?reply(1,NULL,dl_live_stream_snapshot(s->live_stream,requested)):fail("native_agent_scope_invalid");
 else if(eq(method,"agent.inbox")||eq(method,"agent.status"))result=requested&&native_owned(s,source,package,chat,requested)?eq(method,"agent.inbox")?native_agent_inbox(s,requested):native_agent_control(s,method,args,requested,call):fail("native_agent_scope_invalid");
 else if(eq(method,"agent.models")){
  q=prep(s,"SELECT models FROM configuration_registry WHERE singleton=1");J *models=NULL;if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);if(raw)models=dunlin_json_parse(raw,strlen(raw));}sqlite3_finalize(q);if(!json_object_is_type(models,json_type_array)){json_object_put(models);result=fail("native_model_catalog_unavailable");goto done;}
  J *data=json_object_new_object(),*groups=json_object_new_array(),*group=json_object_new_object(),*rows=json_object_new_array();json_object_object_add(data,"groups",groups);json_object_object_add(data,"failures",json_object_new_array());json_object_object_add(group,"id",json_object_new_string("dunlin-native"));json_object_object_add(group,"name",json_object_new_string("Dunlin C"));json_object_object_add(group,"models",rows);json_object_array_add(groups,group);for(size_t i=0;i<json_object_array_length(models);i++){J *model=json_object_array_get_idx(models,i);const char *mid=str(model,"id"),*name=str(model,"name");if(!mid||!name)continue;J *row=json_object_new_object();json_object_object_add(row,"id",json_object_new_string(mid));json_object_object_add(row,"name",json_object_new_string(name));json_object_array_add(rows,row);}json_object_put(models);result=reply(1,NULL,data);
 }
 else if(eq(method,"helper.submit")||eq(method,"helper.cancel")||eq(method,"settings.commit")||eq(method,"settings.applied")||eq(method,"agent.create")||eq(method,"session.replay")||eq(method,"session.notice")||eq(method,"session.rename")||eq(method,"agent.inject")||eq(method,"agent.dequeue")||eq(method,"agent.cancel")||eq(method,"agent.followup")||eq(method,"agent.model")){
  const char *serialized=json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN);char hash[65];dc_sha256(serialized,strlen(serialized),hash);
  q=prep(s,"INSERT INTO extension_native_effects(call,sequence,method,args_hash,status) VALUES(?,?,?,?,'running')");if(q){bind(q,1,call);sqlite3_bind_int64(q,2,json_object_get_int64(get(request,"sequence")));bind(q,3,method);bind(q,4,hash);}if(!step(q)){result=fail("native_effect_audit_unavailable");goto done;}
  if(eq(method,"helper.submit")||eq(method,"helper.cancel")){json_object_object_add(args,"_sequence",json_object_get(get(request,"sequence")));result=eq(package,"dsh-project-manager")?dc_helpers_native(s,method,args,call):fail("native_helper_owner_invalid");}
  else if(eq(method,"settings.commit")||eq(method,"settings.applied"))result=native_settings_update(s,method,args,source,package,version,call,json_object_get_int64(get(request,"sequence")));
  else if(eq(method,"agent.create")||eq(method,"session.replay"))result=native_agent_create(s,args,source,package,chat,call,eq(method,"session.replay"));
  else if(eq(method,"session.notice"))result=native_replay_notice(s,args,source,package,call);
  else if(eq(method,"agent.followup"))result=requested&&native_owned(s,source,package,chat,requested)?native_followup(s,args,requested,call,source,package,version,instance,json_object_get_int64(get(request,"sequence"))):fail("native_agent_scope_invalid");
  else if(eq(method,"session.rename")){const char *title=str(args,"title");result=requested&&native_owned(s,source,package,chat,requested)&&title&&*title&&strlen(title)<=512?dc_rename_chat(s,requested,title)?reply(1,NULL,json_object_new_boolean(1)):fail("native_session_rename_failed"):fail("native_session_rename_invalid");}
  else result=requested&&native_owned(s,source,package,chat,requested)?native_agent_control(s,method,args,requested,call):fail("native_agent_scope_invalid");
  q=prep(s,"UPDATE extension_native_effects SET status=? WHERE call=? AND sequence=?");if(q){bind(q,1,json_object_get_boolean(get(result,"ok"))?"reported":"reported_failure");bind(q,2,call);sqlite3_bind_int64(q,3,json_object_get_int64(get(request,"sequence")));}if(!step(q)){json_object_put(result);result=fail("native_effect_finalization_unavailable");}
 }else result=fail("native_method_unavailable");
 done:json_object_put(request);if(!result)result=fail("native_admission_failed");char *text=strdup(json_object_to_json_string_ext(result,JSON_C_TO_STRING_PLAIN));json_object_put(result);return text;
}
