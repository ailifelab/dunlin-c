#define _POSIX_C_SOURCE 200809L
#include "recovery.h"
#include "session_outcomes.h"
#include "trajectory.h"
#include "file_history.h"
#include "knowledge_projection.h"
#include "knowledge.h"
#include "extensions.h"
#include "extension_tools.h"
#include "catalog.h"
#include "dunlin/desktop.h"
#include "dunlin/history_index.h"
#include "internal.h"
#include "model_helpers.h"
#include "media_records.h"
#include "platform_grants.h"
#include "config_registry.h"
#include "tool_records.h"
#include "project_files.h"
#include "skills.h"
#include "grants.h"
#include "dunlin/content_hash.h"
#include "dunlin/message_projection.h"
#include "secret_transport.h"
#include "project_context.h"
#include "chat_workspace.h"
#include "attachments.h"
#include "attachment_projection.h"
#include "model_identity.h"
#include "compaction.h"
#include "preparation.h"
#include "index_control.h"
#include "agents.h"
#include "dunlin/compact/runtime.h"
#include "dunlin/compact/session.h"
#include "dunlin/compact/json.h"
#include <json-c/json.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
typedef struct json_object J;
typedef struct {
    dc_store *store;
    const char *id;
    char profile[65];
    J *original;
    const char *display_input;
    const char *expanded_input;
    const char *snapshot;
    char run[65];
    const char *retry_run;
    const char *trace_model,*trace_protocol,*trace_secret;
    dl_trace_span *trace;
    dc_tool_record_run *tool_records;
    const dc_hooks *hooks;
    dl_attachment_draft attachment;
    const char *input;
    int media,input_committed;
}
lease;
static J *get(J *o,const char *k){
    J *v=NULL;
    if(o)json_object_object_get_ex(o,k,&v);
    return v;
}
static size_t message_count(J *o){J *a=get(o,"messages");return json_object_is_type(a,json_type_array)?json_object_array_length(a):0;}
/* Recorded only for newly committed source messages. A clock failure remains
 * explicit unknown zero; never backfill earlier source from a cache timestamp. */
