#define _XOPEN_SOURCE 700
#include "extensions.h"
#include "package_store.h"
#include "dunlin/extension_services.h"
#include "dunlin/plugin_port.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
typedef struct json_object J;
typedef struct {dc_worker *dsh,*python_worker;char *assets;char *node;char *python;J *active;const dc_environment_provider_v1 *provider;} extension_state;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int id(const char *s){return s&&*s&&strlen(s)<=256&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:./@+")==strlen(s);}
static sqlite3_stmt *prep(dc_store *s,const char *v){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,v,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *v){return sqlite3_exec(s->db,v,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(o,"error",json_object_new_string(error));if(data)json_object_object_add(o,"data",data);return o;}
static J *fail(const char *code){return reply(0,code,NULL);}
static extension_state *state(dc_store *s){if(!s->extensions){extension_state *e=calloc(1,sizeof *e);if(e){e->active=json_object_new_object();if(!e->active){free(e);e=NULL;}}s->extensions=e;}return s->extensions;}
int dc_extensions_register_environment(dc_store *s,const dc_environment_provider_v1 *provider){if(!s||!provider||provider->abi_version!=1||!eq(provider->engine,"node-dsh")||!provider->version||!provider->baseline||!provider->prepare)return 0;extension_state *e=state(s);if(!e||e->dsh)return 0;e->provider=provider;return 1;}
static void package_identity(const char *source,const char *package,char out[65]){char joined[1024];snprintf(joined,sizeof joined,"%s:%s",source,package);dc_sha256(joined,strlen(joined),out);}
static void clear_host(extension_state *e,const char *host){J *next=json_object_new_object();json_object_object_foreach(e->active,key,value){if(!eq(str(value,"host"),host))json_object_object_add(next,key,json_object_get(value));}json_object_put(e->active);e->active=next;}
int dc_extensions_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS compatibility_environment(singleton INTEGER PRIMARY KEY CHECK(singleton=1),desired INTEGER NOT NULL DEFAULT 0,state TEXT NOT NULL DEFAULT 'off',node TEXT NOT NULL DEFAULT '',generation INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '');INSERT OR IGNORE INTO compatibility_environment(singleton) VALUES(1);UPDATE compatibility_environment SET state=CASE WHEN desired THEN 'stopped' ELSE 'off' END;"
 "CREATE TABLE IF NOT EXISTS extension_versions(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,digest TEXT NOT NULL,manifest TEXT NOT NULL,path TEXT NOT NULL,files TEXT NOT NULL,quarantined INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '',PRIMARY KEY(source,package,version));"
 "CREATE TABLE IF NOT EXISTS extension_bindings(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,enabled INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(source,package));"
 "CREATE TABLE IF NOT EXISTS extension_configuration(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,config TEXT NOT NULL,PRIMARY KEY(source,package,version));"
 "CREATE TABLE IF NOT EXISTS extension_python(singleton INTEGER PRIMARY KEY CHECK(singleton=1),executable TEXT NOT NULL,desired INTEGER NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_faults(id INTEGER PRIMARY KEY,source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,phase TEXT NOT NULL,error TEXT NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()));"
 "CREATE TABLE IF NOT EXISTS extension_fault_details(fault INTEGER PRIMARY KEY REFERENCES extension_faults(id) ON DELETE CASCADE,diagnostic TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS extension_calls(id TEXT PRIMARY KEY,source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,method TEXT NOT NULL,args_hash TEXT NOT NULL,status TEXT NOT NULL,started INTEGER NOT NULL DEFAULT(unixepoch()),finished INTEGER);INSERT INTO extension_faults(source,package,version,phase,error) SELECT source,package,version,'recovery','interrupted_invocation_unknown' FROM extension_calls WHERE status='running';UPDATE extension_versions SET quarantined=1,error='interrupted_invocation_unknown' WHERE EXISTS(SELECT 1 FROM extension_calls c WHERE c.status='running' AND c.source=extension_versions.source AND c.package=extension_versions.package AND c.version=extension_versions.version);UPDATE extension_bindings SET enabled=0 WHERE EXISTS(SELECT 1 FROM extension_calls c WHERE c.status='running' AND c.source=extension_bindings.source AND c.package=extension_bindings.package);UPDATE extension_calls SET status='unknown',finished=unixepoch() WHERE status='running';");}
