#define _POSIX_C_SOURCE 200809L
#include "../../desktop/config_registry.h"
#include "../../desktop/config_import.h"
#include "../../desktop/connection_test.h"
#include "../../desktop/model_identity.h"
#include "dunlin/static_config.h"
#include "dunlin/content_hash.h"
#include "dunlin/context_budget.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/agent.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

typedef struct json_object J;
static J *get(J *o,const char *key){J *v=NULL;if(json_object_is_type(o,json_type_object))json_object_object_get_ex(o,key,&v);return v;}
static const char *text(J *o,const char *key){J *v=get(o,key);if(!json_object_is_type(v,json_type_string))return NULL;const char *s=json_object_get_string(v);return strlen(s)==(size_t)json_object_get_string_len(v)?s:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int valid_text(const char *s,size_t limit,int empty){return s&&(empty||*s)&&strlen(s)<=limit&&ah_utf8_text(s,strlen(s));}
static int ident(const char *s){return valid_text(s,64,0)&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(s);}
static void add(J *o,const char *k,const char *s){json_object_object_add(o,k,json_object_new_string(s?s:""));}
static const char *raw(J *o){return json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);}
static J *reply(int ok,const char *error,J *data){J *v=json_object_new_object();json_object_object_add(v,"ok",json_object_new_boolean(ok));if(error)add(v,"error",error);if(data)json_object_object_add(v,"data",data);return v;}
static J *fail(const char *error){return reply(0,error,NULL);}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind_text(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v,-1,SQLITE_TRANSIENT);}
static int done(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *v){return sqlite3_exec(s->db,v,NULL,NULL,NULL)==SQLITE_OK;}
static void nonce(char id[65]){unsigned char bytes[32];sqlite3_randomness(sizeof bytes,bytes);dc_sha256(bytes,sizeof bytes,id);}
static int protocol(const char *v){return eq(v,"chat")||eq(v,"responses")||eq(v,"anthropic")||eq(v,"ollama")||eq(v,"gemini");}
static int endpoint(const char *v){
 if(!valid_text(v,2048,0))return 0;
 CURLU *url=curl_url();if(!url)return 0;char *scheme=NULL,*host=NULL,*part=NULL;
 int ok=curl_url_set(url,CURLUPART_URL,v,0)==CURLUE_OK&&curl_url_get(url,CURLUPART_SCHEME,&scheme,0)==CURLUE_OK&&curl_url_get(url,CURLUPART_HOST,&host,0)==CURLUE_OK&&*host&&(eq(scheme,"http")||eq(scheme,"https"));
 CURLUPart forbidden[]={CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_QUERY,CURLUPART_FRAGMENT};
 for(size_t i=0;i<sizeof forbidden/sizeof *forbidden;i++){if(curl_url_get(url,forbidden[i],&part,0)==CURLUE_OK){ok=0;curl_free(part);part=NULL;}}
 curl_free(scheme);curl_free(host);curl_url_cleanup(url);return ok;
}
static int integer(J *v,int64_t min,int64_t max){return json_object_is_type(v,json_type_int)&&json_object_get_int64(v)>=min&&json_object_get_int64(v)<=max&&(json_object_get_int64(v)<0||json_object_get_uint64(v)<=INT64_MAX);}
static int fields(J *v,const char *const *keys){
 if(!json_object_is_type(v,json_type_object))return 0;
 json_object_object_foreach(v,k,value){(void)value;int found=0;for(int i=0;keys[i];i++)if(eq(k,keys[i]))found=1;if(!found)return 0;}return 1;
}
static J *find(J *a,const char *id){for(size_t i=0;i<json_object_array_length(a);i++){J *v=json_object_array_get_idx(a,i);if(eq(text(v,"id"),id))return v;}return NULL;}
static const char *const provider_keys[]={"id","name","routeID","keyEnvironment","endpoint","protocolName","authentication","credentialRevision","credentialID","testModel",NULL};
static const char *const model_keys[]={"id","name","endpoint","model","protocolName","maxOutput","providerID","credentialID","keyEnvironment","enableThinking","reasoningSupported","reasoningCapabilities","reasoningEffort","temperature","inputModalities","systemPrompt","chatTemplate","authentication","credentialRevision","contextLimit","deploymentLimit","outputLimit","advertisedOutputLimit","contextBudget","contextSource","contextReserve","compactionStrategy",NULL};
static int optional_text(J *o,const char *k,size_t cap){J *v=get(o,k);return !v||valid_text(text(o,k),cap,1);}
static int optional_int(J *o,const char *k,int64_t min,int64_t max){J *v=get(o,k);return !v||integer(v,min,max);}
static int connection(J *v){return ident(text(v,"id"))&&valid_text(text(v,"name"),256,0)&&endpoint(text(v,"endpoint"))&&protocol(text(v,"protocolName"))&&(!get(v,"credentialID")||ident(text(v,"credentialID")))&&(!get(v,"authentication")||eq(text(v,"authentication"),"auto")||eq(text(v,"authentication"),"bearer")||eq(text(v,"authentication"),"none"))&&optional_int(v,"credentialRevision",0,INT64_MAX);}
int dc_config_provider_valid(J *v){return fields(v,provider_keys)&&connection(v)&&(!get(v,"routeID")||ident(text(v,"routeID")))&&optional_text(v,"keyEnvironment",256)&&(!get(v,"testModel")||valid_text(text(v,"testModel"),512,0));}
static int request_options(J *v){
 const char *wire=text(v,"protocolName");J *temperature=get(v,"temperature");
 if(temperature){double value=json_object_get_double(temperature);if((!json_object_is_type(temperature,json_type_int)&&!json_object_is_type(temperature,json_type_double))||!(value>=0&&value<=2)||eq(wire,"anthropic"))return 0;}
 if((get(v,"enableThinking")||get(v,"chatTemplate"))&&!eq(wire,"chat"))return 0;
 if(get(v,"chatTemplate")&&!valid_text(text(v,"chatTemplate"),4096,0))return 0;
 unsigned capabilities=0;J *array=get(v,"reasoningCapabilities");
 if(array){if(!json_object_is_type(array,json_type_array)||json_object_array_length(array)>3)return 0;for(size_t i=0;i<json_object_array_length(array);i++){J *item=json_object_array_get_idx(array,i);const char *name=json_object_is_type(item,json_type_string)?json_object_get_string(item):NULL;unsigned bit=eq(name,"none")?1:eq(name,"levels")?2:eq(name,"xhigh")?4:0;if(!bit||strlen(name)!=(size_t)json_object_get_string_len(item)||(capabilities&bit))return 0;capabilities|=bit;}}
 const char *effort=text(v,"reasoningEffort");if(!get(v,"reasoningEffort")||eq(effort,"default"))return 1;
 if(eq(effort,"none"))return (capabilities&1)!=0;
 if(eq(effort,"low")||eq(effort,"medium")||eq(effort,"high"))return (capabilities&2)!=0;
 return eq(effort,"xhigh")&&(capabilities&4)&&(eq(wire,"chat")||eq(wire,"responses"));
}
static int legacy_options(J *v){
 J *copy=NULL;if(json_object_deep_copy(v,&copy,NULL))return 0;
 if(!eq(text(copy,"protocolName"),"chat")){json_object_object_del(copy,"enableThinking");json_object_object_del(copy,"chatTemplate");}
 else if(eq(text(copy,"chatTemplate"),""))json_object_object_del(copy,"chatTemplate");
 int valid=request_options(copy);json_object_put(copy);return valid;
}
static int validate_mode(J *models,J *providers,int legacy){
 if(!json_object_is_type(models,json_type_array)||!json_object_is_type(providers,json_type_array)||json_object_array_length(models)>1000||json_object_array_length(providers)>256)return 0;
 for(size_t i=0;i<json_object_array_length(providers);i++){
  J *v=json_object_array_get_idx(providers,i);if(!dc_config_provider_valid(v))return 0;
  for(size_t j=0;j<i;j++){J *old=json_object_array_get_idx(providers,j);if(eq(text(v,"id"),text(old,"id"))||eq(text(v,"routeID"),text(old,"routeID"))||eq(text(v,"routeID"),text(old,"id"))||eq(text(v,"id"),text(old,"routeID")))return 0;}
 }
 for(size_t i=0;i<json_object_array_length(models);i++){
  J *v=json_object_array_get_idx(models,i);if(!fields(v,model_keys)||!connection(v)||!valid_text(text(v,"model"),512,0)||!integer(get(v,"maxOutput"),1,65536))return 0;
  for(size_t j=0;j<i;j++)if(eq(text(v,"id"),text(json_object_array_get_idx(models,j),"id")))return 0;
  if(!optional_text(v,"providerID",64)||!optional_text(v,"keyEnvironment",256)||!optional_text(v,"systemPrompt",65536)||!optional_text(v,"chatTemplate",4096)||!optional_text(v,"contextSource",512))return 0;
  const char *ints[]={"contextLimit","deploymentLimit","outputLimit","advertisedOutputLimit","contextBudget",NULL};for(int j=0;ints[j];j++)if(!optional_int(v,ints[j],1,INT32_MAX))return 0;
  if(!optional_int(v,"contextReserve",0,INT32_MAX)||(get(v,"enableThinking")&&!json_object_is_type(get(v,"enableThinking"),json_type_boolean)))return 0;
  if(get(v,"reasoningSupported")&&!json_object_is_type(get(v,"reasoningSupported"),json_type_boolean))return 0;
  J *input=get(v,"inputModalities");if(input){int has_text=0;if(!json_object_is_type(input,json_type_array)||!json_object_array_length(input)||json_object_array_length(input)>2)return 0;for(size_t j=0;j<json_object_array_length(input);j++){J *modality=json_object_array_get_idx(input,j);const char *name=json_object_is_type(modality,json_type_string)?json_object_get_string(modality):NULL;if(!name||strlen(name)!=(size_t)json_object_get_string_len(modality)||(!eq(name,"text")&&!eq(name,"image")))return 0;if(eq(name,"text"))has_text=1;for(size_t k=0;k<j;k++)if(json_object_equal(modality,json_object_array_get_idx(input,k)))return 0;}if(!has_text)return 0;}
  if((!request_options(v)&&(!legacy||!legacy_options(v)))||(get(v,"compactionStrategy")&&!eq(text(v,"compactionStrategy"),"recent")&&!eq(text(v,"compactionStrategy"),"semantic")))return 0;
  dc_context_capabilities cap={.model_limit=(uint64_t)json_object_get_int64(get(v,"contextLimit")),.deployment_limit=(uint64_t)json_object_get_int64(get(v,"deploymentLimit")),.selected_limit=(uint64_t)json_object_get_int64(get(v,"contextBudget")),.client_limit=4194304,.output_limit=65536,.requested_output=(uint64_t)json_object_get_int64(get(v,"maxOutput")),.overhead_reserve=get(v,"contextReserve")?(uint64_t)json_object_get_int64(get(v,"contextReserve")):512};
  const char *out_keys[]={"outputLimit","advertisedOutputLimit",NULL};for(int j=0;out_keys[j];j++)if(get(v,out_keys[j])&&(uint64_t)json_object_get_int64(get(v,out_keys[j]))<cap.output_limit)cap.output_limit=(uint64_t)json_object_get_int64(get(v,out_keys[j]));
  dc_context_range range;if(cap.requested_output>cap.output_limit||dc_context_budget(&cap,&range)==DC_CONTEXT_INVALID)return 0;
  const char *provider=text(v,"providerID");if(provider&&*provider){J *p=find(providers,provider);if(!p||!eq(text(p,"endpoint"),text(v,"endpoint"))||!eq(text(p,"protocolName"),text(v,"protocolName")))return 0;const char *a=text(p,"authentication"),*b=text(v,"authentication"),*c=text(p,"credentialID"),*d=text(v,"credentialID");if(!eq(a?a:"auto",b?b:"auto")||!eq(c?c:"",d?d:"")||json_object_get_int64(get(p,"credentialRevision"))!=json_object_get_int64(get(v,"credentialRevision")))return 0;}
 }
 return strlen(raw(models))+strlen(raw(providers))<=1048576;
}
static int validate(J *models,J *providers){return validate_mode(models,providers,0);}
int dc_config_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS configuration_registry(singleton INTEGER PRIMARY KEY CHECK(singleton=1),revision INTEGER NOT NULL,models TEXT NOT NULL,providers TEXT NOT NULL);"
 "INSERT OR IGNORE INTO configuration_registry VALUES(1,0,'[]','[]');"
 "CREATE TABLE IF NOT EXISTS configuration_imports(id TEXT PRIMARY KEY,revision INTEGER NOT NULL,models TEXT NOT NULL,providers TEXT NOT NULL,warnings TEXT NOT NULL,expires INTEGER NOT NULL,consumed INTEGER NOT NULL DEFAULT 0);")&&dc_connection_test_schema(s);}
