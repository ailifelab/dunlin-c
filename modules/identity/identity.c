#define _XOPEN_SOURCE 700
#include "dunlin/identity.h"
#include "dunlin/compact/agent.h"
#include "dunlin/compact/json.h"
#include <sqlite3.h>
#include <json-c/json.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
typedef struct json_object J;
struct dc_identity_catalog {sqlite3 *db;char *root;int lock;};
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);if(!json_object_is_type(v,json_type_string))return NULL;const char *s=json_object_get_string(v);return strlen(s)==(size_t)json_object_get_string_len(v)?s:NULL;}
static sqlite3_stmt *prep(dc_identity_catalog *c,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(c->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int i,const char *v){sqlite3_bind_text(q,i,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static J *reply(int ok,const char *error,J *data){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(r,"error",json_object_new_string(error));if(data)json_object_object_add(r,"data",data);return r;}
static int ident(const char *s){return s&&*s&&strlen(s)<=64&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(s);}
static J *list(dc_identity_catalog *c){
 sqlite3_stmt *q=prep(c,"SELECT id,name,kind,members,hidden,(id=(SELECT selected FROM identity_state)),(SELECT epoch FROM identity_state) FROM identities ORDER BY hidden,name,id");if(!q)return NULL;J *a=json_object_new_array();int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){J *o=json_object_new_object();const char *keys[]={"id","name","kind","members","hidden","selected","epoch"};for(int i=0;i<7;i++){const char *s=(const char*)sqlite3_column_text(q,i);J *v=i==3?json_tokener_parse(s):i==4||i==5?json_object_new_boolean(sqlite3_column_int(q,i)):i==6?json_object_new_int64(sqlite3_column_int64(q,i)):json_object_new_string(s);json_object_object_add(o,keys[i],v);}char path[PATH_MAX];const char *id=str(o,"id");int n=!strcmp(id,"local")?snprintf(path,sizeof path,"%s",c->root):snprintf(path,sizeof path,"%s/Identities/%s",c->root,id);if(n<0||n>=(int)sizeof path){json_object_put(o);json_object_put(a);sqlite3_finalize(q);return NULL;}json_object_object_add(o,"directory",json_object_new_string(path));json_object_array_add(a,o);}
 sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return NULL;}return a;
}
static J *request(dc_identity_catalog *c,J *r){
 const char *op=str(r,"op");if(!op)return reply(0,"invalid_request",NULL);
 if(!strcmp(op,"list")){J *a=list(c);return reply(a!=NULL,a?NULL:"storage_error",a);}
 const char *id=str(r,"id");if(!ident(id))return reply(0,"invalid_identity",NULL);
 if(!strcmp(op,"create")){
  const char *name=str(r,"name"),*kind=str(r,"kind");J *members=get(r,"members");
  if(!name||!*name||strlen(name)>128||!ah_utf8_text(name,strlen(name))||!kind||(strcmp(kind,"person")&&strcmp(kind,"shared"))||!json_object_is_type(members,json_type_array)||json_object_array_length(members)>32)return reply(0,"invalid_identity",NULL);
  for(size_t i=0;i<json_object_array_length(members);i++){J *v=json_object_array_get_idx(members,i);const char *s=json_object_get_string(v);if(!json_object_is_type(v,json_type_string)||!ident(s))return reply(0,"invalid_member",NULL);sqlite3_stmt *q=prep(c,"SELECT 1 FROM identities WHERE id=? AND kind='person' AND hidden=0");if(q)bind(q,1,s);int found=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);if(!found)return reply(0,"invalid_member",NULL);}
  if(!strcmp(kind,"person")&&json_object_array_length(members))return reply(0,"invalid_member",NULL);
  char path[PATH_MAX];snprintf(path,sizeof path,"%s/Identities/%s",c->root,id);if(mkdir(path,0700))return reply(0,"directory_exists_or_unavailable",NULL);
  sqlite3_stmt *q=prep(c,"INSERT INTO identities(id,name,kind,members) VALUES(?,?,?,?)");if(q){bind(q,1,id);bind(q,2,name);bind(q,3,kind);bind(q,4,json_object_to_json_string_ext(members,JSON_C_TO_STRING_PLAIN));}
  if(!step(q)){rmdir(path);return reply(0,"duplicate_name_or_storage_error",NULL);}return reply(1,NULL,list(c));
 }
 if(!strcmp(op,"select")){
  J *epoch=get(r,"epoch");if(!json_object_is_type(epoch,json_type_int))return reply(0,"missing_epoch",NULL);
  sqlite3_stmt *q=prep(c,"UPDATE identity_state SET selected=?,epoch=epoch+1 WHERE epoch=? AND EXISTS(SELECT 1 FROM identities WHERE id=? AND hidden=0)");if(q){bind(q,1,id);sqlite3_bind_int64(q,2,json_object_get_int64(epoch));bind(q,3,id);}int ok=step(q)&&sqlite3_changes(c->db)==1;return reply(ok,ok?NULL:"identity_missing_or_stale_epoch",ok?list(c):NULL);
 }
 if(!strcmp(op,"remove")||!strcmp(op,"restore")){
  int hidden=!strcmp(op,"remove");sqlite3_stmt *q=prep(c,"UPDATE identities SET hidden=? WHERE id=? AND id<>'local' AND id<>(SELECT selected FROM identity_state) AND NOT EXISTS(SELECT 1 FROM identities g,json_each(g.members) m WHERE g.hidden=0 AND m.value=identities.id)");
  if(q){sqlite3_bind_int(q,1,hidden);bind(q,2,id);}int ok=step(q)&&sqlite3_changes(c->db)==1;return reply(ok,ok?NULL:"identity_selected_or_shared_member",ok?list(c):NULL);
 }
 return reply(0,"unsupported_operation",NULL);
}
dc_identity_catalog *dc_identity_open(const char *root){
 struct stat st;if(!root||stat(root,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||strlen(root)>PATH_MAX-128)return NULL;
 dc_identity_catalog *c=calloc(1,sizeof *c);if(!c)return NULL;c->lock=-1;c->root=realpath(root,NULL);if(!c->root){dc_identity_close(c);return NULL;}char path[PATH_MAX];snprintf(path,sizeof path,"%s/identities.lock",c->root);c->lock=open(path,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);if(c->lock<0||flock(c->lock,LOCK_EX|LOCK_NB)){dc_identity_close(c);return NULL;}
 snprintf(path,sizeof path,"%s/Identities",c->root);if(mkdir(path,0700)&&errno!=EEXIST){dc_identity_close(c);return NULL;}if(lstat(path,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){dc_identity_close(c);return NULL;}
 snprintf(path,sizeof path,"%s/identities.sqlite",c->root);if(sqlite3_open(path,&c->db)!=SQLITE_OK){dc_identity_close(c);return NULL;}chmod(path,0600);sqlite3_busy_timeout(c->db,3000);
 if(sqlite3_exec(c->db,"PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;CREATE TABLE IF NOT EXISTS identities(id TEXT PRIMARY KEY,name TEXT NOT NULL UNIQUE,kind TEXT NOT NULL,members TEXT NOT NULL DEFAULT '[]',hidden INTEGER NOT NULL DEFAULT 0);CREATE TABLE IF NOT EXISTS identity_state(singleton INTEGER PRIMARY KEY CHECK(singleton=1),selected TEXT NOT NULL,epoch INTEGER NOT NULL);INSERT OR IGNORE INTO identities(id,name,kind) VALUES('local','本机身份','person');INSERT OR IGNORE INTO identity_state VALUES(1,'local',1);",NULL,NULL,NULL)!=SQLITE_OK){dc_identity_close(c);return NULL;}return c;
}
void dc_identity_close(dc_identity_catalog *c){if(c){sqlite3_close(c->db);if(c->lock>=0)close(c->lock);free(c->root);free(c);}}
char *dc_identity_request(dc_identity_catalog *c,const char *raw){if(!c||!raw||strlen(raw)>16384)return NULL;J *r=dunlin_json_parse(raw,strlen(raw)),*o=json_object_is_type(r,json_type_object)?request(c,r):reply(0,"invalid_json",NULL);char *out=o?strdup(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(r);json_object_put(o);return out;}