void dc_extensions_close(dc_store *s){extension_state *e=s?s->extensions:NULL;if(!e)return;dc_worker_close(e->dsh);dc_worker_close(e->python_worker);free(e->assets);free(e->node);free(e->python);json_object_put(e->active);free(e);s->extensions=NULL;}
static J *environment(dc_store *s){sqlite3_stmt *q=prep(s,"SELECT desired,state,node,generation,error FROM compatibility_environment");if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("storage_error");}J *o=json_object_new_object();json_object_object_add(o,"enabled",json_object_new_boolean(sqlite3_column_int(q,0)));json_object_object_add(o,"state",json_object_new_string((const char*)sqlite3_column_text(q,1)));json_object_object_add(o,"node",json_object_new_string((const char*)sqlite3_column_text(q,2)));json_object_object_add(o,"generation",json_object_new_int64(sqlite3_column_int64(q,3)));json_object_object_add(o,"error",json_object_new_string((const char*)sqlite3_column_text(q,4)));extension_state *e=state(s);json_object_object_add(o,"healthy",json_object_new_boolean(e&&dc_worker_alive(e->dsh)));json_object_object_add(o,"baseline",json_object_new_string(e&&e->provider?e->provider->baseline:"DSH 适配器未装配"));sqlite3_finalize(q);return reply(1,NULL,o);}
const char *dc_extension_gate(dc_store *s,J *engines,const char *kind){
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
 if(!eq(str(get(manifest,"engines"),"host"),"node-dsh")||!eq(str(client,"format"),"dsh-lazy-cjs@1")||!array_strings(client,"slots",1))return 0;
 J *slots=get(client,"slots");if(json_object_array_length(slots)!=1||!eq(json_object_get_string(json_object_array_get_idx(slots,0)),"conversation.composer.dock"))return 0;
 const char *entry=str(client,"entry");return entry&&strlen(entry)>3&&eq(entry+strlen(entry)-3,".js");
}
J *dc_extension_import(dc_store *s,const char *source,const char *path,J *expected){
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
static sqlite3_int64 fault(dc_store *s,const char *source,const char *package,const char *version,const char *phase,const char *error){
 extension_state *e=state(s);char identity[65];package_identity(source,package,identity);if(e)json_object_object_del(e->active,identity);
 sqlite3_stmt *q=prep(s,"UPDATE extension_versions SET quarantined=1,error=? WHERE source=? AND package=? AND version=?");if(q){bind(q,1,error);bind(q,2,source);bind(q,3,package);bind(q,4,version);}step(q);q=prep(s,"UPDATE extension_bindings SET enabled=0 WHERE source=? AND package=?");if(q){bind(q,1,source);bind(q,2,package);}step(q);q=prep(s,"INSERT INTO extension_faults(source,package,version,phase,error) VALUES(?,?,?,?,?)");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);bind(q,4,phase);bind(q,5,error);}return step(q)?sqlite3_last_insert_rowid(s->db):0;
}
static void fault_diagnostic(dc_store *s,sqlite3_int64 fault_id,J *response){
 const char *text=str(get(response,"data"),"diagnostic");
 if(fault_id<=0||!text||!*text||strlen(text)>8192)return;
 sqlite3_stmt *q=prep(s,"INSERT INTO extension_fault_details(fault,diagnostic) VALUES(?,?)");
 if(q){sqlite3_bind_int64(q,1,fault_id);bind(q,2,text);}step(q);
}
static J *worker_request(dc_worker *worker,const char *method,J *params,const dc_hooks *hooks){char *raw=dc_worker_call(worker,method,json_object_to_json_string_ext(params,JSON_C_TO_STRING_PLAIN),hooks);J *result=raw?dunlin_json_parse(raw,strlen(raw)):NULL;dc_free(raw);return result;}
/* Persist lifecycle intent before calling user code. Configuration is hashed,
 * never copied into the audit record. A missing terminal write keeps the running
 * row so startup recovery quarantines instead of replaying a partial apply. */
