#define _POSIX_C_SOURCE 200809L
#include "../../desktop/config_import.h"
#include "dunlin/content_hash.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

typedef struct json_object J;
static J *get(J *o,const char *key){J *v=NULL;if(json_object_is_type(o,json_type_object))json_object_object_get_ex(o,key,&v);return v;}
static const char *str(J *o,const char *key){J *v=get(o,key);if(!json_object_is_type(v,json_type_string))return NULL;const char *s=json_object_get_string(v);return strlen(s)==(size_t)json_object_get_string_len(v)?s:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int ident(const char *s){return s&&*s&&strlen(s)<=64&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(s);}
static void add(J *o,const char *key,const char *value){json_object_object_add(o,key,json_object_new_string(value?value:""));}
static void warn(J *a,const char *value){json_object_array_add(a,json_object_new_string(value));}
static void nonce(char out[65]){unsigned char bytes[32];sqlite3_randomness(sizeof bytes,bytes);dc_sha256(bytes,sizeof bytes,out);}
static int issue(J **out,const char *code,const char *path){if(!*out){*out=json_object_new_object();add(*out,"code",code);add(*out,"path",path);}return 0;}
static size_t prefix(const char *s,size_t max){size_t n=strlen(s);if(n>max)n=max;while(n&&((unsigned char)s[n]&0xc0)==0x80)n--;return n;}
static int fields(J *o,const char *const *keys,const char *path,J **error){
 if(!json_object_is_type(o,json_type_object))return issue(error,"object_required",path);
 json_object_object_foreach(o,key,value){(void)value;int found=0;for(int i=0;keys[i];i++)if(eq(key,keys[i]))found=1;if(!found){char field[384];size_t n=strlen(key);if(n>72)n=72;while(n&&((unsigned char)key[n]&0xc0)==0x80)n--;snprintf(field,sizeof field,"%s.%.*s",path,(int)n,key);return issue(error,eq(key,"modelOverrides")?"explicit_models_required":"unsupported_field",field);}}return 1;
}
static const char *protocol(const char *api){
 if(eq(api,"openai-completions"))return "chat";
 if(eq(api,"openai-responses"))return "responses";
 if(eq(api,"anthropic-messages"))return "anthropic";
 if(eq(api,"google-generative-ai"))return "gemini";
 if(eq(api,"ollama"))return "ollama";
 return NULL;
}
static int alias_used(J *providers,const char *alias){for(size_t i=0;i<json_object_array_length(providers);i++){J *p=json_object_array_get_idx(providers,i);if(eq(str(p,"routeID"),alias)||eq(str(p,"id"),alias))return 1;}return 0;}
void dc_config_import_fresh_alias(J *provider,J *existing,J *additional,J *warnings){
 const char *route=str(provider,"routeID");if(!route||(!alias_used(existing,route)&&!alias_used(additional,route)))return;
 char alias[65],suffix[65];for(int attempt=0;attempt<16;attempt++){nonce(suffix);snprintf(alias,sizeof alias,"%.44s-imported-%.8s",route,suffix);if(!alias_used(existing,alias)&&!alias_used(additional,alias))break;}
 add(provider,"routeID",alias);warn(warnings,"provider_alias_renamed");
}
static int one_provider(const char *route,J *source,J *existing,J *models,J *providers,J *warnings,const char *base,J **error){
 char path[256];if(!ident(route))return issue(error,"invalid_route",base);snprintf(path,sizeof path,"%s.providers.%s",base,route);
 const char *const keys[]={"displayName","api","baseURL","apiKeyEnv","apiKey","models",NULL};
 if(!fields(source,keys,path,error))return 0;
 if(alias_used(providers,route))return issue(error,"duplicate_provider_route",path);
 const char *wire=protocol(str(source,"api"));if(!wire)return issue(error,"explicit_protocol_required",path);
 const char *url=str(source,"baseURL");if(!url||!*url)return issue(error,"explicit_endpoint_required",path);
 J *list=get(source,"models");if(!json_object_is_type(list,json_type_array)||!json_object_array_length(list)||json_object_array_length(list)>1000)return issue(error,"explicit_models_required",path);
 if(json_object_array_length(providers)>=256||json_object_array_length(models)+json_object_array_length(list)>1000)return issue(error,"configuration_limit",path);
 const char *name=str(source,"displayName");if(get(source,"displayName")&&(!name||!*name))return issue(error,"invalid_name",path);
 const char *reference=str(source,"apiKeyEnv");if(get(source,"apiKeyEnv")&&(!reference||!*reference||strlen(reference)>256))return issue(error,"invalid_credential_reference",path);
 char provider_id[65];nonce(provider_id);
 J *provider=json_object_new_object();add(provider,"id",provider_id);add(provider,"routeID",route);add(provider,"name",name?name:route);add(provider,"endpoint",url);add(provider,"protocolName",wire);json_object_object_add(provider,"credentialRevision",json_object_new_int(0));if(reference)add(provider,"keyEnvironment",reference);dc_config_import_fresh_alias(provider,existing,providers,warnings);
 if(get(source,"apiKey"))warn(warnings,"credential_value_omitted");
 json_object_array_add(providers,provider);
 const char *const model_keys[]={"id","name","api","reasoning","input","contextWindow","maxTokens","cost",NULL};
 for(size_t i=0;i<json_object_array_length(list);i++){
  J *raw=json_object_array_get_idx(list,i);char item_path[320];snprintf(item_path,sizeof item_path,"%s.models[%zu]",path,i);
  if(!fields(raw,model_keys,item_path,error))return 0;
  const char *id=str(raw,"id"),*title=str(raw,"name");if(!id||!*id||strlen(id)>512||(get(raw,"name")&&(!title||!*title)))return issue(error,"explicit_model_id_required",item_path);
  for(size_t j=0;j<i;j++)if(eq(str(json_object_array_get_idx(list,j),"id"),id))return issue(error,"duplicate_model_id",item_path);
  if(get(raw,"api")&&!eq(str(raw,"api"),str(source,"api")))return issue(error,"model_protocol_override_not_supported",item_path);
  if(get(raw,"reasoning")&&!json_object_is_type(get(raw,"reasoning"),json_type_boolean))return issue(error,"invalid_capability",item_path);
  J *input=get(raw,"input");if(input){int has_text=0;if(!json_object_is_type(input,json_type_array)||!json_object_array_length(input)||json_object_array_length(input)>2)return issue(error,"invalid_capability",item_path);for(size_t j=0;j<json_object_array_length(input);j++){J *v=json_object_array_get_idx(input,j);const char *modality=json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;if(!eq(modality,"text")&&!eq(modality,"image"))return issue(error,"unsupported_modality",item_path);if(eq(modality,"text"))has_text=1;for(size_t k=0;k<j;k++)if(json_object_equal(v,json_object_array_get_idx(input,k)))return issue(error,"invalid_capability",item_path);}if(!has_text)return issue(error,"text_modality_required",item_path);}
  char uuid[65];nonce(uuid);J *model=json_object_new_object();add(model,"id",uuid);add(model,"providerID",provider_id);add(model,"name",title?title:id);add(model,"endpoint",url);add(model,"protocolName",wire);add(model,"model",id);json_object_object_add(model,"credentialRevision",json_object_new_int(0));
  J *max=get(raw,"maxTokens"),*window=get(raw,"contextWindow");int64_t requested=4096;
  if(max){if(!json_object_is_type(max,json_type_int)||json_object_get_int64(max)<=0||json_object_get_int64(max)>INT32_MAX)return json_object_put(model),issue(error,"invalid_output_limit",item_path);json_object_object_add(model,"advertisedOutputLimit",json_object_get(max));if(json_object_get_int64(max)<requested)requested=json_object_get_int64(max);}
  if(window){if(!json_object_is_type(window,json_type_int)||json_object_get_int64(window)<=513||json_object_get_int64(window)>INT32_MAX)return json_object_put(model),issue(error,"invalid_context_window",item_path);int64_t size=json_object_get_int64(window),recommended=size/4,available=size-513;if(recommended<1)recommended=1;if(requested>recommended)requested=recommended;if(requested>available)requested=available;json_object_object_add(model,"contextLimit",json_object_get(window));add(model,"contextSource","导入配置声明（尚未联调核验）");}
  json_object_object_add(model,"maxOutput",json_object_new_int64(requested));
  if(get(raw,"reasoning"))json_object_object_add(model,"reasoningSupported",json_object_get(get(raw,"reasoning")));
  if(input)json_object_object_add(model,"inputModalities",json_object_get(input));
  if(get(raw,"cost"))warn(warnings,"pricing_not_imported");
  json_object_array_add(models,model);
 }
 return 1;
}
static int provider_map(J *source,J *existing,J *models,J *providers,J *warnings,const char *base,J **error){
 const char *const keys[]={"providers","defaultModel","model",NULL};if(!fields(source,keys,base,error))return 0;
 J *map=get(source,"providers");if(!json_object_is_type(map,json_type_object)||!json_object_object_length(map))return issue(error,"explicit_providers_required",base);if(json_object_object_length(map)>256)return issue(error,"configuration_limit",base);
 if(get(source,"defaultModel")||get(source,"model"))warn(warnings,"default_model_not_replaced");
 json_object_object_foreach(map,route,value){if(!one_provider(route,value,existing,models,providers,warnings,base,error))return 0;}
 return 1;
}
static int static_tree(J *v,const char *path,J **error){
 if(json_object_is_type(v,json_type_object)){
  json_object_object_foreach(v,key,value){char child[384];snprintf(child,sizeof child,"%.*s.%.*s",(int)prefix(path,240),path,(int)prefix(key,72),key);if(eq(key,"include")||eq(key,"includes")||eq(key,"$include"))return issue(error,"dynamic_include_not_supported",child);if(!static_tree(value,child,error))return 0;}
 }else if(json_object_is_type(v,json_type_array))for(size_t i=0;i<json_object_array_length(v);i++){char child[384];snprintf(child,sizeof child,"%.*s[%zu]",(int)prefix(path,240),path,i);if(!static_tree(json_object_array_get_idx(v,i),child,error))return 0;}
 return 1;
}
int dc_config_import_dsh(J *document,J *existing,J *models,J *providers,J *warnings,J **diagnostic){
 if(!static_tree(document,"$",diagnostic))return -1;
 if(get(document,"providers"))return provider_map(document,existing,models,providers,warnings,"$",diagnostic)?1:-1;
 if(!json_object_is_type(document,json_type_array))return 0;
 int found=0;
 for(size_t i=0;i<json_object_array_length(document);i++){
  J *entry=json_object_array_get_idx(document,i);char path[64];snprintf(path,sizeof path,"$[%zu]",i);
  const char *const keys[]={"id","disabled","config",NULL};if(!fields(entry,keys,path,diagnostic))return -1;
  const char *id=str(entry,"id");if(!id||!*id){issue(diagnostic,"plugin_id_required",path);return -1;}
  if(get(entry,"disabled")&&!json_object_is_type(get(entry,"disabled"),json_type_boolean)){issue(diagnostic,"invalid_disabled_flag",path);return -1;}
  if(!eq(id,"llm-pi-ai")||json_object_get_boolean(get(entry,"disabled"))){warn(warnings,"plugin_declaration_not_installed");continue;}
  if(found){issue(diagnostic,"multiple_provider_blocks_not_supported",path);return -1;}found=1;char base[96];snprintf(base,sizeof base,"%s.config",path);
  if(!provider_map(get(entry,"config"),existing,models,providers,warnings,base,diagnostic))return -1;
 }
 if(!found){issue(diagnostic,"explicit_providers_required","$");return -1;}
 warn(warnings,"static_dsh_subset_only");return 1;
}
