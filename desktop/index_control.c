#define _POSIX_C_SOURCE 200809L
#include "index_control.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static int sql(dc_store *s,const char *sql){return sqlite3_exec(s->db,sql,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(r,"error",json_object_new_string(error));if(data)json_object_object_add(r,"data",data);return r;}
int dc_index_schema(dc_store *s){return sql(s,"CREATE TABLE IF NOT EXISTS index_preferences(singleton INTEGER PRIMARY KEY CHECK(singleton=1),paused INTEGER NOT NULL,quota INTEGER NOT NULL);INSERT OR IGNORE INTO index_preferences VALUES(1,0,67108864);CREATE TABLE IF NOT EXISTS index_deletions(chat TEXT PRIMARY KEY);");}
static int64_t value(dc_store *s,const char *query){sqlite3_stmt *q=prep(s,query);if(!q)return -1;int64_t v=sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int64(q,0):-1;sqlite3_finalize(q);return v;}
int dc_index_cleanup_deleted(dc_store *s,unsigned limit){
 if(!s||limit>64)return -1;
 for(unsigned i=0;i<limit;i++){
  sqlite3_stmt *q=prep(s,"SELECT chat FROM index_deletions ORDER BY rowid LIMIT 1");
  if(!q)return -1;int rc=sqlite3_step(q);char *chat=rc==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);
  if(rc==SQLITE_DONE){if(s->index_fault==3)s->index_fault=0;return (int)i;}
  if(rc!=SQLITE_ROW||!chat){free(chat);return -1;}
  /* Source deletion committed first. A crash after this cache DELETE is safe:
   * its intent remains and is retried before any new jobs for the reused ID. */
  if(!dl_index_delete(s->index,"local",chat)){s->index_fault=3;free(chat);return -1;}
  q=prep(s,"DELETE FROM index_deletions WHERE chat=?");if(q)sqlite3_bind_text(q,1,chat,-1,SQLITE_TRANSIENT);free(chat);
  int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);if(!ok){s->index_fault=3;return -1;}
 }
 return (int)limit;
}
int dc_index_paused(dc_store *s){return value(s,"SELECT paused FROM index_preferences")!=0;}
size_t dc_index_quota(dc_store *s){int64_t quota=value(s,"SELECT quota FROM index_preferences");return quota>0&&(uint64_t)quota<=SIZE_MAX?(size_t)quota:67108864;}
static J *status(dc_store *s){
 J *d=json_object_new_object();json_object_object_add(d,"paused",json_object_new_boolean(dc_index_paused(s)));json_object_object_add(d,"available",json_object_new_boolean(s->index!=NULL));json_object_object_add(d,"bytes",json_object_new_int64((int64_t)dl_index_bytes(s->index)));json_object_object_add(d,"quota",json_object_new_int64((int64_t)dc_index_quota(s)));json_object_object_add(d,"documents",json_object_new_int64(dl_index_count(s->index)));json_object_object_add(d,"pending",json_object_new_int64(value(s,"SELECT count(*) FROM index_jobs")));json_object_object_add(d,"pendingDeletes",json_object_new_int64(value(s,"SELECT count(*) FROM index_deletions")));json_object_object_add(d,"prepared",json_object_new_int64(value(s,"SELECT count(*) FROM turn_preparation")));json_object_object_add(d,"partial",json_object_new_int64(value(s,"SELECT count(*) FROM turn_preparation WHERE partial<>0")));json_object_object_add(d,"fault",json_object_new_int(s->index_fault));return reply(1,NULL,d);
}
static J *rebuild(dc_store *s){
 char path[4096],staging[4096];snprintf(path,sizeof path,"%s/history.sqlite",s->directory);snprintf(staging,sizeof staging,"%s/history-rebuild-XXXXXX",s->directory);int fd=mkstemp(staging);if(fd<0)return reply(0,"cannot_stage_index",NULL);close(fd);dl_index *next=dl_index_open(staging);
 if(!next||!dl_index_limit_bytes(next,dc_index_quota(s))){dl_index_close(next);unlink(staging);return reply(0,"cannot_prepare_index",NULL);}dl_index_close(next);
 if(!sql(s,"BEGIN IMMEDIATE")){unlink(staging);return reply(0,"storage_error",NULL);}
 const char *seed="DELETE FROM index_jobs;DELETE FROM turn_preparation;INSERT INTO index_jobs(chat,seq,text) SELECT c.id,CAST(m.key AS INTEGER),COALESCE(json_extract(m.value,'$._dunlin_display'),json_extract(m.value,'$.content')) FROM chats c,json_each(c.state,'$.messages') m WHERE json_type(m.value,'$.content')='text' AND length(CAST(COALESCE(json_extract(m.value,'$._dunlin_display'),json_extract(m.value,'$.content')) AS BLOB))<=262144;";
 if(!sql(s,seed)){sql(s,"ROLLBACK");unlink(staging);return reply(0,"cannot_rebuild_source_jobs",NULL);}
 if(!sql(s,"COMMIT")){sql(s,"ROLLBACK");unlink(staging);return reply(0,"storage_error",NULL);}
 dl_index_close(s->index);s->index=NULL;
 const char *suffixes[]={"-wal","-shm","-journal"};
 for(size_t i=0;i<3;i++){char sidecar[4096];snprintf(sidecar,sizeof sidecar,"%s/history.sqlite%s",s->directory,suffixes[i]);if(unlink(sidecar)&&errno!=ENOENT){unlink(staging);s->index=dl_index_open(path);return reply(0,"cannot_replace_index_sidecar",NULL);}}
 if(rename(staging,path)){unlink(staging);s->index=dl_index_open(path);return reply(0,"cannot_replace_index",NULL);}
 s->index_fault=0;chmod(path,0600);s->index=dl_index_open(path);if(s->index)dl_index_limit_bytes(s->index,dc_index_quota(s));return s->index?status(s):reply(0,"cannot_reopen_index",NULL);
}
J *dc_index_request(dc_store *s,J *r){
 const char *op=str(r,"op");if(!op)return reply(0,"invalid_index_request",NULL);
 if(!strcmp(op,"index.status"))return status(s);
 if(!strcmp(op,"index.rebuild"))return rebuild(s);
 if(!strcmp(op,"index.pause")){
  J *v=get(r,"value");if(!json_object_is_type(v,json_type_boolean))return reply(0,"invalid_pause_value",NULL);sqlite3_stmt *q=prep(s,"UPDATE index_preferences SET paused=?");if(!q)return reply(0,"storage_error",NULL);sqlite3_bind_int(q,1,json_object_get_boolean(v));int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok?status(s):reply(0,"storage_error",NULL);
 }
 if(!strcmp(op,"index.quota")){
  J *v=get(r,"bytes");int64_t quota=json_object_get_int64(v);if(!json_object_is_type(v,json_type_int)||quota<1048576||quota>4294967296LL)return reply(0,"unsupported_index_quota",NULL);sqlite3_stmt *q=prep(s,"UPDATE index_preferences SET quota=?");if(!q)return reply(0,"storage_error",NULL);sqlite3_bind_int64(q,1,quota);int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);if(s->index&&dl_index_limit_bytes(s->index,(size_t)quota))s->index_fault=0;return ok?status(s):reply(0,"storage_error",NULL);
 }
 return reply(0,"unsupported_index_operation",NULL);
}
