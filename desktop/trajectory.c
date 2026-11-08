#define _POSIX_C_SOURCE 200809L
#include "trajectory.h"
#include "dunlin/trajectory.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
int dc_trajectory_schema(dc_store *s){return sqlite3_exec(s->db,"CREATE TABLE IF NOT EXISTS trace_chat_epochs(chat TEXT PRIMARY KEY,epoch TEXT NOT NULL UNIQUE);INSERT OR IGNORE INTO trace_chat_epochs(chat,epoch) SELECT id,lower(hex(randomblob(16))) FROM chats;",NULL,NULL,NULL)==SQLITE_OK;}
char *dc_trajectory_chat_key(dc_store *s,const char *chat){
    sqlite3_stmt *read=NULL;char *existing=NULL;
    if(sqlite3_prepare_v2(s->db,"SELECT epoch FROM trace_chat_epochs WHERE chat=?",-1,&read,NULL)==SQLITE_OK){sqlite3_bind_text(read,1,chat,-1,SQLITE_TRANSIENT);if(sqlite3_step(read)==SQLITE_ROW)existing=strdup((const char*)sqlite3_column_text(read,0));}sqlite3_finalize(read);if(existing)return existing;
    sqlite3_stmt *q=NULL;int ok=sqlite3_prepare_v2(s->db,"INSERT OR IGNORE INTO trace_chat_epochs(chat,epoch) SELECT ?,lower(hex(randomblob(16))) WHERE EXISTS(SELECT 1 FROM chats WHERE id=?)",-1,&q,NULL)==SQLITE_OK;
    if(ok){sqlite3_bind_text(q,1,chat,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,2,chat,-1,SQLITE_TRANSIENT);ok=sqlite3_step(q)==SQLITE_DONE;}sqlite3_finalize(q);q=NULL;
    char *key=NULL;if(ok&&sqlite3_prepare_v2(s->db,"SELECT epoch FROM trace_chat_epochs WHERE chat=?",-1,&q,NULL)==SQLITE_OK){sqlite3_bind_text(q,1,chat,-1,SQLITE_TRANSIENT);if(sqlite3_step(q)==SQLITE_ROW)key=strdup((const char*)sqlite3_column_text(q,0));}sqlite3_finalize(q);return key;
}
dl_trace_span *dc_trajectory_begin(dc_store *s,const char *chat,const char *run,const char *model,const char *protocol,long long start,const char *secret){
    if(!s->trajectory)return NULL;char *key=dc_trajectory_chat_key(s,chat);dl_trace_span *span=key?dl_trajectory_begin(s->trajectory,key,run,model,protocol,start,secret):NULL;free(key);return span;
}
struct json_object *dc_trajectory_request(dc_store *s,struct json_object *r) {
    struct json_object *v=NULL;json_object_object_get_ex(r,"op",&v);
    const char *op=json_object_get_string(v);v=NULL;json_object_object_get_ex(r,"chat",&v);
    const char *chat=json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;
    if(op && (!strcmp(op,"trace.status")||!strcmp(op,"trace.config")))return dl_trajectory_request(s->trajectory,r);
    sqlite3_stmt *q=NULL;
    int ok=chat && strlen(chat)<=64 && sqlite3_prepare_v2(s->db,"SELECT 1 FROM chats WHERE id=?",-1,&q,NULL)==SQLITE_OK;
    if(ok){sqlite3_bind_text(q,1,chat,-1,SQLITE_TRANSIENT);ok=sqlite3_step(q)==SQLITE_ROW;}
    sqlite3_finalize(q);
    if(ok){
        char *key=dc_trajectory_chat_key(s,chat);struct json_object *copy=json_tokener_parse(json_object_to_json_string_ext(r,JSON_C_TO_STRING_PLAIN));
        if(copy&&key){json_object_object_add(copy,"chat",json_object_new_string(key));struct json_object *out=dl_trajectory_request(s->trajectory,copy);json_object_put(copy);free(key);struct json_object *data=NULL;if(op&&!strcmp(op,"trace.export")&&json_object_object_get_ex(out,"data",&data)&&data)json_object_object_add(data,"chat",json_object_new_string(chat));return out;}
        free(key);json_object_put(copy);
    }
    struct json_object *out=json_object_new_object();
    json_object_object_add(out,"ok",json_object_new_boolean(0));
    json_object_object_add(out,"error",json_object_new_string("chat_not_found"));return out;
}
char *dc_read_trajectory(const char *directory,const char *raw){
    if(!raw||strlen(raw)>4096)return NULL;
    struct json_object *request=json_tokener_parse(raw),*v=NULL;if(request)json_object_object_get_ex(request,"op",&v);const char *op=json_object_get_string(v);
    int valid=op&&(!strcmp(op,"trace.status")||!strcmp(op,"trace.segments")||!strcmp(op,"trace.events")||!strcmp(op,"trace.export"));json_object_put(request);return valid?dc_read_view(directory,raw):NULL;
}
