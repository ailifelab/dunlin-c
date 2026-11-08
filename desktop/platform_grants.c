#define _POSIX_C_SOURCE 200809L
#include "platform_grants.h"
#include "media_records.h"
#include "dunlin/grant_broker.h"
#include "dunlin/compact/json.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
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
int dc_platform_grants_schema(dc_store *s){dl_grant_broker *b=dl_grant_open(s->db,"local",0);if(!b)return 0;dl_grant_close(b);return sqlite3_exec(s->db,"UPDATE capability_leases SET state=CASE WHEN state='revoking' THEN 'unknown' ELSE 'expired' END WHERE state IN('revoking','active')",NULL,NULL,NULL)==SQLITE_OK;}
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
J *dc_platform_grants_request(dc_store *s,J *r){
 const char *op=str(r,"op");char *principal=owner(s);if(!principal)return reply("identity_not_bound",NULL);
 dl_grant_broker *b=dl_grant_open(s->db,principal,0);free(principal);if(!b)return reply("storage_error",NULL);
 dl_grant_adapter adapter={"macos-tcc",s,revoke_tcc};dl_grant_register(b,&adapter);J *out=NULL;
 if(op&&!strcmp(op,"grants.platform.list")){
  char *raw=dl_grant_list(b,"");J *data=raw?dunlin_json_parse(raw,strlen(raw)):NULL;dl_grant_free(raw);out=reply(data?NULL:"storage_error",data);
 }else if(op&&!strcmp(op,"grants.platform.record")){
  const char *call=str(r,"call"),*resource=str(r,"resource");sqlite3_stmt *q=NULL;sqlite3_prepare_v2(s->db,"SELECT chat FROM media_calls WHERE id=? AND state IN('connecting','live')",-1,&q,NULL);if(q)sqlite3_bind_text(q,1,call?call:"",-1,SQLITE_TRANSIENT);char *chat=q&&sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);
  if(!chat||!call||strlen(call)>64||!resource||(strcmp(resource,"microphone")&&strcmp(resource,"camera")&&strcmp(resource,"speech"))||!s->platform_bundle)out=reply("invalid_call_permission",NULL);
  else{char id[100];snprintf(id,sizeof id,"%s-%s",call,resource);int64_t now=(int64_t)time(NULL);int ok=dl_grant_issue(b,id,chat,"macos-tcc",resource,call,now,now+3600);out=reply(ok?NULL:"permission_record_failed",NULL);}free(chat);
 }else if(op&&!strcmp(op,"grants.platform.revoke")){
  /* App-wide OS permission reset only after every app call has stopped. */
  sqlite3_stmt *q=NULL;sqlite3_prepare_v2(s->db,"SELECT 1 FROM media_calls WHERE state IN('connecting','live') LIMIT 1",-1,&q,NULL);int rc=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);
  if(rc!=SQLITE_DONE)out=reply("stop_media_before_revoking",NULL);
  else{int ok=dl_grant_revoke(b,str(r,"id"),(int64_t)time(NULL));char *raw=ok?dl_grant_list(b,""):NULL;J *data=raw?dunlin_json_parse(raw,strlen(raw)):NULL;dl_grant_free(raw);out=reply(ok?NULL:"permission_revoke_failed",data);}
 }else out=reply("unsupported_operation",NULL);
 dl_grant_close(b);return out;
}