static int lifecycle_begin(dc_store *s,const char *source,const char *package,const char *version,J *request,char call[65]){
 char digest[65];const char *raw=json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN);dc_sha256(raw,strlen(raw),digest);
 sqlite3_stmt *q=prep(s,"SELECT lower(hex(randomblob(16)))");if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return 0;}
 snprintf(call,65,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);
 q=prep(s,"INSERT INTO extension_calls(id,source,package,version,method,args_hash,status) VALUES(?,?,?,?,'plugins.bind',?,'running')");
 if(q){bind(q,1,call);bind(q,2,source);bind(q,3,package);bind(q,4,version);bind(q,5,digest);}if(!step(q)){call[0]=0;return 0;}return 1;
}
static J *activate(dc_store *s,J *r,const dc_hooks *hooks){
 if(!sqlite3_get_autocommit(s->db))return fail("extension_transaction_busy");
 const char *source=str(r,"source"),*package=str(r,"package"),*version=str(r,"version");J *desired=get(r,"enabled");if(!id(source)||!id(package)||!id(version)||!json_object_is_type(desired,json_type_boolean))return fail("invalid_extension_binding");int enable=json_object_get_boolean(desired);J *configuration=get(r,"config");if(configuration&&(!json_object_is_type(configuration,json_type_object)||strlen(json_object_to_json_string_ext(configuration,JSON_C_TO_STRING_PLAIN))>32768))return fail("invalid_extension_configuration");sqlite3_stmt *q=prep(s,"SELECT manifest,path,files,quarantined FROM extension_versions WHERE source=? AND package=? AND version=?");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("extension_version_missing");}const char *raw=(const char*)sqlite3_column_text(q,0);J *manifest=dunlin_json_parse(raw,strlen(raw));char *path=strdup((const char*)sqlite3_column_text(q,1));raw=(const char*)sqlite3_column_text(q,2);J *files=dunlin_json_parse(raw,strlen(raw));int quarantined=sqlite3_column_int(q,3);sqlite3_finalize(q);const char *error=NULL;char lifecycle[65]={0};int lifecycle_unknown=0;
 extension_state *e=state(s);if(!e){error="out_of_memory";goto done;}if(enable&&!eq(str(get(manifest,"engines"),"host"),"native-theme")&&!json_object_get_boolean(get(r,"acceptHostPermissions"))){error="host_permission_confirmation_required";goto done;}if(enable&&quarantined){error="extension_quarantined";goto done;}if(enable&&(error=dc_extension_gate(s,get(manifest,"engines"),str(manifest,"kind"))))goto done;if(enable&&!dc_package_verify(path,files)){error="extension_integrity_failed";fault(s,source,package,version,"integrity",error);goto done;}
 char identity[65];package_identity(source,package,identity);
 dc_worker *host=eq(str(get(manifest,"engines"),"host"),"node-dsh")?e->dsh:eq(str(get(manifest,"engines"),"host"),"python")?e->python_worker:NULL;
 if(host){if(!lifecycle_begin(s,source,package,version,r,lifecycle)){error="invocation_audit_unavailable";goto done;}J *args=json_object_new_object();json_object_object_add(args,"id",json_object_new_string(identity));J *deactivated=worker_request(host,"plugins.deactivate",args,hooks);if(!deactivated||!json_object_get_boolean(get(deactivated,"ok"))){lifecycle_unknown=!deactivated||eq(str(deactivated,"effectState"),"unknown");json_object_put(deactivated);json_object_put(args);error="extension_dispose_failed";fault(s,source,package,version,"dispose",error);goto done;}json_object_put(deactivated);json_object_object_del(e->active,identity);
  if(enable){json_object_object_add(args,"root",json_object_new_string(path));json_object_object_add(args,"entry",json_object_new_string(str(manifest,"entry")?str(manifest,"entry"):"index.ts"));json_object_object_add(args,"config",get(r,"config")?json_object_get(get(r,"config")):json_object_new_object());J *activated=worker_request(host,"plugins.activate",args,hooks);if(!activated||!json_object_get_boolean(get(activated,"ok"))){lifecycle_unknown=!activated||eq(str(activated,"effectState"),"unknown");error="extension_activation_failed";sqlite3_int64 fault_id=fault(s,source,package,version,"activate",error);fault_diagnostic(s,fault_id,activated);}json_object_put(activated);}json_object_put(args);if(error)goto done;
 }

 int persisted=sql(s,"BEGIN IMMEDIATE");
 if(persisted&&enable){q=prep(s,"INSERT INTO extension_configuration(source,package,version,config) VALUES(?,?,?,?) ON CONFLICT(source,package,version) DO UPDATE SET config=excluded.config");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);bind(q,4,configuration?json_object_to_json_string_ext(configuration,JSON_C_TO_STRING_PLAIN):"{}");}persisted=step(q);}
 if(persisted){q=prep(s,"UPDATE extension_bindings SET version=?,enabled=? WHERE source=? AND package=?");if(q){bind(q,1,version);sqlite3_bind_int(q,2,enable);bind(q,3,source);bind(q,4,package);}persisted=step(q);}
 if(!sql(s,persisted?"COMMIT":"ROLLBACK")){sql(s,"ROLLBACK");persisted=0;}
 if(!persisted){lifecycle_unknown=1;error="storage_error";if(host){J *args=json_object_new_object();json_object_object_add(args,"id",json_object_new_string(identity));J *result=worker_request(host,"plugins.deactivate",args,NULL);json_object_put(result);json_object_put(args);}}
 if(!error){json_object_object_del(e->active,identity);if(enable){J *a=json_object_new_object();json_object_object_add(a,"source",json_object_new_string(source));json_object_object_add(a,"package",json_object_new_string(package));json_object_object_add(a,"version",json_object_new_string(version));json_object_object_add(a,"host",json_object_get(get(get(manifest,"engines"),"host")));json_object_object_add(e->active,identity,a);}}
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
 const char *queries[]={"DELETE FROM extension_configuration WHERE source=? AND package=?","DELETE FROM extension_bindings WHERE source=? AND package=?","DELETE FROM extension_versions WHERE source=? AND package=?","DELETE FROM catalog_packages WHERE source=? AND package=?"};
 for(size_t i=0;i<sizeof queries/sizeof *queries;i++){q=prep(s,queries[i]);if(q){bind(q,1,source);bind(q,2,package);}if(!step(q)){sql(s,"ROLLBACK");json_object_put(paths);return fail("storage_error");}}
 if(!sql(s,"COMMIT")){sql(s,"ROLLBACK");json_object_put(paths);return fail("storage_error");}int cleaned=1;for(size_t i=0;i<json_object_array_length(paths);i++)if(!dc_package_collect(s,json_object_get_string(json_object_array_get_idx(paths,i))))cleaned=0;json_object_put(paths);return cleaned?inventory(s):fail("extension_removed_cache_cleanup_failed");
}
static void host_fault(dc_store *s,extension_state *e,const char *host,const char *code){
 J *affected=json_object_new_array();json_object_object_foreach(e->active,key,value){(void)key;if(eq(str(value,"host"),host))json_object_array_add(affected,json_object_get(value));}
 for(size_t i=0;i<json_object_array_length(affected);i++){J *v=json_object_array_get_idx(affected,i);fault(s,str(v,"source"),str(v,"package"),str(v,"version"),"host",code);}json_object_put(affected);
 if(eq(host,"node-dsh")){dc_worker_close(e->dsh);e->dsh=NULL;sql(s,"UPDATE compatibility_environment SET state='faulted',error='host_unavailable'");}else{dc_worker_close(e->python_worker);e->python_worker=NULL;}
}
static J *command(dc_store *s,J *r,const dc_hooks *hooks){
 const char *source=str(r,"source"),*package=str(r,"package"),*method=str(r,"command");if(!id(source)||!id(package)||!id(method))return fail("invalid_extension_command");extension_state *e=state(s);char identity[65];package_identity(source,package,identity);J *active=get(e->active,identity);if(!active)return fail("extension_not_active");char host[32],version[257];snprintf(host,sizeof host,"%s",str(active,"host")?str(active,"host"):"");snprintf(version,sizeof version,"%s",str(active,"version")?str(active,"version"):"");dc_worker *worker=eq(host,"node-dsh")?e->dsh:e->python_worker;if(!dc_worker_alive(worker)){host_fault(s,e,host,"host_unavailable");return fail("extension_host_unavailable");}
 sqlite3_stmt *check=prep(s,"SELECT quarantined FROM extension_versions WHERE source=? AND package=? AND version=?");if(check){bind(check,1,source);bind(check,2,package);bind(check,3,version);}int checked=check?sqlite3_step(check):SQLITE_ERROR;int blocked=checked!=SQLITE_ROW||sqlite3_column_int(check,0);sqlite3_finalize(check);if(blocked)return fail("extension_quarantined");
 J *arguments=get(r,"arguments");if(!json_object_is_type(arguments,json_type_object))return fail("invalid_extension_arguments");const char *raw=json_object_to_json_string_ext(arguments,JSON_C_TO_STRING_PLAIN);if(strlen(raw)>131072)return fail("extension_arguments_exceed_limit");char digest[65],call[65],full[512];dc_sha256(raw,strlen(raw),digest);if(!strncmp(method,identity,64)&&method[64]==':')snprintf(full,sizeof full,"%s",method);else snprintf(full,sizeof full,"%s:%s",identity,method);
 sqlite3_stmt *q=prep(s,"SELECT lower(hex(randomblob(16)))");if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return fail("storage_error");}snprintf(call,sizeof call,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);q=prep(s,"INSERT INTO extension_calls(id,source,package,version,method,args_hash,status) VALUES(?,?,?,?,?,?,'running')");if(q){const char *v[]={call,source,package,version,method,digest};for(int i=0;i<6;i++)bind(q,i+1,v[i]);}if(!step(q))return fail("invocation_audit_unavailable");
 J *params=json_object_new_object();json_object_object_add(params,"plugin",json_object_new_string(identity));json_object_object_add(params,"command",json_object_new_string(full));json_object_object_add(params,"arguments",json_object_get(arguments));J *result=worker_request(worker,"plugins.command",params,hooks);json_object_put(params);const char *effect=result?str(result,"effectState"):"unknown";if(!effect)effect="unknown";
 q=prep(s,"UPDATE extension_calls SET status=?,finished=unixepoch() WHERE id=?");if(q){bind(q,1,effect);bind(q,2,call);}if(!step(q)){json_object_put(result);host_fault(s,e,host,"invocation_finalization_failed");return fail("invocation_finalization_failed");}
 int success=result&&json_object_get_boolean(get(result,"ok"));if(!success){if(eq(effect,"unknown")){host_fault(s,e,host,"host_disconnected_or_protocol_error");}else if(json_object_get_int(get(get(result,"data"),"code"))==-32603){char *saved=strdup(version);sqlite3_int64 fault_id=fault(s,source,package,saved,"command","plugin_unhandled_error");fault_diagnostic(s,fault_id,result);free(saved);J *params=json_object_new_object();json_object_object_add(params,"id",json_object_new_string(identity));J *stopped=worker_request(worker,"plugins.deactivate",params,NULL);json_object_put(stopped);json_object_put(params);}}
 J *out=reply(success,success?NULL:eq(effect,"unknown")?"extension_effect_unknown":"extension_command_failed",result);return out;
}
void dc_extensions_enforce(dc_store *s,const dc_hooks *hooks){
 extension_state *e=state(s);if(!e)return;J *checklist=json_object_new_array();json_object_object_foreach(e->active,key,value){J *v=json_object_new_object();json_object_object_add(v,"id",json_object_new_string(key));json_object_object_add(v,"record",json_object_get(value));json_object_array_add(checklist,v);}
 for(size_t i=0;i<json_object_array_length(checklist);i++){J *item=json_object_array_get_idx(checklist,i),*v=get(item,"record");sqlite3_stmt *q=prep(s,"SELECT quarantined FROM extension_versions WHERE source=? AND package=? AND version=?");if(q){bind(q,1,str(v,"source"));bind(q,2,str(v,"package"));bind(q,3,str(v,"version"));}int row=q?sqlite3_step(q):SQLITE_ERROR;int denied=row!=SQLITE_ROW||sqlite3_column_int(q,0);sqlite3_finalize(q);if(!denied)continue;const char *host=str(v,"host");dc_worker *worker=eq(host,"node-dsh")?e->dsh:eq(host,"python")?e->python_worker:NULL;if(worker){J *p=json_object_new_object();json_object_object_add(p,"id",json_object_get(get(item,"id")));J *stopped=worker_request(worker,"plugins.deactivate",p,hooks);json_object_put(p);if(!stopped||!json_object_get_boolean(get(stopped,"ok")))host_fault(s,e,host,"quarantine_dispose_failed");json_object_put(stopped);}json_object_object_del(e->active,str(item,"id"));}
 json_object_put(checklist);
}
static int color(const char *p){return p&&strlen(p)==7&&p[0]=='#'&&strspn(p+1,"0123456789abcdefABCDEF")==6;}
static int color_value(J *v){return json_object_is_type(v,json_type_string)&&json_object_get_string_len(v)==7&&color(json_object_get_string(v));}
static J *theme_tokens(J *values){
 J *tokens=json_object_new_object();const char *keys[]={"background","surface","text","secondary"};
 for(size_t k=0;k<4;k++){J *v=get(values,keys[k]);if(color_value(v))json_object_object_add(tokens,keys[k],json_object_get(v));}
 return tokens;
}
static J *themes(dc_store *s,const dc_hooks *hooks){
 J *a=json_object_new_array();sqlite3_stmt *q=prep(s,"SELECT source,package,version,digest,manifest FROM extension_versions WHERE quarantined=0 ORDER BY rowid DESC");if(!q){json_object_put(a);return fail("storage_error");}int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,4);J *m=dunlin_json_parse(raw,strlen(raw)),*theme=get(m,"theme");if(eq(str(m,"kind"),"theme")&&eq(str(get(m,"engines"),"host"),"native-theme")&&color_value(get(get(theme,"tokens"),"accent"))&&(eq(str(theme,"scheme"),"light")||eq(str(theme,"scheme"),"dark"))){J *o=json_object_new_object();char identity[65],key[200];package_identity((const char*)sqlite3_column_text(q,0),(const char*)sqlite3_column_text(q,1),identity);snprintf(key,sizeof key,"extension:%s:%s",sqlite3_column_text(q,3),identity);json_object_object_add(o,"id",json_object_new_string(key));json_object_object_add(o,"name",json_object_new_string(str(m,"displayName")?str(m,"displayName"):str(m,"packageId")));json_object_object_add(o,"scheme",json_object_get(get(theme,"scheme")));json_object_object_add(o,"accent",json_object_get(get(get(theme,"tokens"),"accent")));json_object_object_add(o,"tokens",theme_tokens(get(theme,"tokens")));json_object_array_add(a,o);}json_object_put(m);}
 sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}extension_state *e=state(s);dc_worker *hosts[]={e->dsh,e->python_worker};for(size_t h=0;h<2;h++){if(!dc_worker_alive(hosts[h]))continue;J *p=json_object_new_object(),*r=worker_request(hosts[h],"plugins.contributions",p,hooks);json_object_put(p);J *data=get(r,"data");for(size_t i=0;json_object_is_type(data,json_type_array)&&i<json_object_array_length(data);i++){J *v=json_object_array_get_idx(data,i);if(!eq(str(v,"kind"),"theme")||!color_value(get(get(v,"tokens"),"accent"))||(!eq(str(v,"scheme"),"light")&&!eq(str(v,"scheme"),"dark")))continue;J *o=json_object_new_object();char key[512];snprintf(key,sizeof key,"plugin:%s",str(v,"id"));json_object_object_add(o,"id",json_object_new_string(key));json_object_object_add(o,"name",json_object_new_string(str(v,"name")?str(v,"name"):"扩展主题"));json_object_object_add(o,"scheme",json_object_get(get(v,"scheme")));json_object_object_add(o,"accent",json_object_get(get(get(v,"tokens"),"accent")));json_object_object_add(o,"tokens",theme_tokens(get(v,"tokens")));json_object_array_add(a,o);}json_object_put(r);}return reply(1,NULL,a);
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
  json_object_put(files);json_object_put(manifest);
  if(!code){
   J *params=json_object_new_object();json_object_object_add(params,"id",json_object_new_string(identity));J *stopped=worker_request(e->dsh,"plugins.deactivate",params,hooks);json_object_put(params);
   int cleaned=stopped&&json_object_get_boolean(get(stopped,"ok"));json_object_put(stopped);
   if(!cleaned)host_fault(s,e,"node-dsh","client_integrity_cleanup_failed");
   fault(s,source,package,version,"client","extension_client_integrity_failed");continue;
  }
  total+=strlen(code);if(total>8388608||json_object_array_length(a)>=32){free(code);sqlite3_finalize(q);json_object_put(a);return fail("extension_client_limit");}
  J *o=json_object_new_object();json_object_object_add(o,"source",json_object_new_string(source));json_object_object_add(o,"package",json_object_new_string(package));json_object_object_add(o,"version",json_object_new_string(version));json_object_object_add(o,"runtimeID",json_object_new_string(identity));json_object_object_add(o,"digest",json_object_new_string((const char*)sqlite3_column_text(q,6)));json_object_object_add(o,"code",json_object_new_string(code));free(code);
  raw=(const char*)sqlite3_column_text(q,7);json_object_object_add(o,"config",dunlin_json_parse(raw,strlen(raw)));json_object_array_add(a,o);
 }
 sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}return reply(1,NULL,a);
}
J *dc_extensions_request(dc_store *s,J *r,const dc_hooks *hooks){
 const char *op=str(r,"op");if(!op)return fail("invalid_extension_request");extension_state *e=state(s);if(!e)return fail("out_of_memory");
 if(eq(op,"extensions.assets")){const char *path=str(r,"path");if(!path||strlen(path)>=PATH_MAX||path[0]!='/')return fail("invalid_assets_path");free(e->assets);e->assets=strdup(path);return environment(s);}
 if(eq(op,"extensions.uninstall"))return uninstall(s,r,hooks);
 if(eq(op,"extensions.restore"))return restore(s,e,hooks);
 if(eq(op,"compat.status"))return environment(s);
 if(eq(op,"compat.enable")){
  if(!e->provider)return fail("dsh_adapter_not_built");
  if(dc_worker_alive(e->dsh)){
    const char *requested=str(r,"node");char selected[PATH_MAX];if(!requested||!*requested||(realpath(requested,selected)&&eq(selected,e->node)))return environment(s);
    if(!json_object_get_boolean(get(r,"confirmUpgrade")))return fail("environment_switch_confirmation_required");
  }
  if(!e->assets)return fail("compatibility_assets_missing");char *error=NULL,*actual=NULL;dc_worker *next=e->provider->prepare(s->directory,e->assets,str(r,"node"),json_object_get_boolean(get(r,"confirmUpgrade")),hooks,&error,&actual);
  if(next&&hooks&&hooks->cancelled&&hooks->cancelled(hooks->user)){dc_worker_close(next);free(error);free(actual);return fail("cancelled");}
  if(!next){sqlite3_stmt *q=prep(s,"UPDATE compatibility_environment SET state=CASE WHEN desired THEN state WHEN ?='engine_upgrade_confirmation_required' THEN 'outdated' ELSE 'faulted' END,error=?");if(q){bind(q,1,error);bind(q,2,error);}step(q);J *out=fail(error?error:"compatibility_prepare_failed");free(error);free(actual);return out;}
  dc_worker_close(e->dsh);clear_host(e,"node-dsh");e->dsh=next;free(e->node);e->node=actual;sqlite3_stmt *q=prep(s,"UPDATE compatibility_environment SET desired=1,state='ready',node=?,generation=generation+1,error=''");if(q)bind(q,1,e->node);if(!step(q)){dc_worker_close(e->dsh);e->dsh=NULL;return fail("storage_error");}return environment(s);
 }
 if(eq(op,"compat.disable")){dc_worker_close(e->dsh);clear_host(e,"node-dsh");e->dsh=NULL;if(!sql(s,"UPDATE compatibility_environment SET desired=0,state='off',error=''"))return fail("storage_error");return environment(s);}
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
  char identity[65];package_identity(source,package,identity);J *active=get(e->active,identity);if(!eq(str(active,"version"),version)||!eq(str(active,"host"),"node-dsh"))return fail("stale_client_fault");
  J *disable=json_object_new_object();json_object_object_add(disable,"source",json_object_new_string(source));json_object_object_add(disable,"package",json_object_new_string(package));json_object_object_add(disable,"version",json_object_new_string(version));json_object_object_add(disable,"enabled",json_object_new_boolean(0));J *result=activate(s,disable,hooks);json_object_put(disable);
  int stopped=json_object_get_boolean(get(result,"ok"));json_object_put(result);if(!stopped)host_fault(s,e,"node-dsh","client_cleanup_failed");
  if(!fault(s,source,package,version,"client","client_surface_failed"))return fail("storage_error");return inventory(s);
 }

 if(eq(op,"extensions.import")){const char *path=str(r,"path");char canonical[PATH_MAX],digest[65],source[80];if(!path||!realpath(path,canonical))return fail("invalid_extension_directory");dc_sha256(canonical,strlen(canonical),digest);snprintf(source,sizeof source,"local:%s",digest);return dc_extension_import(s,source,canonical,NULL);}
 if(eq(op,"extensions.bind"))return activate(s,r,hooks);
 if(eq(op,"extensions.themes"))return themes(s,hooks);
 if(eq(op,"extensions.recover"))return recover(s,r);
 if(eq(op,"extensions.command"))return command(s,r,hooks);
 if(eq(op,"extensions.contributions")){dc_extensions_enforce(s,hooks);J *a=json_object_new_array();dc_worker *hosts[]={e->dsh,e->python_worker};for(size_t i=0;i<2;i++){if(!dc_worker_alive(hosts[i]))continue;J *params=json_object_new_object(),*result=worker_request(hosts[i],"plugins.contributions",params,hooks);json_object_put(params);J *data=get(result,"data");if(!result||!json_object_get_boolean(get(result,"ok"))||!json_object_is_type(data,json_type_array)){json_object_put(result);json_object_put(a);return fail("extension_contributions_failed");}for(size_t j=0;j<json_object_array_length(data);j++)json_object_array_add(a,json_object_get(json_object_array_get_idx(data,j)));json_object_put(result);}return reply(1,NULL,a);}
 if(eq(op,"extensions.faults")){sqlite3_stmt *q=prep(s,"SELECT f.id,f.source,f.package,f.version,f.phase,f.error,f.created,coalesce(d.diagnostic,'') FROM extension_faults f LEFT JOIN extension_fault_details d ON d.fault=f.id ORDER BY f.id DESC LIMIT 200");if(!q)return fail("storage_error");J *a=json_object_new_array();int rc;const char *keys[]={"id","source","package","version","phase","error","created","diagnostic"};while((rc=sqlite3_step(q))==SQLITE_ROW){J *o=json_object_new_object();for(int i=0;i<8;i++)json_object_object_add(o,keys[i],i==0||i==6?json_object_new_int64(sqlite3_column_int64(q,i)):json_object_new_string((const char*)sqlite3_column_text(q,i)));json_object_array_add(a,o);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}return reply(1,NULL,a);}
 return fail("unsupported_extension_operation");
}
char *dc_extensions_call(dc_store *s,const char *raw,const dc_hooks *hooks){if(!s||!raw||strlen(raw)>1048576)return NULL;J *r=dunlin_json_parse(raw,strlen(raw)),*out=json_object_is_type(r,json_type_object)?dc_extensions_request(s,r,hooks):fail("invalid_extension_json");char *result=out?strdup(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(out);json_object_put(r);return result;}
