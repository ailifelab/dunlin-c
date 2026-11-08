#define _XOPEN_SOURCE 700
#include "chat_workspace.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct json_object J;
static J *get(J *o,const char *key){J *v=NULL;if(o)json_object_object_get_ex(o,key,&v);return v;}
static const char *str(J *o,const char *key){J *v=get(o,key);if(!json_object_is_type(v,json_type_string))return NULL;const char *p=json_object_get_string(v);return p&&strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static void text(J *o,const char *k,const char *v){json_object_object_add(o,k,json_object_new_string(v?v:""));}
static J *reply(J *data,const char *error){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(!error));if(error)text(o,"error",error);if(data)json_object_object_add(o,"data",data);return o;}
static int execute(dc_store *s,const char *sql){return sqlite3_exec(s->db,sql,NULL,NULL,NULL)==SQLITE_OK;}
static sqlite3_stmt *prepare(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *p){sqlite3_bind_text(q,n,p?p:"",-1,SQLITE_TRANSIENT);}
static const char *column(sqlite3_stmt *q,int n){const char *p=(const char*)sqlite3_column_text(q,n);return sqlite3_column_type(q,n)==SQLITE_TEXT&&p&&strlen(p)==(size_t)sqlite3_column_bytes(q,n)?p:NULL;}
static int ident(const char *p){return p&&*p&&strlen(p)<=64&&strspn(p,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(p);}
static int number(const char *p){return p&&*p&&strlen(p)<32&&strspn(p,"0123456789")==strlen(p);}
static int positive(J *v){return json_object_is_type(v,json_type_int)&&json_object_get_int64(v)>0&&json_object_get_uint64(v)<=INT64_MAX;}
static int identity(const char *path,char dev[32],char ino[32]){
    if(!path)return 0;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return 0;struct stat st;int ok=!fstat(fd,&st)&&S_ISDIR(st.st_mode)&&!access(path,R_OK|X_OK);
    if(ok){snprintf(dev,32,"%ju",(uintmax_t)st.st_dev);snprintf(ino,32,"%ju",(uintmax_t)st.st_ino);}close(fd);return ok;
}
static int put_binding(dc_store *s,const char *chat,const char *dev,const char *ino,const char *origin){
    sqlite3_stmt *q=prepare(s,"INSERT INTO chat_workspace_bindings(chat,device,inode,origin) VALUES(?,?,?,?) ON CONFLICT(chat) DO UPDATE SET device=excluded.device,inode=excluded.inode,origin=excluded.origin,revision=chat_workspace_bindings.revision+1 WHERE typeof(chat_workspace_bindings.revision)='integer' AND chat_workspace_bindings.revision<9223372036854775807");
    if(!q)return 0;bind(q,1,chat);bind(q,2,dev);bind(q,3,ino);bind(q,4,origin);int ok=sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;sqlite3_finalize(q);return ok;
}
static void writer_capability(sqlite3_context *context,int argc,sqlite3_value **argv){
    (void)argc;(void)argv;sqlite3_result_int(context,1);
}
int dc_chat_workspace_writer(dc_store *s){return sqlite3_create_function_v2(s->db,"dunlin_workspace_writer_v1",0,SQLITE_UTF8,NULL,writer_capability,NULL,NULL,NULL)==SQLITE_OK;}
int dc_chat_workspaces_schema(dc_store *s){
    if(!dc_chat_workspace_writer(s))return 0;
    sqlite3_stmt *version_query=prepare(s,"SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name='dunlin_workspace_writer_update'");
    if(!version_query)return 0;int version_row=sqlite3_step(version_query),known=version_row==SQLITE_ROW&&sqlite3_column_int(version_query,0)>0;sqlite3_finalize(version_query);
    if(version_row!=SQLITE_ROW)return 0;
    char upgrade_backup[160]="";uint64_t backup_bytes=0;
    if(!known&&!dc_workspace_backup_before_upgrade(s,upgrade_backup,&backup_bytes))return 0;
    if(!execute(s,"CREATE TABLE IF NOT EXISTS chat_workspace_bindings(chat TEXT PRIMARY KEY,device TEXT NOT NULL DEFAULT '',inode TEXT NOT NULL DEFAULT '',revision INTEGER NOT NULL DEFAULT 1 CHECK(typeof(revision)='integer' AND revision>0),origin TEXT NOT NULL);"))return 0;
    if(!execute(s,"BEGIN IMMEDIATE"))return 0;
    sqlite3_stmt *q=prepare(s,"SELECT c.id,c.workspace,COALESCE(m.project,''),COALESCE(p.path,''),COALESCE(p.device,''),COALESCE(p.inode,'') FROM chats c LEFT JOIN chat_properties m ON m.chat=c.id LEFT JOIN projects p ON p.id=m.project WHERE NOT EXISTS(SELECT 1 FROM chat_workspace_bindings b WHERE b.chat=c.id)");
    int ok=q!=NULL,row=SQLITE_DONE;
    while(ok&&(row=sqlite3_step(q))==SQLITE_ROW){
        const char *id=column(q,0),*path=column(q,1),*project=column(q,2),*project_path=column(q,3),*d=column(q,4),*i=column(q,5);
        if(!id||!path||!project||!project_path||!d||!i){ok=0;break;}
        char dev[32]="",ino[32]="";const char *origin="legacy_unknown";
        if(*project&&!strcmp(path,project_path)&&number(d)&&number(i)){
            snprintf(dev,sizeof dev,"%s",d);snprintf(ino,sizeof ino,"%s",i);origin="legacy_project_identity";
        }else if(!*project&&identity(path,dev,ino))origin="legacy_observed_now";
        /* Do not infer a past project identity from whatever directory happens
         * to exist now, nor rewrite an old checkpoint or workspace path. */
        ok=put_binding(s,id,dev,ino,origin);
    }
    sqlite3_finalize(q);if(row!=SQLITE_DONE)ok=0;
    if(ok)ok=execute(s,
      "CREATE TABLE IF NOT EXISTS desktop_workspace_migration(id INTEGER PRIMARY KEY CHECK(id=1),version INTEGER NOT NULL,backup TEXT NOT NULL,backup_bytes INTEGER NOT NULL,kind TEXT NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()));"
      "CREATE TRIGGER IF NOT EXISTS dunlin_workspace_writer_insert BEFORE INSERT ON chats BEGIN SELECT CASE WHEN dunlin_workspace_writer_v1()!=1 THEN RAISE(ABORT,'workspace_writer_version') END;END;"
      "CREATE TRIGGER IF NOT EXISTS dunlin_workspace_writer_update BEFORE UPDATE OF state,workspace ON chats BEGIN SELECT CASE WHEN dunlin_workspace_writer_v1()!=1 THEN RAISE(ABORT,'workspace_writer_version') END;END;"
      "CREATE TRIGGER IF NOT EXISTS dunlin_workspace_writer_delete BEFORE DELETE ON chats BEGIN SELECT CASE WHEN dunlin_workspace_writer_v1()!=1 THEN RAISE(ABORT,'workspace_writer_version') END;END;");
    if(ok){
        sqlite3_stmt *record=prepare(s,"INSERT OR IGNORE INTO desktop_workspace_migration(id,version,backup,backup_bytes,kind) VALUES(1,1,?,?,?)");
        if(record){bind(record,1,upgrade_backup);sqlite3_bind_int64(record,2,(sqlite3_int64)backup_bytes);bind(record,3,*upgrade_backup?"upgrade_backup":known?"prior_guard_record_missing":"initial_format");}
        ok=record&&sqlite3_step(record)==SQLITE_DONE;sqlite3_finalize(record);
    }
    if(ok)ok=execute(s,"COMMIT");if(!ok)execute(s,"ROLLBACK");return ok;
}
int dc_chat_workspace_capture(dc_store *s,const char *chat,const char *path){
    if(!s||sqlite3_get_autocommit(s->db)||!ident(chat)||!path)return 0;
    char dev[32]="",ino[32]="";int found=identity(path,dev,ino);
    sqlite3_stmt *q=prepare(s,"SELECT p.device,p.inode FROM chats c JOIN chat_properties m ON m.chat=c.id JOIN projects p ON p.id=m.project WHERE c.id=? AND p.path=c.workspace");
    if(!q)return 0;bind(q,1,chat);int row=sqlite3_step(q),ok=row==SQLITE_DONE;
    if(row==SQLITE_ROW){const char *d=column(q,0),*i=column(q,1);ok=found&&d&&i&&!strcmp(dev,d)&&!strcmp(ino,i);}sqlite3_finalize(q);
    return ok&&put_binding(s,chat,dev,ino,found?"captured":"unknown");
}
int dc_chat_workspaces_relocate(dc_store *s,const char *project,const char *path,const char *dev,const char *ino){
    if(sqlite3_get_autocommit(s->db)||!ident(project)||!path||!number(dev)||!number(ino))return 0;
    const char *queries[]={
        "UPDATE chats SET workspace=? WHERE id IN(SELECT b.chat FROM chat_workspace_bindings b JOIN chat_properties m ON m.chat=b.chat WHERE m.project=? AND b.device=? AND b.inode=?)",
        "UPDATE chat_workspace_bindings SET revision=CASE WHEN typeof(revision)='integer' AND revision<9223372036854775807 THEN revision+1 ELSE NULL END WHERE chat IN(SELECT chat FROM chat_properties WHERE project=?) AND device=? AND inode=?"};
    for(unsigned n=0;n<2;n++){
        sqlite3_stmt *q=prepare(s,queries[n]);if(!q)return 0;
        if(!n){bind(q,1,path);bind(q,2,project);bind(q,3,dev);bind(q,4,ino);}
        else{bind(q,1,project);bind(q,2,dev);bind(q,3,ino);}
        int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);if(!ok)return 0;
    }
    return 1;
}
J *dc_chat_workspace_view(dc_store *s,const char *chat){
    if(!s||!ident(chat))return NULL;
    sqlite3_stmt *q=prepare(s,"SELECT c.workspace,COALESCE(b.device,''),COALESCE(b.inode,''),COALESCE(b.revision,0),COALESCE(b.origin,'missing'),COALESCE(m.project,''),COALESCE(p.revision,0) FROM chats c LEFT JOIN chat_workspace_bindings b ON b.chat=c.id LEFT JOIN chat_properties m ON m.chat=c.id LEFT JOIN projects p ON p.id=m.project WHERE c.id=?");
    if(!q)return NULL;bind(q,1,chat);J *out=NULL;
    if(sqlite3_step(q)==SQLITE_ROW){
        const char *path=column(q,0),*dev=column(q,1),*ino=column(q,2),*origin=column(q,4),*project=column(q,5);
        if(!path||!dev||!ino||!origin||!project)goto done;
        char current_dev[32]="",current_ino[32]="";int readable=identity(path,current_dev,current_ino);
        int revision_valid=sqlite3_column_type(q,3)==SQLITE_INTEGER&&sqlite3_column_int64(q,3)>0;
        int known=number(dev)&&number(ino)&&revision_valid;
        int available=known&&readable&&!strcmp(dev,current_dev)&&!strcmp(ino,current_ino);
        out=json_object_new_object();text(out,"path",path);text(out,"device",dev);text(out,"inode",ino);text(out,"origin",origin);text(out,"project",project);
        json_object_object_add(out,"revision",json_object_new_int64(revision_valid?sqlite3_column_int64(q,3):0));
        json_object_object_add(out,"projectRevision",json_object_new_int64(sqlite3_column_int64(q,6)));
        json_object_object_add(out,"available",json_object_new_boolean(available));
        text(out,"availability",available?"可用":!known?"聊天工作区身份尚未确认，请单独重新关联":!readable?"聊天工作区不可访问，已保存的历史仍保留":"聊天工作区目录身份已变化，请单独重新关联");
    }
done:sqlite3_finalize(q);return out;
}
static J *review(dc_store *s,J *r,const char **error){
    const char *chat=str(r,"chat"),*path=str(r,"path");if(!ident(chat)||!path){*error="invalid_workspace_request";return NULL;}
    J *view=dc_chat_workspace_view(s,chat);if(!view){*error="chat_not_found_or_binding_unreadable";return NULL;}
    char normalized[PATH_MAX],dev[32],ino[32];
    if(!dc_workspace_root(s,path,normalized,dev,ino)){json_object_put(view);*error="invalid_workspace_path";return NULL;}
    sqlite3_stmt *q=prepare(s,"SELECT state FROM chats WHERE id=?");if(!q){json_object_put(view);*error="storage_error";return NULL;}bind(q,1,chat);
    char hash[65];size_t count=0;int active=0,pending=0,valid=0;
    if(sqlite3_step(q)==SQLITE_ROW){
        const char *raw=(const char*)sqlite3_column_text(q,0);
        if(sqlite3_column_type(q,0)==SQLITE_NULL){dc_sha256("",0,hash);valid=1;}
        else if(raw&&strlen(raw)==(size_t)sqlite3_column_bytes(q,0)&&strlen(raw)<=8388608){
            J *state=dunlin_json_parse(raw,strlen(raw)),*messages=get(state,"messages"),*version=get(state,"version");
            valid=json_object_is_type(state,json_type_object)&&json_object_is_type(messages,json_type_array)
                &&json_object_is_type(get(state,"active"),json_type_boolean)&&json_object_is_type(version,json_type_int)
                &&(json_object_get_int64(version)==1||json_object_get_int64(version)==2);
            if(valid){count=json_object_array_length(messages);active=json_object_get_boolean(get(state,"active"));pending=get(state,"pending")!=NULL;dc_sha256(raw,strlen(raw),hash);}
            for(size_t i=0;valid&&i<count;i++){J *m=json_object_array_get_idx(messages,i);const char *role=str(m,"role");if(!json_object_is_type(m,json_type_object)||!role||(strcmp(role,"user")&&strcmp(role,"assistant")&&strcmp(role,"tool")&&strcmp(role,"system")))valid=0;}
            json_object_put(state);
        }
    }
    sqlite3_finalize(q);if(!valid||count>8192||!positive(get(view,"revision"))){json_object_put(view);*error="invalid_workspace_checkpoint_or_binding";return NULL;}
    J *out=json_object_new_object();text(out,"chat",chat);text(out,"path",normalized);text(out,"device",dev);text(out,"inode",ino);text(out,"historyHash",hash);
    text(out,"currentPath",str(view,"path"));text(out,"currentDevice",str(view,"device"));text(out,"currentInode",str(view,"inode"));text(out,"project",str(view,"project"));
    json_object_object_add(out,"bindingRevision",json_object_get(get(view,"revision")));
    json_object_object_add(out,"projectRevision",json_object_get(get(view,"projectRevision")));
    json_object_object_add(out,"messageCount",json_object_new_int64((int64_t)count));
    json_object_object_add(out,"canApply",json_object_new_boolean(!active&&!pending));text(out,"reason",active||pending?"active_checkpoint":"");
    json_object_put(view);return out;
}
J *dc_chat_workspace_request(dc_store *s,J *r){
    const char *op=str(r,"op");
    if(op&&!strcmp(op,"chats.workspace.migration")){J *v=dc_workspace_migration_view(s);return reply(v,v?NULL:"workspace_migration_record_unreadable");}
    if(op&&!strcmp(op,"chats.workspace.get")){J *v=dc_chat_workspace_view(s,str(r,"chat"));return reply(v,v?NULL:"chat_not_found_or_binding_unreadable");}
    int apply=op&&!strcmp(op,"chats.workspace.apply"),preview=op&&!strcmp(op,"chats.workspace.preview");
    if(!apply&&!preview)return reply(NULL,"unsupported_workspace_operation");
    if(!execute(s,apply?"BEGIN IMMEDIATE":"BEGIN"))return reply(NULL,"storage_error");
    const char *error=NULL;J *plan=review(s,r,&error);
    if(apply&&plan){
        if(!json_object_get_boolean(get(plan,"canApply")))error="active_checkpoint";
        J *project_revision=get(r,"projectRevision");
        if(!positive(get(r,"bindingRevision"))||!json_object_is_type(project_revision,json_type_int)
           ||json_object_get_int64(project_revision)<0||json_object_get_uint64(project_revision)>INT64_MAX)error="workspace_review_stale";
        const char *keys[]={"chat","path","device","inode","project","bindingRevision","projectRevision","historyHash"};
        for(unsigned i=0;!error&&i<sizeof keys/sizeof *keys;i++){
            J *a=get(r,keys[i]),*b=get(plan,keys[i]);
            if(!a||!b||!json_object_equal(a,b))error="workspace_review_stale";
        }
        if(!error){
            char dev[32],ino[32];const char *path=str(plan,"path");
            if(!identity(path,dev,ino)||strcmp(dev,str(plan,"device"))||strcmp(ino,str(plan,"inode")))error="workspace_review_stale";
            else{
                sqlite3_stmt *q=prepare(s,"UPDATE chats SET workspace=? WHERE id=?");
                if(q){bind(q,1,path);bind(q,2,str(plan,"chat"));}int ok=q&&sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;sqlite3_finalize(q);
                if(!ok||!put_binding(s,str(plan,"chat"),dev,ino,"explicit_chat_relink"))error="storage_error";
            }
        }
    }
    if(error||!plan){execute(s,"ROLLBACK");json_object_put(plan);return reply(NULL,error?error:"storage_error");}
    if(!execute(s,"COMMIT")){execute(s,"ROLLBACK");json_object_put(plan);return reply(NULL,"storage_error");}
    return reply(plan,NULL);
}
