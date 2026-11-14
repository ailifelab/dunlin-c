/* Desktop-only native workbench state. Included after lifecycle helpers so
 * every mutation validates the same active package/version before dispatch. */
static J *ui_workspace_call(dc_store *s,J *request){
 const char *raw=json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN);char *text=dc_workspace_request(s,raw);J *out=text?dunlin_json_parse(text,strlen(text)):NULL;free(text);return out?out:fail("storage_error");
}
static J *ui_host_call(dc_store *s,const char *source,const char *package,const char *version,const char *chat,const char *method,J *args,const dc_hooks *hooks){
 J *r=json_object_new_object();json_object_object_add(r,"source",json_object_new_string(source));json_object_object_add(r,"package",json_object_new_string(package));json_object_object_add(r,"version",json_object_new_string(version));if(chat)json_object_object_add(r,"chat",json_object_new_string(chat));json_object_object_add(r,"command",json_object_new_string(method));json_object_object_add(r,"arguments",args?json_object_get(args):json_object_new_object());J *out=command(s,r,hooks);json_object_put(r);return out;
}
#include "extension_settings.inc"
#include "extension_terminal.inc"
#include "extension_jobs.inc"
static J *client_context(dc_store *s,const char *chat){
 char *raw=dc_chats(s);J *chats=raw?dunlin_json_parse(raw,strlen(raw)):NULL;free(raw);if(!json_object_is_type(chats,json_type_array)){json_object_put(chats);return fail("storage_error");}
 J *req=json_object_new_object();json_object_object_add(req,"op",json_object_new_string("projects.list"));J *projects=ui_workspace_call(s,req);json_object_put(req);if(!json_object_get_boolean(get(projects,"ok"))){json_object_put(chats);return projects;}
 J *out=json_object_new_object(),*sessions=json_object_new_object(),*ids=json_object_new_array(),*by_id=json_object_new_object(),*workspaces=json_object_new_object(),*items=json_object_new_array(),*archived=json_object_new_array();
 for(size_t i=0;i<json_object_array_length(chats);i++){
  J *row=json_object_array_get_idx(chats,i);const char *cid=str(row,"id");if(!cid)continue;J *v=json_object_new_object();const char *fields[]={"id","title","updated","archived","active","project"};
  for(size_t k=0;k<sizeof fields/sizeof *fields;k++)json_object_object_add(v,fields[k],json_object_get(get(row,fields[k])));
  json_object_object_add(v,"displayTitle",json_object_get(get(row,"title")));
  json_object_object_add(v,"cwd",json_object_get(get(row,"workspace")));json_object_object_add(v,"updatedAt",json_object_new_int64(json_object_get_int64(get(row,"updated"))*1000));json_object_object_add(v,"phase",json_object_new_string(json_object_get_boolean(get(row,"active"))?"pending":"idle"));
  sqlite3_stmt *running=prep(s,"SELECT EXISTS(SELECT 1 FROM run_records WHERE chat=?1 AND status='running') OR EXISTS(SELECT 1 FROM agent_jobs WHERE chat=?1 AND state IN('running','waiting','cancel_requested'))");if(running)bind(running,1,cid);if(!running||sqlite3_step(running)!=SQLITE_ROW){sqlite3_finalize(running);json_object_put(v);json_object_put(chats);json_object_put(projects);json_object_put(out);json_object_put(sessions);json_object_put(ids);json_object_put(by_id);json_object_put(workspaces);json_object_put(items);json_object_put(archived);return fail("native_session_status_unavailable");}int is_running=sqlite3_column_int(running,0);sqlite3_finalize(running);json_object_object_add(v,"running",json_object_new_boolean(is_running));json_object_object_add(v,"phase",json_object_new_string(is_running?"pending":"idle"));
  sqlite3_stmt *blank=prep(s,"SELECT state IS NULL OR json_array_length(json_extract(state,'$.messages'))=0 FROM chats WHERE id=?");if(blank)bind(blank,1,cid);if(blank&&sqlite3_step(blank)==SQLITE_ROW)json_object_object_add(v,"blank",json_object_new_boolean(sqlite3_column_int(blank,0)));sqlite3_finalize(blank);
  sqlite3_stmt *fq=prep(s,"SELECT parent,through FROM extension_forks WHERE chat=?");if(fq)bind(fq,1,cid);if(fq&&sqlite3_step(fq)==SQLITE_ROW){json_object_object_add(v,"parentId",json_object_new_string((const char*)sqlite3_column_text(fq,0)));json_object_object_add(v,"forkThrough",json_object_new_int64(sqlite3_column_int64(fq,1)));}sqlite3_finalize(fq);
  sqlite3_stmt *agent=prep(s,"SELECT parent FROM extension_agent_sessions WHERE chat=?");if(agent)bind(agent,1,cid);if(agent&&sqlite3_step(agent)==SQLITE_ROW){json_object_object_add(v,"origin",json_object_new_string("subagent"));json_object_object_add(v,"parentId",json_object_new_string((const char*)sqlite3_column_text(agent,0)));}sqlite3_finalize(agent);
  json_object_object_add(by_id,cid,v);json_object_array_add(ids,json_object_new_string(cid));if(json_object_get_boolean(get(row,"archived")))json_object_array_add(archived,json_object_new_string(cid));
 }
 J *rows=get(projects,"data");for(size_t i=0;i<json_object_array_length(rows);i++){
  J *p=json_object_array_get_idx(rows,i),*w=json_object_new_object(),*members=json_object_new_array();const char *pid=str(p,"id");if(!pid){json_object_put(w);json_object_put(members);continue;}
  json_object_object_add(w,"workspaceId",json_object_new_string(pid));json_object_object_add(w,"name",json_object_get(get(p,"name")));json_object_object_add(w,"title",json_object_get(get(p,"name")));json_object_object_add(w,"cwd",json_object_get(get(p,"path")));json_object_object_add(w,"path",json_object_get(get(p,"path")));json_object_object_add(w,"revision",json_object_get(get(p,"revision")));
  for(size_t n=0;n<json_object_array_length(chats);n++){J *c=json_object_array_get_idx(chats,n);if(eq(str(c,"project"),pid))json_object_array_add(members,json_object_get(get(c,"id")));}json_object_object_add(w,"sessionIds",members);json_object_array_add(items,w);
 }
 /* A directory-bound ordinary chat is a workspace, without becoming a project. */
 J *binding=chat?dc_chat_workspace_view(s,chat):NULL,*current=get(by_id,chat?chat:"");int represented=0;
 for(size_t i=0;i<json_object_array_length(items);i++){J *members=get(json_object_array_get_idx(items,i),"sessionIds");for(size_t n=0;n<json_object_array_length(members);n++)if(eq(json_object_get_string(json_object_array_get_idx(members,n)),chat))represented=1;}
 if(!represented&&current&&json_object_get_boolean(get(binding,"available"))){
  const char *path=str(binding,"path");char hash[65],key[80];dc_sha256(path,strlen(path),hash);snprintf(key,sizeof key,"native-path:%s",hash);J *w=json_object_new_object(),*members=json_object_new_array();
  json_object_object_add(w,"workspaceId",json_object_new_string(key));json_object_object_add(w,"kind",json_object_new_string("native-session-workspace"));json_object_object_add(w,"title",json_object_new_string("当前聊天工作区"));json_object_object_add(w,"name",json_object_new_string("当前聊天工作区"));json_object_object_add(w,"path",json_object_new_string(path));json_object_object_add(w,"cwd",json_object_new_string(path));
  for(size_t i=0;i<json_object_array_length(chats);i++){J *row=json_object_array_get_idx(chats,i);if(eq(str(row,"workspace"),path))json_object_array_add(members,json_object_get(get(row,"id")));}json_object_object_add(w,"sessionIds",members);json_object_array_add(items,w);
 }json_object_put(binding);
 json_object_object_add(sessions,"ids",ids);json_object_object_add(sessions,"byId",by_id);json_object_object_add(sessions,"phase",json_object_new_string("ready"));json_object_object_add(workspaces,"items",items);json_object_object_add(workspaces,"archivedSessionIds",archived);json_object_object_add(out,"sessions",sessions);json_object_object_add(out,"workspaces",workspaces);json_object_object_add(out,"current",chat?json_object_new_string(chat):NULL);json_object_put(chats);json_object_put(projects);return reply(1,NULL,out);
}
static int client_session_owner(dc_store *s,const char *chat,const char *source,const char *package,const char *root){
 sqlite3_stmt *q=prep(s,"INSERT INTO extension_client_sessions(chat,source,package,root) VALUES(?,?,?,?)");if(q){bind(q,1,chat);bind(q,2,source);bind(q,3,package);bind(q,4,root);}return step(q);
}
static J *client_native(dc_store *s,J *r,const dc_hooks *hooks){
 const char *source=str(r,"source"),*package=str(r,"package"),*version=str(r,"version");if(!id(source)||!id(package)||!id(version))return fail("invalid_extension_reference");
 char identity[65];package_identity(source,package,identity);extension_state *e=state(s);J *active=get(e->active,identity);if(!active||!eq(str(active,"version"),version)||!dc_worker_alive(e->dsh))return fail("extension_version_changed");if(get(r,"instance")&&!eq(str(r,"instance"),str(active,"instance")))return fail("extension_instance_changed");
 sqlite3_stmt *check=prep(s,"SELECT quarantined FROM extension_versions WHERE source=? AND package=? AND version=?");if(check){bind(check,1,source);bind(check,2,package);bind(check,3,version);}int valid=check&&sqlite3_step(check)==SQLITE_ROW&&!sqlite3_column_int(check,0);sqlite3_finalize(check);if(!valid)return fail("extension_quarantined");
 const char *op=str(r,"op"),*chat=str(r,"chat");if(eq(op,"extensions.clientContext")){
  J *snapshot=client_context(s,chat);if(!json_object_get_boolean(get(snapshot,"ok")))return snapshot;if(eq(package,"dshmarket"))json_object_object_add(get(snapshot,"data"),"managedInstall",market_install_policy());
  J *sdk=ui_host_call(s,source,package,version,chat,"ui.workspaces",NULL,hooks),*items=get(get(get(snapshot,"data"),"workspaces"),"items"),*extra=get(get(sdk,"data"),"data");
  if(!json_object_get_boolean(get(sdk,"ok"))){json_object_put(snapshot);return sdk;}
  for(size_t i=0;i<json_object_array_length(extra);i++){
   J *row=json_object_array_get_idx(extra,i);int duplicate=0;for(size_t n=0;n<json_object_array_length(items);n++)if(eq(str(json_object_array_get_idx(items,n),"workspaceId"),str(row,"workspaceId")))duplicate=1;
   if(!duplicate){
    /* SDK sessions are an engine-nonce cache. At cold start its registry may
     * prune old cached headers, while the C sessions remain valid. Derive
     * membership from the canonical path AND the pinned directory identity;
     * never publish stale/orphan SDK-only ids or grant model authority here. */
    J *members=json_object_new_array(),*by_id=get(get(get(snapshot,"data"),"sessions"),"byId");const char *path=str(row,"path");
    if(by_id&&path){json_object_object_foreach(by_id,cid,session){if(eq(str(session,"cwd"),path)){J *view=dc_chat_workspace_view(s,cid);if(json_object_get_boolean(get(view,"available"))&&eq(str(view,"path"),path))json_object_array_add(members,json_object_new_string(cid));json_object_put(view);}}}
    json_object_object_add(row,"sessionIds",members);json_object_array_add(items,json_object_get(row));
   }
  }json_object_put(sdk);return snapshot;
 }
 if(eq(op,"extensions.settings.apply")){
  J *config=configuration_request(s,r);if(!json_object_get_boolean(get(config,"ok")))return config;const char *hash=str(get(config,"data"),"hash"),*expected=str(r,"expectedHash");if(!expected||!eq(hash,expected)){json_object_put(config);return fail("extension_configuration_changed");}json_object_put(config);
  return client_settings_apply(s,r,hooks);
 }
 const char *method=str(r,"method");J *args=get(r,"arguments");if(!method||!json_object_is_type(args,json_type_object))return fail("invalid_native_action");
 if(!strncmp(method,"terminal.",9))return client_terminal(s,r,active,identity);
 if(!strncmp(method,"jobs.",5))return client_jobs(s,r);
 J *value=NULL;
 if(eq(method,"session.prompt")){
  const char *cid=str(args,"id");if(!id(cid)||!eq(str(args,"mode"),"queue"))return fail("invalid_native_prompt");
  J *response=ui_host_call(s,source,package,version,cid,"ui.prompt",args,hooks);if(!json_object_get_boolean(get(response,"ok")))return response;
  J *accepted=get(get(get(get(response,"data"),"data"),"body"),"value");if(!json_object_get_boolean(get(accepted,"accepted"))||!str(accepted,"messageId")){json_object_put(response);return fail("native_prompt_receipt_invalid");}
  J *out=json_object_new_object();json_object_object_add(out,"value",json_object_get(accepted));json_object_put(response);return reply(1,NULL,out);
 }else if(eq(method,"session.create")){
  const char *project=str(args,"workspaceId");char root[PATH_MAX]="",cid[65]="";
  if(project&&*project){sqlite3_stmt *q=prep(s,"SELECT path FROM projects WHERE id=?");if(q)bind(q,1,project);if(q&&sqlite3_step(q)==SQLITE_ROW)snprintf(root,sizeof root,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);}
  if(!*root&&project&&*project){J *args=json_object_new_object();json_object_object_add(args,"id",json_object_new_string(project));J *response=ui_host_call(s,source,package,version,chat,"ui.workspace",args,hooks);json_object_put(args);J *workspace=get(get(response,"data"),"data");const char *path=str(workspace,"path");if(path)snprintf(root,sizeof root,"%s",path);json_object_put(response);}
  if(!*root&&chat){J *w=dc_chat_workspace_view(s,chat);if(json_object_get_boolean(get(w,"available")))snprintf(root,sizeof root,"%s",str(w,"path"));json_object_put(w);}
  sqlite3_stmt *q=prep(s,"SELECT lower(hex(randomblob(16)))");if(q&&sqlite3_step(q)==SQLITE_ROW)snprintf(cid,sizeof cid,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);
  if(!*root||!*cid||!dc_add_chat(s,cid,"新聊天",root))return fail("native_session_create_failed");
  if(!client_session_owner(s,cid,source,package,chat)){dc_delete_chat(s,cid);return fail("native_session_owner_unavailable");}
  if(chat){J *req=json_object_new_object();json_object_object_add(req,"op",json_object_new_string("chats.metadata"));json_object_object_add(req,"id",json_object_new_string(chat));J *parent=ui_workspace_call(s,req);const char *model=str(get(parent,"data"),"model");if(model&&*model){json_object_object_add(req,"op",json_object_new_string("chats.update"));json_object_object_add(req,"id",json_object_new_string(cid));json_object_object_add(req,"field",json_object_new_string("model"));json_object_object_add(req,"value",json_object_new_string(model));J *saved=ui_workspace_call(s,req);json_object_put(saved);}json_object_put(parent);json_object_put(req);}
  J *adopted=ui_host_call(s,source,package,version,cid,"ui.adoptSession",NULL,hooks);int present=json_object_get_boolean(get(adopted,"ok"));json_object_put(adopted);if(!present)return fail("native_session_projection_failed");
  value=json_object_new_object();json_object_object_add(value,"id",json_object_new_string(cid));
 }else if(eq(method,"session.fork")){
  J *fork=client_fork(s,r);if(!json_object_get_boolean(get(fork,"ok")))return fork;value=json_object_get(get(fork,"data"));json_object_put(fork);
  if(!client_session_owner(s,str(value,"id"),source,package,chat)){dc_delete_chat(s,str(value,"id"));json_object_put(value);return fail("native_session_owner_unavailable");}
  J *adopted=ui_host_call(s,source,package,version,str(value,"id"),"ui.adoptSession",NULL,hooks);int present=json_object_get_boolean(get(adopted,"ok"));json_object_put(adopted);if(!present){json_object_put(value);return fail("native_session_projection_failed");}
 }else if(eq(method,"session.archive")||eq(method,"session.unarchive")){
  const char *cid=str(args,"id");if(!id(cid))return fail("invalid_native_session");
  J *req=json_object_new_object();json_object_object_add(req,"op",json_object_new_string("chats.update"));json_object_object_add(req,"id",json_object_new_string(cid));json_object_object_add(req,"field",json_object_new_string("archived"));json_object_object_add(req,"value",json_object_new_boolean(eq(method,"session.archive")));J *saved=ui_workspace_call(s,req);json_object_put(req);if(!json_object_get_boolean(get(saved,"ok")))return saved;json_object_put(saved);value=json_object_new_boolean(1);
 }else if(eq(method,"session.read")){
  const char *cid=str(args,"id");if(!id(cid))return fail("invalid_native_session");char *raw=dc_messages(s,cid);value=raw?dunlin_json_parse(raw,strlen(raw)):NULL;free(raw);if(!value)return fail("native_session_read_failed");
 }else if(eq(method,"session.rename")||eq(method,"draft.set")){
  const char *cid=str(args,"id"),*text=str(args,eq(method,"draft.set")?"text":"title");if(!id(cid)||!text||strlen(text)>32768)return fail("invalid_native_action");
  if(eq(method,"session.rename")){if(!dc_rename_chat(s,cid,text))return fail("native_session_rename_failed");value=json_object_new_boolean(1);}
  else {J *req=json_object_new_object();json_object_object_add(req,"op",json_object_new_string("chats.update"));json_object_object_add(req,"id",json_object_new_string(cid));json_object_object_add(req,"field",json_object_new_string("draft"));json_object_object_add(req,"value",json_object_new_string(text));J *result=ui_workspace_call(s,req);json_object_put(req);if(!json_object_get_boolean(get(result,"ok")))return result;json_object_put(result);value=json_object_new_boolean(1);}
 }else if(eq(method,"workspace.create")||eq(method,"workspace.update")){
  const char *title=str(args,"name");if(!title)title=str(args,"title");const char *path=str(args,"cwd");if(!path)path=str(args,"path");if(!title||!path)return fail("native_workspace_directory_required");
  J *req=json_object_new_object();json_object_object_add(req,"op",json_object_new_string(eq(method,"workspace.create")?"projects.create":"projects.update"));
  const char *pid=str(args,"workspaceId");char generated[65]="";if(!pid){sqlite3_stmt *q=prep(s,"SELECT lower(hex(randomblob(16)))");if(q&&sqlite3_step(q)==SQLITE_ROW)snprintf(generated,sizeof generated,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);pid=generated;}
  json_object_object_add(req,"id",json_object_new_string(pid));json_object_object_add(req,"name",json_object_new_string(title));json_object_object_add(req,"path",json_object_new_string(path));json_object_object_add(req,"instructions",json_object_new_string(""));json_object_object_add(req,"policy",json_object_new_string("ask"));json_object_object_add(req,"context",json_object_new_array());if(get(args,"revision"))json_object_object_add(req,"revision",json_object_get(get(args,"revision")));
  J *result=ui_workspace_call(s,req);json_object_put(req);if(!json_object_get_boolean(get(result,"ok")))return result;value=json_object_get(get(result,"data"));json_object_put(result);
 }else return fail("native_action_unsupported");
 J *refresh=json_object_new_object();json_object_object_add(refresh,"op",json_object_new_string("extensions.clientContext"));const char *keys[]={"source","package","version","chat"};for(size_t i=0;i<4;i++)if(get(r,keys[i]))json_object_object_add(refresh,keys[i],json_object_get(get(r,keys[i])));J *snapshot=client_native(s,refresh,hooks);json_object_put(refresh);if(!json_object_get_boolean(get(snapshot,"ok"))){json_object_put(value);return snapshot;}J *out=json_object_new_object();json_object_object_add(out,"value",value);json_object_object_add(out,"snapshot",json_object_get(get(snapshot,"data")));json_object_put(snapshot);return reply(1,NULL,out);
}