static J *list(dc_store *s){
 sqlite3_stmt *q=prep(s,"SELECT revision,models,providers FROM configuration_registry WHERE singleton=1");J *v=NULL;
 if(q&&sqlite3_step(q)==SQLITE_ROW){J *m=dunlin_json_parse((const char*)sqlite3_column_text(q,1),(size_t)sqlite3_column_bytes(q,1)),*p=dunlin_json_parse((const char*)sqlite3_column_text(q,2),(size_t)sqlite3_column_bytes(q,2));if(validate_mode(m,p,1)){v=json_object_new_object();json_object_object_add(v,"revision",json_object_new_int64(sqlite3_column_int64(q,0)));json_object_object_add(v,"models",m);json_object_object_add(v,"providers",p);J *issues=json_object_new_array();for(size_t i=0;i<json_object_array_length(m);i++){J *model=json_object_array_get_idx(m,i);if(!request_options(model)){J *issue=json_object_new_object();add(issue,"id",text(model,"id"));add(issue,"error","unsupported_request_options");json_object_array_add(issues,issue);}}json_object_object_add(v,"modelIssues",issues);J *tests=dc_connection_tests(s,p);if(tests)json_object_object_add(v,"connectionTests",tests);else{json_object_put(v);v=NULL;}}else{json_object_put(m);json_object_put(p);}}sqlite3_finalize(q);return v;
}
static int same_runtime(J *a,J *b){
 if(!a||!b)return 0;J *x=NULL,*y=NULL;if(json_object_deep_copy(a,&x,NULL)||json_object_deep_copy(b,&y,NULL)){json_object_put(x);json_object_put(y);return 0;}
 json_object_object_del(x,"name");json_object_object_del(y,"name");int equal=json_object_equal(x,y);json_object_put(x);json_object_put(y);return equal;
}
static int valid_update(J *models,J *providers,J *old_models){
 if(!validate_mode(models,providers,1))return 0;
 for(size_t i=0;i<json_object_array_length(models);i++){J *model=json_object_array_get_idx(models,i);if(!request_options(model)&&!same_runtime(model,find(old_models,text(model,"id"))))return 0;}return 1;
}
static int active_guard(dc_store *s,J *old,J *models){
 sqlite3_stmt *q=prep(s,"SELECT DISTINCT " DC_CHAT_MODEL_ID " FROM chat_properties m JOIN chats c ON c.id=m.chat " DC_ACTIVE_RUN_JOIN " WHERE m.active<>0");if(!q)return 0;int rc,ok=1;
 while((rc=sqlite3_step(q))==SQLITE_ROW){const char *id=(const char*)sqlite3_column_text(q,0);
  if(id){J *before=find(get(old,"models"),id);if(before&&!same_runtime(before,find(models,id)))ok=0;}
  else for(size_t i=0;i<json_object_array_length(get(old,"models"));i++){J *before=json_object_array_get_idx(get(old,"models"),i);if(!same_runtime(before,find(models,text(before,"id"))))ok=0;}
 }
 sqlite3_finalize(q);return ok&&rc==SQLITE_DONE;
}
static int publish(dc_store *s,J *models,J *providers,int64_t revision){
 sqlite3_stmt *q=prep(s,"UPDATE configuration_registry SET models=?,providers=?,revision=revision+1 WHERE singleton=1 AND revision=? AND revision<9223372036854775807");if(q){bind_text(q,1,raw(models));bind_text(q,2,raw(providers));sqlite3_bind_int64(q,3,revision);}return done(q)&&sqlite3_changes(s->db)==1;
}
static J *save(dc_store *s,J *r){
 J *models=get(r,"models"),*providers=get(r,"providers"),*old=list(s);if(!old)return fail("configuration_storage_error");int64_t rev=json_object_get_int64(get(old,"revision"));
 if(!valid_update(models,providers,get(old,"models"))){json_object_put(old);return fail("configuration_invalid");}
 if(!integer(get(r,"revision"),0,INT64_MAX)||json_object_get_int64(get(r,"revision"))!=rev){json_object_put(old);return fail("configuration_revision_conflict");}
 if(!active_guard(s,old,models)){json_object_put(old);return fail("configuration_active_route");}json_object_put(old);
 if(!publish(s,models,providers,rev))return fail("configuration_revision_or_storage_error");return reply(1,NULL,list(s));
}
static void warning(J *a,const char *v){json_object_array_add(a,json_object_new_string(v));}
static int copy_key(J *to,J *from,const char *source,const char *target){J *v=get(from,source);if(v)json_object_object_add(to,target,json_object_get(v));return v!=NULL;}
static J *model_settings(dc_store *s,J *r){
 const char *id=text(r,"id");if(!ident(id)||!integer(get(r,"revision"),0,INT64_MAX))return fail("configuration_invalid");J *registry=list(s);if(!registry)return fail("configuration_storage_error");
 if(json_object_get_int64(get(registry,"revision"))!=json_object_get_int64(get(r,"revision"))){json_object_put(registry);return fail("configuration_revision_conflict");}
 J *model=find(get(registry,"models"),id);if(!model){json_object_put(registry);return fail("configuration_model_missing");}if(!request_options(model)){json_object_put(registry);return fail("configuration_model_options_unsupported");}J *cfg=json_object_new_object();if(!cfg){json_object_put(registry);return fail("configuration_storage_error");}
 const char *mapping[][2]={{"endpoint","base_url"},{"model","model"},{"protocolName","protocol"},{"maxOutput","max_tokens"},{"temperature","temperature"},{"reasoningCapabilities","reasoning_capabilities"},{"reasoningEffort","reasoning_effort"},{"enableThinking","enable_thinking"},{"chatTemplate","omlx_chat_template"}};
 for(size_t i=0;i<sizeof mapping/sizeof *mapping;i++)copy_key(cfg,model,mapping[i][0],mapping[i][1]);
 copy_key(cfg,model,"inputModalities","desktop_input_modalities");
 if(eq(text(model,"reasoningEffort"),"default"))json_object_object_del(cfg,"reasoning_effort");
 J *caps=get(cfg,"reasoning_capabilities");if(caps&&!json_object_array_length(caps))json_object_object_del(cfg,"reasoning_capabilities");
 const char *prompt=text(model,"systemPrompt");add(cfg,"system_prompt",prompt?prompt:"You are a coding assistant. Reply to ordinary chat directly. For project changes use provided tools and rely on results. Read existing files first and pass their expected_sha256 to write_file or patch_file. Paths are project-relative and may include subdirectories. Never claim execution without tool evidence.");
 add(cfg,"desktop_auth",text(model,"authentication")?text(model,"authentication"):"auto");add(cfg,"desktop_tools","noTools");add(cfg,"desktop_profile_id",id);json_object_object_add(cfg,"desktop_credential_revision",json_object_new_int64(json_object_get_int64(get(model,"credentialRevision"))));
 const char *provider=text(model,"providerID"),*credential=text(model,"credentialID");if(provider&&*provider)add(cfg,"desktop_provider_id",provider);if(credential||(provider&&*provider))add(cfg,"desktop_credential_id",credential?credential:provider);
 json_object_put(registry);return reply(1,NULL,cfg);
}
static J *apply_capabilities(dc_store *s,J *r){
 const char *id=text(r,"id"),*source=text(r,"source");
 if(!ident(id)||!integer(get(r,"revision"),0,INT64_MAX)||!integer(get(r,"contextLimit"),0,INT32_MAX)||!integer(get(r,"outputLimit"),0,INT32_MAX)||(!eq(source,"provider-model-inventory")&&!eq(source,"provider-input-limit-conservative")))return fail("configuration_invalid");
 J *registry=list(s);if(!registry)return fail("configuration_storage_error");
 if(json_object_get_int64(get(registry,"revision"))!=json_object_get_int64(get(r,"revision"))){json_object_put(registry);return fail("configuration_revision_conflict");}
 J *model=find(get(registry,"models"),id);if(!model){json_object_put(registry);return fail("configuration_model_missing");}
 if(!request_options(model)){json_object_put(registry);return fail("configuration_model_options_unsupported");}
 uint64_t window=(uint64_t)json_object_get_int64(get(r,"contextLimit")),output=(uint64_t)json_object_get_int64(get(r,"outputLimit"));
 json_object_object_del(model,"contextLimit");json_object_object_del(model,"contextSource");json_object_object_del(model,"advertisedOutputLimit");
 if(window){json_object_object_add(model,"contextLimit",json_object_new_int64((int64_t)window));add(model,"contextSource",eq(source,"provider-input-limit-conservative")?"服务返回输入上限，作为保守总窗口使用":"服务模型目录声明");}
 if(output)json_object_object_add(model,"advertisedOutputLimit",json_object_new_int64((int64_t)output));
 uint64_t deployment=(uint64_t)json_object_get_int64(get(model,"deploymentLimit"));if(!window||(deployment&&deployment<window))window=deployment;if(window>4194304)window=4194304;
 uint64_t requested=(uint64_t)json_object_get_int64(get(model,"maxOutput")),reserve=get(model,"contextReserve")?(uint64_t)json_object_get_int64(get(model,"contextReserve")):512;
 if(output&&requested>output)requested=output;uint64_t manual=(uint64_t)json_object_get_int64(get(model,"outputLimit"));if(manual&&requested>manual)requested=manual;
 if(window){if(reserve+1>=window){json_object_put(registry);return fail("configuration_capability_conflict");}uint64_t recommended=window/4;if(!recommended)recommended=1;if(requested>recommended)requested=recommended;if(requested>window-reserve-1)requested=window-reserve-1;}
 json_object_object_add(model,"maxOutput",json_object_new_int64((int64_t)requested));uint64_t selected=(uint64_t)json_object_get_int64(get(model,"contextBudget"));if(!window||selected>window||(selected&&selected<requested+reserve+1))json_object_object_del(model,"contextBudget");
 J *request=json_object_new_object();json_object_object_add(request,"revision",json_object_get(get(r,"revision")));json_object_object_add(request,"models",json_object_get(get(registry,"models")));json_object_object_add(request,"providers",json_object_get(get(registry,"providers")));J *result=save(s,request);json_object_put(request);json_object_put(registry);return result;
}
static int runtime_fields(J *v,int top){
 static const char *const keys[]={"base_url","model","protocol","max_tokens","key_env","enable_thinking","system_prompt","omlx_chat_template","api_key","key","reasoning_effort","reasoning_capabilities","temperature",NULL};
 static const char *const extras[]={"models","active_model","workspace","session_dir","stream","show_reasoning","timeout_ms",NULL};
 if(!json_object_is_type(v,json_type_object))return 0;
 json_object_object_foreach(v,k,value){(void)value;int found=0;for(int i=0;keys[i];i++)if(eq(k,keys[i]))found=1;if(top)for(int i=0;extras[i];i++)if(eq(k,extras[i]))found=1;if(!found)return 0;}return 1;
}
static J *runtime_model(J *defaults,J *v,const char *name,J *warnings){
 if(!runtime_fields(v,v==defaults))return NULL;J *p=json_object_new_object();char id[65];nonce(id);add(p,"id",id);add(p,"name",name);add(p,"protocolName","chat");json_object_object_add(p,"maxOutput",json_object_new_int(4096));
 J *layers[]={defaults,v};const char *mapping[][2]={{"base_url","endpoint"},{"model","model"},{"protocol","protocolName"},{"max_tokens","maxOutput"},{"key_env","keyEnvironment"},{"enable_thinking","enableThinking"},{"system_prompt","systemPrompt"},{"omlx_chat_template","chatTemplate"},{"temperature","temperature"},{"reasoning_effort","reasoningEffort"},{"reasoning_capabilities","reasoningCapabilities"}};
 for(int i=0;i<(v==defaults?1:2);i++){for(size_t j=0;j<sizeof mapping/sizeof *mapping;j++)copy_key(p,layers[i],mapping[j][0],mapping[j][1]);if(get(layers[i],"api_key")||get(layers[i],"key"))warning(warnings,"Credential value omitted; enter it separately in Keychain.");}
 return p;
}
static J *preview(dc_store *s,J *r){
 const char *source=text(r,"source");if(!valid_text(source,1048576,0))return fail("configuration_source_invalid");dc_static_config_error parse_error;
 J *parsed=dc_static_config_parse(source,strlen(source),&parse_error);if(!parsed){
  static const char *const codes[]={"ok","static_syntax","static_limit","static_tag","static_alias","static_duplicate","static_key","static_document","static_root","static_encoding","static_memory"};
  J *out=fail("configuration_source_invalid"),*diagnostic=json_object_new_object();add(diagnostic,"code",parse_error.code<=DC_STATIC_MEMORY?codes[parse_error.code]:"static_syntax");json_object_object_add(diagnostic,"line",json_object_new_int64((int64_t)parse_error.line));json_object_object_add(diagnostic,"column",json_object_new_int64((int64_t)parse_error.column));json_object_object_add(out,"diagnostic",diagnostic);return out;
 }
 J *old=list(s);if(!old){json_object_put(parsed);return fail("configuration_storage_error");}
 J *models=json_object_new_array(),*providers=json_object_new_array(),*warnings=json_object_new_array(),*diagnostic=NULL,*source_providers=json_object_new_object();int ok=1;
 if(eq(text(parsed,"format"),"dunlin.config")){
  const char *const keys[]={"format","version","models","providers",NULL};ok=fields(parsed,keys)&&integer(get(parsed,"version"),1,1)&&validate(get(parsed,"models"),get(parsed,"providers"));
  J *mapping=json_object_new_object();
  if(ok)for(size_t i=0;i<json_object_array_length(get(parsed,"providers"));i++){J *v=NULL;json_object_deep_copy(json_object_array_get_idx(get(parsed,"providers"),i),&v,NULL);if(!v){ok=0;break;}char id[65];nonce(id);add(mapping,text(v,"id"),id);add(v,"id",id);json_object_object_del(v,"credentialID");json_object_object_add(v,"credentialRevision",json_object_new_int(0));dc_config_import_fresh_alias(v,get(old,"providers"),providers,warnings);json_object_array_add(providers,v);}
  if(ok)for(size_t i=0;i<json_object_array_length(get(parsed,"models"));i++){J *v=NULL;json_object_deep_copy(json_object_array_get_idx(get(parsed,"models"),i),&v,NULL);if(!v){ok=0;break;}char id[65];nonce(id);add(v,"id",id);const char *p=text(v,"providerID");if(p&&*p)add(v,"providerID",text(mapping,p));json_object_object_del(v,"credentialID");json_object_object_add(v,"credentialRevision",json_object_new_int(0));if(get(v,"contextLimit")||get(v,"advertisedOutputLimit"))add(v,"contextSource","导入配置声明（尚未联调核验）");json_object_array_add(models,v);}
  json_object_put(mapping);warning(warnings,"imported_routes_are_new");
 }else{
  int dsh=dc_config_import_dsh(parsed,get(old,"providers"),models,providers,warnings,source_providers,&diagnostic);
  if(dsh<0)ok=0;
  else if(!dsh){
  ok=runtime_fields(parsed,1);J *routes=get(parsed,"models");if(routes&&!json_object_is_type(routes,json_type_object))ok=0;
  if(ok&&routes){json_object_object_foreach(routes,name,v){J *p=runtime_model(parsed,v,name,warnings);if(!p){ok=0;break;}json_object_array_add(models,p);}}
  else if(ok){J *p=runtime_model(parsed,parsed,"Imported model",warnings);if(p)json_object_array_add(models,p);else ok=0;}
  warning(warnings,"static_runtime_subset_only");
  }
 }
 json_object_put(parsed);if(!ok||!json_object_array_length(models)||!validate(models,providers)){json_object_put(source_providers);json_object_put(old);json_object_put(models);json_object_put(providers);json_object_put(warnings);J *out=fail("configuration_unsupported_or_invalid");if(diagnostic)json_object_object_add(out,"diagnostic",diagnostic);return out;}json_object_put(diagnostic);
 int64_t revision=json_object_get_int64(get(old,"revision"));json_object_put(old);char id[65];nonce(id);
 if(!sql(s,"DELETE FROM configuration_imports WHERE consumed<>0 OR expires<unixepoch()")){json_object_put(source_providers);json_object_put(models);json_object_put(providers);json_object_put(warnings);return fail("configuration_storage_error");}
 sqlite3_stmt *q=prep(s,"INSERT INTO configuration_imports(id,revision,models,providers,warnings,expires) VALUES(?,?,?,?,?,unixepoch()+600)");if(q){bind_text(q,1,id);sqlite3_bind_int64(q,2,revision);bind_text(q,3,raw(models));bind_text(q,4,raw(providers));bind_text(q,5,raw(warnings));}ok=done(q);
 J *data=json_object_new_object();json_object_object_add(data,"sourceProviders",source_providers);add(data,"plan",id);json_object_object_add(data,"revision",json_object_new_int64(revision));json_object_object_add(data,"models",models);json_object_object_add(data,"providers",providers);json_object_object_add(data,"warnings",warnings);json_object_object_add(data,"lifetimeSeconds",json_object_new_int(600));if(!ok){json_object_put(data);return fail("configuration_storage_error");}return reply(1,NULL,data);
}
static J *apply(dc_store *s,J *r){
 const char *id=text(r,"plan");if(!ident(id)||!json_object_is_type(get(r,"confirmed"),json_type_boolean)||!json_object_get_boolean(get(r,"confirmed")))return fail("configuration_confirmation_required");
 if(!sql(s,"BEGIN IMMEDIATE"))return fail("configuration_storage_error");
 sqlite3_stmt *q=prep(s,"SELECT revision,models,providers FROM configuration_imports WHERE id=? AND consumed=0 AND expires>=unixepoch()");if(q)bind_text(q,1,id);int found=q&&sqlite3_step(q)==SQLITE_ROW;int64_t revision=found?sqlite3_column_int64(q,0):-1;J *m=found?dunlin_json_parse((const char*)sqlite3_column_text(q,1),(size_t)sqlite3_column_bytes(q,1)):NULL,*p=found?dunlin_json_parse((const char*)sqlite3_column_text(q,2),(size_t)sqlite3_column_bytes(q,2)):NULL;sqlite3_finalize(q);
 J *current=list(s);int ok=current&&json_object_get_int64(get(current,"revision"))==revision&&validate(m,p);
 if(ok){for(size_t i=0;i<json_object_array_length(m);i++){J *v=json_object_array_get_idx(m,i);if(find(get(current,"models"),text(v,"id")))ok=0;json_object_array_add(get(current,"models"),json_object_get(v));}for(size_t i=0;i<json_object_array_length(p);i++){J *v=json_object_array_get_idx(p,i);if(find(get(current,"providers"),text(v,"id")))ok=0;json_object_array_add(get(current,"providers"),json_object_get(v));}}
 ok=ok&&validate_mode(get(current,"models"),get(current,"providers"),1)&&publish(s,get(current,"models"),get(current,"providers"),revision);
 if(ok){q=prep(s,"UPDATE configuration_imports SET consumed=1 WHERE id=? AND consumed=0");if(q)bind_text(q,1,id);ok=done(q)&&sqlite3_changes(s->db)==1;}
 if(!ok||!sql(s,"COMMIT")){sql(s,"ROLLBACK");ok=0;}json_object_put(m);json_object_put(p);json_object_put(current);return ok?reply(1,NULL,list(s)):fail("configuration_plan_stale_or_storage_error");
}
J *dc_config_request(dc_store *s,J *r){
 const char *op=text(r,"op");if(eq(op,"config.list")){J *v=list(s);return v?reply(1,NULL,v):fail("configuration_storage_error");}
 if(eq(op,"config.model"))return model_settings(s,r);
 if(eq(op,"config.capabilities"))return apply_capabilities(s,r);
 if(eq(op,"config.save"))return save(s,r);
 if(eq(op,"config.preview"))return preview(s,r);
 if(eq(op,"config.apply"))return apply(s,r);
 return fail("configuration_operation_unknown");
}
