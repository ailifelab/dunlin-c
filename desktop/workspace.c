#define _XOPEN_SOURCE 700
#include "recovery.h"
#include "config_registry.h"
#include "tool_records.h"
#include "trajectory.h"
#include "file_history.h"
#include "knowledge.h"
#include "extensions.h"
#include "catalog.h"
#include "internal.h"
#include "media_records.h"
#include "agents.h"
#include "project_files.h"
#include "project_context.h"
#include "model_identity.h"
#include "model_switch.h"
#include "chat_workspace.h"
#include "attachments.h"
#include "skills.h"
#include "grants.h"
#include "compaction.h"
#include "index_control.h"
#include "dunlin/compact/agent.h"
#include "dunlin/compact/json.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);if(!json_object_is_type(v,json_type_string))return NULL;const char *p=json_object_get_string(v);return strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static int text(const char *p,size_t cap,int empty){return p&&(empty||*p)&&strlen(p)<=cap&&ah_utf8_text(p,strlen(p));}
static int ident(const char *p){return p&&*p&&strlen(p)<=64&&strspn(p,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(p);}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v?v:"",-1,SQLITE_TRANSIENT);}
static int sql(dc_store *s,const char *v){return sqlite3_exec(s->db,v,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(o,"error",json_object_new_string(error));if(data)json_object_object_add(o,"data",data);return o;}
static J *fail(const char *error){return reply(0,error,NULL);}
static J *done(void){return reply(1,NULL,NULL);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int add_column(dc_store *s,const char *name,const char *definition){
 sqlite3_stmt *q=prep(s,"PRAGMA table_info(chat_properties)");if(!q)return 0;int found=0,rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW)if(!strcmp((const char*)sqlite3_column_text(q,1),name))found=1;
 sqlite3_finalize(q);if(rc!=SQLITE_DONE)return 0;if(found)return 1;char query[256];snprintf(query,sizeof query,"ALTER TABLE chat_properties ADD COLUMN %s %s",name,definition);return sql(s,query);
}
int dc_workspace_schema(dc_store *s){
 if(!sql(s,
 "CREATE TABLE IF NOT EXISTS projects(id TEXT PRIMARY KEY,name TEXT NOT NULL COLLATE NOCASE UNIQUE,path TEXT NOT NULL UNIQUE,instructions TEXT NOT NULL DEFAULT '',model TEXT NOT NULL DEFAULT '',policy TEXT NOT NULL DEFAULT 'ask',context TEXT NOT NULL DEFAULT '[]',expanded INTEGER NOT NULL DEFAULT 1,revision INTEGER NOT NULL DEFAULT 1,device TEXT NOT NULL DEFAULT '',inode TEXT NOT NULL DEFAULT '',bookmark TEXT NOT NULL DEFAULT '');"
 "CREATE TABLE IF NOT EXISTS chat_properties(chat TEXT PRIMARY KEY,project TEXT NOT NULL DEFAULT '',archived INTEGER NOT NULL DEFAULT 0,pinned INTEGER NOT NULL DEFAULT 0,draft TEXT NOT NULL DEFAULT '',model TEXT NOT NULL DEFAULT '',active INTEGER NOT NULL DEFAULT 0,last_profile TEXT NOT NULL DEFAULT '',project_explicit INTEGER NOT NULL DEFAULT 0);"
 "CREATE INDEX IF NOT EXISTS chat_project ON chat_properties(project,archived);"
 "CREATE INDEX IF NOT EXISTS chat_updated ON chats(updated DESC);"
 "CREATE UNIQUE INDEX IF NOT EXISTS project_directory_identity ON projects(device,inode) WHERE device<>'' AND inode<>'';"))return 0;
 if(!add_column(s,"active","INTEGER NOT NULL DEFAULT 0")||!add_column(s,"last_profile","TEXT NOT NULL DEFAULT ''")||!add_column(s,"project_explicit","INTEGER NOT NULL DEFAULT 0")||!dc_project_reference_schema(s))return 0;
 return sql(s,
 "CREATE TRIGGER IF NOT EXISTS dunlin_chat_insert AFTER INSERT ON chats BEGIN INSERT OR IGNORE INTO chat_properties(chat,project) VALUES(NEW.id,COALESCE((SELECT id FROM projects WHERE path=NEW.workspace),''));END;"
 "CREATE TRIGGER IF NOT EXISTS dunlin_chat_state AFTER UPDATE OF state ON chats BEGIN INSERT OR IGNORE INTO chat_properties(chat) VALUES(NEW.id);UPDATE chat_properties SET active=CASE WHEN NEW.state IS NULL THEN 0 WHEN json_valid(NEW.state) THEN CASE WHEN json_type(NEW.state,'$.active')='false' THEN 0 ELSE 1 END ELSE 1 END,last_profile=CASE WHEN json_valid(NEW.state) THEN COALESCE(json_extract(NEW.state,'$._dunlin_profile'),'') ELSE '' END WHERE chat=NEW.id;END;"
 "INSERT OR IGNORE INTO chat_properties(chat,project) SELECT c.id,COALESCE((SELECT p.id FROM projects p WHERE p.path=c.workspace),'') FROM chats c;"
 "UPDATE chat_properties SET active=(SELECT CASE WHEN c.state IS NULL THEN 0 WHEN json_valid(c.state) THEN CASE WHEN json_type(c.state,'$.active')='false' THEN 0 ELSE 1 END ELSE 1 END FROM chats c WHERE c.id=chat),last_profile=(SELECT CASE WHEN json_valid(c.state) THEN COALESCE(json_extract(c.state,'$._dunlin_profile'),'') ELSE '' END FROM chats c WHERE c.id=chat) WHERE EXISTS(SELECT 1 FROM chats c WHERE c.id=chat);"
 "DELETE FROM chat_properties WHERE NOT EXISTS(SELECT 1 FROM chats c WHERE c.id=chat);"
 );
}
static int prefix(const char *a,const char *b){size_t n=strlen(a);return !strncmp(a,b,n)&&(n==1||!b[n]||b[n]=='/');}
int dc_workspace_root(dc_store *s,const char *path,char *out,char device[32],char inode[32]){
 struct stat st;char store[PATH_MAX];
 if(!path||!realpath(path,out)||stat(out,&st)||!S_ISDIR(st.st_mode)||access(out,R_OK|X_OK))return 0;
 if(!realpath(s->directory,store)||prefix(store,out)||prefix(out,store))return 0;
 snprintf(device,32,"%llu",(unsigned long long)st.st_dev);snprintf(inode,32,"%llu",(unsigned long long)st.st_ino);return 1;
}
static int exists(dc_store *s,const char *table,const char *id){
 const char *query=!strcmp(table,"projects")?"SELECT 1 FROM projects WHERE id=?":"SELECT 1 FROM chats WHERE id=?";
 sqlite3_stmt *q=prep(s,query);if(!q)return 0;bind(q,1,id);int ok=sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);return ok;
}
static int active(dc_store *s,const char *project,const char *chat){
 sqlite3_stmt *q=prep(s,chat?"SELECT 1 FROM chats WHERE id=? AND COALESCE(json_extract(state,'$.active'),0)!=0":"SELECT 1 FROM chats c JOIN chat_properties m ON m.chat=c.id WHERE m.project=? AND COALESCE(json_extract(c.state,'$.active'),0)!=0 LIMIT 1");
 if(!q)return 1;bind(q,1,chat?chat:project);int a=sqlite3_step(q)!=SQLITE_DONE;sqlite3_finalize(q);return a;
}
static J *project_row(sqlite3_stmt *q){
 const char *keys[]={"id","name","path","instructions","model","policy","context","expanded","revision","device","inode","bookmark","chatCount"};
 J *o=json_object_new_object();
 for(int i=0;i<13;i++){
  J *v=NULL;const char *p=(const char*)sqlite3_column_text(q,i);
  if(i==6)v=p?json_tokener_parse(p):NULL;
  else if(i==7)v=json_object_new_boolean(sqlite3_column_int(q,i));
  else if(i==8||i==12)v=json_object_new_int64(sqlite3_column_int64(q,i));
  else v=json_object_new_string(p?p:"");
  json_object_object_add(o,keys[i],v);
 }
 struct stat st;const char *path=str(o,"path");char dev[32],ino[32];
 int available=path&&!stat(path,&st)&&S_ISDIR(st.st_mode)&&!access(path,R_OK|X_OK);
 snprintf(dev,32,"%llu",available?(unsigned long long)st.st_dev:0);snprintf(ino,32,"%llu",available?(unsigned long long)st.st_ino:0);
 const char *d=str(o,"device"),*i=str(o,"inode");int same=available&&*d&&!strcmp(d,dev)&&!strcmp(i,ino);
 json_object_object_add(o,"available",json_object_new_boolean(same));
 json_object_object_add(o,"availability",json_object_new_string(!available?"目录不存在或无法访问":!same?"目录身份已变化，请重新关联":"可用"));return o;
}
static const char *select_projects="SELECT p.id,p.name,p.path,p.instructions,p.model,p.policy,p.context,p.expanded,p.revision,p.device,p.inode,p.bookmark,(SELECT count(*) FROM chat_properties m JOIN chats c ON c.id=m.chat WHERE m.project=p.id AND m.archived=0) FROM projects p ";
static J *project(dc_store *s,const char *id){
 char query[768];snprintf(query,sizeof query,"%s WHERE p.id=?",select_projects);sqlite3_stmt *q=prep(s,query);if(!q)return NULL;bind(q,1,id);J *p=sqlite3_step(q)==SQLITE_ROW?project_row(q):NULL;sqlite3_finalize(q);return p;
}
J *dc_chat_project(dc_store *s,const char *id){
 sqlite3_stmt *q=prep(s,"SELECT project FROM chat_properties WHERE chat=?");if(!q)return NULL;bind(q,1,id);
 char *p=sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);
 J *r=p&&*p?project(s,p):NULL;free(p);if(!r)return NULL;
 J *binding=dc_chat_workspace_view(s,id);
 if(!binding){json_object_object_add(r,"available",json_object_new_boolean(0));json_object_object_add(r,"availability",json_object_new_string("聊天工作区绑定不可读"));return r;}
 int same=str(r,"device")&&str(r,"inode")&&str(binding,"device")&&str(binding,"inode")&&
  !strcmp(str(r,"device"),str(binding,"device"))&&!strcmp(str(r,"inode"),str(binding,"inode"));
 const char *fields[]={"path","device","inode","available","availability"};
 for(size_t i=0;i<sizeof fields/sizeof *fields;i++)json_object_object_add(r,fields[i],json_object_get(get(binding,fields[i])));
 json_object_object_add(r,"referenceBaselineApplicable",json_object_new_boolean(same));
 json_object_object_add(r,"workspaceRevision",json_object_get(get(binding,"revision")));
 json_object_put(binding);return r;
}
static J *projects(dc_store *s){
 char query[768];snprintf(query,sizeof query,"%s ORDER BY p.name COLLATE NOCASE,p.id",select_projects);sqlite3_stmt *q=prep(s,query);if(!q)return fail("storage_error");J *a=json_object_new_array();int rc;while((rc=sqlite3_step(q))==SQLITE_ROW)json_object_array_add(a,project_row(q));sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}return reply(1,NULL,a);
}
static int relative(const char *p){
 if(!text(p,1024,0)||p[0]=='/'||strchr(p,'\\'))return 0;
 const char *c=p;while(*c){const char *e=strchr(c,'/');size_t n=e?(size_t)(e-c):strlen(c);if(!n||n>NAME_MAX||(n==1&&*c=='.')||(n==2&&!memcmp(c,"..",2))||!strncmp(c,".dunlin-",8))return 0;if(!e)break;c=e+1;if(!*c)return 0;}return 1;
}
static int contexts(J *a){if(!json_object_is_type(a,json_type_array)||json_object_array_length(a)>24)return 0;for(size_t i=0;i<json_object_array_length(a);i++){J *v=json_object_array_get_idx(a,i);if(!json_object_is_type(v,json_type_string)||strlen(json_object_get_string(v))!=(size_t)json_object_get_string_len(v)||!relative(json_object_get_string(v)))return 0;}return 1;}
static J *save_project(dc_store *s,J *r,int create){
 const char *id=str(r,"id"),*name=str(r,"name"),*path=str(r,"path"),*instructions=str(r,"instructions"),*model=str(r,"model"),*policy=str(r,"policy"),*bookmark=str(r,"bookmark");
 if(!ident(id)||!text(name,256,0)||!text(path,PATH_MAX-1,0)||!text(instructions,16384,1)||!text(model,128,1)||!policy||(strcmp(policy,"ask")&&strcmp(policy,"readOnly")&&strcmp(policy,"noTools"))||!contexts(get(r,"context"))||(bookmark&&!text(bookmark,65536,1)))return fail("invalid_project");
 J *relink_value=NULL;int has_relink=json_object_object_get_ex(r,"relink",&relink_value);
 if(has_relink&&!json_object_is_type(relink_value,json_type_boolean))return fail("invalid_project");
 J *revision=get(r,"revision");
 if(!create&&(!json_object_is_type(revision,json_type_int)||json_object_get_int64(revision)<1||json_object_get_uint64(revision)>INT64_MAX))return fail("invalid_project");
 int nonspace=0;for(const unsigned char *c=(const unsigned char*)name;*c;c++)if(*c>32)nonspace=1;if(!nonspace)return fail("invalid_name");
 J *old=create?NULL:project(s,id);if(!create&&!old)return fail("project_not_found");
 if(old&&json_object_get_int64(get(old,"revision"))!=json_object_get_int64(get(r,"revision"))){json_object_put(old);return fail("revision_conflict");}
 int changed=old&&strcmp(path,str(old,"path"));int relink=json_object_get_boolean(get(r,"relink"));
 if(old&&active(s,id,NULL)){json_object_put(old);return fail("active_checkpoint");}
 char normalized[PATH_MAX],dev[32],ino[32];
 if(!dc_workspace_root(s,path,normalized,dev,ino)){
  if(create||changed||relink){json_object_put(old);return fail("unavailable_or_overlapping_directory");}
  snprintf(normalized,sizeof normalized,"%s",path);snprintf(dev,32,"%s",str(old,"device"));snprintf(ino,32,"%s",str(old,"inode"));
 }else{
  /* Saving a name/default model must not silently adopt a replacement mount
   * or same-path symlink target. A textual alias of the original canonical
   * path is not a different-directory selection either. */
  if(old&&!relink&&(!changed||!strcmp(normalized,str(old,"path")))&&
     (strcmp(dev,str(old,"device"))||strcmp(ino,str(old,"inode")))){
   json_object_put(old);return fail("directory_identity_changed");
  }
  changed=old&&strcmp(normalized,str(old,"path"));
 }
 J *versions=NULL;int has_versions=json_object_object_get_ex(r,"contextVersions",&versions);
 if(has_versions&&(!versions||!dc_project_reference_versions(normalized,get(r,"context"),versions))){json_object_put(old);return fail("reference_version_conflict");}
 const char *c=json_object_to_json_string_ext(get(r,"context"),JSON_C_TO_STRING_PLAIN);
 if(!sql(s,"BEGIN IMMEDIATE")){json_object_put(old);return fail("storage_error");}
 sqlite3_stmt *q=prep(s,create?"INSERT INTO projects(name,path,instructions,model,policy,context,device,inode,bookmark,id) VALUES(?,?,?,?,?,?,?,?,?,?)":"UPDATE projects SET name=?,path=?,instructions=?,model=?,policy=?,context=?,device=?,inode=?,bookmark=?,revision=revision+1 WHERE id=? AND revision=?");
 int ok=q!=NULL;
 if(q){const char *v[]={name,normalized,instructions,model,policy,c,dev,ino,bookmark?bookmark:old?str(old,"bookmark"):"",id};for(int i=0;i<10;i++)bind(q,i+1,v[i]);if(!create)sqlite3_bind_int64(q,11,json_object_get_int64(get(r,"revision")));ok=step(q)&&sqlite3_changes(s->db)==1;}
 if(ok&&(changed||relink))ok=dc_chat_workspaces_relocate(s,id,normalized,dev,ino);
 if(ok)ok=dc_project_reference_sync(s,id,get(r,"context"),versions,changed||relink);
 if(!ok){sql(s,"ROLLBACK");json_object_put(old);return fail("duplicate_name_or_directory_or_revision_conflict");}
 ok=sql(s,"COMMIT");if(!ok)sql(s,"ROLLBACK");json_object_put(old);return ok?reply(1,NULL,project(s,id)):fail("storage_error");
}
static J *chat_update(dc_store *s,J *r){
 const char *id=str(r,"id"),*field=str(r,"field");if(!ident(id)||!field||!exists(s,"chats",id))return fail("chat_not_found");
 const char *query=NULL,*value=NULL;int flag=0;
 if(!strcmp(field,"archived")||!strcmp(field,"pinned")){
  if(!json_object_is_type(get(r,"value"),json_type_boolean))return fail("invalid_value");flag=json_object_get_boolean(get(r,"value"));
  query=!strcmp(field,"archived")?"UPDATE chat_properties SET archived=? WHERE chat=?":"UPDATE chat_properties SET pinned=? WHERE chat=?";
 }else if(!strcmp(field,"project")){
  value=str(r,"value");if(!value||(*value&&(!ident(value)||!exists(s,"projects",value))))return fail("project_not_found");
  if(active(s,NULL,id))return fail("active_checkpoint");query="UPDATE chat_properties SET project=?,project_explicit=1 WHERE chat=?";
 }else if(!strcmp(field,"draft")||!strcmp(field,"model")){
  value=str(r,"value");if(!text(value,!strcmp(field,"draft")?65536:128,1))return fail("invalid_value");if(!strcmp(field,"model")&&active(s,NULL,id))return fail("active_checkpoint");query=!strcmp(field,"draft")?"UPDATE chat_properties SET draft=? WHERE chat=?":"UPDATE chat_properties SET model=? WHERE chat=?";
 }else return fail("unsupported_field");
 if(!sql(s,"BEGIN IMMEDIATE"))return fail("storage_error");sqlite3_stmt *q=prep(s,"INSERT OR IGNORE INTO chat_properties(chat) VALUES(?)");if(q)bind(q,1,id);int ok=step(q);
 q=prep(s,query);if(q){if(value)bind(q,1,value);else sqlite3_bind_int(q,1,flag);bind(q,2,id);}ok=step(q)&&ok;
 if(ok&&!strcmp(field,"project")&&*value){J *p=project(s,value);q=prep(s,"UPDATE chats SET workspace=? WHERE id=?");if(q){bind(q,1,str(p,"path"));bind(q,2,id);}ok=step(q);if(ok)ok=dc_chat_workspace_capture(s,id,str(p,"path"));json_object_put(p);}
 if(!ok){sql(s,"ROLLBACK");return fail("storage_error");}ok=sql(s,"COMMIT");return ok?done():fail("storage_error");
}
static J *remove_project(dc_store *s,J *r){
 const char *id=str(r,"id"),*mode=str(r,"mode");if(!ident(id)||!mode||(strcmp(mode,"keep")&&strcmp(mode,"archive")))return fail("invalid_remove_mode");J *p=project(s,id);if(!p)return fail("project_not_found");
 if(json_object_get_int64(get(p,"revision"))!=json_object_get_int64(get(r,"revision"))){json_object_put(p);return fail("revision_conflict");}json_object_put(p);
 if(active(s,id,NULL))return fail("active_checkpoint");if(!sql(s,"BEGIN IMMEDIATE"))return fail("storage_error");
 sqlite3_stmt *q=prep(s,!strcmp(mode,"archive")?"UPDATE chat_properties SET project='',archived=1,project_explicit=1 WHERE project=?":"UPDATE chat_properties SET project='',project_explicit=1 WHERE project=?");if(q)bind(q,1,id);int ok=step(q);
 q=prep(s,"DELETE FROM skill_bindings WHERE scope='project' AND scope_id=?");if(q)bind(q,1,id);ok=step(q)&&ok;
 q=prep(s,"DELETE FROM project_reference_baselines WHERE project=?");if(q)bind(q,1,id);ok=step(q)&&ok;
 q=prep(s,"DELETE FROM projects WHERE id=?");if(q)bind(q,1,id);ok=step(q)&&ok;if(!ok){sql(s,"ROLLBACK");return fail("storage_error");}return sql(s,"COMMIT")?done():fail("storage_error");
}
static J *scoped_project(dc_store *s,J *r,const char **error){
 const char *id=str(r,"project");if(!ident(id)){*error="project_not_found";return NULL;}
 J *chat=NULL;int has_chat=json_object_object_get_ex(r,"chat",&chat);
 const char *chat_id=has_chat?str(r,"chat"):NULL;
 if(has_chat&&!ident(chat_id)){*error="invalid_chat_scope";return NULL;}
 J *p=has_chat?dc_chat_project(s,chat_id):project(s,id);
 if(!p||!str(p,"id")||strcmp(str(p,"id"),id)){json_object_put(p);*error=has_chat?"chat_project_mismatch":"project_not_found";return NULL;}
 return p;
}
static J *request(dc_store *s,J *r){
 const char *op=str(r,"op");if(!op)return fail("invalid_request");
 if(!strncmp(op,"agents.",7))return dc_agents_request(s,r);
 if(!strncmp(op,"recovery.",9))return dc_recovery_request(s,r);
 if(!strncmp(op,"config.",7))return dc_config_request(s,r);
 if(!strncmp(op,"tools.",6))return dc_tool_records_request(s,r);
 if(!strncmp(op,"trace.",6))return dc_trajectory_request(s,r);
 if(!strncmp(op,"receipts.",9))return dc_file_history_request(s,r);
 if(!strncmp(op,"knowledge.",10)||!strncmp(op,"memory.",7)||!strncmp(op,"graph.",6))return dc_knowledge_request(s,r);
 if(!strncmp(op,"extensions.",11)||!strncmp(op,"compat.",7)||!strncmp(op,"python.",7))return dc_extensions_request(s,r,NULL);
 if(!strncmp(op,"catalog.",8))return dc_catalog_request(s,r,NULL);
 if(!strncmp(op,"skills.",7))return dc_skills_request(s,r);
 if(!strncmp(op,"grants.",7))return dc_grants_request(s,r);
 if(!strncmp(op,"context.",8))return dc_compaction_request(s,r);
 if(!strncmp(op,"index.",6))return dc_index_request(s,r);
 if(!strcmp(op,"projects.list"))return projects(s);
 if(!strcmp(op,"projects.create"))return save_project(s,r,1);
 if(!strcmp(op,"projects.update"))return save_project(s,r,0);
 if(!strcmp(op,"projects.remove"))return remove_project(s,r);
 if(!strcmp(op,"files.references")){
  const char *error=NULL;J *p=scoped_project(s,r,&error);if(!p)return fail(error);J *data=dc_project_reference_list(s,p);json_object_put(p);return data?reply(1,NULL,data):fail("storage_error");
 }
 if(!strcmp(op,"files.list")||!strcmp(op,"files.read")){
  const char *error=NULL;J *p=scoped_project(s,r,&error);if(!p)return fail(error);
  if(!json_object_get_boolean(get(p,"available"))){json_object_put(p);return fail("project_unavailable");}
  J *value=dc_project_file_query_bound(str(p,"path"),str(p,"device"),str(p,"inode"),r);json_object_put(p);return value?reply(1,NULL,value):fail("unsafe_or_unreadable_path");
 }
 if(!strcmp(op,"projects.expanded")){
  const char *id=str(r,"id");if(!ident(id)||!json_object_is_type(get(r,"value"),json_type_boolean))return fail("invalid_request");sqlite3_stmt *q=prep(s,"UPDATE projects SET expanded=? WHERE id=?");if(q){sqlite3_bind_int(q,1,json_object_get_boolean(get(r,"value")));bind(q,2,id);}return step(q)&&sqlite3_changes(s->db)==1?done():fail("project_not_found");
 }
 if(!strcmp(op,"chats.model.preview")||!strcmp(op,"chats.model.apply"))return dc_model_switch_request(s,r);
 if(!strncmp(op,"media.",6))return dc_media_request(s,r);
 if(!strncmp(op,"attachments.",12))return dc_attachments_request(s,r);
 if(!strncmp(op,"chats.workspace.",16))return dc_chat_workspace_request(s,r);
 if(!strcmp(op,"chats.update"))return chat_update(s,r);
 if(!strcmp(op,"chats.metadata")){
  const char *id=str(r,"id");if(!ident(id)||!exists(s,"chats",id))return fail("chat_not_found");sqlite3_stmt *q=prep(s,"SELECT COALESCE(m.draft,''),COALESCE(" DC_CHAT_MODEL_ID ",'') FROM chats c LEFT JOIN chat_properties m ON m.chat=c.id " DC_ACTIVE_RUN_JOIN " WHERE c.id=?");if(!q)return fail("storage_error");bind(q,1,id);int row=sqlite3_step(q);if(row!=SQLITE_ROW){sqlite3_finalize(q);return fail("storage_error");}J *o=json_object_new_object();json_object_object_add(o,"draft",json_object_new_string((const char*)sqlite3_column_text(q,0)));json_object_object_add(o,"model",json_object_new_string((const char*)sqlite3_column_text(q,1)));sqlite3_finalize(q);return reply(1,NULL,o);
 }
 return fail("unsupported_operation");
}
char *dc_workspace_request(dc_store *s,const char *raw){
 if(!s||!raw||strlen(raw)>8388608)return NULL;
 J *r=dunlin_json_parse(raw,strlen(raw));const char *op=str(r,"op");
 if(strlen(raw)>131072&&(!op||(strcmp(op,"config.preview")&&strcmp(op,"config.save")))){json_object_put(r);return NULL;}
 J *out=json_object_is_type(r,json_type_object)?request(s,r):fail("invalid_json");
 char *text=out?strdup(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(out);json_object_put(r);return text;
}
