#define _XOPEN_SOURCE 700
#include "dunlin/transcript.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/agent.h"
#include "internal.h"
#include "chat_workspace.h"
#include "trajectory.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct json_object J;
enum { archive_limit=8*1024*1024, turn_limit=16384, message_limit=1024*1024 };
static J *field(J *o,const char *name){J *v=NULL;if(o)json_object_object_get_ex(o,name,&v);return v;}
static const char *string(J *o,const char *key,size_t limit){
    J *v=field(o,key);
    if(!json_object_is_type(v,json_type_string))return NULL;
    const char *p=json_object_get_string(v);size_t n=(size_t)json_object_get_string_len(v);
    return n<=limit && !memchr(p,0,n) && ah_utf8_text(p,n)?p:NULL;
}
static int identifier(const char *v){return v && *v && strlen(v)<=64 && strspn(v,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(v);}
static int visible(const char *role){return role && (!strcmp(role,"user") || !strcmp(role,"assistant"));}
static sqlite3_stmt *prepare(dc_store *s,const char *sql){
    sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;
}
static void bind(sqlite3_stmt *q,int i,const char *text){sqlite3_bind_text(q,i,text,-1,SQLITE_TRANSIENT);}
static int execute(sqlite3_stmt *q){int ok=q && sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static J *parse(const char *text){
    if(!text)return NULL;
    size_t size=strnlen(text,archive_limit+1);if(!size || size>archive_limit)return NULL;
    J *doc=dunlin_json_parse(text,size),*rows=field(doc,"messages");const char *name=string(doc,"title",256);
    if(!json_object_is_type(doc,json_type_object) || json_object_object_length(doc)!=2 ||
       !name || !*name || !json_object_is_type(rows,json_type_array) || (size_t)json_object_array_length(rows)>turn_limit)goto bad;
    for(size_t i=0;i<(size_t)json_object_array_length(rows);i++){
        J *m=json_object_array_get_idx(rows,i);
        if(!json_object_is_type(m,json_type_object) || json_object_object_length(m)!=2 ||
           !visible(string(m,"role",9)) || !string(m,"content",message_limit))goto bad;
    }
    return doc;
bad:
    json_object_put(doc);return NULL;
}
char *dc_transcript_preview(const char *archive){
    J *doc=parse(archive);if(!doc)return NULL;
    J *o=json_object_new_object();json_object_object_add(o,"title",json_object_get(field(doc,"title")));
    json_object_object_add(o,"messages",json_object_new_int64(json_object_array_length(field(doc,"messages"))));
    char *out=strdup(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));json_object_put(o);json_object_put(doc);return out;
}
char *dc_transcript_export(dc_store *s,const char *chat){
    if(!s || !identifier(chat))return NULL;
    sqlite3_stmt *q=prepare(s,"SELECT title FROM chats WHERE id=?");if(!q)return NULL;bind(q,1,chat);
    J *out=NULL;if(sqlite3_step(q)==SQLITE_ROW){out=json_object_new_object();json_object_object_add(out,"title",json_object_new_string((const char*)sqlite3_column_text(q,0)));}
    sqlite3_finalize(q);if(!out)return NULL;
    char *source=dc_messages(s,chat);J *rows=source?dunlin_json_parse(source,strlen(source)):NULL;free(source);
    J *messages=json_object_new_array();json_object_object_add(out,"messages",messages);
    int ok=json_object_is_type(rows,json_type_array);
    size_t total=0;
    for(size_t i=0;ok && i<(size_t)json_object_array_length(rows);i++){
        J *m=json_object_array_get_idx(rows,i);const char *role=string(m,"role",32);if(!visible(role))continue;
        const char *content=string(m,"content",message_limit);
        if(!content || (total+=strlen(content))>archive_limit || (size_t)json_object_array_length(messages)>=turn_limit){ok=0;break;}
        J *r=json_object_new_object();json_object_object_add(r,"role",json_object_get(field(m,"role")));
        json_object_object_add(r,"content",json_object_get(field(m,"content")));json_object_array_add(messages,r);
    }
    const char *wire=ok?json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN):NULL;
    char *result=wire&&strlen(wire)<=archive_limit?strdup(wire):NULL;json_object_put(out);json_object_put(rows);return result;
}
int dc_transcript_restore(dc_store *s,const char *id,const char *identity,const char *archive){
    if(!s || !identifier(id) || !identifier(identity))return 0;
    J *doc=parse(archive);if(!doc)return 0;
    char dir[4096];struct stat st;int used=snprintf(dir,sizeof dir,"%s/Chat Workspace",s->directory);
    int ok=used>0 && (size_t)used<sizeof dir && (!mkdir(dir,0700) || errno==EEXIST) &&
        !lstat(dir,&st) && S_ISDIR(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&077);
#ifdef DC_HAS_WORKSPACE_ROOT_LEASES
    struct root_lease *lease=ok?dc_root_reserve(dir):NULL;
    ok=lease!=NULL;
#endif
    if(!ok || sqlite3_exec(s->db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){
#ifdef DC_HAS_WORKSPACE_ROOT_LEASES
        dc_root_unreserve(lease);
#endif
        json_object_put(doc);
        return 0;
    }
    sqlite3_stmt *q=prepare(s,"SELECT 1 FROM knowledge_meta WHERE key='owner' AND value=?");
    if(q)bind(q,1,identity);
    ok=q && sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);
    J *messages=field(doc,"messages"),*state=json_object_new_object();
    json_object_object_add(state,"version",json_object_new_int(1));json_object_object_add(state,"rounds",json_object_new_int(0));
    json_object_object_add(state,"active",json_object_new_boolean(0));json_object_object_add(state,"messages",json_object_get(messages));
    q=ok?prepare(s,"INSERT INTO chats(id,title,workspace,state,updated) VALUES(?,?,?,?,unixepoch())"):NULL;
    if(q){bind(q,1,id);bind(q,2,string(doc,"title",256));bind(q,3,dir);bind(q,4,json_object_to_json_string_ext(state,JSON_C_TO_STRING_PLAIN));}
    ok=execute(q);json_object_put(state);
    if(ok)ok=dc_chat_workspace_capture(s,id,dir);
    if(ok){char *key=dc_trajectory_chat_key(s,id);ok=key!=NULL;free(key);}
    q=ok?prepare(s,"INSERT INTO index_jobs(chat,seq,text) VALUES(?,?,?)"):NULL;ok=q!=NULL;
    for(size_t i=0;ok && i<(size_t)json_object_array_length(messages);i++){
        const char *text=string(json_object_array_get_idx(messages,i),"content",message_limit);
        if(strlen(text)>262144 || !*text)continue;
        sqlite3_reset(q);sqlite3_clear_bindings(q);bind(q,1,id);sqlite3_bind_int(q,2,(int)i);bind(q,3,text);
        ok=sqlite3_step(q)==SQLITE_DONE;
    }
    sqlite3_finalize(q);
    if(ok){
#ifdef DC_HAS_WORKSPACE_ROOT_LEASES
        ok=dc_roots_commit_reserved(s,&lease);
#else
        ok=sqlite3_exec(s->db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK;
#endif
    }
    if(!ok && !sqlite3_get_autocommit(s->db))sqlite3_exec(s->db,"ROLLBACK",NULL,NULL,NULL);
#ifdef DC_HAS_WORKSPACE_ROOT_LEASES
    dc_root_unreserve(lease);
#endif
    json_object_put(doc);
    return ok;
}
