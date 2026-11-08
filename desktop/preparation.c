#define _POSIX_C_SOURCE 200809L
#include "preparation.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include <string.h>
#include <stdint.h>
typedef struct json_object J;
int dc_preparation_schema(dc_store *s){return sqlite3_exec(s->db,"CREATE TABLE IF NOT EXISTS turn_preparation(chat TEXT NOT NULL,sequence INTEGER NOT NULL,source_hash TEXT NOT NULL,analysis_rev INTEGER NOT NULL,spans TEXT NOT NULL,partial INTEGER NOT NULL,PRIMARY KEY(chat,sequence));",NULL,NULL,NULL)==SQLITE_OK;}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
J *dc_preparation_messages(dc_store *s,const char *chat){
 if(!s||!chat)return NULL;sqlite3_stmt *q=prep(s,"SELECT state FROM chats WHERE id=?");J *checkpoint=NULL,*messages=NULL;
 if(q){sqlite3_bind_text(q,1,chat,-1,SQLITE_TRANSIENT);if(sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);int n=sqlite3_column_bytes(q,0);if(raw&&n>0&&n<=4194304&&strlen(raw)==(size_t)n)checkpoint=dunlin_json_parse(raw,(size_t)n);}}
 sqlite3_finalize(q);J *v=NULL;if(checkpoint)json_object_object_get_ex(checkpoint,"messages",&v);if(json_object_is_type(v,json_type_array))messages=json_object_get(v);json_object_put(checkpoint);return messages;
}
int dc_preparation_put(dc_store *s,const char *chat,int sequence,const char *text){
 dc_source_span spans[4];int partial=0;size_t n=dc_prepare_text_spans(text,strlen(text),spans,4,&partial);char hash[65];dc_sha256(text,strlen(text),hash);J *a=json_object_new_array();
 for(size_t i=0;i<n;i++){J *o=json_object_new_object();json_object_object_add(o,"start",json_object_new_int64((int64_t)spans[i].start));json_object_object_add(o,"end",json_object_new_int64((int64_t)spans[i].end));json_object_object_add(o,"flags",json_object_new_int((int)spans[i].flags));json_object_array_add(a,o);}
 sqlite3_stmt *q=prep(s,"INSERT INTO turn_preparation(chat,sequence,source_hash,analysis_rev,spans,partial) VALUES(?,?,?,1,?,?) ON CONFLICT(chat,sequence) DO UPDATE SET source_hash=excluded.source_hash,analysis_rev=excluded.analysis_rev,spans=excluded.spans,partial=excluded.partial");int ok=q!=NULL;if(q){sqlite3_bind_text(q,1,chat,-1,SQLITE_TRANSIENT);sqlite3_bind_int(q,2,sequence);sqlite3_bind_text(q,3,hash,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,4,json_object_to_json_string_ext(a,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT);sqlite3_bind_int(q,5,partial);ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);}json_object_put(a);return ok;
}
size_t dc_preparation_get(dc_store *s,const char *chat,int sequence,const char *text,dc_source_span out[4]){
 char hash[65];size_t len=strlen(text);dc_sha256(text,len,hash);sqlite3_stmt *q=prep(s,"SELECT spans FROM turn_preparation WHERE chat=? AND sequence=? AND source_hash=? AND analysis_rev=1");J *a=NULL;if(q){sqlite3_bind_text(q,1,chat,-1,SQLITE_TRANSIENT);sqlite3_bind_int(q,2,sequence);sqlite3_bind_text(q,3,hash,-1,SQLITE_TRANSIENT);if(sqlite3_step(q)==SQLITE_ROW)a=json_tokener_parse((const char*)sqlite3_column_text(q,0));sqlite3_finalize(q);}
 if(json_object_is_type(a,json_type_array)&&json_object_array_length(a)<=4){size_t n=json_object_array_length(a),used=0;for(size_t i=0;i<n;i++){J *o=json_object_array_get_idx(a,i),*start=NULL,*end=NULL,*flags=NULL;json_object_object_get_ex(o,"start",&start);json_object_object_get_ex(o,"end",&end);json_object_object_get_ex(o,"flags",&flags);int64_t x=json_object_get_int64(start),y=json_object_get_int64(end);if(x<0||y<=x||(uint64_t)y>len||y-x>384||((unsigned char)text[x]&192)==128||(y<(int64_t)len&&((unsigned char)text[y]&192)==128))break;out[used++]=(dc_source_span){(size_t)x,(size_t)y,(unsigned)json_object_get_int(flags)};}json_object_put(a);if(used==n)return n;}else json_object_put(a);
 return dc_prepare_text_spans(text,len,out,4,NULL);
}
