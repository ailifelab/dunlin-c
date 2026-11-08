#define _POSIX_C_SOURCE 200809L
#include "attachments.h"
#include "chat_workspace.h"
#include "dunlin/image_resource.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(json_object_is_type(o,json_type_object))json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);const char *p=json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;return p&&strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static int ident(const char *s){return s&&*s&&strlen(s)<=64&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(s);}
static int equal(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static void text(J *o,const char *k,const char *v){json_object_object_add(o,k,json_object_new_string(v?v:""));}
static void number(J *o,const char *k,int64_t v){json_object_object_add(o,k,json_object_new_int64(v));}
static J *reply(J *data,const char *error){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(error==NULL));if(error)text(r,"error",error);else json_object_object_add(r,"data",data);return r;}
static char *owned(J *o){char *p=o?strdup(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(o);return p;}
static sqlite3_stmt *prepare(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;if(sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)!=SQLITE_OK)return NULL;return q;}
static void bind(sqlite3_stmt *q,int i,const char *p){if(q)sqlite3_bind_text(q,i,p,-1,SQLITE_TRANSIENT);}
static int sql(dc_store *s,const char *raw){return sqlite3_exec(s->db,raw,NULL,NULL,NULL)==SQLITE_OK;}
static int64_t scalar(dc_store *s,const char *raw){sqlite3_stmt *q=prepare(s,raw);int64_t n=-1;if(q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_type(q,0)==SQLITE_INTEGER)n=sqlite3_column_int64(q,0);sqlite3_finalize(q);return n;}
static const char *status_error(dl_attachment_status status){
    switch(status){case DL_ATTACHMENT_OK:return NULL;case DL_ATTACHMENT_INVALID:return "attachment_invalid_format_or_name";
    case DL_ATTACHMENT_LIMIT:return "attachment_size_or_quota_limit";case DL_ATTACHMENT_NOT_FOUND:return "attachment_not_found";
    case DL_ATTACHMENT_STALE:return "attachment_selection_changed";case DL_ATTACHMENT_TRANSACTION:return "attachment_transaction_required";
    case DL_ATTACHMENT_NOMEM:return "attachment_out_of_memory";default:return "attachment_storage_error";}
}
static void writer(sqlite3_context *context,int argc,sqlite3_value **argv){(void)argc;(void)argv;sqlite3_result_int(context,1);}
int dc_attachments_initialize(dc_store *s,int create){
    if(!s||!s->db)return 0;if(s->attachments)return 1;
    int readonly=sqlite3_db_readonly(s->db,"main");if(readonly<0||!sqlite3_get_autocommit(s->db))return 0;
    if(!readonly&&sqlite3_create_function_v2(s->db,"dunlin_attachment_writer_v1",0,SQLITE_UTF8,NULL,writer,NULL,NULL,NULL)!=SQLITE_OK)return 0;
    int64_t tables=scalar(s,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN('attachment_resources','attachment_drafts','attachment_messages')");
    if(tables<0)return 0;if(!tables&&!create)return 1;
    int64_t guards=scalar(s,"SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN('dunlin_attachment_writer_insert','dunlin_attachment_writer_update','dunlin_attachment_writer_delete')");
    int64_t record=scalar(s,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name='desktop_attachment_migration'");
    if(guards<0||record<0)return 0;
    if(guards==3&&record==1&&tables==3){
        if(scalar(s,"SELECT count(*) FROM desktop_attachment_migration WHERE id=1 AND version=1")!=1)return 0;
        s->attachments=readonly?dl_attachments_reader(s->db):dl_attachments_open(s->db,268435456);
        return s->attachments!=NULL;
    }
    if(readonly||guards||record||(tables&&tables!=3))return 0;
    /* A crash between the generic empty schema creation and guard publication
     * can be retried. A populated unversioned format is never guessed/adopted. */
    if(tables&&(scalar(s,"SELECT count(*) FROM attachment_drafts")!=0||scalar(s,"SELECT count(*) FROM attachment_messages")!=0||scalar(s,"SELECT count(*) FROM attachment_resources")!=0))return 0;
    char backup[160]="";uint64_t backup_bytes=0;
    if(!dc_backup_before_upgrade(s,"chat-attachments-v1",backup,&backup_bytes))return 0;
    dl_attachment_store *store=dl_attachments_open(s->db,268435456);if(!store)return 0;
    if(!sql(s,"BEGIN IMMEDIATE")){dl_attachments_close(store);return 0;}
    int ok=sql(s,
      "CREATE TABLE desktop_attachment_migration(id INTEGER PRIMARY KEY CHECK(id=1),version INTEGER NOT NULL,backup TEXT NOT NULL,backup_bytes INTEGER NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()));"
      "CREATE TABLE chat_attachment_policy(chat TEXT PRIMARY KEY,images INTEGER NOT NULL CHECK(typeof(images)='integer' AND images IN(0,1)));"
      "CREATE TRIGGER dunlin_attachment_writer_insert BEFORE INSERT ON chats BEGIN SELECT CASE WHEN dunlin_attachment_writer_v1()!=1 THEN RAISE(ABORT,'attachment_writer_version') END;END;"
      "CREATE TRIGGER dunlin_attachment_writer_update BEFORE UPDATE OF state,workspace ON chats BEGIN SELECT CASE WHEN dunlin_attachment_writer_v1()!=1 THEN RAISE(ABORT,'attachment_writer_version') END;END;"
      "CREATE TRIGGER dunlin_attachment_writer_delete BEFORE DELETE ON chats BEGIN SELECT CASE WHEN dunlin_attachment_writer_v1()!=1 THEN RAISE(ABORT,'attachment_writer_version') END;END;");
    sqlite3_stmt *q=ok?prepare(s,"INSERT INTO desktop_attachment_migration(id,version,backup,backup_bytes) VALUES(1,1,?,?)"):NULL;
    if(q){bind(q,1,backup);sqlite3_bind_int64(q,2,(sqlite3_int64)backup_bytes);}ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);
    if(ok)ok=sql(s,"COMMIT");if(!ok){sql(s,"ROLLBACK");dl_attachments_close(store);return 0;}
    s->attachments=store;return 1;
}
J *dc_attachment_view(const dl_attachment_draft *a){
    if(!a)return NULL;J *o=json_object_new_object();
    text(o,"id",a->id);text(o,"name",a->name);text(o,"kind",a->kind);text(o,"sha256",a->content.sha256);
    text(o,"format",a->content.format==DL_ATTACHMENT_PNG?"png":"text");text(o,"mime",dl_attachment_mime(a->content.format));
    number(o,"bytes",(int64_t)a->content.bytes);number(o,"width",a->content.width);number(o,"height",a->content.height);return o;
}
int dc_attachment_message_hash(J *m,char hash[65]){
    if(!json_object_is_type(m,json_type_object)||!hash)return 0;
    const char *raw=json_object_to_json_string_ext(m,JSON_C_TO_STRING_PLAIN);if(!raw)return 0;dc_sha256(raw,strlen(raw),hash);return 1;
}
int dc_attachment_manifest_matches(J *message,const dl_attachment_draft *a){
    J *parts=get(message,"_dunlin_attachments");if(!json_object_is_type(parts,json_type_array)||json_object_array_length(parts)!=1||!equal(str(message,"role"),"user"))return 0;
    J *expected=dc_attachment_view(a);int ok=expected&&json_object_equal(expected,json_object_array_get_idx(parts,0));json_object_put(expected);return ok;
}
J *dc_attachment_message_view(dc_store *s,const char *chat,int64_t seq,J *message){
    if(!get(message,"_dunlin_attachments"))return NULL;
    if(!s||!s->attachments)return NULL;
    char hash[65];dl_attachment_draft a;
    if(!dc_attachment_message_hash(message,hash)||dl_attachments_info(s->attachments,DL_ATTACHMENT_CHAT,chat,seq,hash,&a)!=DL_ATTACHMENT_OK||!dc_attachment_manifest_matches(message,&a))return NULL;
    J *out=dc_attachment_view(&a);if(out){text(out,"canonicalHash",hash);number(out,"sequence",seq);}return out;
}
/* A malformed or active checkpoint cannot accept resource edits. */
static const char *chat_check(dc_store *s,const char *chat,int idle){
    if(!ident(chat))return "attachment_chat_invalid";
    sqlite3_stmt *q=prepare(s,"SELECT state FROM chats WHERE id=?");bind(q,1,chat);
    int row=q?sqlite3_step(q):SQLITE_ERROR;const char *error=NULL;J *value=NULL;
    if(row!=SQLITE_ROW)error=row==SQLITE_DONE?"attachment_chat_missing":"attachment_storage_error";
    else if(sqlite3_column_type(q,0)!=SQLITE_NULL){
        const char *raw=(const char*)sqlite3_column_text(q,0);int n=sqlite3_column_bytes(q,0);
        if(!raw||n<1||n>4194304||strlen(raw)!=(size_t)n||!(value=dunlin_json_parse(raw,(size_t)n))
           ||!json_object_is_type(value,json_type_object)||!json_object_is_type(get(value,"messages"),json_type_array)||!json_object_is_type(get(value,"active"),json_type_boolean))error="attachment_history_invalid";
        else if(idle&&(json_object_get_boolean(get(value,"active"))||get(value,"pending")))error="attachment_active_checkpoint";
    }
    sqlite3_finalize(q);json_object_put(value);return error;
}
static J *stage(dc_store *s,const char *chat,const char *expected,const char *name,const char *origin,int format,const void *bytes,size_t n){
    if(!s)return reply(NULL,"attachment_invalid_store");
    const char *error=chat_check(s,chat,1);if(error)return reply(NULL,error);
    dl_attachment_info check;dl_attachment_status status=dl_attachment_inspect((dl_attachment_format)format,bytes,n,&check);
    if(status!=DL_ATTACHMENT_OK)return reply(NULL,status_error(status));
    if(!dc_attachments_initialize(s,1))return reply(NULL,"attachment_upgrade_backup_or_schema_failed");
    dl_attachment_draft a;status=dl_attachments_stage(s->attachments,DL_ATTACHMENT_CHAT,chat,expected,name,origin,(dl_attachment_format)format,bytes,n,&a);
    return reply(status==DL_ATTACHMENT_OK?dc_attachment_view(&a):NULL,status_error(status));
}
static J *preview(const char *name,const char *origin,int format,const void *bytes,size_t n){
    dl_attachment_draft a={0};dl_attachment_status status=dl_attachment_inspect((dl_attachment_format)format,bytes,n,&a.content);
    if(status!=DL_ATTACHMENT_OK)return reply(NULL,status_error(status));
    if(!name||!*name||strlen(name)>256||!ah_utf8_text(name,strlen(name))||!origin
       ||(strcmp(origin,"file")&&strcmp(origin,"clipboard")&&strcmp(origin,"screenshot")))return reply(NULL,"attachment_invalid_format_or_name");
    for(const unsigned char *p=(const unsigned char*)name;*p;p++)if(*p<32||*p==127)return reply(NULL,"attachment_invalid_format_or_name");
    snprintf(a.name,sizeof a.name,"%s",name);snprintf(a.kind,sizeof a.kind,"%s",origin);
    J *o=dc_attachment_view(&a);char *url=NULL;
    if(format==DL_ATTACHMENT_PNG){if(dl_png_data_url(bytes,n,&url)!=DL_IMAGE_OK){json_object_put(o);return reply(NULL,"attachment_payload_invalid");}text(o,"dataURL",url);free(url);}
    else json_object_object_add(o,"text",json_object_new_string_len(n?bytes:"",(int)n));
    return reply(o,NULL);
}
char *dc_attachment_preview(const char *name,const char *origin,int format,const void *bytes,size_t n){return owned(preview(name,origin,format,bytes,n));}
char *dc_attachment_stage(dc_store *s,const char *chat,const char *expected,const char *name,const char *origin,int format,const void *bytes,size_t size){
    return owned(stage(s,chat,expected,name,origin,format,bytes,size));
}
static J *stage_file(dc_store *s,J *r){
    const char *chat=str(r,"chat"),*path=str(r,"path"),*format=str(r,"format"),*expected=str(r,"expectedID");
    if(get(r,"expectedID")&&!expected)return reply(NULL,"attachment_selection_invalid");
    const char *error=chat_check(s,chat,1);if(error)return reply(NULL,error);
    dl_attachment_format type=equal(format,"png")?DL_ATTACHMENT_PNG:equal(format,"text")?DL_ATTACHMENT_TEXT:0;
    if(!type||!path||path[0]!='/'||strlen(path)>4095)return reply(NULL,"attachment_file_invalid");
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat before,after;
    if(fd<0)return reply(NULL,"attachment_file_unreadable");
    size_t limit=type==DL_ATTACHMENT_PNG?1048576:65536;
    if(fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_size<0||(uint64_t)before.st_size>limit){close(fd);return reply(NULL,"attachment_file_type_or_size_limit");}
    size_t n=(size_t)before.st_size,offset=0;unsigned char *p=malloc(n+1);if(!p){close(fd);return reply(NULL,"attachment_out_of_memory");}
    while(offset<n){ssize_t got=read(fd,p+offset,n-offset);if(got<0&&errno==EINTR)continue;if(got<=0)break;offset+=(size_t)got;}
    unsigned char trailing;ssize_t extra;do{extra=read(fd,&trailing,1);}while(extra<0&&errno==EINTR);
    int ok=offset==n&&extra==0&&!fstat(fd,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size;
#ifdef __APPLE__
    ok=ok&&before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec&&before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec&&before.st_ctimespec.tv_sec==after.st_ctimespec.tv_sec&&before.st_ctimespec.tv_nsec==after.st_ctimespec.tv_nsec;
#else
    ok=ok&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec;
#endif
    close(fd);const char *name=strrchr(path,'/');name=name?name+1:path;
    J *out=NULL;
    if(!ok)out=reply(NULL,"attachment_file_changed_during_read");
    else if(equal(str(r,"op"),"attachments.previewFile"))out=preview(name,"file",type,p,n);
    else{
        const char *hash=str(r,"expectedSHA");dl_attachment_info info;
        if(get(r,"expectedSHA")&&(!hash||dl_attachment_inspect(type,p,n,&info)!=DL_ATTACHMENT_OK||strcmp(hash,info.sha256)))out=reply(NULL,"attachment_file_changed_since_preview");
        else out=stage(s,chat,expected,name,"file",type,p,n);
    }
    free(p);return out;
}
static J *read_message(dc_store *s,const char *chat,int64_t seq){
    sqlite3_stmt *q=prepare(s,"SELECT state FROM chats WHERE id=?");bind(q,1,chat);J *state=NULL,*message=NULL;
    if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);int n=sqlite3_column_bytes(q,0);if(raw&&n>0&&n<=4194304&&strlen(raw)==(size_t)n)state=dunlin_json_parse(raw,(size_t)n);}
    sqlite3_finalize(q);J *messages=get(state,"messages");
    if(json_object_is_type(messages,json_type_array)&&(uint64_t)seq<json_object_array_length(messages))message=json_object_get(json_object_array_get_idx(messages,(size_t)seq));json_object_put(state);return message;
}
J *dc_attachments_request(dc_store *s,J *r){
    const char *op=str(r,"op"),*chat=str(r,"chat");if(!s||!op)return reply(NULL,"attachment_invalid_request");
    if(equal(op,"attachments.stageFile")||equal(op,"attachments.previewFile"))return stage_file(s,r);
    const char *error=chat_check(s,chat,equal(op,"attachments.remove"));if(error)return reply(NULL,error);
    if(!dc_attachments_initialize(s,0))return reply(NULL,"attachment_schema_invalid");
    if(equal(op,"attachments.pending")){
        if(!s->attachments)return reply(NULL,NULL);
        dl_attachment_draft a;dl_attachment_status status=dl_attachments_draft(s->attachments,DL_ATTACHMENT_CHAT,chat,&a);
        return reply(status==DL_ATTACHMENT_OK?dc_attachment_view(&a):NULL,status==DL_ATTACHMENT_NOT_FOUND?NULL:status_error(status));
    }
    if(equal(op,"attachments.remove")){
        if(!s->attachments)return reply(NULL,"attachment_selection_changed");
        return reply(NULL,status_error(dl_attachments_remove(s->attachments,DL_ATTACHMENT_CHAT,chat,str(r,"expectedID"))));
    }
    if(!equal(op,"attachments.read"))return reply(NULL,"attachment_unsupported_operation");
    J *sequence=get(r,"sequence");int64_t seq=-1;
    if(sequence){if(!json_object_is_type(sequence,json_type_int)||json_object_get_int64(sequence)<0||json_object_get_uint64(sequence)>INT64_MAX)return reply(NULL,"attachment_sequence_invalid");seq=json_object_get_int64(sequence);}
    if(!s->attachments)return reply(NULL,"attachment_not_found");
    J *message=seq>=0?read_message(s,chat,seq):NULL;char hash[65]="";
    if(seq>=0&&(!dc_attachment_message_hash(message,hash)||!equal(hash,str(r,"canonicalHash")))){json_object_put(message);return reply(NULL,"attachment_source_changed");}
    dl_attachment_draft a;void *p=NULL;size_t n=0;
    dl_attachment_status status=dl_attachments_read(s->attachments,DL_ATTACHMENT_CHAT,chat,seq,str(r,"draftID"),hash,&p,&n,&a);
    if(status!=DL_ATTACHMENT_OK){json_object_put(message);return reply(NULL,status_error(status));}
    if(seq>=0&&!dc_attachment_manifest_matches(message,&a)){json_object_put(message);free(p);return reply(NULL,"attachment_manifest_invalid");}json_object_put(message);
    J *out=dc_attachment_view(&a);char *url=NULL;
    if(a.content.format==DL_ATTACHMENT_PNG){if(dl_png_data_url(p,n,&url)!=DL_IMAGE_OK){json_object_put(out);free(p);return reply(NULL,"attachment_payload_invalid");}text(out,"dataURL",url);free(url);}
    else json_object_object_add(out,"text",json_object_new_string_len(p,(int)n));
    free(p);return reply(out,NULL);
}
int dc_attachment_admit(dc_store *s,const char *chat,const char *expected,J *settings,dl_attachment_draft *out,char *error,size_t size){
    if(out)memset(out,0,sizeof *out);const char *problem=NULL;
    if(!s||!out||!ident(chat))problem="无效的附件聊天。";
    else if(!dc_attachments_initialize(s,0))problem="附件存储格式不可用，未发送请求。";
    else {
        dl_attachment_status status=s->attachments?dl_attachments_draft(s->attachments,DL_ATTACHMENT_CHAT,chat,out):DL_ATTACHMENT_NOT_FOUND;
        if(status!=DL_ATTACHMENT_OK&&status!=DL_ATTACHMENT_NOT_FOUND)problem="无法读取待发附件，未发送请求。";
        else if((expected&&(status!=DL_ATTACHMENT_OK||strcmp(expected,out->id)))||(!expected&&status!=DL_ATTACHMENT_NOT_FOUND))problem="待发附件已变化，请重新检查后发送。";
        else if(status==DL_ATTACHMENT_OK){
            if(out->content.format==DL_ATTACHMENT_PNG){
                if(!dc_attachment_model_images(settings))problem="当前模型未配置可用的图片输入，请选择支持图片的模型。原图和草稿保留。";
                else if(dc_attachment_policy(s,chat)!=1)problem="此聊天目前仅传文字，请先在模型切换确认中恢复图片输入。待发原图保留。";
            }
            if(!problem){void *p=NULL;size_t n=0;dl_attachment_draft actual;
                if(dl_attachments_read(s->attachments,DL_ATTACHMENT_CHAT,chat,-1,out->id,NULL,&p,&n,&actual)!=DL_ATTACHMENT_OK)problem="附件原始数据校验失败，未发送请求。";
                free(p);
            }
        }
    }
    if(problem&&error&&size)snprintf(error,size,"%s",problem);return problem==NULL;
}
int dc_attachment_model_images(J *settings){
    J *input=get(settings,"desktop_input_modalities");int vision=0;
    for(size_t i=0;json_object_is_type(input,json_type_array)&&i<json_object_array_length(input);i++){
        J *v=json_object_array_get_idx(input,i);if(json_object_is_type(v,json_type_string)&&json_object_get_string_len(v)==5&&equal(json_object_get_string(v),"image"))vision=1;
    }
    /* A vendor capability declaration cannot add a serializer. The shared
     * non-Chat adapters currently accept text user content, so fail admission
     * before consuming a PNG until those actual codecs are implemented. */
    return vision&&equal(str(settings,"protocol"),"chat");
}
int dc_attachment_policy(dc_store *s,const char *chat){
    if(!s||!ident(chat))return -1;if(!s->attachments)return 1;
    sqlite3_stmt *q=prepare(s,"SELECT images FROM chat_attachment_policy WHERE chat=?");bind(q,1,chat);
    int row=q?sqlite3_step(q):SQLITE_ERROR,value=-1;
    if(row==SQLITE_DONE)value=1;
    else if(row==SQLITE_ROW&&sqlite3_column_type(q,0)==SQLITE_INTEGER&&(sqlite3_column_int64(q,0)==0||sqlite3_column_int64(q,0)==1))value=sqlite3_column_int(q,0);
    sqlite3_finalize(q);return value;
}
int dc_attachment_set_policy(dc_store *s,const char *chat,int images){
    if(!s||!s->attachments||!ident(chat)||(images!=0&&images!=1)||sqlite3_get_autocommit(s->db))return 0;
    sqlite3_stmt *q=prepare(s,"INSERT INTO chat_attachment_policy(chat,images) VALUES(?,?) ON CONFLICT(chat) DO UPDATE SET images=excluded.images");
    if(q){bind(q,1,chat);sqlite3_bind_int(q,2,images);}int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;
}
