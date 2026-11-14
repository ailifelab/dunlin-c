#define _XOPEN_SOURCE 700
#include "extensions.h"
#include "extension_mutex.h"
#include "chat_workspace.h"
#include "catalog.h"
#include "config_registry.h"
#include "model_helpers.h"
#include "package_store.h"
#include "dunlin/extension_services.h"
#include "dunlin/plugin_port.h"
#include "dunlin/content_hash.h"
#include "dunlin/terminal.h"
#include "dunlin/agent_scheduler.h"
#include "dunlin/compact/json.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
typedef struct json_object J;
typedef struct {extension_mutex mutex;unsigned users;dc_worker *dsh,*python_worker;char *assets;char *node;char *python;J *active;const dc_environment_provider_v1 *provider;dc_environment_detector detect;dl_terminal_pool *terminals;} extension_state;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);const char *text=json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;return text&&strlen(text)==(size_t)json_object_get_string_len(v)?text:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int id(const char *s){return s&&*s&&strlen(s)<=256&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:./@+")==strlen(s);}
static sqlite3_stmt *prep(dc_store *s,const char *v){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,v,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *v){return sqlite3_exec(s->db,v,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(o,"error",json_object_new_string(error));if(data)json_object_object_add(o,"data",data);return o;}
static J *fail(const char *code){return reply(0,code,NULL);}
static extension_state *state(dc_store *s){if(!s->extensions){extension_state *e=calloc(1,sizeof *e);if(e){if(!extension_mutex_init(&e->mutex)){free(e);e=NULL;}else{e->users=1;e->active=json_object_new_object();if(!e->active){extension_mutex_destroy(&e->mutex);free(e);e=NULL;}}}s->extensions=e;}return s->extensions;}
static int register_environment_unlocked(dc_store *s,const dc_environment_provider_v1 *provider){if(!s||!provider||provider->abi_version!=1||!eq(provider->engine,"node-dsh")||!provider->version||!provider->baseline||!provider->prepare)return 0;extension_state *e=state(s);if(!e||e->dsh)return 0;e->provider=provider;return 1;}
static void package_identity(const char *source,const char *package,char out[65]){char joined[1024];snprintf(joined,sizeof joined,"%s:%s",source,package);dc_sha256(joined,strlen(joined),out);}
static void clear_host(extension_state *e,const char *host){J *next=json_object_new_object();json_object_object_foreach(e->active,key,value){if(!eq(str(value,"host"),host))json_object_object_add(next,key,json_object_get(value));else dl_terminal_close_owner(e->terminals,key);}json_object_put(e->active);e->active=next;}
int dc_extensions_schema(dc_store *s){int ready=sql(s,
 "CREATE TABLE IF NOT EXISTS compatibility_environment(singleton INTEGER PRIMARY KEY CHECK(singleton=1),desired INTEGER NOT NULL DEFAULT 0,state TEXT NOT NULL DEFAULT 'off',node TEXT NOT NULL DEFAULT '',generation INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '');INSERT OR IGNORE INTO compatibility_environment(singleton) VALUES(1);UPDATE compatibility_environment SET state='stopped' WHERE state='ready';"
 "CREATE TABLE IF NOT EXISTS compatibility_candidate(singleton INTEGER PRIMARY KEY CHECK(singleton=1),path TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_versions(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,digest TEXT NOT NULL,manifest TEXT NOT NULL,path TEXT NOT NULL,files TEXT NOT NULL,quarantined INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '',PRIMARY KEY(source,package,version));"
 "CREATE TABLE IF NOT EXISTS extension_bindings(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,enabled INTEGER NOT NULL DEFAULT 0,runtime_instance TEXT NOT NULL DEFAULT '',PRIMARY KEY(source,package));"
 "CREATE TABLE IF NOT EXISTS extension_configuration(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,config TEXT NOT NULL,PRIMARY KEY(source,package,version));"
 "CREATE TABLE IF NOT EXISTS extension_client_sessions(chat TEXT PRIMARY KEY,source TEXT NOT NULL,package TEXT NOT NULL,root TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_agent_scopes(scope TEXT NOT NULL,chat TEXT NOT NULL,parent TEXT NOT NULL,PRIMARY KEY(scope,chat));"
 "CREATE TABLE IF NOT EXISTS extension_agent_jobs(job TEXT PRIMARY KEY,chat TEXT NOT NULL,message TEXT NOT NULL,call TEXT NOT NULL,source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,instance TEXT NOT NULL,config_hash TEXT NOT NULL,registry_revision INTEGER NOT NULL,request_hash TEXT NOT NULL,state TEXT NOT NULL,effect_sequence INTEGER NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_agent_inputs(chat TEXT NOT NULL,id TEXT NOT NULL,kind TEXT NOT NULL,body TEXT NOT NULL,call TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'queued',job TEXT NOT NULL DEFAULT '',PRIMARY KEY(chat,id));"
 "CREATE TABLE IF NOT EXISTS extension_agent_sessions(chat TEXT PRIMARY KEY,source TEXT NOT NULL,package TEXT NOT NULL,parent TEXT NOT NULL,metadata TEXT NOT NULL,created_call TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_native_effects(call TEXT NOT NULL,sequence INTEGER NOT NULL,method TEXT NOT NULL,args_hash TEXT NOT NULL,status TEXT NOT NULL,PRIMARY KEY(call,sequence));"
 "CREATE TABLE IF NOT EXISTS extension_native_settings(call TEXT PRIMARY KEY,sequence INTEGER NOT NULL,config_hash TEXT NOT NULL,status TEXT NOT NULL);UPDATE extension_native_settings SET status='unknown' WHERE status='pending';"
 "CREATE TABLE IF NOT EXISTS extension_native_scopes(call TEXT PRIMARY KEY,instance TEXT NOT NULL,chat TEXT NOT NULL,surface TEXT NOT NULL,operation TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_forks(chat TEXT PRIMARY KEY,parent TEXT NOT NULL,through INTEGER NOT NULL,source_hash TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_presentations(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,digest TEXT NOT NULL,value TEXT NOT NULL,PRIMARY KEY(source,package));"
 "CREATE TABLE IF NOT EXISTS extension_client_storage(source TEXT NOT NULL,package TEXT NOT NULL,value TEXT NOT NULL,PRIMARY KEY(source,package));"
 "CREATE TABLE IF NOT EXISTS extension_python(singleton INTEGER PRIMARY KEY CHECK(singleton=1),executable TEXT NOT NULL,desired INTEGER NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_faults(id INTEGER PRIMARY KEY,source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,phase TEXT NOT NULL,error TEXT NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()));"
 "CREATE TABLE IF NOT EXISTS extension_fault_details(fault INTEGER PRIMARY KEY REFERENCES extension_faults(id) ON DELETE CASCADE,diagnostic TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_questions(call TEXT NOT NULL,id TEXT NOT NULL,question_hash TEXT NOT NULL,decision TEXT NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()),PRIMARY KEY(call,id));"
 "CREATE TABLE IF NOT EXISTS extension_terminal_owners(id TEXT PRIMARY KEY,source TEXT NOT NULL,package TEXT NOT NULL,chat TEXT NOT NULL,instance TEXT NOT NULL,state TEXT NOT NULL);UPDATE extension_terminal_owners SET state='unknown' WHERE state='running';"
 "CREATE TABLE IF NOT EXISTS extension_requests(id TEXT PRIMARY KEY,call TEXT NOT NULL UNIQUE,source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,instance TEXT NOT NULL,surface TEXT NOT NULL,chat TEXT NOT NULL,native_session TEXT NOT NULL DEFAULT '',state TEXT NOT NULL DEFAULT 'running');"
 "CREATE TABLE IF NOT EXISTS extension_calls(id TEXT PRIMARY KEY,source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,method TEXT NOT NULL,args_hash TEXT NOT NULL,status TEXT NOT NULL,started INTEGER NOT NULL DEFAULT(unixepoch()),finished INTEGER);INSERT INTO extension_faults(source,package,version,phase,error) SELECT source,package,version,'recovery','interrupted_invocation_unknown' FROM extension_calls WHERE status='running';UPDATE extension_versions SET quarantined=1,error='interrupted_invocation_unknown' WHERE EXISTS(SELECT 1 FROM extension_calls c WHERE c.status='running' AND c.source=extension_versions.source AND c.package=extension_versions.package AND c.version=extension_versions.version);UPDATE extension_bindings SET enabled=0 WHERE EXISTS(SELECT 1 FROM extension_calls c WHERE c.status='running' AND c.source=extension_bindings.source AND c.package=extension_bindings.package);UPDATE extension_calls SET status='unknown',finished=unixepoch() WHERE status='running';UPDATE extension_requests SET state='unknown' WHERE state='running';UPDATE extension_native_effects SET status='unknown' WHERE status='running';UPDATE extension_agent_jobs SET state='interrupted' WHERE state IN('preparing','admitted','ready');");if(ready){const char *checks[]={"SELECT count(*) FROM pragma_table_info('extension_agent_jobs') WHERE name='effect_sequence'","SELECT count(*) FROM pragma_table_info('extension_native_scopes') WHERE name='operation'","SELECT count(*) FROM pragma_table_info('extension_bindings') WHERE name='runtime_instance'"};const char *changes[]={"ALTER TABLE extension_agent_jobs ADD COLUMN effect_sequence INTEGER NOT NULL DEFAULT 0","ALTER TABLE extension_native_scopes ADD COLUMN operation TEXT NOT NULL DEFAULT 'none'","ALTER TABLE extension_bindings ADD COLUMN runtime_instance TEXT NOT NULL DEFAULT ''"};for(size_t i=0;ready&&i<3;i++){sqlite3_stmt *q=prep(s,checks[i]);int rc=q?sqlite3_step(q):SQLITE_ERROR,exists=rc==SQLITE_ROW?sqlite3_column_int(q,0):0;sqlite3_finalize(q);ready=rc==SQLITE_ROW&&(exists||sql(s,changes[i]));}}if(ready)ready=sql(s,"UPDATE extension_bindings SET runtime_instance=''");return ready&&dc_helpers_schema(s)&&state(s)!=NULL;}
void *dc_extensions_scope_enter(dc_store *s){if(!s||(s->lock<0&&!s->extensions))return NULL;extension_state *e=state(s);if(e)extension_mutex_lock(&e->mutex);return e;}
void dc_extensions_scope_leave(void *scope){extension_state *e=scope;if(e)extension_mutex_unlock(&e->mutex);}
int dc_extensions_share(dc_store *owner,dc_store *worker){if(!owner||!worker||worker->extensions||!owner->directory||!worker->directory||strcmp(owner->directory,worker->directory))return 0;extension_state *e=dc_extensions_scope_enter(owner);if(!e)return 0;if(e->users==UINT_MAX){dc_extensions_scope_leave(e);return 0;}e->users++;worker->extensions=e;dc_extensions_scope_leave(e);return 1;}
void dc_extensions_close(dc_store *s){extension_state *e=s?s->extensions:NULL;if(!e)return;extension_mutex_lock(&e->mutex);s->extensions=NULL;if(--e->users){extension_mutex_unlock(&e->mutex);return;}dl_terminal_pool_close(e->terminals);sql(s,"UPDATE extension_terminal_owners SET state='stopped' WHERE state='running'");dc_worker_close(e->dsh);dc_worker_close(e->python_worker);free(e->assets);free(e->node);free(e->python);json_object_put(e->active);extension_mutex_unlock(&e->mutex);extension_mutex_destroy(&e->mutex);free(e);}
static J *environment(dc_store *s){sqlite3_stmt *q=prep(s,"SELECT desired,state,CASE WHEN state='outdated' AND desired=0 THEN coalesce((SELECT path FROM compatibility_candidate WHERE singleton=1),node) ELSE node END,generation,error FROM compatibility_environment");if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("storage_error");}J *o=json_object_new_object();json_object_object_add(o,"enabled",json_object_new_boolean(sqlite3_column_int(q,0)));json_object_object_add(o,"state",json_object_new_string((const char*)sqlite3_column_text(q,1)));json_object_object_add(o,"node",json_object_new_string((const char*)sqlite3_column_text(q,2)));json_object_object_add(o,"generation",json_object_new_int64(sqlite3_column_int64(q,3)));json_object_object_add(o,"error",json_object_new_string((const char*)sqlite3_column_text(q,4)));extension_state *e=state(s);json_object_object_add(o,"healthy",json_object_new_boolean(e&&dc_worker_alive(e->dsh)));json_object_object_add(o,"baseline",json_object_new_string(e&&e->provider?e->provider->baseline:"DSH 适配器未装配"));sqlite3_finalize(q);return reply(1,NULL,o);}
static const char *gate_unlocked(dc_store *s,J *engines,const char *kind){
 if(!eq(str(engines,"dunlin"),"desktop@1"))return "unknown_dunlin_abi";const char *host=str(engines,"host");if(eq(kind,"theme")&&eq(host,"native-theme"))return NULL;
 extension_state *e=state(s);if(eq(host,"node-dsh")){if(!e||!e->provider)return "dsh_adapter_not_built";if(!eq(str(engines,"dsh"),e->provider->version))return "dsh_version_unsupported";return e&&dc_worker_alive(e->dsh)?NULL:"dsh_environment_not_ready";}
 if(eq(host,"python"))return e&&dc_worker_alive(e->python_worker)?NULL:"python_environment_not_ready";return "extension_host_unsupported";
}
static J *versions(dc_store *s,const char *source,const char *package){sqlite3_stmt *q=prep(s,"SELECT version,digest,manifest,quarantined,error FROM extension_versions WHERE source=? AND package=? ORDER BY rowid DESC");if(!q)return NULL;bind(q,1,source);bind(q,2,package);J *a=json_object_new_array();int rc;while((rc=sqlite3_step(q))==SQLITE_ROW){J *o=json_object_new_object();json_object_object_add(o,"version",json_object_new_string((const char*)sqlite3_column_text(q,0)));json_object_object_add(o,"digest",json_object_new_string((const char*)sqlite3_column_text(q,1)));const char *raw=(const char*)sqlite3_column_text(q,2);J *manifest=dunlin_json_parse(raw,strlen(raw));json_object_object_add(o,"manifest",manifest);json_object_object_add(o,"quarantined",json_object_new_boolean(sqlite3_column_int(q,3)));const char *reason=dc_extension_gate(s,get(manifest,"engines"),str(manifest,"kind"));if(sqlite3_column_int(q,3))reason=(const char*)sqlite3_column_text(q,4);json_object_object_add(o,"reason",json_object_new_string(reason?reason:""));json_object_array_add(a,o);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return NULL;}return a;}
static void host_fault(dc_store *,extension_state *,const char *,const char *);
static J *inventory(dc_store *s){extension_state *e=state(s);if(e&&e->dsh&&!dc_worker_alive(e->dsh))host_fault(s,e,"node-dsh","host_exited");if(e&&e->python_worker&&!dc_worker_alive(e->python_worker))host_fault(s,e,"python","host_exited");sqlite3_stmt *q=prep(s,"SELECT source,package,version,enabled FROM extension_bindings ORDER BY package,source");if(!q)return fail("storage_error");J *a=json_object_new_array();int rc;while((rc=sqlite3_step(q))==SQLITE_ROW){const char *source=(const char*)sqlite3_column_text(q,0),*package=(const char*)sqlite3_column_text(q,1);J *o=json_object_new_object();json_object_object_add(o,"source",json_object_new_string(source));json_object_object_add(o,"package",json_object_new_string(package));json_object_object_add(o,"version",json_object_new_string((const char*)sqlite3_column_text(q,2)));json_object_object_add(o,"enabled",json_object_new_boolean(sqlite3_column_int(q,3)));J *vs=versions(s,source,package);if(!vs){json_object_put(o);json_object_put(a);sqlite3_finalize(q);return fail("storage_error");}json_object_object_add(o,"versions",vs);int active=sqlite3_column_int(q,3);J *chosen=NULL;for(size_t k=0;k<json_object_array_length(vs);k++){J *v=json_object_array_get_idx(vs,k);if(eq(str(v,"version"),(const char*)sqlite3_column_text(q,2)))chosen=v;}if(!chosen||json_object_get_boolean(get(chosen,"quarantined"))||*str(chosen,"reason"))active=0;char identity[65];package_identity(source,package,identity);json_object_object_add(o,"runtimeID",json_object_new_string(identity));extension_state *e=state(s);J *current=e?get(e->active,identity):NULL;if(!eq(str(get(get(chosen,"manifest"),"engines"),"host"),"native-theme")&&(!current||!eq(str(current,"version"),(const char*)sqlite3_column_text(q,2))))active=0;json_object_object_add(o,"active",json_object_new_boolean(active));json_object_array_add(a,o);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}return reply(1,NULL,a);}
static int array_strings(J *m,const char *key,size_t limit){J *a=get(m,key);if(!json_object_is_type(a,json_type_array)||json_object_array_length(a)>limit)return 0;for(size_t i=0;i<json_object_array_length(a);i++)if(!json_object_is_type(json_object_array_get_idx(a,i),json_type_string))return 0;return 1;}
static int client_manifest_valid(J *manifest){
 J *client=get(manifest,"client");if(!client)return 1;
 if(!eq(str(get(manifest,"engines"),"host"),"node-dsh")||!eq(str(client,"format"),"dsh-lazy-cjs@1")||!array_strings(client,"slots",5))return 0;
 J *slots=get(client,"slots");size_t count=json_object_array_length(slots);
 if(!count)return 0;
 for(size_t i=0;i<count;i++){
  const char *slot=json_object_get_string(json_object_array_get_idx(slots,i));
  if(!eq(slot,"conversation.composer.dock")&&!eq(slot,"conversation.chat.reasoning.body")
    &&!eq(slot,"settings.general.item")&&!eq(slot,"dunlin.workbench")&&!eq(slot,"shell.overlay"))return 0;
  for(size_t j=0;j<i;j++)if(eq(slot,json_object_get_string(json_object_array_get_idx(slots,j))))return 0;
 }
 const char *entry=str(client,"entry");return entry&&strlen(entry)>3&&eq(entry+strlen(entry)-3,".js");
}
static J *import_unlocked(dc_store *s,const char *source,const char *path,J *expected){
 J *snapshot=dc_package_snapshot(s,path),*manifest=get(snapshot,"manifest");if(!snapshot)return fail("unsafe_extension_directory");const char *package=str(manifest,"packageId"),*version=str(manifest,"version"),*kind=str(manifest,"kind"),*error="invalid_extension_manifest";
 if((expected&&!json_object_equal(expected,manifest))||!eq(str(manifest,"protocol"),"dunlin.extension")||json_object_get_int(get(manifest,"major"))!=1||!json_object_is_type(get(manifest,"major"),json_type_int)||!id(source)||!id(package)||!id(version)||(!eq(kind,"plugin")&&!eq(kind,"theme"))||!array_strings(manifest,"capabilities",64)||!array_strings(manifest,"requires",64)||!array_strings(manifest,"dependencies",64))goto done;
 error=dc_extension_gate(s,get(manifest,"engines"),kind);if(error)goto done;
 if(!client_manifest_valid(manifest)){error="extension_client_unsupported";goto done;}
 if(get(manifest,"client")){char *bytes=dc_package_read_text(str(snapshot,"path"),get(snapshot,"files"),str(get(manifest,"client"),"entry"));if(!bytes){error="extension_client_invalid";goto done;}free(bytes);}

 J *requires=get(manifest,"requires");for(size_t i=0;i<json_object_array_length(requires);i++)if(!eq(json_object_get_string(json_object_array_get_idx(requires,i)),"dunlin.contributions@1")&&!(eq(json_object_get_string(json_object_array_get_idx(requires,i)),"dsh.theme.register@1")&&eq(str(get(manifest,"engines"),"host"),"node-dsh"))){error="extension_service_unsupported";goto done;}
 if(json_object_array_length(get(manifest,"dependencies"))){error="dependencies_not_resolved";goto done;}
 sqlite3_stmt *q=prep(s,"SELECT digest FROM extension_versions WHERE source=? AND package=? AND version=?");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}int rc=q?sqlite3_step(q):SQLITE_ERROR;int identical=rc==SQLITE_ROW&&eq((const char*)sqlite3_column_text(q,0),str(snapshot,"digest"));sqlite3_finalize(q);if(rc==SQLITE_ROW&&!identical){error="immutable_extension_version_changed";goto done;}if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE){error="storage_error";goto done;}
 q=prep(s,"INSERT OR IGNORE INTO extension_versions(source,package,version,digest,manifest,path,files) VALUES(?,?,?,?,?,?,?)");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);bind(q,4,str(snapshot,"digest"));bind(q,5,json_object_to_json_string_ext(manifest,JSON_C_TO_STRING_PLAIN));bind(q,6,str(snapshot,"path"));bind(q,7,json_object_to_json_string_ext(get(snapshot,"files"),JSON_C_TO_STRING_PLAIN));}if(!step(q)){error="storage_error";goto done;}
 q=prep(s,"INSERT OR IGNORE INTO extension_bindings(source,package,version) VALUES(?,?,?)");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}error=step(q)?NULL:"storage_error";
 done:json_object_put(snapshot);return error?fail(error):inventory(s);
}
static sqlite3_int64 version_fault(dc_store *s,const char *source,const char *package,const char *version,const char *phase,const char *error){
 sqlite3_stmt *q=prep(s,"UPDATE extension_versions SET quarantined=1,error=? WHERE source=? AND package=? AND version=?");if(q){bind(q,1,error);bind(q,2,source);bind(q,3,package);bind(q,4,version);}step(q);q=prep(s,"INSERT INTO extension_faults(source,package,version,phase,error) VALUES(?,?,?,?,?)");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);bind(q,4,phase);bind(q,5,error);}return step(q)?sqlite3_last_insert_rowid(s->db):0;
}
static sqlite3_int64 fault(dc_store *s,const char *source,const char *package,const char *version,const char *phase,const char *error){
 extension_state *e=state(s);char identity[65];package_identity(source,package,identity);if(e){dl_terminal_close_owner(e->terminals,identity);json_object_object_del(e->active,identity);}
 sqlite3_stmt *tq=prep(s,"UPDATE extension_terminal_owners SET state='faulted' WHERE source=? AND package=? AND state='running'");if(tq){bind(tq,1,source);bind(tq,2,package);}step(tq);
 sqlite3_stmt *q=prep(s,"UPDATE extension_bindings SET enabled=0 WHERE source=? AND package=?");if(q){bind(q,1,source);bind(q,2,package);}step(q);return version_fault(s,source,package,version,phase,error);
}
static void fault_diagnostic(dc_store *s,sqlite3_int64 fault_id,J *response){
 const char *text=str(get(response,"data"),"diagnostic");
 if(fault_id<=0||!text||!*text||strlen(text)>8192)return;
 sqlite3_stmt *q=prep(s,"INSERT INTO extension_fault_details(fault,diagnostic) VALUES(?,?)");
 if(q){sqlite3_bind_int64(q,1,fault_id);bind(q,2,text);}step(q);
}
static char *native_admission(void *,const char *,const dc_hooks *);
static int native_jobs_revoke(dc_store *,const char *,const char *);
static int native_jobs_receipt(dc_store *,const char *,J *);
static int native_effects_settled(dc_store *,const char *);
static int native_settings_settled(dc_store *,const char *);
static J *worker_request(dc_store *s,dc_worker *worker,const char *method,J *params,const dc_hooks *hooks){char *raw=dc_worker_call_native(worker,method,json_object_to_json_string_ext(params,JSON_C_TO_STRING_PLAIN),hooks,native_admission,s);J *result=raw?dunlin_json_parse(raw,strlen(raw)):NULL;dc_free(raw);return result;}
/* Persist lifecycle intent before calling user code. Configuration is hashed,
 * never copied into the audit record. A missing terminal write keeps the running
 * row so startup recovery quarantines instead of replaying a partial apply. */