static int64_t source_time_ms(void){struct timespec t;if(timespec_get(&t,TIME_UTC)!=TIME_UTC||t.tv_sec<=0||(uint64_t)t.tv_sec>9007199254740ULL)return 0;return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static char *copyjson(J *j){
    return j?strdup(json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN)):NULL;
}
static J *clone(J *j){
    char *s=copyjson(j);
    J *r=s?json_tokener_parse(s):NULL;
    free(s);
    return r;
}
void dc_free(void *p){
    free(p);
}
static int valid(const char *s){
    return s&&*s&&strlen(s)<=64&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(s);
}
static sqlite3_stmt *prepare(dc_store *s,const char *sql){
    sqlite3_stmt *q=NULL;
    if(sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)!=SQLITE_OK)return NULL;
    return q;
}
static void bind(sqlite3_stmt *q,int i,const char *s){
    sqlite3_bind_text(q,i,s?s:"",-1,SQLITE_TRANSIENT);
}
static int exec(dc_store *s,const char *sql){
    return sqlite3_exec(s->db,sql,NULL,NULL,NULL)==SQLITE_OK;
}
static int database_path(const char *path){
    struct stat st;
    if(lstat(path,&st))return errno==ENOENT;
    return S_ISREG(st.st_mode)&&st.st_nlink==1&&st.st_uid==geteuid();
}
#ifdef DUNLIN_WITH_DSH
void dc_desktop_install_dsh(dc_store *);
#endif
dc_store *dc_open(const char *directory){
    if(!directory||strlen(directory)>3968)return NULL;
    dc_store *s=calloc(1,sizeof *s);
    if(!s)return NULL;
    s->lock=-1;
    struct stat st;
    if(stat(directory,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){
        dc_close(s);
        return NULL;
    }
    s->directory=strdup(directory);
    if(!s->directory){
        dc_close(s);
        return NULL;
    }
    char p[4096];
    snprintf(p,sizeof p,"%s/desktop.lock",directory);
    s->lock=open(p,O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    if(s->lock<0||flock(s->lock,LOCK_EX|LOCK_NB)){
        dc_close(s);
        return NULL;
    }
    snprintf(p,sizeof p,"%s/conversations.sqlite",directory);
    if(!database_path(p)){dc_close(s);return NULL;}
    if(sqlite3_open(p,&s->db)!=SQLITE_OK){
        dc_close(s);
        return NULL;
    }
    chmod(p,0600);
    sqlite3_busy_timeout(s->db,3000);
    if(!exec(s,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS chats(id TEXT PRIMARY KEY,title TEXT NOT NULL,workspace TEXT NOT NULL,state TEXT,updated INTEGER NOT NULL DEFAULT 0);CREATE TABLE IF NOT EXISTS index_jobs(chat TEXT,seq INTEGER,text TEXT,PRIMARY KEY(chat,seq));")){
        dc_close(s);
        return NULL;
    }
    if(!dc_workspace_schema(s)||!dc_chat_workspaces_schema(s)||!dc_skills_schema(s)||!dc_grants_schema(s)||!dc_compaction_schema(s)||!dc_preparation_schema(s)||!exec(s,"CREATE TABLE IF NOT EXISTS run_records(id TEXT PRIMARY KEY,chat TEXT NOT NULL,profile TEXT NOT NULL,snapshot TEXT NOT NULL,started INTEGER NOT NULL DEFAULT(unixepoch()),finished INTEGER,status TEXT NOT NULL DEFAULT 'running');CREATE INDEX IF NOT EXISTS runs_chat ON run_records(chat,started DESC);CREATE INDEX IF NOT EXISTS runs_chat_profile ON run_records(chat,profile,started DESC);")){dc_close(s);return NULL;}
    if(!dc_index_schema(s)||!dc_catalog_schema(s)||!dc_extensions_schema(s)||!dc_knowledge_schema(s)||!dc_recovery_schema(s)||!dc_trajectory_schema(s)||!dc_file_history_schema(s)||!dc_config_schema(s)||!dc_tool_records_schema(s)||!dc_tool_records_import(s)||!dc_attachments_initialize(s,0)){dc_close(s);return NULL;}
    snprintf(p,sizeof p,"%s/history.sqlite",directory);
    s->index=database_path(p)?dl_index_open(p):NULL;
    if(s->index)chmod(p,0600);
    if(s->index)dl_index_limit_bytes(s->index,dc_index_quota(s));
    /* Optional derived cache; its corruption must not disable source history. */
    s->trajectory=dl_trajectory_open(directory);
    s->live_stream=dl_live_stream_open();
    if(!dc_platform_grants_schema(s)||!dc_media_schema(s)||!dc_agents_initialize(s)){dc_close(s);return NULL;}
    dc_file_history_cleanup(s,8);
    #ifdef DUNLIN_WITH_DSH
    dc_desktop_install_dsh(s);
    #endif
    return s;
}
dc_store *dc_open_agent_worker(dc_store *owner){
    if(!owner||owner->lock<0||!owner->directory||!sqlite3_get_autocommit(owner->db))return NULL;
    dc_store *s=calloc(1,sizeof *s);if(!s)return NULL;s->lock=dup(owner->lock);s->directory=strdup(owner->directory);
    char path[4096],canonical[4096];snprintf(path,sizeof path,"%s/conversations.sqlite",owner->directory);
    if(s->lock<0||!s->directory||!database_path(path)||!realpath(path,canonical)||sqlite3_open_v2(canonical,&s->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOFOLLOW,NULL)!=SQLITE_OK){dc_close(s);return NULL;}
    fcntl(s->lock,F_SETFD,FD_CLOEXEC);sqlite3_busy_timeout(s->db,3000);
    if(!exec(s,"PRAGMA synchronous=FULL;")||!dc_chat_workspace_writer(s)||!dc_attachments_initialize(s,0)){dc_close(s);return NULL;}
    /* Derived traces are optional, but workers must not run startup recovery. */
    if(owner->trajectory)s->trajectory=dl_trajectory_open_worker(owner->trajectory);
    s->live_stream=dl_live_stream_retain(owner->live_stream);
    if(!dc_extensions_share(owner,s)){dc_close(s);return NULL;}
    return s;
}
void dc_close(dc_store *s){
    if(s){
        dc_extensions_close(s);
        dl_live_stream_close(s->live_stream);
        dl_trajectory_close(s->trajectory);
        dl_index_close(s->index);
        dl_attachments_close(s->attachments);
        sqlite3_close(s->db);
        if(s->lock>=0)close(s->lock);
        free(s->platform_bundle);
    free(s->directory);
        free(s);
    }
}
int dc_add_chat(dc_store *s,const char *id,const char *title,const char *workspace){
    if(!s||!valid(id)||!title||!workspace)return 0;
    char canonical[4096];
    if(realpath(workspace,canonical))workspace=canonical;
    if(!exec(s,"BEGIN IMMEDIATE"))return 0;
    sqlite3_stmt *q=prepare(s,"INSERT INTO chats(id,title,workspace,updated) VALUES(?,?,?,unixepoch())");
    if(!q){exec(s,"ROLLBACK");return 0;}
    bind(q,1,id);
    bind(q,2,title);
    bind(q,3,workspace);
    int ok=sqlite3_step(q)==SQLITE_DONE;
    sqlite3_finalize(q);
    if(ok){
        q=prepare(s,"INSERT OR IGNORE INTO chat_properties(chat,project) SELECT ?,id FROM projects WHERE path=?");
        if(q){bind(q,1,id);bind(q,2,workspace);ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);}
        else ok=0;
    }
    if(ok)ok=dc_chat_workspace_capture(s,id,workspace);
    if(ok){char *key=dc_trajectory_chat_key(s,id);ok=key!=NULL;free(key);}
    if(!ok){exec(s,"ROLLBACK");return 0;}
    if(!exec(s,"COMMIT")){exec(s,"ROLLBACK");return 0;}
    return 1;
}
static J *state(dc_store *s,const char *id){
    sqlite3_stmt *q=prepare(s,"SELECT state FROM chats WHERE id=?");
    if(!q)return NULL;
    bind(q,1,id);
    J *j=NULL;
    if(sqlite3_step(q)==SQLITE_ROW){
        const char *t=(const char*)sqlite3_column_text(q,0);
        if(t)j=dunlin_json_parse(t,strlen(t));
    }
    sqlite3_finalize(q);
    return j;
}
int dc_rename_chat(dc_store *s,const char *id,const char *title){
    if(!s||!valid(id)||!title||!*title||strlen(title)>256||!ah_utf8_text(title,strlen(title)))return 0;
    sqlite3_stmt *q=prepare(s,"UPDATE chats SET title=? WHERE id=?");
    if(!q)return 0;
    bind(q,1,title);bind(q,2,id);
    int ok=sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;
    sqlite3_finalize(q);return ok;
}
char *dc_chats(dc_store *s){
    if(!s)return NULL;
    sqlite3_stmt *q=prepare(s,"SELECT c.id,c.title,c.workspace,COALESCE(m.project,''),COALESCE(m.archived,0),COALESCE(m.pinned,0),COALESCE(m.active,1),COALESCE(" DC_CHAT_MODEL_ID ",''),(COALESCE(m.active,1) AND length(m.last_profile)=16),c.updated,(SELECT status FROM run_records WHERE chat=c.id ORDER BY started DESC,rowid DESC LIMIT 1) FROM chats c LEFT JOIN chat_properties m ON m.chat=c.id " DC_ACTIVE_RUN_JOIN " ORDER BY COALESCE(m.pinned,0) DESC,c.updated DESC,c.rowid DESC");
    if(!q)return NULL;
    J *a=json_object_new_array();
    int row_status;
    while((row_status=sqlite3_step(q))==SQLITE_ROW){
        J *o=json_object_new_object();
        const char *keys[]={
            "id","title","workspace"}
        ;
        for(int i=0;i<3;i++)json_object_object_add(o,keys[i],json_object_new_string((const char*)sqlite3_column_text(q,i)));
        json_object_object_add(o,"project",json_object_new_string((const char*)sqlite3_column_text(q,3)));
        json_object_object_add(o,"archived",json_object_new_boolean(sqlite3_column_int(q,4)));
        json_object_object_add(o,"pinned",json_object_new_boolean(sqlite3_column_int(q,5)));
        json_object_object_add(o,"active",json_object_new_boolean(sqlite3_column_int(q,6)));
        json_object_object_add(o,"model",json_object_new_string((const char*)sqlite3_column_text(q,7)));
        json_object_object_add(o,"unsupportedCheckpoint",json_object_new_boolean(sqlite3_column_int(q,8)));
        json_object_object_add(o,"updated",json_object_new_int64(sqlite3_column_int64(q,9)));
        if(sqlite3_column_type(q,10)!=SQLITE_NULL)json_object_object_add(o,"lastStatus",json_object_new_string((const char*)sqlite3_column_text(q,10)));
        J *binding=dc_chat_workspace_view(s,(const char*)sqlite3_column_text(q,0));
        if(!binding){json_object_put(o);json_object_put(a);sqlite3_finalize(q);return NULL;}
        const char *binding_fields[]={"device","inode","revision","available","availability","origin"};
        const char *view_fields[]={"workspaceDevice","workspaceInode","workspaceRevision","workspaceAvailable","workspaceAvailability","workspaceOrigin"};
        for(size_t k=0;k<sizeof binding_fields/sizeof *binding_fields;k++)json_object_object_add(o,view_fields[k],json_object_get(get(binding,binding_fields[k])));
        json_object_put(binding);
        json_object_array_add(a,o);
    }
    sqlite3_finalize(q);
    if(row_status!=SQLITE_DONE){json_object_put(a);return NULL;}
    char *r=copyjson(a);
    json_object_put(a);
    return r;
}
char *dc_messages_through(dc_store *s,const char *id,int64_t through){
    if(!s||!valid(id)||through<0)return NULL;
    J *j=state(s,id);
    if(!j){sqlite3_stmt *q=prepare(s,"SELECT state IS NULL FROM chats WHERE id=?");if(q)bind(q,1,id);int empty=q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_int(q,0);sqlite3_finalize(q);if(!empty)return NULL;}
    if(j&&(!json_object_is_type(j,json_type_object)||!json_object_is_type(get(j,"messages"),json_type_array))){json_object_put(j);return NULL;}
    J *a=json_object_new_array(),*m=get(j,"messages"),*names=json_object_new_object();
    if(!a||!names){json_object_put(a);json_object_put(names);json_object_put(j);return NULL;}
    char *r=NULL;
    sqlite3_stmt *model_query=prepare(s,"SELECT snapshot FROM run_records WHERE chat=? AND profile=? ORDER BY started DESC,rowid DESC LIMIT 1");
    for(size_t i=0;m&&i<json_object_array_length(m)&&(uint64_t)i<=(uint64_t)through;i++){
        J *old=json_object_array_get_idx(m,i);
        if(!json_object_is_type(old,json_type_object))goto finish;
        J *role_value=get(old,"role");
        if(!json_object_is_type(role_value,json_type_string))goto finish;
        const char *role=json_object_get_string(role_value);
        if(!*role||strlen(role)!=(size_t)json_object_get_string_len(role_value)||(strcmp(role,"user")&&strcmp(role,"assistant")&&strcmp(role,"tool")&&strcmp(role,"system")))goto finish;
        J *o=json_object_new_object();if(!o)goto finish;
        char *reasoning=NULL;const char *kind=NULL;
        int projected=dl_message_reasoning(old,&reasoning,&kind);
        J *c=get(old,"content");
        if(!json_object_is_type(c,json_type_string)&&!(!strcmp(role,"user")&&json_object_is_type(c,json_type_array))&&!(!strcmp(role,"assistant")&&!c)){
            dl_message_projection_free(reasoning);json_object_put(o);goto finish;
        }
        if(role&&!strcmp(role,"user")&&json_object_is_type(get(old,"_dunlin_display"),json_type_string))c=get(old,"_dunlin_display");
        if(json_object_is_type(c,json_type_string)&&strlen(json_object_get_string(c))!=(size_t)json_object_get_string_len(c)){
            dl_message_projection_free(reasoning);json_object_put(o);goto finish;
        }
        int empty_reasoning=role&&!strcmp(role,"assistant")&&(!c||json_object_is_type(c,json_type_null))&&projected!=0;
        if(!role||(!json_object_is_type(c,json_type_string)&&!empty_reasoning)){
            dl_message_projection_free(reasoning);
            json_object_put(o);
            continue;
        }
        char source_hash[65];const char *display=empty_reasoning?"":json_object_get_string(c);dc_sha256(display,strlen(display),source_hash);
        json_object_object_add(o,"sourceHash",json_object_new_string(source_hash));
        json_object_object_add(o,"sequence",json_object_new_int64((int64_t)i));
        if(get(old,"_dunlin_media"))json_object_object_add(o,"media",json_object_get(get(old,"_dunlin_media")));
        if(get(old,"_dunlin_observation"))json_object_object_add(o,"observation",json_object_get(get(old,"_dunlin_observation")));
        json_object_object_add(o,"role",json_object_new_string(get(old,"_dunlin_reference")?"reference":role));
        if(get(old,"_dunlin_reference"))json_object_object_add(o,"reference",json_object_get(get(old,"_dunlin_reference")));
        json_object_object_add(o,"content",empty_reasoning?json_object_new_string(""):json_object_get(c));
        if(get(old,"_dunlin_attachments")){
            J *attachment=dc_attachment_message_view(s,id,(int64_t)i,old);
            if(!attachment){dl_message_projection_free(reasoning);json_object_put(o);goto finish;}
            json_object_object_add(o,"attachment",attachment);
        }
        /* Historical labels come from the admitted configuration, never the
         * currently selected model. Older unlabelled history stays unlabelled. */
        J *origin_value=get(old,"_dunlin_profile");
        const char *origin=json_object_is_type(origin_value,json_type_string)?json_object_get_string(origin_value):NULL;
        if(!strcmp(role,"assistant")&&origin&&strlen(origin)==(size_t)json_object_get_string_len(origin_value)&&(strlen(origin)==16||strlen(origin)==64)&&strspn(origin,"0123456789abcdef")==strlen(origin)){
            J *label=get(names,origin);
            if(!label){
                const char *name="";J *snapshot=NULL;
                if(model_query){
                    sqlite3_reset(model_query);sqlite3_clear_bindings(model_query);bind(model_query,1,id);bind(model_query,2,origin);
                    if(sqlite3_step(model_query)==SQLITE_ROW){
                        const char *raw=(const char*)sqlite3_column_text(model_query,0);int bytes=sqlite3_column_bytes(model_query,0);
                        if(raw&&bytes>0&&bytes<=4194304&&strlen(raw)==(size_t)bytes)snapshot=dunlin_json_parse(raw,(size_t)bytes);
                        J *value=json_object_is_type(snapshot,json_type_object)?get(snapshot,"model"):NULL;const char *v=json_object_is_type(value,json_type_string)?json_object_get_string(value):NULL;
                        if(v&&*v&&strlen(v)<=512&&strlen(v)==(size_t)json_object_get_string_len(value))name=v;
                    }
                }
                label=json_object_new_string(name);json_object_put(snapshot);
                if(!label||json_object_object_add(names,origin,label)){
                    json_object_put(label);dl_message_projection_free(reasoning);json_object_put(o);goto finish;
                }
            }
            if(json_object_get_string_len(label))json_object_object_add(o,"modelName",json_object_get(label));
        }
        if(projected>0){json_object_object_add(o,"reasoning",json_object_new_string(reasoning));json_object_object_add(o,"reasoningKind",json_object_new_string(kind));}
        else if(projected<0)json_object_object_add(o,"reasoningUnavailable",json_object_new_boolean(1));
        dl_message_projection_free(reasoning);
        json_object_array_add(a,o);
    }
    r=copyjson(a);
finish:
    sqlite3_finalize(model_query);
    json_object_put(names);
    json_object_put(a);
    json_object_put(j);
    return r;
}
char *dc_messages(dc_store *s,const char *id){return dc_messages_through(s,id,INT64_MAX);}
static int delete_chat_unlocked(dc_store *s,const char *id,int preserve){
    if(!s||!valid(id)||dc_media_chat_busy(s,id)||dc_extensions_chat_busy(s,id))return 0;
    J *checkpoint=state(s,id);
    int unfinished=json_object_get_boolean(get(checkpoint,"active"))||get(checkpoint,"pending")!=NULL;
    if(!checkpoint){
        sqlite3_stmt *check=prepare(s,"SELECT state IS NOT NULL FROM chats WHERE id=?");
        if(!check)return 0;bind(check,1,id);int rc=sqlite3_step(check);if(rc==SQLITE_ROW&&sqlite3_column_int(check,0))unfinished=1;else if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)unfinished=1;sqlite3_finalize(check);
    }
    json_object_put(checkpoint);
    if(unfinished||!exec(s,"BEGIN IMMEDIATE"))return 0;
    if(preserve){
        sqlite3_stmt *keep=prepare(s,"UPDATE graph_nodes SET independent=1,revision=revision+1,updated=unixepoch() "
            "WHERE source_chat=? AND kind='memory' AND independent=0 AND scope!='chat' AND state!='deleted'");
        if(keep)bind(keep,1,id);
        int saved=keep&&sqlite3_step(keep)==SQLITE_DONE;
        sqlite3_finalize(keep);
        if(!saved){exec(s,"ROLLBACK");return 0;}
    }
    char *trace_key=dc_trajectory_chat_key(s,id);
    int derived_ok=dc_helpers_drop_chat(s,id)&&dc_media_drop_chat(s,id)&&dc_agents_forget_chat(s,id)&&dc_knowledge_drop_chat(s,id)&&dc_file_history_drop_chat(s,id)
        &&(!s->attachments||dl_attachments_delete_owner(s->attachments,DL_ATTACHMENT_CHAT,id)==DL_ATTACHMENT_OK);
    sqlite3_stmt *deletion=prepare(s,"INSERT OR IGNORE INTO index_deletions(chat) VALUES(?)");
    if(deletion)bind(deletion,1,id);
    derived_ok=derived_ok&&deletion&&sqlite3_step(deletion)==SQLITE_DONE;
    sqlite3_finalize(deletion);
    sqlite3_stmt *q=prepare(s,"DELETE FROM chats WHERE id=?");
    int ok=q!=NULL&&derived_ok;
    if(ok){
        bind(q,1,id);
        ok=sqlite3_step(q)==SQLITE_DONE;
    }
    sqlite3_finalize(q);
    if(s->attachments){q=prepare(s,"DELETE FROM chat_attachment_policy WHERE chat=?");if(q)bind(q,1,id);ok=ok&&q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);}
    q=prepare(s,"DELETE FROM extension_forks WHERE chat=?");
    if(q){bind(q,1,id);ok=ok&&sqlite3_step(q)==SQLITE_DONE;}else ok=0;sqlite3_finalize(q);
    q=prepare(s,"DELETE FROM chat_properties WHERE chat=?");
    if(q){bind(q,1,id);ok=ok&&sqlite3_step(q)==SQLITE_DONE;}else ok=0;
    sqlite3_finalize(q);
    q=prepare(s,"DELETE FROM chat_workspace_bindings WHERE chat=?");
    if(q){bind(q,1,id);ok=ok&&sqlite3_step(q)==SQLITE_DONE;}else ok=0;
    sqlite3_finalize(q);
    q=prepare(s,"DELETE FROM index_jobs WHERE chat=?");
    if(q){
        bind(q,1,id);
        ok=ok&&sqlite3_step(q)==SQLITE_DONE;
    }
    else ok=0;
    sqlite3_finalize(q);
    /* End local access atomically with chat removal. Preserve remote cleanup
     * records so the account can still be revoked from permission settings. */
    const char *cleanups[]={"UPDATE capability_leases SET state='expired' WHERE chat=? AND state='active'","DELETE FROM extension_client_sessions WHERE chat=?","DELETE FROM extension_agent_jobs WHERE chat=?","DELETE FROM extension_agent_scopes WHERE chat=?","DELETE FROM extension_agent_inputs WHERE chat=?","DELETE FROM extension_agent_sessions WHERE chat=?","DELETE FROM extension_terminal_owners WHERE chat=?","DELETE FROM grant_records WHERE chat=?","DELETE FROM run_records WHERE chat=?","DELETE FROM skill_bindings WHERE scope='chat' AND scope_id=?","DELETE FROM compact_sources WHERE chat=?","DELETE FROM compaction_records WHERE chat=?","DELETE FROM turn_preparation WHERE chat=?","DELETE FROM conversation_events WHERE chat=?","DELETE FROM trace_chat_epochs WHERE chat=?","DELETE FROM tool_record_schemas WHERE record IN(SELECT id FROM tool_records WHERE chat=?)","DELETE FROM tool_records WHERE chat=?","DELETE FROM tool_record_migrations WHERE chat=?"};
    for(size_t i=0;ok&&i<sizeof cleanups/sizeof *cleanups;i++){
        q=prepare(s,cleanups[i]);if(q){bind(q,1,id);ok=sqlite3_step(q)==SQLITE_DONE;}else ok=0;sqlite3_finalize(q);
    }
    if(!exec(s,ok?"COMMIT":"ROLLBACK")){exec(s,"ROLLBACK");ok=0;}
    if(ok){dc_extensions_forget_chat(s,id);dc_index_cleanup_deleted(s,8);if(trace_key)dl_trajectory_forget(s->trajectory,trace_key);dc_file_history_cleanup(s,8);}free(trace_key);
    return ok;
}
int dc_delete_chat_preserving_memories(dc_store *s,const char *id,int preserve){
    void *scope=dc_extensions_scope_enter(s);
    if(!scope)return 0;
    int out=delete_chat_unlocked(s,id,preserve);
    dc_extensions_scope_leave(scope);
    return out;
}
int dc_delete_chat(dc_store *s,const char *id){return dc_delete_chat_preserving_memories(s,id,0);}
static int retryable_input(J *state){
    size_t n=message_count(state);
    if(!n||get(state,"pending")||get(state,"sealed")||json_object_get_boolean(get(state,"active")))return 0;
    const char *role=json_object_get_string(get(json_object_array_get_idx(get(state,"messages"),n-1),"role"));
    int status=json_object_get_int(get(state,"status"));
    return role&&!strcmp(role,"user")&&(status==AH_FAILED||status==AH_BUDGET||status==AH_TIMEOUT||status==AH_CANCELLED);
}
static ah_status session_open(void *ctx,const char *id,void **out,J **value){
    lease *l=ctx;
    (void)id;
    J *j=state(l->store,l->id);
    l->original=j;
    if(!j){
        sqlite3_stmt *q=prepare(l->store,"SELECT state IS NOT NULL FROM chats WHERE id=?");
        if(!q)return AH_FAILED;
        bind(q,1,l->id);
        int row=sqlite3_step(q);
        int bad=row!=SQLITE_ROW||sqlite3_column_int(q,0);
        sqlite3_finalize(q);
        if(bad)return AH_FAILED;
    }
    if(l->retry_run){
        const char *last=json_object_get_string(get(j,"_dunlin_run"));
        if(!last||strcmp(last,l->retry_run)||!retryable_input(j))return AH_BUSY;
    }
    if(j){
        const char *p=json_object_get_string(get(j,"_dunlin_profile"));
        if(json_object_get_boolean(get(j,"active"))&&(!p||strcmp(p,l->profile)))return AH_BUSY;
        const char *run=json_object_get_string(get(j,"_dunlin_run"));
        if(json_object_get_boolean(get(j,"active"))&&valid(run))snprintf(l->run,sizeof l->run,"%s",run);
        *value=clone(j);
        if(!*value)return AH_FAILED;
        /* Reuse the committed question in the private request copy. A new run
         * records its own outcome. A stale selection never reopens history. */
        if(l->retry_run)json_object_object_add(*value,"active",json_object_new_boolean(1));
        J *m=get(*value,"messages");
        for(size_t i=0;m&&i<json_object_array_length(m);i++){
            J *v=json_object_array_get_idx(m,i);
            const char *origin=json_object_get_string(get(v,"_dunlin_profile"));
            if(!origin||strcmp(origin,l->profile)){
                json_object_object_del(v,"_dunlin_provider");
                json_object_object_del(v,"reasoning_content");
            }
            json_object_object_del(v,"_dunlin_display");
            json_object_object_del(v,"_dunlin_reference");
            json_object_object_del(v,"_dunlin_message_id");
            json_object_object_del(v,"_dunlin_time");
            json_object_object_del(v,"_dunlin_profile");
            json_object_object_del(v,"_dunlin_observation");
            json_object_object_del(v,"_dunlin_media");
            json_object_object_del(v,"_dunlin_attachments");
            json_object_object_del(v,"_dunlin_source_seq");
            if(l->media)json_object_object_add(v,"_dunlin_source_seq",json_object_new_int64((int64_t)i));
        }
    }
    else *value=NULL;
    l->trace=dc_trajectory_begin(l->store,l->id,l->run,l->trace_model?l->trace_model:"",l->trace_protocol?l->trace_protocol:"",(long long)message_count(j),l->trace_secret);
    const char *start_kind=l->retry_run?"retry":json_object_get_boolean(get(j,"active"))?"resume":"input";
    dl_trajectory_event(l->trace,"turn.start",start_kind,strlen(start_kind));
    *out=l;
    return AH_DONE;
}
static int session_save(void *ctx,J *value){
    lease *l=ctx;
    dc_store *s=l->store;
    J *saved=clone(value);
    if(!saved)return -1;
    J *messages=get(saved,"messages"),*old=get(l->original,"messages");
    size_t before=old?json_object_array_length(old):0,n=json_object_array_length(messages);
    int64_t input_sequence=-1;
    if(before>n){
        json_object_put(saved);
        return -1;
    }
    for(size_t i=0;i<n;i++){
        J *m=json_object_array_get_idx(messages,i);
        json_object_object_del(m,"_dunlin_source_seq");
        if(i<before){
            J *a=json_object_array_get_idx(old,i),*x=clone(a),*y=clone(m);
            if(!x||!y){json_object_put(x);json_object_put(y);json_object_put(saved);return -1;}
            json_object_object_del(x,"_dunlin_provider");
            json_object_object_del(y,"_dunlin_provider");
            json_object_object_del(x,"reasoning_content");
            json_object_object_del(y,"reasoning_content");
            json_object_object_del(x,"_dunlin_profile");
            json_object_object_del(y,"_dunlin_profile");
            json_object_object_del(x,"_dunlin_display");
            json_object_object_del(x,"_dunlin_reference");
            json_object_object_del(x,"_dunlin_message_id");
            json_object_object_del(y,"_dunlin_display");
            json_object_object_del(y,"_dunlin_reference");
            json_object_object_del(y,"_dunlin_message_id");
            json_object_object_del(x,"_dunlin_time");
            json_object_object_del(y,"_dunlin_time");
            json_object_object_del(x,"_dunlin_media");
            json_object_object_del(y,"_dunlin_media");
            json_object_object_del(x,"_dunlin_observation");
            json_object_object_del(y,"_dunlin_observation");
            json_object_object_del(x,"_dunlin_attachments");
            json_object_object_del(y,"_dunlin_attachments");
            int equal=json_object_equal(x,y);
            json_object_put(x);
            json_object_put(y);
            if(!equal){
                json_object_put(saved);
                return -1;
            }
            json_object_array_put_idx(messages,i,json_object_get(a));
        }
        else {
            json_object_object_del(m,"_dunlin_reference");
            json_object_object_del(m,"_dunlin_attachments");
            json_object_object_add(m,"_dunlin_profile",json_object_new_string(l->profile));
            json_object_object_add(m,"_dunlin_time",json_object_new_int64(source_time_ms()));
            const char *role=json_object_get_string(get(m,"role")),*content=json_object_get_string(get(m,"content"));
            if(l->display_input&&l->expanded_input&&role&&!strcmp(role,"user")&&content&&!strcmp(content,l->expanded_input))
                json_object_object_add(m,"_dunlin_display",json_object_new_string(l->display_input));
            if(l->input&&!l->input_committed&&input_sequence<0&&role&&!strcmp(role,"user")&&content
                &&!strcmp(content,l->expanded_input?l->expanded_input:l->input)){
                input_sequence=(int64_t)i;
                if(l->attachment.id[0]){J *parts=json_object_new_array();json_object_array_add(parts,dc_attachment_view(&l->attachment));json_object_object_add(m,"_dunlin_attachments",parts);}
            }
        }
    }
    if(input_sequence>=0&&!dc_agents_source_message(s,json_object_array_get_idx(messages,(size_t)input_sequence))){json_object_put(saved);return -1;}
    json_object_object_add(saved,"_dunlin_profile",json_object_new_string(l->profile));
    json_object_object_add(saved,"_dunlin_run",json_object_new_string(l->run));
    if(!dc_outcomes_save(saved,l->original,l->run)){json_object_put(saved);return -1;}
    char *text=copyjson(saved);
    int ok=text&&exec(s,"BEGIN IMMEDIATE");
    sqlite3_stmt *q=ok?prepare(s,"UPDATE chats SET state=?,updated=unixepoch() WHERE id=?"):NULL;
    if(q){
        bind(q,1,text);
        bind(q,2,l->id);
        ok=sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;
    }
    else ok=0;
    sqlite3_finalize(q);
    if(ok&&input_sequence>=0&&l->attachment.id[0]){
        char hash[65];ok=dc_attachment_message_hash(json_object_array_get_idx(messages,(size_t)input_sequence),hash)
            &&dl_attachments_bind(s->attachments,DL_ATTACHMENT_CHAT,l->id,l->attachment.id,input_sequence,hash)==DL_ATTACHMENT_OK;
    }
    if(ok)ok=dc_agents_commit_output(s,messages,before);
    if(ok&&input_sequence>=0){
        ok=dc_agents_commit_input(s,l->id,input_sequence,json_object_array_get_idx(messages,(size_t)input_sequence));
    }
    if(ok&&input_sequence>=0){
        /* The host persists the submitted draft before admission. Clear only
         * that exact value atomically with the user message, not on UI click
         * or on a failed HTTP response after it has already been committed. */
        q=prepare(s,"UPDATE chat_properties SET draft='' WHERE chat=? AND draft=?");
        if(q){bind(q,1,l->id);bind(q,2,l->input);}ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);
    }
    if(ok&&l->snapshot){
        q=prepare(s,"INSERT OR IGNORE INTO run_records(id,chat,profile,snapshot) VALUES(?,?,?,?)");
        if(q){bind(q,1,l->run);bind(q,2,l->id);bind(q,3,l->profile);bind(q,4,l->snapshot);ok=sqlite3_step(q)==SQLITE_DONE;}else ok=0;
        sqlite3_finalize(q);
    }
    free(text);
    for(size_t i=before;ok&&i<n;i++){
        J *m=json_object_array_get_idx(messages,i),*body=get(m,"content");
        if(get(m,"_dunlin_display"))body=get(m,"_dunlin_display");
        if(!json_object_is_type(body,json_type_string))continue;
        const char *t=json_object_get_string(body);
        if(strlen(t)>262144)continue;
        q=prepare(s,"INSERT OR REPLACE INTO index_jobs VALUES(?,?,?)");
        if(!q){
            ok=0;
            break;
        }
        bind(q,1,l->id);
        sqlite3_bind_int(q,2,(int)i);
        bind(q,3,t);
        ok=sqlite3_step(q)==SQLITE_DONE;
        sqlite3_finalize(q);
    }
    if(ok)ok=dc_tool_records_append(s,l->id,l->run,saved,before,l->tool_records);
    if(!exec(s,ok?"COMMIT":"ROLLBACK")){
        exec(s,"ROLLBACK");
        ok=0;
    }
    if(ok){
        if(input_sequence>=0)l->input_committed=1;
        if(l->media){
            J *runtime_messages=get(value,"messages");
            for(size_t i=0;i<json_object_array_length(runtime_messages);i++){
                J *m=json_object_array_get_idx(runtime_messages,i);
                json_object_object_del(m,"_dunlin_attachments");
                json_object_object_add(m,"_dunlin_source_seq",json_object_new_int64((int64_t)i));
            }
        }
        if(l->trace)for(size_t i=before;i<n;i++){
            J *m=json_object_array_get_idx(messages,i),*v=json_object_new_object();
            const char *fields[]={"role","content","tool_calls","tool_call_id"};
            json_object_object_add(v,"sequence",json_object_new_int64((int64_t)i));
            for(size_t k=0;k<sizeof fields/sizeof *fields;k++){J *p=get(m,fields[k]);if(k==1&&get(m,"_dunlin_display"))p=get(m,"_dunlin_display");if(p)json_object_object_add(v,fields[k],json_object_get(p));}
            J *content=get(v,"content");if(json_object_is_type(content,json_type_string)){char hash[65];dc_sha256(json_object_get_string(content),(size_t)json_object_get_string_len(content),hash);json_object_object_add(v,"sourceHash",json_object_new_string(hash));}
            const char *raw=json_object_to_json_string_ext(v,JSON_C_TO_STRING_PLAIN);dl_trajectory_event(l->trace,"source.committed",raw,strlen(raw));json_object_put(v);
        }
        json_object_put(l->original);
        l->original=saved;
        /* Only successful durable appends notify readers. The cursor is the
         * canonical sequence, including hidden tool proposals. A later WAL read
         * can restrict itself to this committed prefix without future deltas. */
        if(input_sequence>=0)dc_agents_input_committed(s);
        if(n>before&&l->hooks&&l->hooks->event){
            J *notice=json_object_new_object();
            J *last=json_object_array_get_idx(messages,n-1);
            const char *role=json_object_get_string(get(last,"role"));
            json_object_object_add(notice,"chat",json_object_new_string(l->id));
            json_object_object_add(notice,"run",json_object_new_string(l->run));
            json_object_object_add(notice,"through",json_object_new_int64((int64_t)n-1));
            json_object_object_add(notice,"settlesModel",json_object_new_boolean(role&&!strcmp(role,"assistant")));
            l->hooks->event(l->hooks->user,"chat.commit",json_object_to_json_string_ext(notice,JSON_C_TO_STRING_PLAIN));
            json_object_put(notice);
        }
    }
    else json_object_put(saved);
    return ok?0:-1;
}
static void session_close(void *ctx){
    (void)ctx;
}
typedef struct {const dc_hooks *hooks;uint64_t approval_ms;dc_store *store;const char *chat;int grant_error;lease *session;dc_file_history_run *history;long long last_grant;char *approved_arguments;} dc_observer;
static uint64_t desktop_now(void *ctx){dc_observer *o=ctx;uint64_t now=ah_linux_now_ms(NULL);return now>=o->approval_ms?now-o->approval_ms:0;}
static int approval(void *ctx,const char *name,const char *args){
    dc_observer *o=ctx;const dc_hooks *h=o->hooks;
    long long grant=dc_grant_begin(o->store,o->chat,name,args);
    if(!grant){o->grant_error=1;return 0;}
    if(!dc_tool_records_approval(o->session?o->session->tool_records:NULL,grant,-1,0)){o->grant_error=1;return 0;}
    J *review=json_object_new_object();json_object_object_add(review,"grant",json_object_new_int64(grant));json_object_object_add(review,"tool",json_object_new_string(name));json_object_object_add(review,"arguments",json_object_new_string(args));
    const char *raw=json_object_to_json_string_ext(review,JSON_C_TO_STRING_PLAIN);dl_trajectory_event(o->session?o->session->trace:NULL,"approval.request",raw,strlen(raw));json_object_put(review);
    uint64_t start=ah_linux_now_ms(NULL);
    int result=h&&h->approve?h->approve(h->user,name,args):0;
    uint64_t elapsed=ah_linux_now_ms(NULL)-start;o->approval_ms+=elapsed;
    int committed=dc_grant_decide(o->store,grant,result,elapsed);
    committed=committed&&dc_tool_records_approval(o->session?o->session->tool_records:NULL,grant,result!=0,elapsed);
    char decision[256];int n=snprintf(decision,sizeof decision,"{\"grant\":%lld,\"allowed\":%s,\"wait_ms\":%llu,\"committed\":%s}",grant,result?"true":"false",(unsigned long long)elapsed,committed?"true":"false");
    if(n>0&&(size_t)n<sizeof decision)dl_trajectory_event(o->session?o->session->trace:NULL,"approval.decision",decision,(size_t)n);
    if(!committed){o->grant_error=1;return 0;}
    o->last_grant=result?grant:0;
    free(o->approved_arguments);o->approved_arguments=result?strdup(args):NULL;
    return result;
}
static int cancelled(void *ctx){
    dc_observer *o=ctx;const dc_hooks *h=o->hooks;
    return o->grant_error||(h&&h->cancelled?h->cancelled(h->user):0);
}
static void event(void *ctx,const char *name,const char *text,size_t n){
    dc_observer *o=ctx;const dc_hooks *h=o->hooks;
    if(o->session){
        const char *run=o->session->run;
        if(!strcmp(name,"model.request"))dl_live_stream_begin(o->store->live_stream,o->chat,run,(long long)message_count(o->session->original));
        else if(!strcmp(name,"delta"))dl_live_stream_append(o->store->live_stream,o->chat,run,text,n);
        else if(!strcmp(name,"model.request.end"))dl_live_stream_end(o->store->live_stream,o->chat,run);
    }
    if(o->last_grant&&(!strcmp(name,"tool.post_execute")||!strcmp(name,"tool.outcome_unknown"))){
        if(!dc_grant_end(o->store,o->last_grant))o->grant_error=2;
        o->last_grant=0;
    }
    dc_tool_records_event(o->session?o->session->tool_records:NULL,name,text,n);
    if(o->history&&!strcmp(name,"tool.pre_execute")){
        dc_file_history_run *history=o->history;history->call[0]=0;history->proposal=-1;history->grant=o->last_grant;history->authorized_arguments=o->approved_arguments;
        if(n&&n<sizeof history->call&&!memchr(text,0,n)){memcpy(history->call,text,n);history->call[n]=0;}
        J *messages=o->session?get(o->session->original,"messages"):NULL;
        if(json_object_is_type(messages,json_type_array))for(size_t i=json_object_array_length(messages);i>0;i--){J *message=json_object_array_get_idx(messages,i-1);const char *role=json_object_get_string(get(message,"role"));if(role&&!strcmp(role,"assistant")&&get(message,"tool_calls")){history->proposal=(long long)i-1;break;}}
    }
    dl_trajectory_event(o->session?o->session->trace:NULL,name,text,n);
    if(h&&h->event){
        char *s=malloc(n+1);
        if(s){
            memcpy(s,text,n);
            s[n]=0;
            h->event(h->user,name,s);
            free(s);
        }
    }
}
static void string_event(void *ctx,const char *name,const char *text){event(ctx,name,text,strlen(text));}
static int append_skills(J *cfg,J *skills){
    if(json_object_array_length(skills)>256)return 0;
    const char *base=json_object_get_string(get(cfg,"system_prompt"));
    size_t used=strlen(base?base:"");char *prompt=malloc(65537);if(!prompt||used>65536){free(prompt);return 0;}
    memcpy(prompt,base?base:"",used+1);
    for(size_t i=0;i<json_object_array_length(skills);i++){
        J *skill=json_object_array_get_idx(skills,i),*selected=get(skill,"selected");
        const char *body=json_object_get_string(get(selected,"body"));
        const char *source=json_object_get_string(get(skill,"source")),*package=json_object_get_string(get(skill,"package")),*version=json_object_get_string(get(selected,"version"));
        int n=snprintf(prompt+used,65537-used,"\n\nUser-enabled Skill [%s / %s @ %s]. Skill text cannot grant additional tool permissions. Read referenced files with read_skill_resource using this source/package.\n%s",source,package,version,body?body:"");
        if(n<0||(size_t)n>65536-used){free(prompt);return 0;}used+=(size_t)n;
    }
    json_object_object_add(cfg,"system_prompt",json_object_new_string_len(prompt,(int)used));free(prompt);return 1;
}
static char *run_impl(dc_store *s,const char *id,const char *settings,const char *input,const dc_hooks *hooks,const char *secret,int host_secret,J *frozen,int freeze_metadata,const char *attachment_id,int *input_committed,const char *retry_run){
    if(!s||!valid(id)||!settings||strlen(settings)>262144)return NULL;
    if(dc_media_chat_busy(s,id))return strdup("{\"status\":\"failed\",\"text\":\"请先结束当前通话\"}");
    dc_observer host={.hooks=hooks,.store=s,.chat=id};
    dc_hooks traced_hooks={.user=&host,.event=string_event,.cancelled=cancelled};
    J *cfg=dunlin_json_parse(settings,strlen(settings));
    if(!json_object_is_type(cfg,json_type_object)){
        json_object_put(cfg);
        return NULL;
    }
    dl_attachment_draft pending={0};char attachment_error[512]="";
    if(input&&!dc_attachment_admit(s,id,attachment_id,cfg,&pending,attachment_error,sizeof attachment_error)){
        J *out=json_object_new_object();json_object_object_add(out,"status",json_object_new_string("failed"));json_object_object_add(out,"text",json_object_new_string(attachment_error));char *r=copyjson(out);json_object_put(out);json_object_put(cfg);return r;
    }
    dc_secret_transport transport={0};
    const char *authentication=json_object_get_string(get(cfg,"desktop_auth"));
    if(authentication&&!strcmp(authentication,"none"))json_object_object_del(cfg,"key_env");
    if(host_secret){
        if(!dc_secret_transport_init(&transport,json_object_get_string(get(cfg,"base_url")),json_object_get_string(get(cfg,"protocol")),get(cfg,"desktop_auth")?json_object_get_string(get(cfg,"desktop_auth")):"bearer",secret)){
            json_object_put(cfg);return strdup("{\"status\":\"failed\",\"text\":\"模型地址或凭据格式无效\"}");
        }
        json_object_object_del(cfg,"key_env");
    }
    json_object_object_del(cfg,"desktop_auth");
    J *profile_reference=get(cfg,"desktop_profile_id"),*credential_revision=get(cfg,"desktop_credential_revision");
    const char *route_keys[]={"desktop_provider_id","desktop_credential_id"};
    for(size_t i=0;i<sizeof route_keys/sizeof *route_keys;i++){J *v=get(cfg,route_keys[i]);if(v&&(!json_object_is_type(v,json_type_string)||!valid(json_object_get_string(v)))){json_object_put(cfg);return strdup("{\"status\":\"failed\",\"text\":\"无效的服务商或凭据引用\"}");}}
    if((profile_reference&&(!json_object_is_type(profile_reference,json_type_string)||!valid(json_object_get_string(profile_reference))))||
       (credential_revision&&(!json_object_is_type(credential_revision,json_type_int)||json_object_get_int64(credential_revision)<0))){
        json_object_put(cfg);return strdup("{\"status\":\"failed\",\"text\":\"无效的模型或凭据版本引用\"}");
    }
    sqlite3_stmt *q=prepare(s,"SELECT workspace FROM chats WHERE id=?");
    if(!q){
        json_object_put(cfg);
        return NULL;
    }
    bind(q,1,id);
    char *workspace=sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;
    sqlite3_finalize(q);
    if(!workspace){
        json_object_put(cfg);
        return NULL;
    }
    J *workspace_binding=dc_chat_workspace_view(s,id);
    const char *binding_project=workspace_binding?json_object_get_string(get(workspace_binding,"project")):NULL;
    int needs_project=binding_project&&*binding_project;
    int workspace_available=json_object_get_boolean(get(workspace_binding,"available"));
    char workspace_device[32]="",workspace_inode[32]="";
    if(workspace_available){snprintf(workspace_device,sizeof workspace_device,"%s",json_object_get_string(get(workspace_binding,"device")));snprintf(workspace_inode,sizeof workspace_inode,"%s",json_object_get_string(get(workspace_binding,"inode")));}
    J *project=dc_chat_project(s,id);
    if(!workspace_binding||(needs_project&&(!project||!json_object_get_boolean(get(project,"available"))))){
        json_object_put(workspace_binding);
        json_object_put(project);json_object_put(cfg);free(workspace);
        return strdup("{\"status\":\"failed\",\"text\":\"此聊天工作区不可用或目录身份已变化，请单独重新关联。已保存的聊天保留。\"}");
    }
    J *workspace_snapshot=json_object_new_object();
    if(!workspace_snapshot){json_object_put(workspace_binding);json_object_put(project);json_object_put(cfg);free(workspace);return NULL;}
    const char *workspace_fields[]={"path","device","inode","revision"};
    for(size_t k=0;k<sizeof workspace_fields/sizeof *workspace_fields;k++)json_object_object_add(workspace_snapshot,workspace_fields[k],json_object_get(get(workspace_binding,workspace_fields[k])));
    json_object_object_add(workspace_snapshot,"version",json_object_new_int(1));
    json_object_put(workspace_binding);
    J *prior_checkpoint=state(s,id);int prior_active=json_object_get_boolean(get(prior_checkpoint,"active")),media=pending.id[0]!=0,history_images=0;
    const char *prior_profile=json_object_get_string(get(prior_checkpoint,"_dunlin_profile"));
    if(prior_active&&prior_profile&&strlen(prior_profile)==16){
        json_object_put(prior_checkpoint);json_object_put(workspace_snapshot);json_object_put(project);json_object_put(cfg);free(workspace);
        return strdup("{\"status\":\"failed\",\"text\":\"此回合使用不再支持的存档格式，未发送请求。请新建聊天；原记录可查看或删除。\"}");
    }
    J *prior_messages=get(prior_checkpoint,"messages");
    for(size_t i=0;json_object_is_type(prior_messages,json_type_array)&&i<json_object_array_length(prior_messages);i++){
        J *parts=get(json_object_array_get_idx(prior_messages,i),"_dunlin_attachments");if(parts){media=1;J *part=json_object_is_type(parts,json_type_array)?json_object_array_get_idx(parts,0):NULL;const char *format=json_object_get_string(get(part,"format"));if(format&&!strcmp(format,"png"))history_images=1;}
    }
    json_object_put(prior_checkpoint);
    int images=media?dc_attachment_policy(s,id):1;
    if(images<0||(media&&images&&(history_images||pending.content.format==DL_ATTACHMENT_PNG)&&!dc_attachment_model_images(cfg))){
        json_object_put(workspace_snapshot);json_object_put(project);json_object_put(cfg);free(workspace);
        return strdup("{\"status\":\"failed\",\"text\":\"此聊天保留图片，但当前模型未配置可用图片输入。请在模型切换时明确选择仅传文字，或选择图片模型。原图和聊天保留。\"}");
    }
    if(media){
        J *marker=json_object_new_object();json_object_object_add(marker,"version",json_object_new_int(1));json_object_object_add(marker,"images",json_object_new_boolean(images));
        J *old_marker=get(frozen,"_media_projection");
        if(prior_active&&(!old_marker||!json_object_equal(marker,old_marker))){json_object_put(marker);json_object_put(workspace_snapshot);json_object_put(project);json_object_put(cfg);free(workspace);return strdup("{\"status\":\"busy\",\"text\":\"未完成回合的附件投影已变化，未发送请求。\"}");}
        json_object_object_add(cfg,"_media_projection",marker);
    }
    J *frozen_workspace=get(frozen,"_workspace_binding");
    if(prior_active&&frozen_workspace&&!json_object_equal(frozen_workspace,workspace_snapshot)){
        json_object_put(workspace_snapshot);json_object_put(project);json_object_put(cfg);free(workspace);
        return strdup("{\"status\":\"busy\",\"text\":\"未完成回合的工作区绑定已变化，未发送请求。\"}");
    }
    /* Prior-image active snapshots did not include this component. Preserve
     * their exact admitted fingerprint; new turns freeze the captured root. */
    if(!prior_active||frozen_workspace)json_object_object_add(cfg,"_workspace_binding",workspace_snapshot);
    else{json_object_put(workspace_snapshot);json_object_object_del(cfg,"_workspace_binding");}
    const char *project_policy=project?json_object_get_string(get(project,"policy")):NULL;
    const char *raw_mode=json_object_get_string(get(cfg,"desktop_tools"));
    char saved_mode[32];snprintf(saved_mode,sizeof saved_mode,"%s",raw_mode?raw_mode:"noTools");
    const char *mode=raw_mode?saved_mode:NULL;
    J *frozen_skills=get(frozen,"_frozen_skills");
    J *all_skills=frozen_skills?(dc_skills_snapshot_allowed(s,frozen_skills)?clone(frozen_skills):NULL):dc_skills_resolve(s,project?json_object_get_string(get(project,"id")):NULL,id);
    if(!all_skills){json_object_put(project);json_object_put(cfg);free(workspace);return strdup("{\"status\":\"failed\",\"text\":\"无法解析 Skills，未向模型发送请求。\"}");}
    J *enabled_skills=json_object_new_array();
    for(size_t i=0;i<json_object_array_length(all_skills);i++){
        J *v=json_object_array_get_idx(all_skills,i);if(json_object_get_boolean(get(v,"enabled")))json_object_array_add(enabled_skills,json_object_get(v));
    }
    json_object_put(all_skills);
    ah_tool_service file_tools={0};
    /* The optional adapter is isolated from the CLI's compatibility tool set. */
    int use_adapter=project||mode||json_object_array_length(enabled_skills)||get(cfg,"desktop_knowledge")||media;
    int adapter_ok=!use_adapter||dc_project_tools_open(&file_tools,workspace,project_policy?project_policy:mode?mode:"noTools");
    int delegation_allowed=use_adapter&&strcmp(project_policy?project_policy:mode?mode:"noTools","noTools");
    if(adapter_ok&&use_adapter&&(!workspace_available||!dc_project_tools_identity(&file_tools,workspace_device,workspace_inode))){dc_project_tools_close(&file_tools);adapter_ok=0;}
    json_object_object_del(cfg,"desktop_tools");
    if(!adapter_ok){json_object_put(enabled_skills);json_object_put(project);json_object_put(cfg);free(workspace);return strdup("{\"status\":\"failed\",\"text\":\"无法打开项目工具服务\"}");}
    if(project){
        const char *instructions=json_object_get_string(get(project,"instructions"));
        const char *system=json_object_get_string(get(cfg,"system_prompt"));
        if(instructions&&*instructions){
            size_t n=strlen(system?system:"")+strlen(instructions)+64;char *prompt=malloc(n);
            if(!prompt){json_object_put(enabled_skills);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);free(workspace);return NULL;}
            snprintf(prompt,n,"%s\n\nProject instructions:\n%s",system?system:"",instructions);
            json_object_object_add(cfg,"system_prompt",json_object_new_string(prompt));free(prompt);
        }
    }
    if(json_object_array_length(enabled_skills)&&!append_skills(cfg,enabled_skills)){
        json_object_put(enabled_skills);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);free(workspace);return strdup("{\"status\":\"budget_exhausted\",\"text\":\"启用的 Skills 内容超过当前配置上限，请关闭部分项目或对话 Skills。\"}");
    }
    if(use_adapter)dc_project_tools_skills(&file_tools,s->directory,enabled_skills);
    if(freeze_metadata){
        J *leased=clone(enabled_skills);
        if(leased&&!frozen)for(size_t i=0;i<json_object_array_length(leased);i++){J *v=json_object_array_get_idx(leased,i);json_object_object_del(v,"versions");json_object_object_del(v,"scopeMode");}
        J *public_copy=dunlin_json_parse(settings,strlen(settings));
        J *raw_copy=json_object_new_string(settings);
        if(!leased||!public_copy||!raw_copy){json_object_put(leased);json_object_put(public_copy);json_object_put(raw_copy);json_object_put(enabled_skills);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);free(workspace);return NULL;}
        json_object_object_add(cfg,"_frozen_skills",leased);
        json_object_object_add(cfg,"_public_settings",public_copy);
        json_object_object_add(cfg,"_public_settings_raw",raw_copy);
        json_object_object_add(cfg,"_tool_policy",json_object_new_string(project_policy?project_policy:mode?mode:"noTools"));
    }
    json_object_put(enabled_skills);
    char *expanded=NULL;
    const char *core_input=input&&!*input&&pending.id[0]?"请查看附件。":input;
    J *selected_context=NULL;int explicit_context=json_object_object_get_ex(cfg,"desktop_project_context",&selected_context);
    if(input&&(explicit_context||project)){
        const char *context_error=NULL;J *documents=dc_project_references(project,selected_context,!explicit_context,&context_error);
        if(documents&&json_object_array_length(documents))expanded=dc_project_reference_input(core_input,documents);
        if(!documents||(json_object_array_length(documents)&&!expanded)){
            J *out=json_object_new_object();json_object_object_add(out,"status",json_object_new_string("failed"));json_object_object_add(out,"text",json_object_new_string(context_error?context_error:"所选固定资料总量超限，请减少本轮选择。"));char *result=copyjson(out);json_object_put(out);json_object_put(documents);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);free(workspace);return result;
        }json_object_put(documents);
    }
    if(core_input!=input&&!expanded){expanded=strdup(core_input);if(!expanded){dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);free(workspace);return NULL;}}
    json_object_object_del(cfg,"desktop_project_context");
    if(expanded||(project&&json_object_array_length(get(project,"context")))){
        json_object_object_add(cfg,"max_message_bytes",json_object_new_int(131072));
        json_object_object_add(cfg,"max_context_bytes",json_object_new_int(1048576));
    }
    J *knowledge_options=get(cfg,"desktop_knowledge");
    if(knowledge_options){
        const char *knowledge_error=NULL;J *snapshot=get(frozen,"_knowledge_snapshot");
        if(snapshot){if(!dc_knowledge_snapshot_valid(s,snapshot))knowledge_error="引用的记忆或知识来源已更改，未发送恢复请求。";else snapshot=clone(snapshot);}
        else snapshot=dc_knowledge_snapshot(s,id,input,knowledge_options,&knowledge_error);
        if(knowledge_error||!snapshot){if(!knowledge_error)knowledge_error="无法冻结知识引用";free(expanded);free(workspace);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);J *out=json_object_new_object();json_object_object_add(out,"status",json_object_new_string("failed"));json_object_object_add(out,"text",json_object_new_string(knowledge_error));char *result=copyjson(out);json_object_put(out);return result;}
        json_object_object_add(cfg,"_knowledge_snapshot",snapshot);
    }
    /* Freeze complete package/config identities with the admitted model profile.
       Old pending turns without this field retain their original tool set. */
    J *extension_snapshot=NULL;
    int plugin_tools_allowed=use_adapter&&!strcmp(project_policy?project_policy:mode?mode:"noTools","ask")&&(!prior_active||get(frozen,"_extension_catalog"));
    if(plugin_tools_allowed){
        extension_snapshot=dc_extension_tools_snapshot(s);
        J *prior=get(frozen,"_extension_catalog");
        int changed=prior&&extension_snapshot&&strcmp(json_object_to_json_string_ext(prior,JSON_C_TO_STRING_PLAIN),json_object_to_json_string_ext(extension_snapshot,JSON_C_TO_STRING_PLAIN));
        if(!extension_snapshot||changed){json_object_put(extension_snapshot);free(expanded);free(workspace);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);return strdup("{\"status\":\"busy\",\"text\":\"插件工具版本或配置已变化，未执行待处理工具。请先结束原回合。\"}");}
        json_object_object_add(cfg,"_extension_catalog",extension_snapshot);
    }
    /* Runtime context is produced by the optional DSH composition, frozen
     * before any model request. Resumption must retain the same provenance. */
    if(!prior_active||get(frozen,"_extension_context")){
        J *snapshot=dc_extension_context_snapshot(s,id),*prior=get(frozen,"_extension_context");
        int changed=prior&&snapshot&&strcmp(json_object_to_json_string_ext(prior,JSON_C_TO_STRING_PLAIN),json_object_to_json_string_ext(snapshot,JSON_C_TO_STRING_PLAIN));
        if(!snapshot||changed){json_object_put(snapshot);free(expanded);free(workspace);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);return strdup("{\"status\":\"busy\",\"text\":\"插件上下文或来源已变化，未发送模型请求。请先结束原回合。\"}");}
        const char *text=json_object_get_string(get(snapshot,"text"));
        if(text&&*text&&json_object_get_int64(get(cfg,"max_message_bytes"))<262144)json_object_object_add(cfg,"max_message_bytes",json_object_new_int(262144));
        if(text&&*text&&input){const char *original=expanded?expanded:core_input;size_t n=strlen(original)+strlen(text)+128;char *merged=malloc(n);if(!merged){json_object_put(snapshot);free(expanded);free(workspace);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);return NULL;}snprintf(merged,n,"%s\n\n<dunlin-extension-context>\n%s\n</dunlin-extension-context>",original,text);free(expanded);expanded=merged;}
        if(prior||json_object_array_length(get(snapshot,"owners")))json_object_object_add(cfg,"_extension_context",snapshot);else json_object_put(snapshot);
    }
    char *profile_config=NULL;
    if(use_adapter){
        J *identity=json_object_new_object();
        json_object_object_add(identity,"public_settings",json_tokener_parse(settings));
        json_object_object_add(identity,"effective",clone(cfg));
        profile_config=copyjson(identity);json_object_put(identity);
    }
    J *context_settings=get(cfg,"desktop_context");
    char context_error[512]={0};
    dc_compactor *compactor=context_settings?dc_compactor_open(s,id,context_settings,&traced_hooks,context_error,sizeof context_error):NULL;
    if(context_settings&&!compactor){
        free(profile_config);free(expanded);free(workspace);dc_project_tools_close(&file_tools);json_object_put(project);json_object_put(cfg);
        J *out=json_object_new_object();json_object_object_add(out,"status",json_object_new_string("failed"));json_object_object_add(out,"text",json_object_new_string(*context_error?context_error:"无法打开上下文整理模块"));char *raw=copyjson(out);json_object_put(out);return raw;
    }
    if(compactor){
        json_object_object_add(cfg,"max_context_bytes",json_object_new_int(4194304));
        json_object_object_add(cfg,"max_message_bytes",json_object_new_int(1048576));
        json_object_object_add(cfg,"max_messages",json_object_new_int(4096));
    }
    if(media){json_object_object_add(cfg,"max_context_bytes",json_object_new_int(4194304));json_object_object_add(cfg,"max_message_bytes",json_object_new_int(1048576));json_object_object_add(cfg,"max_messages",json_object_new_int(4096));}
    J *context_audit=context_settings?clone(context_settings):NULL;
    json_object_object_del(cfg,"desktop_context");
    json_object_put(project);
    char tmp[4096],sessions[4096];
    snprintf(sessions,sizeof sessions,"%s/recovery",s->directory);
    json_object_object_add(cfg,"workspace",json_object_new_string(workspace));
    free(workspace);
    json_object_object_add(cfg,"session_dir",json_object_new_string(sessions));
    const char *tools[]={
        "read_file","write_file","exec","shell"}
    ;
    J *a=json_object_new_array();
    for(int i=0;i<4;i++)json_object_array_add(a,json_object_new_string(tools[i]));
    json_object_object_add(cfg,"enabled_tools",a);
    snprintf(tmp,sizeof tmp,"%s/config-XXXXXX",s->directory);
    int fd=mkstemp(tmp);
    J *audit=clone(cfg);
    if(audit&&context_audit)json_object_object_add(audit,"_context_policy",context_audit);else json_object_put(context_audit);
    json_object_object_del(cfg,"desktop_knowledge");
    json_object_object_del(cfg,"_knowledge_snapshot");
    json_object_object_del(cfg,"_frozen_skills");
    json_object_object_del(cfg,"_extension_catalog");
    json_object_object_del(cfg,"_extension_context");
    json_object_object_del(cfg,"_public_settings");
    json_object_object_del(cfg,"_public_settings_raw");
    json_object_object_del(cfg,"_tool_policy");
    json_object_object_del(cfg,"_workspace_binding");
    json_object_object_del(cfg,"_media_projection");
    json_object_object_del(cfg,"desktop_input_modalities");
    json_object_object_del(cfg,"desktop_profile_id");
    json_object_object_del(cfg,"desktop_credential_revision");
    json_object_object_del(cfg,"desktop_credential_id");
    json_object_object_del(cfg,"desktop_provider_id");
    char *data=copyjson(cfg);
    char *run_snapshot=copyjson(audit);json_object_put(audit);
    json_object_put(cfg);
    if(fd<0||!data||!run_snapshot||(run_snapshot&&strlen(run_snapshot)>4194304)){
        if(fd>=0){
            close(fd);
            unlink(tmp);
        }
        free(data);
        free(run_snapshot);
        free(profile_config);dc_project_tools_close(&file_tools);
        free(expanded);
        dc_compactor_close(compactor);
        return NULL;
    }
    size_t size=strlen(data);
    size_t off=0;
    while(off<size){
        ssize_t n=write(fd,data+off,size-off);
        if(n<=0)break;
        off+=(size_t)n;
    }
    int written=off==size;
    if(close(fd))written=0;
    free(data);
    if(!written){
        unlink(tmp);
        free(profile_config);dc_project_tools_close(&file_tools);
        free(expanded);
        free(run_snapshot);
        dc_compactor_close(compactor);
        return NULL;
    }
    lease l={
        .store=s,.id=id,.display_input=dc_agents_original_input(s)?dc_agents_original_input(s):expanded?input:NULL,.expanded_input=expanded?expanded:dc_agents_original_input(s)?(char*)input:NULL,.snapshot=run_snapshot,.hooks=hooks,
        .attachment=pending,.input=input,.media=media,.retry_run=retry_run}
    ;
    dc_sha256(profile_config?profile_config:settings,strlen(profile_config?profile_config:settings),l.profile);
    unsigned char nonce[16];sqlite3_randomness(sizeof nonce,nonce);dc_sha256(nonce,sizeof nonce,l.run);
    free(profile_config);
    char error[512]={
        0}
    ;
    J *trace_config=dunlin_json_parse(run_snapshot,strlen(run_snapshot));
    l.trace_model=json_object_get_string(get(trace_config,"model"));l.trace_protocol=json_object_get_string(get(trace_config,"protocol"));l.trace_secret=secret;host.session=&l;
    dc_file_history_run file_history={.store=s,.chat=id,.run=l.run,.proposal=-1};host.history=&file_history;
    dc_tool_record_run tool_records={.store=s,.chat=id,.run=l.run,.proposal=-1};l.tool_records=&tool_records;
    dc_file_journal_v1 journal=dc_file_history_journal(&file_history);if(use_adapter)dc_project_tools_journal(&file_tools,&journal);
    dunlin_clock_v1 clock={desktop_now,&host};
    ah_hooks observer={
        .user=&host,.approve=approval,.event=event,.cancelled=cancelled}
    ;
    dunlin_runtime *r=dunlin_open_with_transports(tmp,&observer,host_secret?&transport.v1:NULL,host_secret?&transport.v2:NULL,error,sizeof error);
    unlink(tmp);
    ah_result result={
        AH_FAILED,NULL}
    ;
    ah_tool_service agent_tools={0};
    int agent_adapter_ok=!delegation_allowed||dc_agents_tools(s,&file_tools,&agent_tools);
    const ah_tool_service *run_tools=agent_tools.ctx?&agent_tools:use_adapter?&file_tools:NULL;
    ah_tool_service extension_tools={0};
    int extension_adapter_ok=1;
    if(r&&agent_adapter_ok&&plugin_tools_allowed){
        extension_adapter_ok=dc_extension_tools_open(s,id,run_tools?run_tools:ak_get(dunlin_scope(r),"tools",1),get(trace_config,"_extension_catalog"),&extension_tools);
        if(extension_adapter_ok)run_tools=&extension_tools;
    }
    if(r&&agent_adapter_ok&&extension_adapter_ok){
        dunlin_set_model_trace(r,1);
        tool_records.inner=ak_get(dunlin_scope(r),"tool.pipeline",1);
        tool_records.tools=run_tools?run_tools:ak_get(dunlin_scope(r),"tools",1);
        ah_tool_pipeline_v1 tool_pipeline=dc_tool_records_pipeline(&tool_records);
        dunlin_set_tool_pipeline(r,&tool_pipeline);
        J *audit=dunlin_json_parse(run_snapshot,strlen(run_snapshot));
        dc_knowledge_projection knowledge_projection={0};
        const dunlin_context_projector_v1 *projection=dc_knowledge_projector(&knowledge_projection,get(audit,"_knowledge_snapshot"),dc_compactor_service(compactor),dunlin_limits(r),&traced_hooks);
        dc_attachment_projection attachments={0};
        if(media)projection=dc_attachment_projector(&attachments,s,id,&l.original,images,json_object_get_boolean(get(get(audit,"_context_policy"),"omitHistoryImages")),projection,dunlin_limits(r),&traced_hooks);
        if(projection)dunlin_set_context_projector(r,projection);
        ah_session_service store={
            &l,session_open,session_save,session_close}
        ;
        result=dunlin_turn_with_adapters(dunlin_scope(r),dunlin_limits(r),&observer,id,expanded?expanded:input,NULL,run_tools,&store,&clock);
        dunlin_close(r);
        json_object_put(audit);
    }
    else if(r)dunlin_close(r);
    dc_extension_tools_close(&extension_tools);
    dc_agents_tools_close(&agent_tools);
    if(input_committed)*input_committed=l.input_committed;
    if(host.grant_error==2)result.status=AH_UNKNOWN;
    else if(host.grant_error==1&&result.status!=AH_UNKNOWN)result.status=AH_DENIED;
    if(!l.trace){l.trace=dc_trajectory_begin(s,id,l.run,l.trace_model?l.trace_model:"",l.trace_protocol?l.trace_protocol:"",-1,secret);const char *detail=*error?error:"no admitted run or trace unavailable";dl_trajectory_event(l.trace,"admission.result",detail,strlen(detail));}
    dl_trajectory_finish(l.trace,ah_status_name(result.status),(long long)message_count(l.original));l.trace=NULL;json_object_put(trace_config);
    dl_live_stream_end(s->live_stream,id,l.run);
    dc_grants_finish(s,id);
    dc_tool_records_finish(&tool_records,result.status);
    q=prepare(s,"UPDATE run_records SET status=?,finished=unixepoch() WHERE id=?");
    if(q){bind(q,1,ah_status_name(result.status));bind(q,2,l.run);sqlite3_step(q);sqlite3_finalize(q);}
    J *out=json_object_new_object();
    json_object_object_add(out,"status",json_object_new_string(ah_status_name(result.status)));
    json_object_object_add(out,"text",json_object_new_string(host.grant_error==2?"工具操作已结束，但授权收尾记录未保存；请核对工具结果，不要重复执行。":host.grant_error?"无法保存授权记录，本次工具调用已拒绝。":result.text?result.text:(*error?error:"operation failed")));
    const char *saved_run=json_object_get_string(get(l.original,"_dunlin_run"));
    if(saved_run&&!strcmp(saved_run,l.run)&&retryable_input(l.original))
        json_object_object_add(out,"retryRun",json_object_new_string(saved_run));
    char *answer=copyjson(out);
    json_object_put(out);
    free(result.text);
    dc_project_tools_close(&file_tools);
    free(host.approved_arguments);host.approved_arguments=NULL;
    dc_file_history_cleanup(s,8);
    free(expanded);
    free(run_snapshot);
    dc_compactor_close(compactor);
    json_object_put(l.original);
    return answer;
}
static char *run(dc_store *s,const char *id,const char *settings,const char *input,const dc_hooks *hooks,const char *secret,int host_secret,const char *attachment,int *input_committed){
 if(!s||!valid(id)||!settings||strlen(settings)>262144)return NULL;
 J *checkpoint=state(s,id),*frozen=NULL,*provided=dunlin_json_parse(settings,strlen(settings));int active=json_object_get_boolean(get(checkpoint,"active"));
 const char *run_id=json_object_get_string(get(checkpoint,"_dunlin_run"));
 if(active&&valid(run_id)){
  sqlite3_stmt *q=prepare(s,"SELECT snapshot FROM run_records WHERE id=? AND chat=?");if(q){bind(q,1,run_id);bind(q,2,id);}if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);if(raw&&strlen(raw)<=4194304)frozen=dunlin_json_parse(raw,strlen(raw));}sqlite3_finalize(q);
 }
 J *saved=get(frozen,"_public_settings");
 if(saved){
  const char *current_id=json_object_get_string(get(provided,"desktop_profile_id")),*saved_id=json_object_get_string(get(saved,"desktop_profile_id"));
  int same=current_id&&saved_id?!strcmp(current_id,saved_id):json_object_equal(get(provided,"base_url"),get(saved,"base_url"))&&json_object_equal(get(provided,"model"),get(saved,"model"))&&json_object_equal(get(provided,"protocol"),get(saved,"protocol"));
  same=same&&json_object_get_int64(get(provided,"desktop_credential_revision"))==json_object_get_int64(get(saved,"desktop_credential_revision"));
  const char *route_keys[]={"desktop_provider_id","desktop_credential_id"};
  for(size_t i=0;i<sizeof route_keys/sizeof *route_keys;i++){J *a=get(provided,route_keys[i]),*b=get(saved,route_keys[i]);if((a||b)&&(!a||!b||!json_object_equal(a,b)))same=0;}
  if(!same){json_object_put(checkpoint);json_object_put(frozen);json_object_put(provided);return strdup("{\"status\":\"busy\",\"text\":\"未完成回合绑定原模型与凭据版本，请先恢复原配置。\"}");}
  J *old_policy=get(saved,"desktop_knowledge"),*current_policy=get(provided,"desktop_knowledge");
  if((json_object_get_boolean(get(old_policy,"memory"))&&!json_object_get_boolean(get(current_policy,"memory")))||(json_object_get_boolean(get(old_policy,"rag"))&&!json_object_get_boolean(get(current_policy,"rag")))){json_object_put(checkpoint);json_object_put(frozen);json_object_put(provided);return strdup("{\"status\":\"failed\",\"text\":\"知识引用许可已关闭，未发送恢复请求。可先结束此回合，保留已知和未知的工具结果。\"}");}
 }
 const char *raw_saved=json_object_get_string(get(frozen,"_public_settings_raw"));
 const char *effective=saved?(raw_saved?raw_saved:json_object_to_json_string_ext(saved,JSON_C_TO_STRING_PLAIN)):settings;
 char *result=run_impl(s,id,effective,input,hooks,secret,host_secret,saved?frozen:NULL,!active||saved!=NULL,attachment,input_committed,NULL);
 json_object_put(checkpoint);json_object_put(frozen);json_object_put(provided);return result;
}
char *dc_run(dc_store *s,const char *id,const char *settings,const char *input,const dc_hooks *hooks){return run(s,id,settings,input,hooks,NULL,0,NULL,NULL);}
static char *run_with_secret(dc_store *s,const char *id,const char *settings,const char *input,const dc_hooks *hooks,const char *secret,const char *attachment,int *input_committed){
 if(!s||!valid(id)||!settings||strlen(settings)>262144)return NULL;
 /* Host-key calls never resolve key_env. Exclude that ignored legacy name
  * before creating a new public snapshot/fingerprint. An active v1 profile
  * without frozen metadata still needs its exact original fingerprint. The
  * run wrapper selects the original frozen raw settings when they exist. */
 J *checkpoint=state(s,id);const char *profile=json_object_get_string(get(checkpoint,"_dunlin_profile"));
 int legacy=json_object_get_boolean(get(checkpoint,"active"))&&profile&&strlen(profile)==16;json_object_put(checkpoint);
 if(legacy)return run(s,id,settings,input,hooks,secret,1,attachment,input_committed);
 J *public=dunlin_json_parse(settings,strlen(settings));if(!json_object_is_type(public,json_type_object)){json_object_put(public);return NULL;}
 json_object_object_del(public,"key_env");char *safe=copyjson(public);json_object_put(public);if(!safe)return NULL;
 char *result=run(s,id,safe,input,hooks,secret,1,attachment,input_committed);free(safe);return result;
}
char *dc_run_with_secret(dc_store *s,const char *id,const char *settings,const char *input,const dc_hooks *hooks,const char *secret){return run_with_secret(s,id,settings,input,hooks,secret,NULL,NULL);}
char *dc_run_message_with_secret(dc_store *s,const char *id,const char *settings,const char *input,const dc_hooks *hooks,const char *secret,const char *attachment,int *input_committed){
 if(input_committed)*input_committed=0;
 if(!s||!id)return NULL;char *expanded=NULL;void *delivery_context=NULL;
 if(!dc_agents_chat_prepare(s,id,input,&expanded,&delivery_context))return strdup("{\"status\":\"failed\",\"text\":\"协作收件箱读取失败，未发送模型请求\"}");
 char *result=run_with_secret(s,id,settings,expanded?expanded:input,hooks,secret,attachment,input_committed);
 dc_agents_chat_finish(s,delivery_context);free(expanded);return result;
}
char *dc_retry_chat_turn(dc_store *s,const char *id,const char *settings,const char *prior_run,const dc_hooks *hooks,const char *secret){
    if(!s||!valid(id)||!prior_run||strlen(prior_run)!=64||strspn(prior_run,"0123456789abcdef")!=64||
       !settings||strlen(settings)>262144)return NULL;
    J *checkpoint=state(s,id);
    const char *last=json_object_get_string(get(checkpoint,"_dunlin_run"));
    int eligible=last&&!strcmp(last,prior_run)&&retryable_input(checkpoint);
    json_object_put(checkpoint);
    if(!eligible)return strdup("{\"status\":\"busy\",\"text\":\"原回合已变化，请重新查看聊天。\"}");
    J *public=dunlin_json_parse(settings,strlen(settings));if(!json_object_is_type(public,json_type_object)){json_object_put(public);return NULL;}
    json_object_object_del(public,"key_env");char *raw=copyjson(public);json_object_put(public);if(!raw)return NULL;
    char *out=run_impl(s,id,raw,NULL,hooks,secret,1,NULL,1,NULL,NULL,prior_run);free(raw);return out;
}
int dc_index_pending(dc_store *s,unsigned limit){
    if(!s)return -1;
    if(!limit)return 0;
    unsigned deletion_budget=limit<64?limit:64;
    int deleted=dc_index_cleanup_deleted(s,deletion_budget);
    if(deleted<0)return -1;
    if((unsigned)deleted==deletion_budget)return 1;
    if(dc_index_paused(s))return 0;
    limit-=(unsigned)deleted;
    if(!s->index)return -1;
    if(dl_index_bytes(s->index)>dc_index_quota(s)){s->index_fault=2;return -1;}
    for(unsigned i=0;i<limit;i++){
        sqlite3_stmt *q=prepare(s,"SELECT j.chat,j.seq,j.text FROM index_jobs j JOIN chats c ON c.id=j.chat ORDER BY j.rowid LIMIT 1");
        if(!q)return -1;
        int row_status=sqlite3_step(q);
        if(row_status!=SQLITE_ROW){
            sqlite3_finalize(q);
            return row_status==SQLITE_DONE?0:-1;
        }
        char *id=strdup((const char*)sqlite3_column_text(q,0)),*text=strdup((const char*)sqlite3_column_text(q,2));
        int seq=sqlite3_column_int(q,1);
        sqlite3_finalize(q);
        if(!id||!text||!dl_index_put(s->index,"local",id,seq,text)){
            s->index_fault=dl_index_last_error(s->index)==SQLITE_FULL?2:1;
            free(id);
            free(text);
            return -1;
        }
        if(!exec(s,"BEGIN IMMEDIATE")||!dc_preparation_put(s,id,seq,text)){
            exec(s,"ROLLBACK");free(text);free(id);return -1;
        }
        free(text);
        q=prepare(s,"DELETE FROM index_jobs WHERE chat=? AND seq=?");
        if(!q){
            exec(s,"ROLLBACK");
            free(id);
            return -1;
        }
        bind(q,1,id);
        sqlite3_bind_int(q,2,seq);
        int ok=sqlite3_step(q)==SQLITE_DONE;
        sqlite3_finalize(q);
        free(id);
        if(!exec(s,ok?"COMMIT":"ROLLBACK")){exec(s,"ROLLBACK");ok=0;}
        if(!ok)return -1;
        s->index_fault=0;
    }
    return 1;
}
char *dc_search(dc_store *s,const char *query){
    return dc_search_filtered(s,query,NULL,0);
}
char *dc_search_filtered(dc_store *s,const char *query,const char *project,int64_t since_ms){
    if(!s||since_ms<0)return NULL;
    J *matches=json_object_new_array();
    if(!matches)return NULL;
    for(int offset=0;offset<=INT32_MAX-50;offset+=50){
        char *raw=dl_index_search_page(s->index,"local",query,offset);
        J *a=raw?json_tokener_parse(raw):NULL;
        free(raw);
        if(!a){json_object_put(matches);return NULL;}
        size_t fetched=json_object_array_length(a);
        for(size_t i=json_object_array_length(a);i>0;i--){
            J *v=json_object_array_get_idx(a,i-1);
            /* A rebuildable cache is never authoritative source. This additionally
             * rejects stale/corrupt text and public-ID reuse after failed cleanup. */
            sqlite3_stmt *q=prepare(s,"SELECT state FROM chats WHERE id=? AND (? IS NULL OR EXISTS(SELECT 1 FROM chat_properties p WHERE p.chat=chats.id AND p.project=?)) AND NOT EXISTS(SELECT 1 FROM index_deletions WHERE chat=chats.id)");
            if(!q){
                json_object_put(a);json_object_put(matches);
                return NULL;
            }
            bind(q,1,json_object_get_string(get(v,"chat")));
            if(project){bind(q,2,project);bind(q,3,project);}
            int row_status=sqlite3_step(q);
            int current=0;
            if(row_status==SQLITE_ROW){
                const char *raw=(const char*)sqlite3_column_text(q,0);int bytes=sqlite3_column_bytes(q,0);
                J *checkpoint=raw&&bytes>0&&strlen(raw)==(size_t)bytes?dunlin_json_parse(raw,(size_t)bytes):NULL;
                J *messages=get(checkpoint,"messages"),*seq=get(v,"sequence"),*indexed=get(v,"text");
                int64_t sequence=json_object_get_int64(seq);
                if(json_object_is_type(messages,json_type_array)&&json_object_is_type(seq,json_type_int)&&sequence>=0&&(uint64_t)sequence<json_object_array_length(messages)){
                    J *message=json_object_array_get_idx(messages,(size_t)sequence),*source=get(message,"_dunlin_display");
                    if(!source)source=get(message,"content");
                    J *time=get(message,"_dunlin_time");
                    J *role=get(message,"role");
                    current=json_object_is_type(role,json_type_string)&&(!since_ms||(json_object_is_type(time,json_type_int)&&json_object_get_int64(time)>=since_ms))&&json_object_is_type(source,json_type_string)&&json_object_is_type(indexed,json_type_string)&&json_object_equal(source,indexed);
                    if(current){
                        json_object_object_add(v,"role",json_object_get(role));
                        if(json_object_is_type(time,json_type_int)&&json_object_get_int64(time)>0)
                            json_object_object_add(v,"time",json_object_get(time));
                    }
                }
                json_object_put(checkpoint);
            }
            sqlite3_finalize(q);
            if(row_status!=SQLITE_ROW&&row_status!=SQLITE_DONE){json_object_put(a);json_object_put(matches);return NULL;}
            if(!current)json_object_array_del_idx(a,i-1,1);
        }
        for(size_t i=0;i<json_object_array_length(a)&&json_object_array_length(matches)<50;i++)
            json_object_array_add(matches,json_object_get(json_object_array_get_idx(a,i)));
        json_object_put(a);
        if(fetched<50||json_object_array_length(matches)==50)break;
    }
    char *r=copyjson(matches);
    json_object_put(matches);
    return r;
}
