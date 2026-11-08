#define _POSIX_C_SOURCE 200809L
#include "../../desktop/connection_test.h"
#include "../../desktop/config_registry.h"
#include "../../desktop/secret_transport.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/protocol.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(json_object_is_type(o,json_type_object))json_object_object_get_ex(o,k,&v);return v;}
static const char *text(J *o,const char *k){return json_object_get_string(get(o,k));}
static void add(J *o,const char *k,const char *v){json_object_object_add(o,k,json_object_new_string(v));}
static int sql(dc_store *s,const char *q){return sqlite3_exec(s->db,q,NULL,NULL,NULL)==SQLITE_OK;}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v,-1,SQLITE_TRANSIENT);}
static int done(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static char *reply(int ok,const char *error,J *data){J *out=json_object_new_object();json_object_object_add(out,"ok",json_object_new_boolean(ok));if(error)add(out,"error",error);if(data)json_object_object_add(out,"data",data);char *raw=strdup(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN));json_object_put(out);return raw;}
static void nonce(char out[65]){unsigned char bytes[32];sqlite3_randomness(sizeof bytes,bytes);dc_sha256(bytes,sizeof bytes,out);}
static int64_t monotonic_ms(void){struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return -1;return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;}
static int fingerprint(J *p,char out[65]){
 if(!dc_config_provider_valid(p)||!get(p,"testModel"))return 0;
 J *v=json_object_new_object();const char *keys[]={"id","endpoint","protocolName","testModel",NULL};
 for(int i=0;keys[i];i++)json_object_object_add(v,keys[i],json_object_get(get(p,keys[i])));
 const char *auth=text(p,"authentication"),*credential=text(p,"credentialID");add(v,"authentication",auth?auth:"auto");
 /* No-auth neither resolves nor uses a credential. Changing an unused stored
  * key does not falsely identify a different request. */
 int none=auth&&!strcmp(auth,"none");add(v,"credentialID",none?"":credential?credential:text(p,"id"));json_object_object_add(v,"credentialRevision",json_object_new_int64(none?0:json_object_get_int64(get(p,"credentialRevision"))));
 const char *raw=json_object_to_json_string_ext(v,JSON_C_TO_STRING_PLAIN);dc_sha256(raw,strlen(raw),out);json_object_put(v);return 1;
}
int dc_connection_test_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS connection_tests(id TEXT PRIMARY KEY,provider TEXT NOT NULL,fingerprint TEXT NOT NULL,model TEXT NOT NULL,started INTEGER NOT NULL DEFAULT(unixepoch()),finished INTEGER,status TEXT NOT NULL,http_status INTEGER,model_status INTEGER,duration_ms INTEGER,error TEXT,request_bytes INTEGER,response_bytes INTEGER);"
 "CREATE INDEX IF NOT EXISTS connection_tests_match ON connection_tests(provider,fingerprint,started DESC);"
 "UPDATE connection_tests SET status='interrupted',error='test_interrupted' WHERE status='running';");}
