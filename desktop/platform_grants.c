#define _POSIX_C_SOURCE 200809L
#include "platform_grants.h"
#include "media_records.h"
#include "dunlin/grant_broker.h"
#include "dunlin/compact/json.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <curl/curl.h>
#include <stdio.h>
#ifdef __APPLE__
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#endif
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);if(!json_object_is_type(v,json_type_string))return NULL;const char *s=json_object_get_string(v);return strlen(s)==(size_t)json_object_get_string_len(v)?s:NULL;}
static J *reply(const char *error,J *data){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(!error));if(error)json_object_object_add(r,"error",json_object_new_string(error));if(data)json_object_object_add(r,"data",data);return r;}
static char *owner(dc_store *s){sqlite3_stmt *q=NULL;sqlite3_prepare_v2(s->db,"SELECT value FROM knowledge_meta WHERE key='owner'",-1,&q,NULL);char *p=q&&sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);return p;}
int dc_platform_grants_schema(dc_store *s){dl_grant_broker *b=dl_grant_open(s->db,"local",0);if(!b)return 0;dl_grant_close(b);return sqlite3_exec(s->db,"CREATE TABLE IF NOT EXISTS remote_grant_endpoints(id TEXT PRIMARY KEY,endpoint TEXT NOT NULL);UPDATE capability_leases SET state=CASE WHEN state='revoking' THEN 'unknown' ELSE 'expired' END WHERE state IN('revoking','active')",NULL,NULL,NULL)==SQLITE_OK;}
int dc_configure_platform_grants(dc_store *s,const char *bundle){
 if(!s||!bundle||!*bundle||strlen(bundle)>200||strspn(bundle,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")!=strlen(bundle))return 0;
 char *copy=strdup(bundle);if(!copy)return 0;free(s->platform_bundle);s->platform_bundle=copy;return 1;
}
static dl_grant_revoke_result revoke_tcc(void *raw,const char *resource){
 dc_store *s=raw;if(!strcmp(resource,"speech"))return DL_GRANT_NEEDS_USER;if(!s->platform_bundle)return DL_GRANT_NEEDS_USER;
 const char *service=!strcmp(resource,"microphone")?"Microphone":!strcmp(resource,"camera")?"Camera":NULL;if(!service)return DL_GRANT_UNKNOWN;
#ifdef __APPLE__
 pid_t child=fork();if(child<0)return DL_GRANT_RETRY;
 if(!child){int sink=open("/dev/null",O_RDWR);if(sink>=0){dup2(sink,STDOUT_FILENO);dup2(sink,STDERR_FILENO);if(sink>2)close(sink);}execl("/usr/bin/tccutil","tccutil","reset",service,s->platform_bundle,(char*)NULL);_exit(127);}
 int status=0;for(int i=0;i<250;i++){pid_t got=waitpid(child,&status,WNOHANG);if(got==child)return WIFEXITED(status)&&WEXITSTATUS(status)==0?DL_GRANT_REVOKED:DL_GRANT_NEEDS_USER;if(got<0&&errno!=EINTR)return DL_GRANT_UNKNOWN;struct timespec delay={0,20000000};nanosleep(&delay,NULL);}
 kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}return DL_GRANT_UNKNOWN;
#else
 return DL_GRANT_NEEDS_USER;
#endif
}
static int valid_endpoint(const char *endpoint){
 if(!endpoint||strlen(endpoint)>2048||strncmp(endpoint,"https://",8)||strpbrk(endpoint,"?#@\\ \t\r\n"))return 0;
 CURLU *url=curl_url();char *host=NULL;
 int ok=url&&curl_url_set(url,CURLUPART_URL,endpoint,0)==CURLUE_OK&&curl_url_get(url,CURLUPART_HOST,&host,0)==CURLUE_OK&&*host;
 curl_free(host);curl_url_cleanup(url);return ok;
}
typedef struct {const char *endpoint,*token;} remote_request;
static size_t discard_response(char *data,size_t size,size_t count,void *unused){
 (void)data;(void)unused;return size*count;
}
static dl_grant_revoke_result revoke_remote(void *user,const char *id){
 (void)id;remote_request *request=user;
 if(!request||!request->token||!*request->token)return DL_GRANT_NEEDS_USER;
 CURL *curl=curl_easy_init();if(!curl)return DL_GRANT_RETRY;
 char *encoded=curl_easy_escape(curl,request->token,0);
 char *body=encoded?malloc(strlen(encoded)+7):NULL;
 long status=0;CURLcode result=CURLE_FAILED_INIT;
 const char *ca=getenv("SSL_CERT_FILE");
 if(body&&(!ca||!*ca||curl_easy_setopt(curl,CURLOPT_CAINFO,ca)==CURLE_OK)){
  sprintf(body,"token=%s",encoded);
  if(curl_easy_setopt(curl,CURLOPT_URL,request->endpoint)==CURLE_OK&&
     curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body)==CURLE_OK&&
     curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L)==CURLE_OK&&
     curl_easy_setopt(curl,CURLOPT_TIMEOUT,15L)==CURLE_OK&&
     curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,discard_response)==CURLE_OK){
   result=curl_easy_perform(curl);curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
  }
 }
 free(body);curl_free(encoded);curl_easy_cleanup(curl);
 if(result!=CURLE_OK)return DL_GRANT_RETRY;
 if(status==200)return DL_GRANT_REVOKED;
 return status==401||status==403?DL_GRANT_NEEDS_USER:DL_GRANT_RETRY;
}
int dc_remote_account_record(dc_store *s,const char *id,const char *chat,const char *label,const char *endpoint,int64_t expires){
 if(!s||!id||!chat||!label||!valid_endpoint(endpoint))return 0;
 char *principal=owner(s);dl_grant_broker *b=principal?dl_grant_open(s->db,principal,0):NULL;free(principal);
 dl_grant_adapter adapter={"oauth-revocation",NULL,revoke_remote};
 int ok=dl_grant_register(b,&adapter)&&sqlite3_exec(s->db,"BEGIN IMMEDIATE",NULL,NULL,NULL)==SQLITE_OK;
 if(ok){
  sqlite3_stmt *q=NULL;
  ok=sqlite3_prepare_v2(s->db,"INSERT INTO remote_grant_endpoints(id,endpoint) SELECT ?,? FROM chats WHERE id=?",-1,&q,NULL)==SQLITE_OK;
  if(ok){sqlite3_bind_text(q,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,2,endpoint,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,3,chat,-1,SQLITE_TRANSIENT);ok=sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;}
  sqlite3_finalize(q);
  ok=ok&&dl_grant_issue(b,id,chat,adapter.protocol,id,label,time(NULL),expires);
  if(ok)ok=sqlite3_exec(s->db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK;
  if(!ok)sqlite3_exec(s->db,"ROLLBACK",NULL,NULL,NULL);
 }
 dl_grant_close(b);return ok;
}
char *dc_remote_account_revoke(dc_store *s,const char *id,const char *endpoint,const char *token){
 if(!s||!id||(token&&*token&&!valid_endpoint(endpoint))||(token&&strlen(token)>16384))return NULL;
 char *principal=owner(s);dl_grant_broker *b=principal?dl_grant_open(s->db,principal,0):NULL;sqlite3_stmt *q=NULL;
 int match=b&&sqlite3_prepare_v2(s->db,"SELECT 1 FROM remote_grant_endpoints e JOIN capability_leases l ON l.id=e.id WHERE e.id=? AND (? IS NULL OR e.endpoint=?) AND l.principal=? AND l.protocol='oauth-revocation'",-1,&q,NULL)==SQLITE_OK;
 if(match){sqlite3_bind_text(q,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,2,endpoint,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,3,endpoint,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,4,principal,-1,SQLITE_TRANSIENT);match=sqlite3_step(q)==SQLITE_ROW;}
 sqlite3_finalize(q);free(principal);
 remote_request request={endpoint,token};dl_grant_adapter adapter={"oauth-revocation",&request,revoke_remote};
 int ok=match&&dl_grant_register(b,&adapter)&&dl_grant_revoke(b,id,time(NULL));
 char *rows=ok?dl_grant_list(b,""):NULL;dl_grant_close(b);return rows;
}
J *dc_platform_grants_request(dc_store *s,J *r){
 const char *op=str(r,"op");char *principal=owner(s);if(!principal)return reply("identity_not_bound",NULL);
 dl_grant_broker *b=dl_grant_open(s->db,principal,0);free(principal);if(!b)return reply("storage_error",NULL);
 dl_grant_adapter adapter={"macos-tcc",s,revoke_tcc};dl_grant_register(b,&adapter);J *out=NULL;
 if(op&&(!strcmp(op,"grants.platform.list")||!strcmp(op,"grants.platform.attempts"))){
  char *raw=!strcmp(op,"grants.platform.list")?dl_grant_list(b,""):dl_grant_attempts(b,str(r,"id"));J *data=raw?dunlin_json_parse(raw,strlen(raw)):NULL;dl_grant_free(raw);out=reply(data?NULL:"storage_error",data);
 }else if(op&&!strcmp(op,"grants.platform.record")){
  const char *call=str(r,"call"),*resource=str(r,"resource");sqlite3_stmt *q=NULL;sqlite3_prepare_v2(s->db,"SELECT chat FROM media_calls WHERE id=? AND state IN('connecting','live')",-1,&q,NULL);if(q)sqlite3_bind_text(q,1,call?call:"",-1,SQLITE_TRANSIENT);char *chat=q&&sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);
  if(!chat||!call||strlen(call)>64||!resource||(strcmp(resource,"microphone")&&strcmp(resource,"camera")&&strcmp(resource,"speech"))||!s->platform_bundle)out=reply("invalid_call_permission",NULL);
  else{char id[100];snprintf(id,sizeof id,"%s-%s",call,resource);int64_t now=(int64_t)time(NULL);int ok=dl_grant_issue(b,id,chat,"macos-tcc",resource,call,now,now+3600);out=reply(ok?NULL:"permission_record_failed",NULL);}free(chat);
 }else if(op&&!strcmp(op,"grants.platform.revoke")){
  sqlite3_stmt *kind=NULL;
  int system_lease=sqlite3_prepare_v2(s->db,"SELECT 1 FROM capability_leases WHERE id=? AND protocol='macos-tcc'",-1,&kind,NULL)==SQLITE_OK;
  if(system_lease){sqlite3_bind_text(kind,1,str(r,"id"),-1,SQLITE_TRANSIENT);system_lease=sqlite3_step(kind)==SQLITE_ROW;}
  sqlite3_finalize(kind);
  if(!system_lease){dl_grant_close(b);return reply("unsupported_permission_adapter",NULL);}
  /* App-wide OS permission reset only after every app call has stopped. */
  sqlite3_stmt *q=NULL;sqlite3_prepare_v2(s->db,"SELECT 1 FROM media_calls WHERE state IN('connecting','live') LIMIT 1",-1,&q,NULL);int rc=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);
  if(rc!=SQLITE_DONE)out=reply("stop_media_before_revoking",NULL);
  else{int ok=dl_grant_revoke(b,str(r,"id"),(int64_t)time(NULL));char *raw=ok?dl_grant_list(b,""):NULL;J *data=raw?dunlin_json_parse(raw,strlen(raw)):NULL;dl_grant_free(raw);out=reply(ok?NULL:"permission_revoke_failed",data);}
 }else out=reply("unsupported_operation",NULL);
 dl_grant_close(b);return out;
}