static int lifecycle_begin(dc_store *s,const char *source,const char *package,const char *version,J *request,char call[65]){
 char digest[65];const char *raw=json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN);dc_sha256(raw,strlen(raw),digest);
 sqlite3_stmt *q=prep(s,"SELECT lower(hex(randomblob(16)))");if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return 0;}
 snprintf(call,65,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);
 q=prep(s,"INSERT INTO extension_calls(id,source,package,version,method,args_hash,status) VALUES(?,?,?,?,?,?,'running')");
 if(q){bind(q,1,call);bind(q,2,source);bind(q,3,package);bind(q,4,version);bind(q,5,eq(str(request,"op"),"extensions.quiesce")?"plugins.quiesce":"plugins.bind");bind(q,6,digest);}if(!step(q)){call[0]=0;return 0;}return 1;
}
#include "extension_pending.inc"
/* Explicit engine stop/replacement differs from an unexpected host loss.
 * Revoke native authority first; settle pending leases only after the actual
 * old worker stops. Desired plugin bindings survive a clean environment stop. */
static int stop_dsh_environment(dc_store *s,extension_state *e){
 J *affected=json_object_new_array();json_object_object_foreach(e->active,key,value){(void)key;if(eq(str(value,"host"),"node-dsh"))json_object_array_add(affected,json_object_get(value));}
 int ok=sql(s,"UPDATE extension_bindings SET runtime_instance='' WHERE EXISTS(SELECT 1 FROM extension_versions v WHERE v.source=extension_bindings.source AND v.package=extension_bindings.package AND v.version=extension_bindings.version AND json_extract(v.manifest,'$.engines.host')='node-dsh')");
 if(!ok){json_object_put(affected);return 0;}
 for(size_t i=0;i<json_object_array_length(affected);i++){J *v=json_object_array_get_idx(affected,i);int revoked=native_jobs_revoke(s,str(v,"source"),str(v,"package"));if(!revoked)fault(s,str(v,"source"),str(v,"package"),str(v,"version"),"dispose","environment_job_revocation_unknown");ok=revoked&&ok;}
 dc_worker_close(e->dsh);e->dsh=NULL;clear_host(e,"node-dsh");
 for(size_t i=0;i<json_object_array_length(affected);i++){
  J *v=json_object_array_get_idx(affected,i);const char *source=str(v,"source"),*package=str(v,"package");int settled=pending_close(s,source,package,NULL,"cancelled_effects_possible");
  sqlite3_stmt *q=prep(s,"UPDATE extension_terminal_owners SET state='stopped' WHERE source=? AND package=? AND state='running'");if(q){bind(q,1,source);bind(q,2,package);}settled=step(q)&&settled;
  if(!settled){
   /* A failed cancelled receipt is never acknowledged as a clean stop.
    * Retain unknown where storage permits; otherwise startup recovery still
    * sees the unfinished original intent. Neither path grants a retry. */
   pending_close(s,source,package,NULL,"unknown");
   sqlite3_int64 fault_id=fault(s,source,package,str(v,"version"),"dispose","environment_shutdown_receipt_unknown");J *response=json_object_new_object(),*data=json_object_new_object();json_object_object_add(response,"data",data);json_object_object_add(data,"diagnostic",json_object_new_string("The optional engine stopped, but its durable shutdown receipts could not be confirmed. Original operations are retained for review; no automatic replay is permitted."));fault_diagnostic(s,fault_id,response);json_object_put(response);
  }ok=settled&&ok;
 }
 json_object_put(affected);return ok;
}
static J *activate(dc_store *s,J *r,const dc_hooks *hooks){
 if(!sqlite3_get_autocommit(s->db))return fail("extension_transaction_busy");
 const char *source=str(r,"source"),*package=str(r,"package"),*version=str(r,"version");J *desired=get(r,"enabled");if(!id(source)||!id(package)||!id(version)||!json_object_is_type(desired,json_type_boolean))return fail("invalid_extension_binding");int enable=json_object_get_boolean(desired);
 if(enable&&!get(r,"config")){
  sqlite3_stmt *saved=prep(s,"SELECT config FROM extension_configuration WHERE source=? AND package=? AND version=?");if(!saved)return fail("storage_error");bind(saved,1,source);bind(saved,2,package);bind(saved,3,version);int row=sqlite3_step(saved);J *value=row==SQLITE_DONE?json_object_new_object():row==SQLITE_ROW&&sqlite3_column_bytes(saved,0)<=32768?dunlin_json_parse((const char*)sqlite3_column_text(saved,0),(size_t)sqlite3_column_bytes(saved,0)):NULL;sqlite3_finalize(saved);if(!json_object_is_type(value,json_type_object)){json_object_put(value);return fail("invalid_extension_configuration");}json_object_object_add(r,"config",value);
 }
 if(eq(package,"dsh-project-manager")&&json_object_is_type(get(r,"config"),json_type_object)&&!get(get(r,"config"),"projectSessionMode"))json_object_object_add(get(r,"config"),"projectSessionMode",json_object_new_string("inherit"));
 J *configuration=get(r,"config");if(configuration&&(!json_object_is_type(configuration,json_type_object)||strlen(json_object_to_json_string_ext(configuration,JSON_C_TO_STRING_PLAIN))>32768))return fail("invalid_extension_configuration");sqlite3_stmt *q=prep(s,"SELECT manifest,path,files,quarantined FROM extension_versions WHERE source=? AND package=? AND version=?");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("extension_version_missing");}const char *raw=(const char*)sqlite3_column_text(q,0);J *manifest=dunlin_json_parse(raw,strlen(raw));char *path=strdup((const char*)sqlite3_column_text(q,1));raw=(const char*)sqlite3_column_text(q,2);J *files=dunlin_json_parse(raw,strlen(raw));int quarantined=sqlite3_column_int(q,3);sqlite3_finalize(q);const char *error=NULL;char lifecycle[65]={0};int lifecycle_unknown=0;
 extension_state *e=state(s);if(!e){error="out_of_memory";goto done;}if(enable&&!eq(str(get(manifest,"engines"),"host"),"native-theme")&&!json_object_get_boolean(get(r,"acceptHostPermissions"))){error="host_permission_confirmation_required";goto done;}if(enable&&quarantined){error="extension_quarantined";goto done;}if(enable&&(error=dc_extension_gate(s,get(manifest,"engines"),str(manifest,"kind"))))goto done;if(enable&&!dc_package_verify(path,files)){
  char runtime_id[65];package_identity(source,package,runtime_id);
  J *running=get(e->active,runtime_id);
  error="extension_integrity_failed";
  if(running&&!eq(str(running,"version"),version))version_fault(s,source,package,version,"integrity",error);
  else fault(s,source,package,version,"integrity",error);
  goto done;
 }
 char identity[65];package_identity(source,package,identity);
 dc_worker *host=eq(str(get(manifest,"engines"),"host"),"node-dsh")?e->dsh:eq(str(get(manifest,"engines"),"host"),"python")?e->python_worker:NULL;
 if(host){if(!lifecycle_begin(s,source,package,version,r,lifecycle)){error="invocation_audit_unavailable";goto done;}if(!native_jobs_revoke(s,source,package)){error="native_agent_revocation_unavailable";goto done;}J *args=json_object_new_object();json_object_object_add(args,"id",json_object_new_string(identity));J *deactivated=worker_request(s,host,"plugins.deactivate",args,hooks);if(!deactivated||!json_object_get_boolean(get(deactivated,"ok"))){lifecycle_unknown=!deactivated||eq(str(deactivated,"effectState"),"unknown");json_object_put(deactivated);json_object_put(args);error="extension_dispose_failed";fault(s,source,package,version,"dispose",error);goto done;}json_object_put(deactivated);dl_terminal_close_owner(e->terminals,identity);sqlite3_stmt *tq=prep(s,"UPDATE extension_terminal_owners SET state='stopped' WHERE source=? AND package=? AND state='running'");if(tq){bind(tq,1,source);bind(tq,2,package);}if(!step(tq)){json_object_put(args);error="terminal_disposal_audit_unavailable";lifecycle_unknown=1;goto done;}if(!pending_close(s,source,package,NULL,"cancelled_effects_possible")){json_object_put(args);error="invocation_finalization_failed";lifecycle_unknown=1;goto done;}json_object_object_del(e->active,identity);
  if(enable){json_object_object_add(args,"root",json_object_new_string(path));json_object_object_add(args,"entry",json_object_new_string(str(manifest,"entry")?str(manifest,"entry"):"index.ts"));json_object_object_add(args,"config",get(r,"config")?json_object_get(get(r,"config")):json_object_new_object());J *activated=worker_request(s,host,"plugins.activate",args,hooks);if(!activated||!json_object_get_boolean(get(activated,"ok"))){lifecycle_unknown=!activated||eq(str(activated,"effectState"),"unknown");error="extension_activation_failed";sqlite3_int64 fault_id=fault(s,source,package,version,"activate",error);fault_diagnostic(s,fault_id,activated);}json_object_put(activated);}json_object_put(args);if(error)goto done;
 }

 int persisted=sql(s,"BEGIN IMMEDIATE");
 if(persisted&&enable){q=prep(s,"INSERT INTO extension_configuration(source,package,version,config) VALUES(?,?,?,?) ON CONFLICT(source,package,version) DO UPDATE SET config=excluded.config");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);bind(q,4,configuration?json_object_to_json_string_ext(configuration,JSON_C_TO_STRING_PLAIN):"{}");}persisted=step(q);}
 if(persisted){q=prep(s,"UPDATE extension_bindings SET version=?,enabled=?,runtime_instance=? WHERE source=? AND package=?");if(q){bind(q,1,version);sqlite3_bind_int(q,2,enable);bind(q,3,enable?lifecycle:"");bind(q,4,source);bind(q,5,package);}persisted=step(q);}
 if(!sql(s,persisted?"COMMIT":"ROLLBACK")){sql(s,"ROLLBACK");persisted=0;}
 if(!persisted){lifecycle_unknown=1;error="storage_error";if(host){J *args=json_object_new_object();json_object_object_add(args,"id",json_object_new_string(identity));J *result=worker_request(s,host,"plugins.deactivate",args,NULL);json_object_put(result);json_object_put(args);}}
 if(!error){json_object_object_del(e->active,identity);if(enable){J *a=json_object_new_object();json_object_object_add(a,"instance",json_object_new_string(lifecycle));json_object_object_add(a,"source",json_object_new_string(source));json_object_object_add(a,"package",json_object_new_string(package));json_object_object_add(a,"version",json_object_new_string(version));json_object_object_add(a,"host",json_object_get(get(get(manifest,"engines"),"host")));json_object_object_add(e->active,identity,a);}}
 done:
 if(lifecycle[0]){
  q=prep(s,"UPDATE extension_calls SET status=?,finished=unixepoch() WHERE id=? AND status='running'");
  if(q){bind(q,1,lifecycle_unknown?"unknown":"reported");bind(q,2,lifecycle);}
  if(!step(q)){lifecycle_unknown=1;error="lifecycle_finalization_failed";}
  if(lifecycle_unknown){fault(s,source,package,version,"lifecycle",error?error:"lifecycle_effect_unknown");host_fault(s,e,str(get(manifest,"engines"),"host"),"lifecycle_effect_unknown");}
 }
 free(path);json_object_put(manifest);json_object_put(files);return error?fail(error):inventory(s);
}
/* Stop before removing bindings; keep forensic call/fault history. */
static J *uninstall(dc_store *s,J *r,const dc_hooks *hooks){
 const char *source=str(r,"source"),*package=str(r,"package");
 if(!id(source)||!id(package))return fail("invalid_extension_reference");
 if(!sqlite3_get_autocommit(s->db))return fail("extension_transaction_busy");
 sqlite3_stmt *q=prep(s,"SELECT version FROM extension_bindings WHERE source=? AND package=?");if(q){bind(q,1,source);bind(q,2,package);}int rc=q?sqlite3_step(q):SQLITE_ERROR;
 char *version=rc==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);
 if(!version)return fail(rc==SQLITE_DONE?"extension_version_missing":"storage_error");
 J *request=json_object_new_object();json_object_object_add(request,"source",json_object_new_string(source));json_object_object_add(request,"package",json_object_new_string(package));json_object_object_add(request,"version",json_object_new_string(version));json_object_object_add(request,"enabled",json_object_new_boolean(0));free(version);
 J *stopped=activate(s,request,hooks);json_object_put(request);int ok=stopped&&json_object_get_boolean(get(stopped,"ok"));json_object_put(stopped);if(!ok)return fail("extension_uninstall_stop_failed");
 J *paths=json_object_new_array();q=prep(s,"SELECT DISTINCT path FROM extension_versions WHERE source=? AND package=?");if(q){bind(q,1,source);bind(q,2,package);}int scanned=SQLITE_ERROR;while(q&&(scanned=sqlite3_step(q))==SQLITE_ROW)json_object_array_add(paths,json_object_new_string((const char*)sqlite3_column_text(q,0)));sqlite3_finalize(q);if(scanned!=SQLITE_DONE){json_object_put(paths);return fail("storage_error");}
 if(!sql(s,"BEGIN IMMEDIATE")){json_object_put(paths);return fail("storage_error");}
 const char *queries[]={"DELETE FROM extension_presentations WHERE source=? AND package=?","DELETE FROM extension_client_storage WHERE source=? AND package=?","DELETE FROM extension_configuration WHERE source=? AND package=?","DELETE FROM extension_bindings WHERE source=? AND package=?","DELETE FROM extension_versions WHERE source=? AND package=?","DELETE FROM catalog_packages WHERE source=? AND package=?"};
 for(size_t i=0;i<sizeof queries/sizeof *queries;i++){q=prep(s,queries[i]);if(q){bind(q,1,source);bind(q,2,package);}if(!step(q)){sql(s,"ROLLBACK");json_object_put(paths);return fail("storage_error");}}
 if(!sql(s,"COMMIT")){sql(s,"ROLLBACK");json_object_put(paths);return fail("storage_error");}int cleaned=1;for(size_t i=0;i<json_object_array_length(paths);i++)if(!dc_package_collect(s,json_object_get_string(json_object_array_get_idx(paths,i))))cleaned=0;json_object_put(paths);return cleaned?inventory(s):fail("extension_removed_cache_cleanup_failed");
}
static void host_fault(dc_store *s,extension_state *e,const char *host,const char *code){
 J *affected=json_object_new_array();json_object_object_foreach(e->active,key,value){(void)key;if(eq(str(value,"host"),host))json_object_array_add(affected,json_object_get(value));}
 for(size_t i=0;i<json_object_array_length(affected);i++){J *v=json_object_array_get_idx(affected,i);pending_close(s,str(v,"source"),str(v,"package"),NULL,"unknown");fault(s,str(v,"source"),str(v,"package"),str(v,"version"),"host",code);}json_object_put(affected);
 if(eq(host,"node-dsh")){dc_worker_close(e->dsh);e->dsh=NULL;sql(s,"UPDATE compatibility_environment SET state='faulted',error='host_unavailable'");}else{dc_worker_close(e->python_worker);e->python_worker=NULL;}
}
static J *configuration_request(dc_store *s,J *r){
 const char *source=str(r,"source"),*package=str(r,"package"),*version=str(r,"version");if(!id(source)||!id(package)||!id(version))return fail("invalid_extension_reference");
 sqlite3_stmt *q=prep(s,"SELECT coalesce(c.config,'{}') FROM extension_versions v LEFT JOIN extension_configuration c ON c.source=v.source AND c.package=v.package AND c.version=v.version WHERE v.source=? AND v.package=? AND v.version=?");if(!q)return fail("storage_error");bind(q,1,source);bind(q,2,package);bind(q,3,version);int row=sqlite3_step(q);J *value=NULL;char hash[65]="";
 if(row==SQLITE_ROW&&sqlite3_column_bytes(q,0)<=32768){const char *raw=(const char*)sqlite3_column_text(q,0);value=dunlin_json_parse(raw,(size_t)sqlite3_column_bytes(q,0));dc_sha256(raw,strlen(raw),hash);}sqlite3_finalize(q);if(!json_object_is_type(value,json_type_object)){json_object_put(value);return fail(row==SQLITE_DONE?"extension_version_missing":"invalid_extension_configuration");}
 char identity[65];package_identity(source,package,identity);extension_state *e=state(s);J *active=e?get(e->active,identity):NULL;int running=active&&eq(str(active,"version"),version);
 if(eq(str(r,"op"),"extensions.config.save")){
  J *config=get(r,"config");const char *expected=str(r,"expectedHash");if(running){json_object_put(value);return fail("extension_configuration_requires_disable");}
  if(!expected||strcmp(expected,hash)){json_object_put(value);return fail("extension_configuration_changed");}
  if(!json_object_is_type(config,json_type_object)||strlen(json_object_to_json_string_ext(config,JSON_C_TO_STRING_PLAIN))>32768){json_object_put(value);return fail("invalid_extension_configuration");}
  const char *raw=json_object_to_json_string_ext(config,JSON_C_TO_STRING_PLAIN);q=prep(s,"INSERT INTO extension_configuration(source,package,version,config) VALUES(?,?,?,?) ON CONFLICT(source,package,version) DO UPDATE SET config=excluded.config");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);bind(q,4,raw);}if(!step(q)){json_object_put(value);return fail("storage_error");}json_object_put(value);value=json_object_get(config);dc_sha256(raw,strlen(raw),hash);
 }
 J *out=json_object_new_object();json_object_object_add(out,"config",value);json_object_object_add(out,"hash",json_object_new_string(hash));json_object_object_add(out,"active",json_object_new_boolean(running));return reply(1,NULL,out);
}
#include "extension_presentation.inc"
static J *client_storage(dc_store *s,J *r){
 const char *source=str(r,"source"),*package=str(r,"package"),*version=str(r,"version");if(!id(source)||!id(package)||!id(version))return fail("invalid_client_storage_owner");
 char identity[65];package_identity(source,package,identity);extension_state *e=state(s);J *active=e?get(e->active,identity):NULL;if(!active||!eq(version,str(active,"version")))return fail("stale_client_storage");if(get(r,"instance")&&!eq(str(r,"instance"),str(active,"instance")))return fail("extension_instance_changed");
 J *value=get(r,"value");sqlite3_stmt *q;
 if(eq(str(r,"op"),"extensions.clientStorage.write")){
  if(!json_object_is_type(value,json_type_object)||json_object_object_length(value)>128||strlen(json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN))>32768)return fail("client_storage_limit");
  json_object_object_foreach(value,key,item){if(strlen(key)>256||!json_object_is_type(item,json_type_string)||json_object_get_string_len(item)>8192||(size_t)json_object_get_string_len(item)!=strlen(json_object_get_string(item)))return fail("invalid_client_storage_value");}
  q=prep(s,"INSERT INTO extension_client_storage(source,package,value) VALUES(?,?,?) ON CONFLICT(source,package) DO UPDATE SET value=excluded.value");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN));}if(!step(q))return fail("storage_error");return reply(1,NULL,json_object_get(value));
 }
 q=prep(s,"SELECT value FROM extension_client_storage WHERE source=? AND package=?");if(!q)return fail("storage_error");bind(q,1,source);bind(q,2,package);int row=sqlite3_step(q);J *out=row==SQLITE_DONE?json_object_new_object():row==SQLITE_ROW?dunlin_json_parse((const char*)sqlite3_column_text(q,0),(size_t)sqlite3_column_bytes(q,0)):NULL;sqlite3_finalize(q);return out?reply(1,NULL,out):fail("storage_error");
}
static J *model_tools(dc_store *s,const dc_hooks *hooks){
 extension_state *e=state(s);J *out=json_object_new_array();if(!e||!dc_worker_alive(e->dsh))return reply(1,NULL,out);
 J *params=json_object_new_object(),*response=worker_request(s,e->dsh,"plugins.tools",params,hooks);json_object_put(params);J *rows=get(response,"data");
 if(!json_object_get_boolean(get(response,"ok"))||!json_object_is_type(rows,json_type_array)){json_object_put(response);json_object_put(out);return fail("extension_tool_inventory_unavailable");}
 for(size_t i=0;i<json_object_array_length(rows)&&i<128;i++){
  J *row=json_object_array_get_idx(rows,i),*active=get(e->active,str(row,"plugin")?str(row,"plugin"):"");if(!active)continue;
  sqlite3_stmt *q=prep(s,"SELECT v.digest,coalesce(c.config,'{}') FROM extension_versions v LEFT JOIN extension_configuration c ON c.source=v.source AND c.package=v.package AND c.version=v.version WHERE v.source=? AND v.package=? AND v.version=? AND v.quarantined=0");if(q){bind(q,1,str(active,"source"));bind(q,2,str(active,"package"));bind(q,3,str(active,"version"));}
  if(q&&sqlite3_step(q)==SQLITE_ROW){J *copy=dunlin_json_parse(json_object_to_json_string_ext(row,JSON_C_TO_STRING_PLAIN),strlen(json_object_to_json_string_ext(row,JSON_C_TO_STRING_PLAIN)));json_object_object_add(copy,"digest",json_object_new_string((const char*)sqlite3_column_text(q,0)));const char *config=(const char*)sqlite3_column_text(q,1);char config_hash[65];dc_sha256(config,strlen(config),config_hash);json_object_object_add(copy,"configurationHash",json_object_new_string(config_hash));for(size_t n=0;n<3;n++){const char *key=(const char*[]){"source","package","version"}[n];json_object_object_add(copy,key,json_object_get(get(active,key)));}json_object_array_add(out,copy);}sqlite3_finalize(q);
 }
 json_object_put(response);return reply(1,NULL,out);
}
#include "extension_market.inc"
static J *native_receipt_fault(dc_store *s,extension_state *e,dc_worker *worker,const char *source,const char *package,const char *version,const char *host,const char *identity,const char *call,const char *error){
 sqlite3_stmt *q=prep(s,"UPDATE extension_native_effects SET status='unknown' WHERE call=? AND status='running'");if(q)bind(q,1,call);step(q);
 q=prep(s,"UPDATE extension_native_settings SET status='unknown' WHERE call=? AND status='pending'");if(q)bind(q,1,call);step(q);
 q=prep(s,"UPDATE extension_calls SET status='unknown',finished=unixepoch() WHERE id=?");if(q)bind(q,1,call);step(q);
 native_jobs_revoke(s,source,package);pending_close(s,source,package,NULL,"unknown");
 sqlite3_int64 fault_id=fault(s,source,package,version,"native",error);
 char note[256];snprintf(note,sizeof note,"Native receipt is unknown for call %.32s. Partial effects are retained; execution is disabled and must not be retried automatically.",call);
 J *diagnostic=json_object_new_object(),*detail=json_object_new_object();json_object_object_add(diagnostic,"data",detail);json_object_object_add(detail,"diagnostic",json_object_new_string(note));fault_diagnostic(s,fault_id,diagnostic);json_object_put(diagnostic);
 J *params=json_object_new_object();json_object_object_add(params,"id",json_object_new_string(identity));J *stopped=worker_request(s,worker,"plugins.deactivate",params,NULL);json_object_put(params);
 int cleaned=stopped&&json_object_get_boolean(get(stopped,"ok"));json_object_put(stopped);if(!cleaned)host_fault(s,e,host,"native_receipt_cleanup_failed");return fail(error);
}
static J *command(dc_store *s,J *r,const dc_hooks *hooks){
 const char *source=str(r,"source"),*package=str(r,"package"),*method=str(r,"command");if(!id(source)||!id(package)||!id(method))return fail("invalid_extension_command");extension_state *e=state(s);char identity[65];package_identity(source,package,identity);J *active=get(e->active,identity);if(!active)return fail("extension_not_active");if(get(r,"instance")&&!eq(str(r,"instance"),str(active,"instance")))return fail("extension_instance_changed");if(get(r,"version")&&!eq(str(r,"version"),str(active,"version")))return fail("extension_version_changed");char host[32],version[257];snprintf(host,sizeof host,"%s",str(active,"host")?str(active,"host"):"");snprintf(version,sizeof version,"%s",str(active,"version")?str(active,"version"):"");dc_worker *worker=eq(host,"node-dsh")?e->dsh:e->python_worker;if(!dc_worker_alive(worker)){host_fault(s,e,host,"host_unavailable");return fail("extension_host_unavailable");}
 sqlite3_stmt *check=prep(s,"SELECT quarantined FROM extension_versions WHERE source=? AND package=? AND version=?");if(check){bind(check,1,source);bind(check,2,package);bind(check,3,version);}int checked=check?sqlite3_step(check):SQLITE_ERROR;int blocked=checked!=SQLITE_ROW||sqlite3_column_int(check,0);sqlite3_finalize(check);if(blocked)return fail("extension_quarantined");
 if(eq(method,"http")){
  const char *url=str(get(r,"arguments"),"url");if(url&&!strncmp(url,"/sidebar/bundle/",16)){
   sqlite3_stmt *q=prep(s,"SELECT path,files FROM extension_versions WHERE source=? AND package=? AND version=?");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}int intact=0;if(q&&sqlite3_step(q)==SQLITE_ROW){const char *path=(const char*)sqlite3_column_text(q,0),*raw=(const char*)sqlite3_column_text(q,1);J *files=dunlin_json_parse(raw,strlen(raw));intact=dc_package_verify(path,files);json_object_put(files);}sqlite3_finalize(q);
   if(!intact){J *params=json_object_new_object();json_object_object_add(params,"id",json_object_new_string(identity));J *stopped=worker_request(s,worker,"plugins.deactivate",params,hooks);json_object_put(params);int cleaned=json_object_get_boolean(get(stopped,"ok"));json_object_put(stopped);if(!cleaned)host_fault(s,e,host,"lazy_client_cleanup_failed");fault(s,source,package,version,"client","extension_client_integrity_failed");return fail("extension_client_integrity_failed");}
  }
 }
 const char *expected_configuration=str(r,"configurationHash");if(expected_configuration){sqlite3_stmt *config=prep(s,"SELECT coalesce((SELECT config FROM extension_configuration WHERE source=? AND package=? AND version=?),'{}')");if(config){bind(config,1,source);bind(config,2,package);bind(config,3,version);}char hash[65]="";if(config&&sqlite3_step(config)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(config,0);dc_sha256(raw,strlen(raw),hash);}sqlite3_finalize(config);if(strcmp(expected_configuration,hash))return fail("extension_configuration_changed");}
 if(eq(method,"http.pending")&&!pending_owned(s,r,active))return fail("extension_request_unavailable");
 J *context=NULL;const char *chat=str(r,"chat");
 if(chat&&!eq(method,"http.pending")&&!eq(method,"ui.close")&&!eq(method,"http.upload.release")){
  context=dc_chat_workspace_view(s,chat);if(!context||!json_object_get_boolean(get(context,"available"))){json_object_put(context);return fail("extension_chat_workspace_unavailable");}
  json_object_object_add(context,"chat",json_object_new_string(chat));
  sqlite3_stmt *cq=prep(s,"SELECT state FROM chats WHERE id=?");if(cq)bind(cq,1,chat);
  if(cq&&sqlite3_step(cq)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(cq,0);J *checkpoint=raw&&strlen(raw)<=8388608?dunlin_json_parse(raw,strlen(raw)):NULL;J *messages=get(checkpoint,"messages");int turns=0;
   if(json_object_is_type(messages,json_type_array))for(size_t i=0;i<json_object_array_length(messages);i++)if(eq(str(json_object_array_get_idx(messages,i),"role"),"user"))turns++;
   int rounds=json_object_get_int(get(checkpoint,"rounds"));if(turns>0&&rounds>0&&!json_object_get_boolean(get(checkpoint,"active"))){J *pos=json_object_new_object();json_object_object_add(pos,"turn",json_object_new_int(turns));json_object_object_add(pos,"step",json_object_new_int(rounds));json_object_object_add(context,"position",pos);}json_object_put(checkpoint);
  }sqlite3_finalize(cq);
 }

 J *arguments=get(r,"arguments");if(!json_object_is_type(arguments,json_type_object)){json_object_put(context);return fail("invalid_extension_arguments");}const char *raw=json_object_to_json_string_ext(arguments,JSON_C_TO_STRING_PLAIN);if(strlen(raw)>131072){json_object_put(context);return fail("extension_arguments_exceed_limit");}char digest[65],call[65],full[512];dc_sha256(raw,strlen(raw),digest);if(!strncmp(method,identity,64)&&method[64]==':')snprintf(full,sizeof full,"%s",method);else snprintf(full,sizeof full,"%s:%s",identity,method);
 sqlite3_stmt *q=prep(s,"SELECT lower(hex(randomblob(16)))");if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);json_object_put(context);return fail("storage_error");}snprintf(call,sizeof call,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);q=prep(s,"INSERT INTO extension_calls(id,source,package,version,method,args_hash,status) VALUES(?,?,?,?,?,?,'running')");if(q){const char *v[]={call,source,package,version,method,digest};for(int i=0;i<6;i++)bind(q,i+1,v[i]);}if(!step(q)){json_object_put(context);return fail("invocation_audit_unavailable");}
 q=prep(s,"INSERT INTO extension_native_scopes(call,instance,chat,surface,operation) VALUES(?,?,?,?,?)");if(q){bind(q,1,call);bind(q,2,str(active,"instance"));bind(q,3,chat);bind(q,4,str(arguments,"_surface"));const char *url=str(arguments,"url");int interactive=eq(package,"dsh-better-sidebar")&&eq(method,"http")&&eq(str(arguments,"method"),"POST")&&(eq(url,"/sidebar/api/sidechat.prompt")||eq(url,"/sidebar/api/sidechat.start"));bind(q,5,eq(method,"ui.prompt")?"interactive-session":interactive?"interactive-agent":"none");}if(!step(q)){json_object_put(context);return fail("native_scope_audit_unavailable");}
 J *helper_plan=dc_helpers_plan(s,call,r);if(!json_object_get_boolean(get(helper_plan,"ok"))){json_object_put(context);q=prep(s,"UPDATE extension_calls SET status='reported_failure',finished=unixepoch() WHERE id=?");if(q)bind(q,1,call);step(q);return helper_plan;}json_object_put(helper_plan);
 J *params=json_object_new_object();json_object_object_add(params,"nativeCall",json_object_new_string(call));if(eq(package,"dshmarket")){J *rows=inventory(s);if(!json_object_get_boolean(get(rows,"ok"))){json_object_put(rows);json_object_put(params);json_object_put(context);return fail("storage_error");}json_object_object_add(params,"nativeInventory",json_object_get(get(rows,"data")));json_object_object_add(params,"nativeManagedInstall",market_install_policy());json_object_put(rows);}if(context)json_object_object_add(params,"nativeContext",context);json_object_object_add(params,"plugin",json_object_new_string(identity));json_object_object_add(params,"command",json_object_new_string(full));json_object_object_add(params,"arguments",json_object_get(arguments));json_object_object_add(params,"nativeQuestions",json_object_new_boolean(hooks&&hooks->approve));J *result=eq(method,"http")?market_http_action(s,package,arguments,call,hooks):NULL;if(!result)result=worker_request(s,worker,"plugins.command",params,hooks);json_object_put(params);
 for(unsigned count=0;result&&json_object_get_boolean(get(result,"ok"))&&get(get(result,"data"),"nativeQuestion");count++){
  J *data=get(result,"data"),*question=get(data,"nativeQuestion");const char *continuation=str(data,"continuation");
  if(count>=4||!continuation||strlen(continuation)>64||!json_object_is_type(question,json_type_object)||strlen(json_object_to_json_string_ext(question,JSON_C_TO_STRING_PLAIN))>32768){json_object_put(result);result=NULL;break;}
  J *review=json_object_new_object();json_object_object_add(review,"package",json_object_new_string(package));json_object_object_add(review,"version",json_object_new_string(version));json_object_object_add(review,"question",json_object_get(question));const char *raw=json_object_to_json_string_ext(review,JSON_C_TO_STRING_PLAIN);char question_hash[65];dc_sha256(raw,strlen(raw),question_hash);
  int allowed=hooks&&hooks->approve&&(!hooks->cancelled||!hooks->cancelled(hooks->user))&&hooks->approve(hooks->user,"extension.confirm",raw)>0;json_object_put(review);
  q=prep(s,"INSERT INTO extension_questions(call,id,question_hash,decision) VALUES(?,?,?,?)");if(q){bind(q,1,call);bind(q,2,continuation);bind(q,3,question_hash);bind(q,4,allowed?"allowed_once":"denied");}int audited=step(q);
  if(audited&&allowed&&eq(package,"dsh-checkpoint-rewind")&&eq(method,"slash:rewind")&&(eq(str(question,"question"),"Restore the plugin configuration to this checkpoint snapshot?")||eq(str(question,"question"),"Restore the workspace, session context, and/or configuration to this checkpoint?"))){q=prep(s,"UPDATE extension_native_scopes SET operation='interactive-config-restore' WHERE call=?");if(q)bind(q,1,call);audited=step(q)&&sqlite3_changes(s->db)==1;}if(!audited)allowed=0;
  params=json_object_new_object();json_object_object_add(params,"plugin",json_object_new_string(identity));json_object_object_add(params,"id",json_object_new_string(continuation));json_object_object_add(params,"allowed",json_object_new_boolean(allowed));json_object_put(result);result=worker_request(s,worker,"plugins.answer",params,hooks);json_object_put(params);if(!audited){json_object_put(result);result=NULL;break;}
 }
 /* The published handler may catch a native exception. A missing C receipt
  * remains authoritative, and cleanup is safe only after the FD3 frame unwinds. */
 if(!native_effects_settled(s,call)||!native_settings_settled(s,call)||dc_helpers_uncertain(s,source,package,str(active,"instance"))){
  json_object_put(result);return native_receipt_fault(s,e,worker,source,package,version,host,identity,call,"native_effect_receipt_unknown");
 }
 int retained=0;J *pending_data=get(result,"data");
 if(result&&json_object_get_boolean(get(result,"ok"))){
  if((eq(method,"http")||eq(method,"http.upload.commit"))&&get(pending_data,"pendingHandle")){retained=pending_begin(s,r,active,version,call,str(pending_data,"pendingHandle"));if(!retained){json_object_put(result);result=NULL;}}
  else if(eq(method,"http.pending")&&json_object_get_boolean(get(pending_data,"ready"))){if(!pending_receipt(s,str(arguments,"id"),get(pending_data,"response"))||!pending_finish(s,str(arguments,"id"),json_object_get_boolean(get(arguments,"release"))&&json_object_get_boolean(get(pending_data,"cancelled"))?"cancelled_effects_possible":"reported")){json_object_put(result);result=NULL;}}
  else if(eq(method,"ui.close")){char owner[161];snprintf(owner,sizeof owner,"%s:%s",identity,str(active,"instance"));dl_terminal_release_surface(e->terminals,owner,str(arguments,"_surface"));if(!pending_close(s,source,package,str(arguments,"_surface")?str(arguments,"_surface"):"","cancelled_effects_possible")){json_object_put(result);result=NULL;}}
 }else if(eq(method,"http.pending")&&result&&!pending_finish(s,str(arguments,"id"),"reported_failure")){json_object_put(result);result=NULL;}
 if(result&&json_object_get_boolean(get(result,"ok"))&&(eq(method,"http")||eq(method,"ui.prompt"))&&!get(get(result,"data"),"pendingHandle")&&!native_jobs_receipt(s,call,get(result,"data"))){json_object_put(result);result=NULL;}
 const char *effect=result?str(result,"effectState"):"unknown";if(!effect)effect="unknown";
 q=prep(s,retained?"UPDATE extension_calls SET status='running' WHERE id=?2":"UPDATE extension_calls SET status=?1,finished=unixepoch() WHERE id=?2");if(q){bind(q,1,effect);bind(q,2,call);}if(!step(q)){json_object_put(result);host_fault(s,e,host,"invocation_finalization_failed");return fail("invocation_finalization_failed");}
 if(retained&&!dc_helpers_handoff(s,call)){json_object_put(result);return native_receipt_fault(s,e,worker,source,package,version,host,identity,call,"helper_handoff_receipt_unknown");}
 int success=result&&json_object_get_boolean(get(result,"ok"));if(!success){if(eq(effect,"unknown")){host_fault(s,e,host,"host_disconnected_or_protocol_error");}else if(json_object_get_int(get(get(result,"data"),"code"))==-32603){char *saved=strdup(version);sqlite3_int64 fault_id=fault(s,source,package,saved,"command","plugin_unhandled_error");fault_diagnostic(s,fault_id,result);free(saved);J *params=json_object_new_object();json_object_object_add(params,"id",json_object_new_string(identity));J *stopped=worker_request(s,worker,"plugins.deactivate",params,NULL);json_object_put(stopped);json_object_put(params);}}
 if(success&&eq(package,"dsh-project-manager")&&eq(method,"http")){
  const char *url=str(arguments,"url");J *body=get(arguments,"body"),*response=get(result,"data"),*response_body=get(response,"body");const char *cid=str(body,"sessionId"),*title=str(response_body,"title");
  if((eq(url,"/_dsh/dsh-project-manager/api/session/bind")||eq(url,"/_dsh/dsh-project-manager/session/bind"))&&id(cid)&&title&&json_object_get_boolean(get(response_body,"ok"))&&!dc_rename_chat(s,cid,title)){json_object_put(result);return fail("native_session_title_sync_failed");}
 }
 J *out=reply(success,success?NULL:eq(effect,"unknown")?"extension_effect_unknown":"extension_command_failed",result);return out;
}
static void enforce_unlocked(dc_store *s,const dc_hooks *hooks){
 extension_state *e=state(s);if(!e)return;J *checklist=json_object_new_array();json_object_object_foreach(e->active,key,value){J *v=json_object_new_object();json_object_object_add(v,"id",json_object_new_string(key));json_object_object_add(v,"record",json_object_get(value));json_object_array_add(checklist,v);}
 for(size_t i=0;i<json_object_array_length(checklist);i++){J *item=json_object_array_get_idx(checklist,i),*v=get(item,"record");sqlite3_stmt *q=prep(s,"SELECT quarantined FROM extension_versions WHERE source=? AND package=? AND version=?");if(q){bind(q,1,str(v,"source"));bind(q,2,str(v,"package"));bind(q,3,str(v,"version"));}int row=q?sqlite3_step(q):SQLITE_ERROR;int denied=row!=SQLITE_ROW||sqlite3_column_int(q,0);sqlite3_finalize(q);if(!denied)continue;const char *host=str(v,"host");dc_worker *worker=eq(host,"node-dsh")?e->dsh:eq(host,"python")?e->python_worker:NULL;if(worker){J *p=json_object_new_object();json_object_object_add(p,"id",json_object_get(get(item,"id")));J *stopped=worker_request(s,worker,"plugins.deactivate",p,hooks);json_object_put(p);if(!stopped||!json_object_get_boolean(get(stopped,"ok")))host_fault(s,e,host,"quarantine_dispose_failed");json_object_put(stopped);}json_object_object_del(e->active,str(item,"id"));}
 json_object_put(checklist);
}
static int color(const char *p){return p&&strlen(p)==7&&p[0]=='#'&&strspn(p+1,"0123456789abcdefABCDEF")==6;}
static int color_value(J *v){return json_object_is_type(v,json_type_string)&&json_object_get_string_len(v)==7&&color(json_object_get_string(v));}
static J *theme_tokens(J *values){
 J *tokens=json_object_new_object();const char *keys[]={"background","surface","text","secondary"};
 for(size_t k=0;k<4;k++){J *v=get(values,keys[k]);if(color_value(v))json_object_object_add(tokens,keys[k],json_object_get(v));}
 return tokens;
}
static J *web_theme_tokens(J *values){
 if(!json_object_is_type(values,json_type_object)||json_object_object_length(values)>256)return NULL;J *out=json_object_new_object();
 json_object_object_foreach(values,key,value){const char *text=json_object_is_type(value,json_type_string)?json_object_get_string(value):NULL;size_t n=text?strlen(text):0;
  if((strncmp(key,"--dsw-",6)&&strncmp(key,"--shiki-",8))||strlen(key)>118||strspn(key,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")!=strlen(key)||!text||!n||n>256||n!=(size_t)json_object_get_string_len(value)||strspn(text,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#(),.% -")!=n||strstr(text,"url")||(!color(text)&&strncmp(text,"rgba(",5)&&strncmp(text,"color-mix(in srgb, ",19))){json_object_put(out);return NULL;}
  json_object_object_add(out,key,json_object_get(value));
 }return out;
}
static J *themes(dc_store *s,const dc_hooks *hooks){
 J *a=json_object_new_array();sqlite3_stmt *q=prep(s,"SELECT source,package,version,digest,manifest FROM extension_versions WHERE quarantined=0 ORDER BY rowid DESC");if(!q){json_object_put(a);return fail("storage_error");}int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,4);J *m=dunlin_json_parse(raw,strlen(raw)),*theme=get(m,"theme");if(eq(str(m,"kind"),"theme")&&eq(str(get(m,"engines"),"host"),"native-theme")&&color_value(get(get(theme,"tokens"),"accent"))&&(eq(str(theme,"scheme"),"light")||eq(str(theme,"scheme"),"dark"))){J *o=json_object_new_object();char identity[65],key[200];package_identity((const char*)sqlite3_column_text(q,0),(const char*)sqlite3_column_text(q,1),identity);snprintf(key,sizeof key,"extension:%s:%s",sqlite3_column_text(q,3),identity);json_object_object_add(o,"id",json_object_new_string(key));json_object_object_add(o,"name",json_object_new_string(str(m,"displayName")?str(m,"displayName"):str(m,"packageId")));json_object_object_add(o,"scheme",json_object_get(get(theme,"scheme")));json_object_object_add(o,"accent",json_object_get(get(get(theme,"tokens"),"accent")));json_object_object_add(o,"tokens",theme_tokens(get(theme,"tokens")));json_object_array_add(a,o);}json_object_put(m);}
 sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}extension_state *e=state(s);dc_worker *hosts[]={e->dsh,e->python_worker};for(size_t h=0;h<2;h++){if(!dc_worker_alive(hosts[h]))continue;J *p=json_object_new_object(),*r=worker_request(s,hosts[h],"plugins.contributions",p,hooks);json_object_put(p);J *data=get(r,"data");for(size_t i=0;json_object_is_type(data,json_type_array)&&i<json_object_array_length(data);i++){J *v=json_object_array_get_idx(data,i);if(!eq(str(v,"kind"),"theme")||!color_value(get(get(v,"tokens"),"accent"))||(!eq(str(v,"scheme"),"light")&&!eq(str(v,"scheme"),"dark")))continue;J *o=json_object_new_object();char key[512];snprintf(key,sizeof key,"plugin:%s",str(v,"id"));json_object_object_add(o,"id",json_object_new_string(key));json_object_object_add(o,"name",json_object_new_string(str(v,"title")?str(v,"title"):str(v,"name")?str(v,"name"):"扩展主题"));json_object_object_add(o,"preference",json_object_new_string(str(v,"name")?str(v,"name"):""));json_object_object_add(o,"scheme",json_object_get(get(v,"scheme")));json_object_object_add(o,"accent",json_object_get(get(get(v,"tokens"),"accent")));json_object_object_add(o,"tokens",theme_tokens(get(v,"tokens")));J *web=web_theme_tokens(get(v,"webTokens"));if(web)json_object_object_add(o,"webTokens",web);json_object_array_add(a,o);}json_object_put(r);}return reply(1,NULL,a);
}
static J *recover(dc_store *s,J *r){
 const char *source=str(r,"source"),*package=str(r,"package"),*version=str(r,"version");if(!id(source)||!id(package)||!id(version))return fail("invalid_extension_reference");sqlite3_stmt *q=prep(s,"SELECT 1 FROM extension_calls WHERE source=? AND package=? AND status='unknown' LIMIT 1");if(q){bind(q,1,source);bind(q,2,package);}int pending=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);if(pending==SQLITE_ROW)return fail("unresolved_extension_effect_unknown");if(pending!=SQLITE_DONE)return fail("storage_error");
 q=prep(s,"SELECT path,files,manifest,error FROM extension_versions WHERE source=? AND package=? AND version=?");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("extension_version_missing");}if(eq((const char*)sqlite3_column_text(q,3),"catalog_withdrawal")){sqlite3_finalize(q);return fail("version_withdrawn");}char *path=strdup((const char*)sqlite3_column_text(q,0));const char *raw=(const char*)sqlite3_column_text(q,1);J *files=dunlin_json_parse(raw,strlen(raw));raw=(const char*)sqlite3_column_text(q,2);J *manifest=dunlin_json_parse(raw,strlen(raw));sqlite3_finalize(q);int intact=dc_package_verify(path,files);const char *gate=dc_extension_gate(s,get(manifest,"engines"),str(manifest,"kind"));free(path);json_object_put(files);json_object_put(manifest);if(!intact)return fail("extension_integrity_failed");if(gate)return fail(gate);
 q=prep(s,"UPDATE extension_versions SET quarantined=0,error='' WHERE source=? AND package=? AND version=?");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}return step(q)?inventory(s):fail("storage_error");
}
/* Startup is explicit and reuses only previously approved executable paths.
 * No download/upgrade and no retry of quarantined or unknown-effect packages. */