static J *row(sqlite3_stmt *q){
 J *v=json_object_new_object();const char *keys[]={"id","providerID","fingerprint","model","started","finished","status","httpStatus","modelStatus","durationMs","error","requestBytes","responseBytes"};
 for(int i=0;i<13;i++){int type=sqlite3_column_type(q,i);if(type==SQLITE_NULL)continue;if(type==SQLITE_INTEGER)json_object_object_add(v,keys[i],json_object_new_int64(sqlite3_column_int64(q,i)));else add(v,keys[i],(const char*)sqlite3_column_text(q,i));}return v;
}
J *dc_connection_tests(dc_store *s,J *providers){
 J *a=json_object_new_array();sqlite3_stmt *q=prep(s,"SELECT id,provider,fingerprint,model,started,finished,status,http_status,model_status,duration_ms,error,request_bytes,response_bytes FROM connection_tests WHERE provider=? AND fingerprint=? ORDER BY started DESC,rowid DESC LIMIT 1");if(!q){json_object_put(a);return NULL;}
 for(size_t i=0;i<json_object_array_length(providers);i++){
  J *p=json_object_array_get_idx(providers,i);char hash[65];if(!fingerprint(p,hash))continue;sqlite3_reset(q);sqlite3_clear_bindings(q);bind(q,1,text(p,"id"));bind(q,2,hash);int rc=sqlite3_step(q);if(rc==SQLITE_ROW)json_object_array_add(a,row(q));else if(rc!=SQLITE_DONE){sqlite3_finalize(q);json_object_put(a);return NULL;}
 }sqlite3_finalize(q);return a;
}
typedef struct {dc_secret_transport secret;long status;} transport;
static dunlin_http_result post(void *ctx,const dunlin_http_request *r,long *status){transport *t=ctx;dunlin_http_result v=t->secret.v1.post(t->secret.v1.user,r,status);t->status=*status;return v;}
static dunlin_http_result post2(void *ctx,const dunlin_http_request_v2 *r,long *status){transport *t=ctx;dunlin_http_result v=t->secret.v2.post(t->secret.v2.user,r,status);t->status=*status;return v;}
static const char *failure(dunlin_model_status status){
 switch(status){case DUNLIN_MODEL_INVALID:return "test_invalid_request";case DUNLIN_MODEL_PROTOCOL:return "test_protocol_error";case DUNLIN_MODEL_HTTP:return "test_http_error";case DUNLIN_MODEL_TRANSPORT:return "test_transport_error";case DUNLIN_MODEL_TIMEOUT:return "test_timeout";case DUNLIN_MODEL_CANCELLED:return "test_cancelled";case DUNLIN_MODEL_LIMIT:return "test_response_limit";case DUNLIN_MODEL_RATE_LIMIT:return "test_rate_limit";default:return "test_model_error";}
}
static int has_text(J *assistant){J *v=get(assistant,"content");if(!json_object_is_type(v,json_type_string))return 0;const char *s=json_object_get_string(v);return s&&strspn(s," \t\r\n")<strlen(s);}
char *dc_test_connection(dc_store *s,const char *raw,const char *secret,const dc_hooks *hooks){
 if(!s||!raw||strlen(raw)>16384)return reply(0,"test_configuration_invalid",NULL);
 J *p=dunlin_json_parse(raw,strlen(raw));char hash[65];transport t={0};
 if(!fingerprint(p,hash)||!dc_secret_transport_init(&t.secret,text(p,"endpoint"),text(p,"protocolName"),text(p,"authentication"),secret)){json_object_put(p);return reply(0,"test_configuration_invalid",NULL);}
 char id[65];nonce(id);sqlite3_stmt *q=NULL;int admitted=sql(s,"BEGIN IMMEDIATE");
 if(admitted){q=prep(s,"INSERT INTO connection_tests(id,provider,fingerprint,model,status) VALUES(?,?,?,?,'running')");if(q){bind(q,1,id);bind(q,2,text(p,"id"));bind(q,3,hash);bind(q,4,text(p,"testModel"));}admitted=done(q)&&sql(s,"DELETE FROM connection_tests WHERE id IN(SELECT id FROM connection_tests ORDER BY started DESC,rowid DESC LIMIT -1 OFFSET 1024)")&&sql(s,"COMMIT");}
 if(!admitted){sql(s,"ROLLBACK");json_object_put(p);return reply(0,"test_storage_error",NULL);}
 dunlin_http_v1 v1={post,&t};dunlin_http_v2 v2={post2,&t};dunlin_model_metrics metrics={0};
 dunlin_model_config config={.base_url=text(p,"endpoint"),.model=text(p,"testModel"),.timeout_ms=15000,.max_response_bytes=65536,.http=&v1};
 const char *wire=text(p,"protocolName");dunlin_protocol kind=!strcmp(wire,"responses")?DUNLIN_PROTOCOL_RESPONSES:!strcmp(wire,"anthropic")?DUNLIN_PROTOCOL_ANTHROPIC:!strcmp(wire,"ollama")?DUNLIN_PROTOCOL_OLLAMA:!strcmp(wire,"gemini")?DUNLIN_PROTOCOL_GEMINI:DUNLIN_PROTOCOL_CHAT;
 dunlin_protocol_options options={.protocol=kind,.max_tokens=16,.metrics=&metrics,.http=&v2};dunlin_model_events events={.user=hooks?hooks->user:NULL,.cancelled=hooks?hooks->cancelled:NULL};
 J *messages=json_object_new_array(),*message=json_object_new_object(),*tools=json_object_new_array(),*assistant=NULL;add(message,"role","user");add(message,"content","Reply with OK.");json_object_array_add(messages,message);
 int64_t before=monotonic_ms();dunlin_model_status status=dunlin_protocol_proposal(&config,&options,messages,tools,&events,&assistant,NULL,0);int64_t after=monotonic_ms();
 const char *error=status==DUNLIN_MODEL_OK?NULL:failure(status);J *calls=get(assistant,"tool_calls");if(!error&&json_object_is_type(calls,json_type_array)&&json_object_array_length(calls))error="test_unexpected_tool_call";if(!error&&!has_text(assistant))error="test_no_text";
 const char *outcome=status==DUNLIN_MODEL_CANCELLED?"cancelled":error?"failed":"passed";
 q=prep(s,"UPDATE connection_tests SET status=?,finished=unixepoch(),http_status=?,model_status=?,duration_ms=?,error=?,request_bytes=?,response_bytes=? WHERE id=? AND status='running'");
 if(q){bind(q,1,outcome);if(t.status>0)sqlite3_bind_int64(q,2,t.status);sqlite3_bind_int(q,3,status);if(before>=0&&after>=before)sqlite3_bind_int64(q,4,after-before);if(error)bind(q,5,error);sqlite3_bind_int64(q,6,(int64_t)metrics.request_bytes);if(metrics.response_bytes_known)sqlite3_bind_int64(q,7,(int64_t)metrics.response_bytes);bind(q,8,id);}
 int stored=done(q)&&sqlite3_changes(s->db)==1;json_object_put(messages);json_object_put(tools);json_object_put(assistant);json_object_put(p);
 if(!stored)return reply(0,"test_result_storage_error",NULL);
 q=prep(s,"SELECT id,provider,fingerprint,model,started,finished,status,http_status,model_status,duration_ms,error,request_bytes,response_bytes FROM connection_tests WHERE id=?");if(q)bind(q,1,id);J *result=q&&sqlite3_step(q)==SQLITE_ROW?row(q):NULL;sqlite3_finalize(q);return result?reply(1,NULL,result):reply(0,"test_result_storage_error",NULL);
}
