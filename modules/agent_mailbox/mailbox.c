#include "dunlin/agent_mailbox.h"
#include "dunlin/content_hash.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

struct dl_agent_mailbox { sqlite3 *db;size_t max_payload;unsigned max_pending;int readonly;char principal[129]; };
static int ident(const char *s,int empty){
    if(!s||(!*s&&!empty))return 0;
    size_t n=0;
    for(;s[n];n++){
        unsigned char c=(unsigned char)s[n];
        if(n>=128||!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return 0;
    }
    return 1;
}
static int digest(const char *s){
    if(!s||strlen(s)!=64)return 0;
    for(size_t i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int envelope(dl_agent_mailbox *s,const char *project,const char *sender,const char *receiver,const char *mid,const char *reply,const char *body_hash,char hash[65]){
    char buffer[1024];
    int n=snprintf(buffer,sizeof buffer,"2\n%s\n%s\n%s\n%s\n%s\n%s\n%s",s->principal,project,sender,receiver,mid,reply,body_hash);
    if(n<0||(size_t)n>=sizeof buffer)return 0;
    dc_sha256(buffer,(size_t)n,hash);return 1;
}
static int utf8(const char *text,size_t n){
    if(!text||!n||memchr(text,0,n))return 0;
    const unsigned char *s=(const unsigned char *)text;
    for(size_t i=0;i<n;){
        unsigned c=s[i++],extra=0;uint32_t value=0,min=0;
        if(c<0x80)continue;
        if(c>=0xc2&&c<=0xdf){extra=1;value=c&31;min=0x80;}
        else if(c>=0xe0&&c<=0xef){extra=2;value=c&15;min=0x800;}
        else if(c>=0xf0&&c<=0xf4){extra=3;value=c&7;min=0x10000;}
        else return 0;
        if(extra>n-i)return 0;
        for(unsigned k=0;k<extra;k++){c=s[i++];if((c&0xc0)!=0x80)return 0;value=(value<<6)|(c&63);}
        if(value<min||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return 0;
    }
    return 1;
}
static sqlite3_stmt *prepare(sqlite3 *db,const char *sql){
    sqlite3_stmt *q=NULL;
    if(sqlite3_prepare_v2(db,sql,-1,&q,NULL)!=SQLITE_OK){sqlite3_finalize(q);return NULL;}
    return q;
}
static int text(sqlite3_stmt *q,int i,const char *s){return sqlite3_bind_text(q,i,s,-1,SQLITE_TRANSIENT)==SQLITE_OK;}
static int execute(sqlite3 *db,const char *sql){return sqlite3_exec(db,sql,NULL,NULL,NULL)==SQLITE_OK;}
static int column_id(sqlite3_stmt *q,int i,char out[129],int empty){
    const char *s=(const char *)sqlite3_column_text(q,i);
    int n=sqlite3_column_bytes(q,i);
    if(sqlite3_column_type(q,i)!=SQLITE_TEXT||!s||n<0||n>128||strlen(s)!=(size_t)n||!ident(s,empty))return 0;
    memcpy(out,s,(size_t)n+1);return 1;
}
static int binding_valid(sqlite3 *db,const char *principal){
    sqlite3_stmt *q=prepare(db,"SELECT version,principal FROM agent_mailbox_meta");
    if(!q)return 0;
    int rc=sqlite3_step(q),ok=rc==SQLITE_ROW&&sqlite3_column_type(q,0)==SQLITE_INTEGER&&sqlite3_column_int(q,0)==2;
    const char *s=ok?(const char *)sqlite3_column_text(q,1):NULL;
    ok=ok&&sqlite3_column_type(q,1)==SQLITE_TEXT&&s&&strlen(s)==(size_t)sqlite3_column_bytes(q,1)&&!strcmp(s,principal);
    if(ok)ok=sqlite3_step(q)==SQLITE_DONE;
    sqlite3_finalize(q);
    return ok;
}
static int schema_valid(sqlite3 *db,const char *principal){
    if(!binding_valid(db,principal))return 0;
    sqlite3_stmt *q=prepare(db,"SELECT project,agent,enabled FROM agent_mailbox_members LIMIT 0");
    int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);
    if(!ok)return 0;
    q=prepare(db,"SELECT sequence,project,sender,receiver,message_id,reply_to,payload,payload_hash,envelope_hash,ack_sequence,ack_hash FROM agent_mailbox_messages LIMIT 0");
    ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;
}
static dl_agent_mailbox *open_mailbox(sqlite3 *db,const char *principal,size_t payload,unsigned pending,int reader){
    if(!db||!ident(principal,0)||!payload||payload>1048576||!pending||pending>4096)return NULL;
    if(!reader){
        if(sqlite3_db_readonly(db,"main")!=0||!sqlite3_get_autocommit(db)||!execute(db,"BEGIN IMMEDIATE"))return NULL;
        sqlite3_stmt *q=prepare(db,"SELECT count(*) FROM sqlite_master WHERE name IN('agent_mailbox_meta','agent_mailbox_members','agent_mailbox_messages')");
        int count=q&&sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int(q,0):-1;sqlite3_finalize(q);
        int ok=count==3?schema_valid(db,principal):0;
        if(count==0){
            ok=execute(db,
                "CREATE TABLE agent_mailbox_meta(version INTEGER NOT NULL,principal TEXT NOT NULL);"
                "CREATE TABLE agent_mailbox_members(project TEXT NOT NULL,agent TEXT NOT NULL,enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),PRIMARY KEY(project,agent));"
                "CREATE TABLE agent_mailbox_messages(sequence INTEGER PRIMARY KEY AUTOINCREMENT,project TEXT NOT NULL,sender TEXT NOT NULL,receiver TEXT NOT NULL,message_id TEXT NOT NULL,reply_to TEXT NOT NULL,payload BLOB NOT NULL,payload_hash TEXT NOT NULL,envelope_hash TEXT NOT NULL,ack_sequence INTEGER,ack_hash TEXT,UNIQUE(project,sender,message_id),CHECK((ack_sequence IS NULL AND ack_hash IS NULL) OR (ack_sequence IS NOT NULL AND ack_hash IS NOT NULL AND ack_sequence>=0 AND length(ack_hash)=64)));"
                "CREATE INDEX agent_mailbox_pending ON agent_mailbox_messages(project,receiver,sequence) WHERE ack_sequence IS NULL;");
            q=ok?prepare(db,"INSERT INTO agent_mailbox_meta VALUES(2,?)"):NULL;
            ok=q&&text(q,1,principal)&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);
        }
        if(!ok||!execute(db,"COMMIT")){execute(db,"ROLLBACK");return NULL;}
    }else if(!schema_valid(db,principal))return NULL;
    dl_agent_mailbox *s=malloc(sizeof *s);
    if(s){*s=(dl_agent_mailbox){.db=db,.max_payload=payload,.max_pending=pending,.readonly=reader};memcpy(s->principal,principal,strlen(principal)+1);}
    return s;
}
dl_agent_mailbox *dl_agent_mailbox_open(sqlite3 *db,const char *p,size_t n,unsigned max){return open_mailbox(db,p,n,max,0);}
dl_agent_mailbox *dl_agent_mailbox_reader(sqlite3 *db,const char *p,size_t n,unsigned max){return open_mailbox(db,p,n,max,1);}
void dl_agent_mailbox_close(dl_agent_mailbox *s){free(s);}
static dl_mail_status member_active(dl_agent_mailbox *s,const char *project,const char *agent){
    if(!binding_valid(s->db,s->principal))return DL_MAIL_STORAGE;
    sqlite3_stmt *q=prepare(s->db,"SELECT enabled FROM agent_mailbox_members WHERE project=? AND agent=?");
    if(!q)return DL_MAIL_STORAGE;
    int bound=text(q,1,project)&&text(q,2,agent),rc=bound?sqlite3_step(q):SQLITE_ERROR;
    dl_mail_status status=rc==SQLITE_DONE?DL_MAIL_DENIED:DL_MAIL_STORAGE;
    if(rc==SQLITE_ROW&&sqlite3_column_type(q,0)==SQLITE_INTEGER){
        int n=sqlite3_column_int(q,0);status=n==1?DL_MAIL_OK:n==0?DL_MAIL_DENIED:DL_MAIL_STORAGE;
    }
    sqlite3_finalize(q);return status;
}
static dl_mail_status begin(dl_agent_mailbox *s){
    if(s->readonly)return DL_MAIL_DENIED;
    if(!sqlite3_get_autocommit(s->db))return DL_MAIL_TRANSACTION;
    if(!execute(s->db,"BEGIN IMMEDIATE"))return DL_MAIL_STORAGE;
    if(binding_valid(s->db,s->principal))return DL_MAIL_OK;
    execute(s->db,"ROLLBACK");return DL_MAIL_STORAGE;
}
static dl_mail_status finish(dl_agent_mailbox *s,dl_mail_status status){
    if(status==DL_MAIL_OK&&execute(s->db,"COMMIT"))return status;
    execute(s->db,"ROLLBACK");return status==DL_MAIL_OK?DL_MAIL_STORAGE:status;
}
static dl_mail_status read_finish(dl_agent_mailbox *s,int owned,dl_mail_status status){
    if(!owned)return status;
    if(status==DL_MAIL_OK&&execute(s->db,"COMMIT"))return status;
    execute(s->db,"ROLLBACK");return status==DL_MAIL_OK?DL_MAIL_STORAGE:status;
}
dl_mail_status dl_agent_mailbox_member(dl_agent_mailbox *s,const char *project,const char *agent,int enabled){
    if(!s||!ident(project,0)||!ident(agent,0)||(enabled!=0&&enabled!=1))return DL_MAIL_INVALID;
    dl_mail_status status=begin(s);if(status!=DL_MAIL_OK)return status;
    sqlite3_stmt *q=prepare(s->db,"INSERT INTO agent_mailbox_members VALUES(?,?,?) ON CONFLICT(project,agent) DO UPDATE SET enabled=excluded.enabled");
    int ok=q&&text(q,1,project)&&text(q,2,agent)&&sqlite3_bind_int(q,3,enabled)==SQLITE_OK&&sqlite3_step(q)==SQLITE_DONE;
    sqlite3_finalize(q);return finish(s,ok?DL_MAIL_OK:DL_MAIL_STORAGE);
}
dl_mail_status dl_agent_mailbox_send(dl_agent_mailbox *s,const char *project,const char *sender,const char *receiver,const char *mid,const char *reply,const char *body,size_t n,int64_t *sequence){
    if(sequence)*sequence=0;
    if(!s||!sequence||!ident(project,0)||!ident(sender,0)||!ident(receiver,0)||!ident(mid,0)||!ident(reply?reply:"",1)||!body||!n)return DL_MAIL_INVALID;
    if(n>s->max_payload)return DL_MAIL_LIMIT;
    if(!utf8(body,n))return DL_MAIL_INVALID;
    dl_mail_status status=begin(s);if(status!=DL_MAIL_OK)return status;
    status=member_active(s,project,sender);if(status==DL_MAIL_OK)status=member_active(s,project,receiver);
    if(status!=DL_MAIL_OK)return finish(s,status);
    char hash[65],source_hash[65];dc_sha256(body,n,hash);
    if(!envelope(s,project,sender,receiver,mid,reply?reply:"",hash,source_hash))return finish(s,DL_MAIL_INVALID);
    sqlite3_stmt *q=prepare(s->db,"SELECT sequence,receiver,reply_to,payload,payload_hash,envelope_hash FROM agent_mailbox_messages WHERE project=? AND sender=? AND message_id=?");
    int bound=q&&text(q,1,project)&&text(q,2,sender)&&text(q,3,mid),rc=bound?sqlite3_step(q):SQLITE_ERROR;
    int64_t id=0;
    if(rc==SQLITE_ROW){
        const char *to=(const char *)sqlite3_column_text(q,1),*ref=(const char *)sqlite3_column_text(q,2),*sha=(const char *)sqlite3_column_text(q,4),*stored_source=(const char *)sqlite3_column_text(q,5);
        int equal=sqlite3_column_type(q,0)==SQLITE_INTEGER&&sqlite3_column_int64(q,0)>0&&to&&ref&&sha
            &&sqlite3_column_type(q,1)==SQLITE_TEXT&&strlen(to)==(size_t)sqlite3_column_bytes(q,1)
            &&sqlite3_column_type(q,2)==SQLITE_TEXT&&strlen(ref)==(size_t)sqlite3_column_bytes(q,2)
            &&sqlite3_column_type(q,4)==SQLITE_TEXT&&strlen(sha)==(size_t)sqlite3_column_bytes(q,4)
            &&sqlite3_column_type(q,5)==SQLITE_TEXT&&stored_source&&strlen(stored_source)==(size_t)sqlite3_column_bytes(q,5)&&!strcmp(stored_source,source_hash)
            &&!strcmp(to,receiver)&&!strcmp(ref,reply?reply:"")&&!strcmp(sha,hash)
            &&sqlite3_column_type(q,3)==SQLITE_BLOB&&sqlite3_column_bytes(q,3)==(int)n&&sqlite3_column_blob(q,3)&&!memcmp(sqlite3_column_blob(q,3),body,n);
        if(equal)id=sqlite3_column_int64(q,0);
        sqlite3_finalize(q);status=finish(s,equal?DL_MAIL_OK:DL_MAIL_STALE);if(status==DL_MAIL_OK)*sequence=id;return status;
    }
    sqlite3_finalize(q);if(rc!=SQLITE_DONE)return finish(s,DL_MAIL_STORAGE);
    q=prepare(s->db,"SELECT count(*) FROM agent_mailbox_messages WHERE project=? AND receiver=? AND ack_sequence IS NULL");
    bound=q&&text(q,1,project)&&text(q,2,receiver);rc=bound?sqlite3_step(q):SQLITE_ERROR;
    int full=rc==SQLITE_ROW&&sqlite3_column_int64(q,0)>=(sqlite3_int64)s->max_pending;
    sqlite3_finalize(q);if(rc!=SQLITE_ROW||full)return finish(s,full?DL_MAIL_LIMIT:DL_MAIL_STORAGE);
    q=prepare(s->db,"INSERT INTO agent_mailbox_messages(project,sender,receiver,message_id,reply_to,payload,payload_hash,envelope_hash) VALUES(?,?,?,?,?,?,?,?)");
    int ok=q&&text(q,1,project)&&text(q,2,sender)&&text(q,3,receiver)&&text(q,4,mid)&&text(q,5,reply?reply:"")
        &&sqlite3_bind_blob(q,6,body,(int)n,SQLITE_TRANSIENT)==SQLITE_OK&&text(q,7,hash)&&text(q,8,source_hash)&&sqlite3_step(q)==SQLITE_DONE;
    if(ok)id=sqlite3_last_insert_rowid(s->db);sqlite3_finalize(q);
    status=finish(s,ok&&id>0?DL_MAIL_OK:DL_MAIL_STORAGE);if(status==DL_MAIL_OK)*sequence=id;return status;
}
/* Shared by delivery and acknowledgement; routing metadata is protected too. */
static int message_valid(dl_agent_mailbox *s,sqlite3_stmt *q,const char *project,const char *receiver,dl_agent_message *m){
    int bytes=sqlite3_column_bytes(q,6);
    const char *body=sqlite3_column_blob(q,6),*sha=(const char *)sqlite3_column_text(q,7),*source=(const char *)sqlite3_column_text(q,8);
    int valid=sqlite3_column_type(q,0)==SQLITE_INTEGER&&sqlite3_column_int64(q,0)>0
        &&column_id(q,1,m->project,0)&&column_id(q,2,m->sender,0)&&column_id(q,3,m->receiver,0)
        &&column_id(q,4,m->message_id,0)&&column_id(q,5,m->reply_to,1)
        &&!strcmp(m->project,project)&&!strcmp(m->receiver,receiver)
        &&sqlite3_column_type(q,6)==SQLITE_BLOB&&bytes>0&&(size_t)bytes<=s->max_payload&&utf8(body,(size_t)bytes)
        &&sqlite3_column_type(q,7)==SQLITE_TEXT&&sha&&strlen(sha)==(size_t)sqlite3_column_bytes(q,7)&&digest(sha)
        &&sqlite3_column_type(q,8)==SQLITE_TEXT&&source&&strlen(source)==(size_t)sqlite3_column_bytes(q,8)&&digest(source);
    char actual[65];if(valid){dc_sha256(body,(size_t)bytes,actual);valid=!strcmp(actual,sha);}
    if(valid)valid=envelope(s,m->project,m->sender,m->receiver,m->message_id,m->reply_to,sha,actual)&&!strcmp(actual,source);
    if(valid&&sqlite3_column_type(q,9)==SQLITE_NULL)valid=sqlite3_column_type(q,10)==SQLITE_NULL;
    else if(valid){
        const char *receipt=(const char *)sqlite3_column_text(q,10);
        valid=sqlite3_column_type(q,9)==SQLITE_INTEGER&&sqlite3_column_int64(q,9)>=0&&sqlite3_column_type(q,10)==SQLITE_TEXT
            &&receipt&&strlen(receipt)==(size_t)sqlite3_column_bytes(q,10)&&digest(receipt);
    }
    if(!valid)return 0;
    m->sequence=sqlite3_column_int64(q,0);m->bytes=(size_t)bytes;
    memcpy(m->payload_sha256,sha,65);memcpy(m->envelope_sha256,source,65);return 1;
}
void dl_agent_messages_free(dl_agent_message *a,size_t count){if(a){for(size_t i=0;i<count;i++)free(a[i].text);free(a);}}
dl_mail_status dl_agent_mailbox_pending(dl_agent_mailbox *s,const char *project,const char *receiver,int64_t after,unsigned limit,dl_agent_message **messages,size_t *count,int *more,int64_t *cursor){
    if(messages)*messages=NULL;if(count)*count=0;if(more)*more=0;if(cursor)*cursor=0;
    if(!s||!messages||!count||!more||!cursor||!ident(project,0)||!ident(receiver,0)||after<0||!limit||limit>200)return DL_MAIL_INVALID;
    int owned=sqlite3_get_autocommit(s->db);
    if(owned&&!execute(s->db,"BEGIN"))return DL_MAIL_STORAGE;
    dl_mail_status status=member_active(s,project,receiver);if(status!=DL_MAIL_OK)return read_finish(s,owned,status);
    sqlite3_stmt *q=prepare(s->db,"SELECT sequence,project,sender,receiver,message_id,reply_to,payload,payload_hash,envelope_hash,ack_sequence,ack_hash FROM agent_mailbox_messages WHERE project=? AND receiver=? AND ack_sequence IS NULL AND sequence>? ORDER BY sequence LIMIT ?");
    int bound=q&&text(q,1,project)&&text(q,2,receiver)&&sqlite3_bind_int64(q,3,after)==SQLITE_OK&&sqlite3_bind_int(q,4,(int)limit+1)==SQLITE_OK;
    if(!bound){sqlite3_finalize(q);return read_finish(s,owned,DL_MAIL_STORAGE);}
    dl_agent_message *out=calloc(limit,sizeof *out);if(!out){sqlite3_finalize(q);return read_finish(s,owned,DL_MAIL_NOMEM);}
    size_t used=0;int rc,has_more=0;status=DL_MAIL_OK;
    while((rc=sqlite3_step(q))==SQLITE_ROW){
        if(used==limit){has_more=1;break;}
        dl_agent_message *m=&out[used];
        if(!message_valid(s,q,project,receiver,m)||m->sequence<=after){status=DL_MAIL_STORAGE;break;}
        m->text=malloc(m->bytes+1);if(!m->text){status=DL_MAIL_NOMEM;break;}
        memcpy(m->text,sqlite3_column_blob(q,6),m->bytes);m->text[m->bytes]=0;used++;
    }
    if(status==DL_MAIL_OK&&rc!=SQLITE_DONE&&!(has_more&&rc==SQLITE_ROW))status=DL_MAIL_STORAGE;
    sqlite3_finalize(q);status=read_finish(s,owned,status);
    if(status!=DL_MAIL_OK){dl_agent_messages_free(out,used);return status;}
    if(!used){free(out);out=NULL;}
    *messages=out;*count=used;*more=has_more;*cursor=used?out[used-1].sequence:after;return DL_MAIL_OK;
}
dl_mail_status dl_agent_mailbox_ack(dl_agent_mailbox *s,const char *project,const char *receiver,int64_t sequence,int64_t canonical,const char *hash){
    if(!s||!ident(project,0)||!ident(receiver,0)||sequence<=0||canonical<0||!digest(hash))return DL_MAIL_INVALID;
    if(s->readonly)return DL_MAIL_DENIED;
    if(sqlite3_get_autocommit(s->db)||sqlite3_txn_state(s->db,"main")!=SQLITE_TXN_WRITE)return DL_MAIL_TRANSACTION;
    dl_mail_status status=member_active(s,project,receiver);if(status!=DL_MAIL_OK)return status;
    sqlite3_stmt *q=prepare(s->db,"SELECT sequence,project,sender,receiver,message_id,reply_to,payload,payload_hash,envelope_hash,ack_sequence,ack_hash FROM agent_mailbox_messages WHERE sequence=? AND project=? AND receiver=?");
    int bound=q&&sqlite3_bind_int64(q,1,sequence)==SQLITE_OK&&text(q,2,project)&&text(q,3,receiver),rc=bound?sqlite3_step(q):SQLITE_ERROR;
    if(rc!=SQLITE_ROW){sqlite3_finalize(q);return rc==SQLITE_DONE?DL_MAIL_NOT_FOUND:DL_MAIL_STORAGE;}
    dl_agent_message verified={0};
    if(!message_valid(s,q,project,receiver,&verified)){sqlite3_finalize(q);return DL_MAIL_STORAGE;}
    if(sqlite3_column_type(q,9)!=SQLITE_NULL){
        const char *prior=(const char *)sqlite3_column_text(q,10);
        int equal=sqlite3_column_type(q,9)==SQLITE_INTEGER&&sqlite3_column_int64(q,9)==canonical&&sqlite3_column_type(q,10)==SQLITE_TEXT&&prior&&strlen(prior)==(size_t)sqlite3_column_bytes(q,10)&&!strcmp(prior,hash);
        sqlite3_finalize(q);return equal?DL_MAIL_OK:DL_MAIL_STALE;
    }
    int unacked=sqlite3_column_type(q,10)==SQLITE_NULL;sqlite3_finalize(q);if(!unacked)return DL_MAIL_STORAGE;
    q=prepare(s->db,"UPDATE agent_mailbox_messages SET ack_sequence=?,ack_hash=? WHERE sequence=? AND project=? AND receiver=? AND ack_sequence IS NULL AND ack_hash IS NULL");
    int ok=q&&sqlite3_bind_int64(q,1,canonical)==SQLITE_OK&&text(q,2,hash)&&sqlite3_bind_int64(q,3,sequence)==SQLITE_OK&&text(q,4,project)&&text(q,5,receiver)&&sqlite3_step(q)==SQLITE_DONE;
    sqlite3_finalize(q);return ok&&sqlite3_changes(s->db)==1?DL_MAIL_OK:DL_MAIL_STORAGE;
}