static J *restore(dc_store *s,extension_state *e,const dc_hooks *hooks){
 if(!e->assets)return fail("compatibility_assets_missing");
 int restored=0,failed=0;J *errors=json_object_new_array();
 const char *queries[]={"SELECT node FROM compatibility_environment WHERE desired=1","SELECT executable FROM extension_python WHERE desired=1"};
 for(int i=0;i<2;i++){
  if(dc_worker_alive(i?e->python_worker:e->dsh))continue;
  sqlite3_stmt *q=prep(s,queries[i]);int rc=q?sqlite3_step(q):SQLITE_ERROR;
  char *path=rc==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);
  if(rc==SQLITE_DONE)continue;
  if(rc!=SQLITE_ROW||!path||!*path||access(path,X_OK)){failed++;json_object_array_add(errors,json_object_new_string(i?"python_saved_engine_unavailable":"dsh_saved_engine_unavailable"));free(path);continue;}
  J *request=json_object_new_object();json_object_object_add(request,"op",json_object_new_string(i?"python.configure":"compat.enable"));json_object_object_add(request,i?"executable":"node",json_object_new_string(path));free(path);
  J *result=dc_extensions_request(s,request,hooks);json_object_put(request);
  if(!result||!json_object_get_boolean(get(result,"ok"))){failed++;json_object_array_add(errors,json_object_new_string(str(result,"error")?str(result,"error"):"environment_restore_failed"));}json_object_put(result);
 }
 sqlite3_stmt *q=prep(s,"SELECT b.source,b.package,b.version,COALESCE(c.config,'{}') FROM extension_bindings b JOIN extension_versions v ON v.source=b.source AND v.package=b.package AND v.version=b.version LEFT JOIN extension_configuration c ON c.source=b.source AND c.package=b.package AND c.version=b.version WHERE b.enabled=1 AND v.quarantined=0 AND (json_extract(v.manifest,'$.engines.host')='native-theme' OR (json_extract(v.manifest,'$.engines.host')='node-dsh' AND EXISTS(SELECT 1 FROM compatibility_environment WHERE desired=1)) OR (json_extract(v.manifest,'$.engines.host')='python' AND EXISTS(SELECT 1 FROM extension_python WHERE desired=1))) ORDER BY b.source,b.package");
 J *pending=json_object_new_array();int rc=SQLITE_ERROR;
 while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){
  J *item=json_object_new_object();const char *keys[]={"source","package","version"};for(int i=0;i<3;i++)json_object_object_add(item,keys[i],json_object_new_string((const char*)sqlite3_column_text(q,i)));
  const char *raw=(const char*)sqlite3_column_text(q,3);J *config=dunlin_json_parse(raw,strlen(raw));json_object_object_add(item,"config",config);json_object_object_add(item,"enabled",json_object_new_boolean(1));json_object_object_add(item,"acceptHostPermissions",json_object_new_boolean(1));json_object_array_add(pending,item);
 }
 sqlite3_finalize(q);
 if(rc!=SQLITE_DONE){json_object_put(pending);json_object_put(errors);return fail("storage_error");}
 for(size_t i=0;i<json_object_array_length(pending);i++){
  if(hooks&&hooks->cancelled&&hooks->cancelled(hooks->user)){failed++;json_object_array_add(errors,json_object_new_string("restore_cancelled"));break;}
  J *item=json_object_array_get_idx(pending,i);char identity[65];package_identity(str(item,"source"),str(item,"package"),identity);
  if(eq(str(get(e->active,identity),"version"),str(item,"version")))continue;
  if(!json_object_is_type(get(item,"config"),json_type_object)){failed++;json_object_array_add(errors,json_object_new_string("saved_configuration_invalid"));continue;}
  J *result=activate(s,item,hooks);if(result&&json_object_get_boolean(get(result,"ok")))restored++;else{failed++;json_object_array_add(errors,json_object_new_string(str(result,"error")?str(result,"error"):"extension_restore_failed"));}json_object_put(result);
 }
 json_object_put(pending);J *report=json_object_new_object();json_object_object_add(report,"restored",json_object_new_int(restored));json_object_object_add(report,"failed",json_object_new_int(failed));json_object_object_add(report,"errors",errors);return reply(1,NULL,report);
}
#include "extension_client_patches.inc"
static J *clients(dc_store *s,const dc_hooks *hooks){
 dc_extensions_enforce(s,hooks);extension_state *e=state(s);J *a=json_object_new_array();
 if(!e||!dc_worker_alive(e->dsh))return reply(1,NULL,a);
 sqlite3_stmt *q=prep(s,"SELECT b.source,b.package,b.version,v.manifest,v.path,v.files,v.digest,COALESCE(c.config,'{}') FROM extension_bindings b JOIN extension_versions v ON v.source=b.source AND v.package=b.package AND v.version=b.version LEFT JOIN extension_configuration c ON c.source=b.source AND c.package=b.package AND c.version=b.version WHERE b.enabled=1 AND v.quarantined=0 ORDER BY b.package,b.source");
 if(!q){json_object_put(a);return fail("storage_error");}int rc;size_t total=0;
 while((rc=sqlite3_step(q))==SQLITE_ROW){
  const char *source=(const char*)sqlite3_column_text(q,0),*package=(const char*)sqlite3_column_text(q,1),*version=(const char*)sqlite3_column_text(q,2);char identity[65];package_identity(source,package,identity);
  J *active=get(e->active,identity);if(!eq(str(active,"version"),version)||!eq(str(active,"host"),"node-dsh"))continue;
  const char *raw=(const char*)sqlite3_column_text(q,3);J *manifest=dunlin_json_parse(raw,strlen(raw)),*client=get(manifest,"client");if(!client){json_object_put(manifest);continue;}
  raw=(const char*)sqlite3_column_text(q,5);J *files=dunlin_json_parse(raw,strlen(raw));const char *path=(const char*)sqlite3_column_text(q,4);
  char *code=client_manifest_valid(manifest)&&dc_package_verify(path,files)?dc_package_read_text(path,files,str(client,"entry")):NULL;
  J *repairs=json_object_new_array();char upstream_hash[65]="";if(code)dc_sha256(code,strlen(code),upstream_hash);code=client_repair(manifest,code,repairs);
  J *client_slots=json_object_get(get(get(manifest,"client"),"slots"));json_object_put(files);json_object_put(manifest);
  if(!code){
   json_object_put(repairs);json_object_put(client_slots);J *params=json_object_new_object();json_object_object_add(params,"id",json_object_new_string(identity));J *stopped=worker_request(s,e->dsh,"plugins.deactivate",params,hooks);json_object_put(params);
   int cleaned=stopped&&json_object_get_boolean(get(stopped,"ok"));json_object_put(stopped);
   if(!cleaned)host_fault(s,e,"node-dsh","client_integrity_cleanup_failed");
   fault(s,source,package,version,"client","extension_client_integrity_failed");continue;
  }
  total+=strlen(code);if(total>33554432||json_object_array_length(a)>=32){json_object_put(repairs);json_object_put(client_slots);free(code);sqlite3_finalize(q);json_object_put(a);return fail("extension_client_limit");}
  J *o=json_object_new_object();json_object_object_add(o,"source",json_object_new_string(source));json_object_object_add(o,"package",json_object_new_string(package));json_object_object_add(o,"version",json_object_new_string(version));json_object_object_add(o,"runtimeID",json_object_new_string(identity));json_object_object_add(o,"instance",json_object_get(get(active,"instance")));json_object_object_add(o,"digest",json_object_new_string((const char*)sqlite3_column_text(q,6)));json_object_object_add(o,"code",json_object_new_string(code));json_object_object_add(o,"slots",client_slots);json_object_object_add(o,"clientRepairs",repairs);json_object_object_add(o,"upstreamClientSHA256",json_object_new_string(upstream_hash));json_object_object_add(o,"presentationPublisher",json_object_new_boolean(presentation_client(package,upstream_hash)));free(code);
  raw=(const char*)sqlite3_column_text(q,7);json_object_object_add(o,"config",dunlin_json_parse(raw,strlen(raw)));json_object_array_add(a,o);
 }
 sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}return reply(1,NULL,a);
}
/* Freeze real upstream prompt contributions with their C-owned package and
 * configuration identities. Called only at native model admission/resume. */
static J *context_snapshot_unlocked(dc_store *s,const char *chat){
 dc_extensions_enforce(s,NULL);extension_state *e=state(s);J *owners=json_object_new_array(),*first=NULL;sqlite3_stmt *q=prep(s,"SELECT v.source,v.package,v.version,v.digest,coalesce(c.config,'{}') FROM extension_versions v JOIN extension_bindings b ON b.source=v.source AND b.package=v.package AND b.version=v.version LEFT JOIN extension_configuration c ON c.source=v.source AND c.package=v.package AND c.version=v.version WHERE b.enabled=1 AND v.quarantined=0 ORDER BY v.source,v.package,v.version");if(!q){json_object_put(owners);return NULL;}
 while(sqlite3_step(q)==SQLITE_ROW){const char *source=(const char*)sqlite3_column_text(q,0),*package=(const char*)sqlite3_column_text(q,1);char identity[65];package_identity(source,package,identity);J *active=get(e->active,identity);if(!eq(str(active,"host"),"node-dsh")||!eq(str(active,"version"),(const char*)sqlite3_column_text(q,2)))continue;
  J *row=json_object_new_object();const char *keys[]={"source","package","version","digest"};for(int i=0;i<4;i++)json_object_object_add(row,keys[i],json_object_new_string((const char*)sqlite3_column_text(q,i)));const char *raw=(const char*)sqlite3_column_text(q,4);char hash[65];dc_sha256(raw,strlen(raw),hash);json_object_object_add(row,"configurationHash",json_object_new_string(hash));if(!first)first=row;json_object_array_add(owners,row);
 }sqlite3_finalize(q);
 J *out=json_object_new_object();json_object_object_add(out,"owners",owners);json_object_object_add(out,"text",json_object_new_string(""));if(!first)return out;
 J *r=json_object_new_object();const char *keys[]={"source","package","version"};for(size_t i=0;i<3;i++)json_object_object_add(r,keys[i],json_object_get(get(first,keys[i])));json_object_object_add(r,"chat",json_object_new_string(chat));json_object_object_add(r,"command",json_object_new_string("ui.context"));json_object_object_add(r,"arguments",json_object_new_object());J *result=command(s,r,NULL);json_object_put(r);J *body=get(get(result,"data"),"data");const char *text=str(body,"text");if(!json_object_get_boolean(get(result,"ok"))||!text||strlen(text)>65536){json_object_put(result);json_object_put(out);return NULL;}json_object_object_add(out,"text",json_object_new_string(text));json_object_put(result);return out;
}
#include "extension_fork.inc"
#include "extension_ui.inc"
#include "extension_native_jobs.inc"
#include "extension_native.inc"
#include "extension_shutdown.inc"
static J *request_unlocked(dc_store *s,J *r,const dc_hooks *hooks){
 const char *op=str(r,"op");if(!op)return fail("invalid_extension_request");extension_state *e=state(s);if(!e)return fail("out_of_memory");
 if(eq(op,"extensions.quiesce"))return quiesce(s,r,hooks);
 if(eq(op,"extensions.clientContext")||eq(op,"extensions.clientAction")||eq(op,"extensions.settings.apply"))return client_native(s,r,hooks);
 if(eq(op,"extensions.assets")){const char *path=str(r,"path");if(!path||strlen(path)>=PATH_MAX||path[0]!='/')return fail("invalid_assets_path");free(e->assets);e->assets=strdup(path);return environment(s);}
 if(eq(op,"extensions.uninstall"))return uninstall(s,r,hooks);
 if(eq(op,"extensions.restore"))return restore(s,e,hooks);
 if(eq(op,"compat.status"))return environment(s);
 if(eq(op,"compat.detect")){
  if(!e->detect)return fail("dsh_detector_unavailable");
  if(!e->assets)return fail("compatibility_assets_missing");
  char *error=NULL,*actual=NULL;
  int found=e->detect(s->directory,e->assets,str(r,"node"),hooks,&error,&actual);
  const char *disposition=found?"reuse":eq(error,"engine_download_required")?"download":eq(error,"engine_upgrade_confirmation_required")?"outdated":NULL;
  if(eq(disposition,"outdated")){
   sqlite3_stmt *q=prep(s,"INSERT OR REPLACE INTO compatibility_candidate(singleton,path) VALUES(1,?)");
   if(q)bind(q,1,str(r,"node"));step(q);
  }
  if(!found&&!eq(error,"cancelled")&&!eq(disposition,"download")){
   sqlite3_stmt *q=prep(s,"UPDATE compatibility_environment SET state=CASE WHEN desired OR state='outdated' THEN state ELSE ? END,error=?");
   if(q){bind(q,1,disposition?"outdated":"faulted");bind(q,2,error);}step(q);
  }
  J *out;
  if(disposition){
   J *info=json_object_new_object();
   json_object_object_add(info,"action",json_object_new_string(disposition));
   json_object_object_add(info,"node",json_object_new_string(actual?actual:""));
   json_object_object_add(info,"baseline",json_object_new_string(e->provider->baseline));
   out=reply(1,NULL,info);
  }else out=fail(error?error:"compatibility_prepare_failed");
  free(error);free(actual);return out;
 }
 if(eq(op,"compat.enable")){
  if(!e->provider)return fail("dsh_adapter_not_built");
  if(dc_worker_alive(e->dsh)){
    const char *requested=str(r,"node");char selected[PATH_MAX];if(!requested||!*requested||(realpath(requested,selected)&&eq(selected,e->node)))return environment(s);
    if(!json_object_get_boolean(get(r,"confirmUpgrade")))return fail("environment_switch_confirmation_required");
  }
  if(!e->assets)return fail("compatibility_assets_missing");char *error=NULL,*actual=NULL;dc_worker *next=e->provider->prepare(s->directory,e->assets,str(r,"node"),json_object_get_boolean(get(r,"confirmUpgrade")),hooks,&error,&actual);
  if(next&&hooks&&hooks->cancelled&&hooks->cancelled(hooks->user)){dc_worker_close(next);free(error);free(actual);return fail("cancelled");}
  if(!next){
   if(!eq(error,"cancelled")){
    sqlite3_stmt *q=prep(s,"UPDATE compatibility_environment SET state=CASE WHEN desired OR state='outdated' THEN state WHEN ?='engine_upgrade_confirmation_required' THEN 'outdated' ELSE 'faulted' END,error=?");
    if(q){bind(q,1,error);bind(q,2,error);}step(q);
   }
   J *out=fail(error?error:"compatibility_prepare_failed");free(error);free(actual);return out;
  }
  if(!stop_dsh_environment(s,e)){dc_worker_close(next);free(actual);sql(s,"UPDATE compatibility_environment SET state='faulted',error='environment_shutdown_receipt_unknown'");return fail("environment_shutdown_receipt_unknown");}
  e->dsh=next;free(e->node);e->node=actual;sqlite3_stmt *q=prep(s,"UPDATE compatibility_environment SET desired=1,state='ready',node=?,generation=generation+1,error=''");if(q)bind(q,1,e->node);if(!step(q)){dc_worker_close(e->dsh);e->dsh=NULL;return fail("storage_error");}return environment(s);
 }
 if(eq(op,"compat.disable")){int stopped=stop_dsh_environment(s,e);if(!sql(s,stopped?"UPDATE compatibility_environment SET desired=0,state='off',error=''":"UPDATE compatibility_environment SET desired=0,state='faulted',error='environment_shutdown_receipt_unknown'"))return fail("storage_error");return stopped?environment(s):fail("environment_shutdown_receipt_unknown");}
 if(eq(op,"python.configure")){
  const char *executable=str(r,"executable");if(!e->assets||!executable||*executable!='/')return fail("python_configuration_invalid");
  char home[PATH_MAX],script[PATH_MAX];snprintf(home,sizeof home,"%s/Python",s->directory);if(mkdir(home,0700)){struct stat st;if(lstat(home,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))return fail("python_home_invalid");}snprintf(script,sizeof script,"%s/hosts/python/worker.py",e->assets);
  J *config=json_object_new_object(),*args=json_object_new_array();json_object_object_add(config,"executable",json_object_new_string(executable));json_object_object_add(config,"home",json_object_new_string(home));json_object_array_add(args,json_object_new_string("-B"));json_object_array_add(args,json_object_new_string(script));json_object_object_add(config,"args",args);char *error=NULL;dc_worker *next=dc_worker_open(json_object_to_json_string_ext(config,JSON_C_TO_STRING_PLAIN),hooks,&error);json_object_put(config);if(!next){J *out=fail(error?error:"python_start_failed");free(error);return out;}
  char *cap=dc_worker_capabilities(next);J *hello=cap?dunlin_json_parse(cap,strlen(cap)):NULL;free(cap);const char *version=str(get(hello,"runtime"),"version");int major=0,minor=0;if(version)sscanf(version,"%d.%d",&major,&minor);int supported=eq(str(get(hello,"runtime"),"kind"),"python")&&major==3&&minor>=8;json_object_put(hello);if(!supported){dc_worker_close(next);return fail("python_version_unsupported");}
  sqlite3_stmt *saved=prep(s,"INSERT INTO extension_python(singleton,executable,desired) VALUES(1,?,1) ON CONFLICT(singleton) DO UPDATE SET executable=excluded.executable,desired=1");if(saved)bind(saved,1,executable);if(!step(saved)){dc_worker_close(next);return fail("storage_error");}
  dc_worker_close(e->python_worker);clear_host(e,"python");e->python_worker=next;free(e->python);e->python=strdup(executable);return reply(1,NULL,json_object_new_object());
 }
 if(eq(op,"python.disable")){if(!sql(s,"UPDATE extension_python SET desired=0"))return fail("storage_error");dc_worker_close(e->python_worker);clear_host(e,"python");e->python_worker=NULL;free(e->python);e->python=NULL;return reply(1,NULL,json_object_new_object());}
 if(eq(op,"python.status")){J *o=json_object_new_object();json_object_object_add(o,"healthy",json_object_new_boolean(dc_worker_alive(e->python_worker)));json_object_object_add(o,"executable",json_object_new_string(e->python?e->python:""));return reply(1,NULL,o);}
 if(eq(op,"extensions.list"))return inventory(s);
 if(eq(op,"extensions.clients"))return clients(s,hooks);
 if(eq(op,"extensions.clientFault")){
  const char *source=str(r,"source"),*package=str(r,"package"),*version=str(r,"version");if(!id(source)||!id(package)||!id(version))return fail("invalid_client_fault");
  char identity[65];package_identity(source,package,identity);J *active=get(e->active,identity);if(!eq(str(active,"version"),version)||!eq(str(active,"host"),"node-dsh")||(get(r,"instance")&&!eq(str(r,"instance"),str(active,"instance"))))return fail("stale_client_fault");
  J *disable=json_object_new_object();json_object_object_add(disable,"source",json_object_new_string(source));json_object_object_add(disable,"package",json_object_new_string(package));json_object_object_add(disable,"version",json_object_new_string(version));json_object_object_add(disable,"enabled",json_object_new_boolean(0));J *result=activate(s,disable,hooks);json_object_put(disable);
  int stopped=json_object_get_boolean(get(result,"ok"));json_object_put(result);if(!stopped)host_fault(s,e,"node-dsh","client_cleanup_failed");
  sqlite3_int64 fault_id=fault(s,source,package,version,"client","client_surface_failed");
  if(!fault_id)return fail("storage_error");
  J *details=json_object_new_object();json_object_object_add(details,"data",json_object_get(r));fault_diagnostic(s,fault_id,details);json_object_put(details);return inventory(s);
 }

 if(eq(op,"extensions.import")){const char *path=str(r,"path");char canonical[PATH_MAX],digest[65],source[80];if(!path||!realpath(path,canonical))return fail("invalid_extension_directory");dc_sha256(canonical,strlen(canonical),digest);snprintf(source,sizeof source,"local:%s",digest);return dc_extension_import(s,source,canonical,NULL);}
 if(eq(op,"extensions.bind"))return activate(s,r,hooks);
 if(eq(op,"extensions.themes"))return themes(s,hooks);
 if(eq(op,"extensions.presentation.read")){dc_extensions_enforce(s,hooks);return presentation_read(s);}
 if(eq(op,"extensions.presentation.write"))return presentation_write(s,r);
 if(eq(op,"extensions.recover"))return recover(s,r);
 if(eq(op,"extensions.clientStorage.read")||eq(op,"extensions.clientStorage.write"))return client_storage(s,r);
 if(eq(op,"extensions.config.get")||eq(op,"extensions.config.save"))return configuration_request(s,r);
 if(eq(op,"extensions.tools"))return model_tools(s,hooks);
 if(eq(op,"extensions.command"))return command(s,r,hooks);
 if(eq(op,"extensions.contributions")){dc_extensions_enforce(s,hooks);J *a=json_object_new_array();dc_worker *hosts[]={e->dsh,e->python_worker};for(size_t i=0;i<2;i++){if(!dc_worker_alive(hosts[i]))continue;J *params=json_object_new_object(),*result=worker_request(s,hosts[i],"plugins.contributions",params,hooks);json_object_put(params);J *data=get(result,"data");if(!result||!json_object_get_boolean(get(result,"ok"))||!json_object_is_type(data,json_type_array)){json_object_put(result);json_object_put(a);return fail("extension_contributions_failed");}for(size_t j=0;j<json_object_array_length(data);j++)json_object_array_add(a,json_object_get(json_object_array_get_idx(data,j)));json_object_put(result);}return reply(1,NULL,a);}
 if(eq(op,"extensions.faults")){sqlite3_stmt *q=prep(s,"SELECT f.id,f.source,f.package,f.version,f.phase,f.error,f.created,coalesce(d.diagnostic,'') FROM extension_faults f LEFT JOIN extension_fault_details d ON d.fault=f.id ORDER BY f.id DESC LIMIT 200");if(!q)return fail("storage_error");J *a=json_object_new_array();int rc;const char *keys[]={"id","source","package","version","phase","error","created","diagnostic"};while((rc=sqlite3_step(q))==SQLITE_ROW){J *o=json_object_new_object();for(int i=0;i<8;i++)json_object_object_add(o,keys[i],i==0||i==6?json_object_new_int64(sqlite3_column_int64(q,i)):json_object_new_string((const char*)sqlite3_column_text(q,i)));json_object_array_add(a,o);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}return reply(1,NULL,a);}
 return fail("unsupported_extension_operation");
}
char *dc_extensions_call(dc_store *s,const char *raw,const dc_hooks *hooks){if(!s||!raw||strlen(raw)>1048576)return NULL;J *r=dunlin_json_parse(raw,strlen(raw)),*out=json_object_is_type(r,json_type_object)?dc_extensions_request(s,r,hooks):fail("invalid_extension_json");char *result=out?strdup(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(out);json_object_put(r);return result;}

/* Public admission points serialize the one shared optional host. Store-local
 * SQL and model state remain on the calling store's independent connection. */
int dc_extensions_register_detector(dc_store *s,dc_environment_detector detector){
 void *scope=dc_extensions_scope_enter(s);if(!scope)return 0;
 extension_state *e=state(s);int ok=e&&e->provider&&!e->dsh&&detector;
 if(ok)e->detect=detector;
 dc_extensions_scope_leave(scope);return ok;
}
int dc_extensions_register_environment(dc_store *s,const dc_environment_provider_v1 *provider){void *scope=dc_extensions_scope_enter(s);if(!scope)return 0;int out=register_environment_unlocked(s,provider);dc_extensions_scope_leave(scope);return out;}
const char *dc_extension_gate(dc_store *s,J *engines,const char *kind){void *scope=dc_extensions_scope_enter(s);if(!scope)return "extension_state_unavailable";const char *out=gate_unlocked(s,engines,kind);dc_extensions_scope_leave(scope);return out;}
J *dc_extension_import(dc_store *s,const char *source,const char *path,J *expected){void *scope=dc_extensions_scope_enter(s);if(!scope)return fail("extension_state_unavailable");J *out=import_unlocked(s,source,path,expected);dc_extensions_scope_leave(scope);return out;}
void dc_extensions_enforce(dc_store *s,const dc_hooks *hooks){void *scope=dc_extensions_scope_enter(s);if(!scope)return;enforce_unlocked(s,hooks);dc_extensions_scope_leave(scope);}
J *dc_extension_context_snapshot(dc_store *s,const char *chat){void *scope=dc_extensions_scope_enter(s);if(!scope)return NULL;J *out=context_snapshot_unlocked(s,chat);dc_extensions_scope_leave(scope);return out;}
J *dc_extensions_request(dc_store *s,J *r,const dc_hooks *hooks){void *scope=dc_extensions_scope_enter(s);if(!scope)return fail("extension_state_unavailable");J *out=request_unlocked(s,r,hooks);dc_extensions_scope_leave(scope);return out;}
