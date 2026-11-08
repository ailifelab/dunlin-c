#include "dunlin/attachment_store.h"
#include "dunlin/image_resource.h"
#include "dunlin/content_hash.h"
#include <sqlite3.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct dl_attachment_store { sqlite3 *db; uint64_t quota; int writable; };
static int execute(dl_attachment_store *s,const char *sql) {
    return sqlite3_exec(s->db,sql,NULL,NULL,NULL)==SQLITE_OK;
}
static sqlite3_stmt *prepare(dl_attachment_store *s,const char *sql) {
    sqlite3_stmt *q=NULL;
    if(sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)!=SQLITE_OK) return NULL;
    return q;
}
static void text(sqlite3_stmt *q,int i,const char *v) {
    sqlite3_bind_text(q,i,v,-1,SQLITE_TRANSIENT);
}
static void payload(sqlite3_stmt *q,int i,const void *p,size_t n) {
    /* SQLite treats a NULL pointer as SQL NULL even for zero-length data. */
    sqlite3_bind_blob(q,i,n?p:"",(int)n,SQLITE_TRANSIENT);
}
static int ascii_id(const char *v,size_t min,size_t max,int hex) {
    if(!v)return 0;
    size_t n=0;
    for(;v[n]&&n<=max;n++) {
        unsigned char c=(unsigned char)v[n];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<=(hex?'f':'z'))
            ||(!hex&&((c>='A'&&c<='Z')||c=='_'||c=='-'))))return 0;
    }
    return n>=min&&n<=max;
}
static int utf8(const unsigned char *p,size_t n,int name) {
    for(size_t i=0;i<n;) {
        unsigned c=p[i++],count;uint32_t value,minimum;
        if(!c||(name&&(c<32||c==127)))return 0;
        if(c<128)continue;
        if(c>=0xc2&&c<=0xdf){count=1;value=c&31;minimum=0x80;}
        else if(c>=0xe0&&c<=0xef){count=2;value=c&15;minimum=0x800;}
        else if(c>=0xf0&&c<=0xf4){count=3;value=c&7;minimum=0x10000;}
        else return 0;
        if(count>n-i)return 0;
        for(unsigned j=0;j<count;j++){if((p[i]&0xc0)!=0x80)return 0;value=value<<6|(p[i++]&63);}
        if(value<minimum||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return 0;
    }
    return 1;
}
static int display_name(const char *v) {
    if(!v)return 0;
    size_t n=0;while(n<=256&&v[n])n++;
    return n&&n<=256&&utf8((const unsigned char*)v,n,1);
}
static int owner_ok(dl_attachment_owner_kind kind,const char *owner) {
    return kind>=DL_ATTACHMENT_CHAT&&kind<=DL_ATTACHMENT_EXTENSION&&ascii_id(owner,1,64,0);
}
static int kind_ok(const char *v) {
    return v&&(!strcmp(v,"file")||!strcmp(v,"clipboard")||!strcmp(v,"screenshot"));
}
static void scope(sqlite3_stmt *q,int index,dl_attachment_owner_kind kind,const char *owner) {
    sqlite3_bind_int(q,index,(int)kind);text(q,index+1,owner);
}
static int column(sqlite3_stmt *q,int i,char *out,size_t max,int hex) {
    if(sqlite3_column_type(q,i)!=SQLITE_TEXT)return 0;
    const char *p=(const char*)sqlite3_column_text(q,i);int n=sqlite3_column_bytes(q,i);
    if(!p||n<1||(size_t)n>max||strlen(p)!=(size_t)n||(hex&&!ascii_id(p,max,max,1)))return 0;
    memcpy(out,p,(size_t)n+1);return 1;
}
static int collect(dl_attachment_store *s) {
    return execute(s,"DELETE FROM attachment_resources WHERE NOT EXISTS(SELECT 1 FROM attachment_drafts d WHERE d.sha=attachment_resources.sha) AND NOT EXISTS(SELECT 1 FROM attachment_messages m WHERE m.sha=attachment_resources.sha)");
}
dl_attachment_status dl_attachment_inspect(dl_attachment_format format,const void *p,size_t n,dl_attachment_info *out) {
    if(out)memset(out,0,sizeof *out);
    if(!out||(!p&&n))return DL_ATTACHMENT_INVALID;
    dl_attachment_info value={0};value.format=format;value.bytes=n;
    if(format==DL_ATTACHMENT_PNG) {
        dl_image_info image;dl_image_status status=dl_png_inspect(p,n,&image);
        if(status!=DL_IMAGE_OK)return status==DL_IMAGE_LIMIT?DL_ATTACHMENT_LIMIT:
            status==DL_IMAGE_NOMEM?DL_ATTACHMENT_NOMEM:DL_ATTACHMENT_INVALID;
        memcpy(value.sha256,image.sha256,sizeof value.sha256);value.width=image.width;value.height=image.height;
    } else if(format==DL_ATTACHMENT_TEXT) {
        if(n>65536)return DL_ATTACHMENT_LIMIT;
        if(!utf8(p,n,0))return DL_ATTACHMENT_INVALID;
        dc_sha256(n?p:"",n,value.sha256);
    } else return DL_ATTACHMENT_INVALID;
    *out=value;return DL_ATTACHMENT_OK;
}
const char *dl_attachment_mime(dl_attachment_format format) {
    return format==DL_ATTACHMENT_PNG?"image/png":format==DL_ATTACHMENT_TEXT?"text/plain; charset=utf-8":NULL;
}
void dl_attachment_free(void *p) {free(p);}
dl_attachment_store *dl_attachments_reader(sqlite3 *db) {
    if(!db)return NULL;
    dl_attachment_store *s=calloc(1,sizeof *s);if(s)s->db=db;return s;
}
dl_attachment_store *dl_attachments_open(sqlite3 *db,uint64_t quota) {
    if(!db||!quota||quota>INT64_MAX||!sqlite3_get_autocommit(db)||sqlite3_db_readonly(db,"main")!=0)return NULL;
    dl_attachment_store *s=dl_attachments_reader(db);if(!s)return NULL;s->quota=quota;s->writable=1;
    if(!execute(s,"BEGIN IMMEDIATE")){free(s);return NULL;}
    int ok=execute(s,
        "CREATE TABLE IF NOT EXISTS attachment_resources(sha TEXT PRIMARY KEY CHECK(length(sha)=64),format INTEGER NOT NULL CHECK(format IN(1,2)),width INTEGER NOT NULL,height INTEGER NOT NULL,data BLOB NOT NULL CHECK(typeof(data)='blob'),CHECK((format=1 AND width BETWEEN 1 AND 1024 AND height BETWEEN 1 AND 1024 AND length(data) BETWEEN 1 AND 1048576) OR (format=2 AND width=0 AND height=0 AND length(data)<=65536)));"
        "CREATE TABLE IF NOT EXISTS attachment_drafts(scope INTEGER NOT NULL CHECK(scope BETWEEN 1 AND 4),owner TEXT NOT NULL,id TEXT NOT NULL UNIQUE CHECK(length(id)=32),sha TEXT NOT NULL,name TEXT NOT NULL,kind TEXT NOT NULL CHECK(kind IN('file','clipboard','screenshot')),PRIMARY KEY(scope,owner));"
        "CREATE INDEX IF NOT EXISTS attachment_draft_sha ON attachment_drafts(sha);"
        "CREATE TABLE IF NOT EXISTS attachment_messages(scope INTEGER NOT NULL CHECK(scope BETWEEN 1 AND 4),owner TEXT NOT NULL,seq INTEGER NOT NULL CHECK(seq>=0),id TEXT NOT NULL CHECK(length(id)=32),sha TEXT NOT NULL,name TEXT NOT NULL,kind TEXT NOT NULL CHECK(kind IN('file','clipboard','screenshot')),source_hash TEXT NOT NULL CHECK(length(source_hash)=64),PRIMARY KEY(scope,owner,seq));"
        "CREATE INDEX IF NOT EXISTS attachment_message_sha ON attachment_messages(sha);");
    if(ok)ok=execute(s,"COMMIT");
    if(!ok){execute(s,"ROLLBACK");free(s);return NULL;}return s;
}
void dl_attachments_close(dl_attachment_store *s){free(s);}
dl_attachment_status dl_attachments_usage(dl_attachment_store *s,uint64_t *bytes) {
    if(bytes)*bytes=0;if(!s||!bytes)return DL_ATTACHMENT_INVALID;
    sqlite3_stmt *q=prepare(s,"SELECT COALESCE(sum(length(data)),0),COALESCE(sum(CASE WHEN typeof(data)!='blob' OR typeof(format)!='integer' OR typeof(width)!='integer' OR typeof(height)!='integer' OR NOT ((format=1 AND width BETWEEN 1 AND 1024 AND height BETWEEN 1 AND 1024 AND length(data) BETWEEN 1 AND 1048576) OR (format=2 AND width=0 AND height=0 AND length(data)<=65536)) THEN 1 ELSE 0 END),0) FROM attachment_resources");
    dl_attachment_status status=DL_ATTACHMENT_STORAGE;
    if(q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_type(q,0)==SQLITE_INTEGER&&sqlite3_column_int64(q,0)>=0&&sqlite3_column_int64(q,1)==0){*bytes=(uint64_t)sqlite3_column_int64(q,0);status=DL_ATTACHMENT_OK;}
    sqlite3_finalize(q);return status;
}
/* Selected row: ID/name/kind/SHA/format/width/height/payload size. */
static dl_attachment_status metadata(sqlite3_stmt *q,dl_attachment_draft *out) {
    dl_attachment_draft v={0};
    if(!column(q,0,v.id,32,1)||!column(q,1,v.name,256,0)||!display_name(v.name)||!column(q,2,v.kind,15,0)||!kind_ok(v.kind)||!column(q,3,v.content.sha256,64,1))return DL_ATTACHMENT_STORAGE;
    for(int i=4;i<=7;i++)if(sqlite3_column_type(q,i)!=SQLITE_INTEGER)return DL_ATTACHMENT_STORAGE;
    sqlite3_int64 f=sqlite3_column_int64(q,4),w=sqlite3_column_int64(q,5),h=sqlite3_column_int64(q,6),n=sqlite3_column_int64(q,7);
    if(!((f==DL_ATTACHMENT_PNG&&w>=1&&w<=1024&&h>=1&&h<=1024&&n>=1&&n<=1048576)
        ||(f==DL_ATTACHMENT_TEXT&&w==0&&h==0&&n>=0&&n<=65536)))return DL_ATTACHMENT_STORAGE;
    v.content.format=(dl_attachment_format)f;v.content.width=(unsigned)w;v.content.height=(unsigned)h;v.content.bytes=(size_t)n;*out=v;return DL_ATTACHMENT_OK;
}
static dl_attachment_status lookup(dl_attachment_store *s,dl_attachment_owner_kind kind,const char *owner,
    int64_t seq,const char *id,const char *hash,int data,void **bytes,size_t *size,dl_attachment_draft *out) {
    if(out)memset(out,0,sizeof *out);if(bytes)*bytes=NULL;if(size)*size=0;
    if(!s||!out||!owner_ok(kind,owner)||seq< -1||(data&&(!bytes||!size))
        ||(seq>=0&&!ascii_id(hash,64,64,1))||(seq<0&&data&&!ascii_id(id,32,32,1)))return DL_ATTACHMENT_INVALID;
    const char *sql=seq<0?(data?
        "SELECT d.id,d.name,d.kind,r.sha,r.format,r.width,r.height,length(r.data),r.data FROM attachment_drafts d LEFT JOIN attachment_resources r ON r.sha=d.sha WHERE d.scope=? AND d.owner=? AND d.id=?":
        "SELECT d.id,d.name,d.kind,r.sha,r.format,r.width,r.height,length(r.data) FROM attachment_drafts d LEFT JOIN attachment_resources r ON r.sha=d.sha WHERE d.scope=? AND d.owner=?"):(data?
        "SELECT m.id,m.name,m.kind,r.sha,r.format,r.width,r.height,length(r.data),r.data,m.source_hash FROM attachment_messages m LEFT JOIN attachment_resources r ON r.sha=m.sha WHERE m.scope=? AND m.owner=? AND m.seq=?":
        "SELECT m.id,m.name,m.kind,r.sha,r.format,r.width,r.height,length(r.data),NULL,m.source_hash FROM attachment_messages m LEFT JOIN attachment_resources r ON r.sha=m.sha WHERE m.scope=? AND m.owner=? AND m.seq=?");
    sqlite3_stmt *q=prepare(s,sql);if(!q)return DL_ATTACHMENT_STORAGE;
    scope(q,1,kind,owner);if(seq>=0)sqlite3_bind_int64(q,3,seq);else if(data)text(q,3,id);
    int row=sqlite3_step(q);dl_attachment_status status=DL_ATTACHMENT_STORAGE;dl_attachment_draft value;
    if(row!=SQLITE_ROW){status=row==SQLITE_DONE?DL_ATTACHMENT_NOT_FOUND:DL_ATTACHMENT_STORAGE;goto done;}
    if(seq>=0){char source[65];if(!column(q,9,source,64,1))goto done;if(strcmp(source,hash)){status=DL_ATTACHMENT_STALE;goto done;}}
    if(metadata(q,&value)!=DL_ATTACHMENT_OK)goto done;
    if(data) {
        if(sqlite3_column_type(q,8)!=SQLITE_BLOB)goto done;
        const void *p=sqlite3_column_blob(q,8);int n=sqlite3_column_bytes(q,8);
        if(n!=(int)value.content.bytes||(!p&&n))goto done;
        dl_attachment_info actual;
        if(dl_attachment_inspect(value.content.format,p,(size_t)n,&actual)!=DL_ATTACHMENT_OK
            ||strcmp(actual.sha256,value.content.sha256)||actual.width!=value.content.width||actual.height!=value.content.height)goto done;
        void *copy=malloc((size_t)n+1);if(!copy){status=DL_ATTACHMENT_NOMEM;goto done;}
        if(n)memcpy(copy,p,(size_t)n);((unsigned char*)copy)[n]=0;*bytes=copy;*size=(size_t)n;
    }
    *out=value;status=DL_ATTACHMENT_OK;
done:sqlite3_finalize(q);return status;
}
dl_attachment_status dl_attachments_draft(dl_attachment_store *s,dl_attachment_owner_kind kind,const char *owner,dl_attachment_draft *out) {
    return lookup(s,kind,owner,-1,NULL,NULL,0,NULL,NULL,out);
}
dl_attachment_status dl_attachments_info(dl_attachment_store *s,dl_attachment_owner_kind kind,const char *owner,int64_t seq,const char *hash,dl_attachment_draft *out) {
    if(seq<0){if(out)memset(out,0,sizeof *out);return DL_ATTACHMENT_INVALID;}
    return lookup(s,kind,owner,seq,NULL,hash,0,NULL,NULL,out);
}
dl_attachment_status dl_attachments_read(dl_attachment_store *s,dl_attachment_owner_kind kind,const char *owner,int64_t seq,const char *id,const char *hash,void **bytes,size_t *size,dl_attachment_draft *out) {
    return lookup(s,kind,owner,seq,id,hash,1,bytes,size,out);
}
dl_attachment_status dl_attachments_stage(dl_attachment_store *s,dl_attachment_owner_kind kind,const char *owner,const char *expected,const char *name,const char *origin,dl_attachment_format format,const void *p,size_t n,dl_attachment_draft *out) {
    if(out)memset(out,0,sizeof *out);
    if(!s||!out||!owner_ok(kind,owner)||(expected&&!ascii_id(expected,32,32,1))||!display_name(name)||!kind_ok(origin))return DL_ATTACHMENT_INVALID;
    if(!s->writable)return DL_ATTACHMENT_STORAGE;
    if(!sqlite3_get_autocommit(s->db))return DL_ATTACHMENT_TRANSACTION;
    dl_attachment_info info;dl_attachment_status status=dl_attachment_inspect(format,p,n,&info);
    if(status!=DL_ATTACHMENT_OK)return status;if(n>s->quota)return DL_ATTACHMENT_LIMIT;
    if(!execute(s,"BEGIN IMMEDIATE"))return DL_ATTACHMENT_STORAGE;
    dl_attachment_draft prior;status=dl_attachments_draft(s,kind,owner,&prior);
    if(status!=DL_ATTACHMENT_OK&&status!=DL_ATTACHMENT_NOT_FOUND)goto rollback;
    if((expected&&(status==DL_ATTACHMENT_NOT_FOUND||strcmp(expected,prior.id)))||(!expected&&status!=DL_ATTACHMENT_NOT_FOUND)){status=DL_ATTACHMENT_STALE;goto rollback;}
    status=DL_ATTACHMENT_OK;
    sqlite3_stmt *q=prepare(s,"INSERT OR IGNORE INTO attachment_resources(sha,format,width,height,data) VALUES(?,?,?,?,?)");
    if(!q)goto failed;text(q,1,info.sha256);sqlite3_bind_int(q,2,(int)format);sqlite3_bind_int(q,3,(int)info.width);sqlite3_bind_int(q,4,(int)info.height);payload(q,5,p,n);
    int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);q=NULL;if(!ok)goto failed;
    q=prepare(s,"SELECT 1 FROM attachment_resources WHERE sha=? AND format=? AND width=? AND height=? AND typeof(data)='blob' AND data=?");
    if(!q)goto failed;text(q,1,info.sha256);sqlite3_bind_int(q,2,(int)format);sqlite3_bind_int(q,3,(int)info.width);sqlite3_bind_int(q,4,(int)info.height);payload(q,5,p,n);
    ok=sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);q=NULL;if(!ok)goto failed;
    q=prepare(s,"INSERT INTO attachment_drafts(scope,owner,id,sha,name,kind) VALUES(?,?,lower(hex(randomblob(16))),?,?,?) ON CONFLICT(scope,owner) DO UPDATE SET id=excluded.id,sha=excluded.sha,name=excluded.name,kind=excluded.kind");
    if(!q)goto failed;scope(q,1,kind,owner);text(q,3,info.sha256);text(q,4,name);text(q,5,origin);
    ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);q=NULL;if(!ok||!collect(s))goto failed;
    uint64_t used=0;status=dl_attachments_usage(s,&used);if(status!=DL_ATTACHMENT_OK)goto failed;
    if(used>s->quota){status=DL_ATTACHMENT_LIMIT;goto failed;}
    dl_attachment_draft value;status=dl_attachments_draft(s,kind,owner,&value);if(status!=DL_ATTACHMENT_OK)goto failed;
    if(!execute(s,"COMMIT")){status=DL_ATTACHMENT_STORAGE;goto failed;}*out=value;return DL_ATTACHMENT_OK;
failed:sqlite3_finalize(q);if(status==DL_ATTACHMENT_OK)status=DL_ATTACHMENT_STORAGE;
rollback:execute(s,"ROLLBACK");return status;
}
dl_attachment_status dl_attachments_remove(dl_attachment_store *s,dl_attachment_owner_kind kind,const char *owner,const char *id) {
    if(!s||!owner_ok(kind,owner)||!ascii_id(id,32,32,1))return DL_ATTACHMENT_INVALID;
    if(!s->writable)return DL_ATTACHMENT_STORAGE;
    if(!sqlite3_get_autocommit(s->db))return DL_ATTACHMENT_TRANSACTION;
    if(!execute(s,"BEGIN IMMEDIATE"))return DL_ATTACHMENT_STORAGE;
    sqlite3_stmt *q=prepare(s,"DELETE FROM attachment_drafts WHERE scope=? AND owner=? AND id=?");
    if(q){scope(q,1,kind,owner);text(q,3,id);}int ok=q&&sqlite3_step(q)==SQLITE_DONE;
    int changes=sqlite3_changes(s->db);sqlite3_finalize(q);
    dl_attachment_status status=!ok?DL_ATTACHMENT_STORAGE:changes!=1?DL_ATTACHMENT_STALE:DL_ATTACHMENT_OK;
    if(status==DL_ATTACHMENT_OK&&(!collect(s)||!execute(s,"COMMIT")))status=DL_ATTACHMENT_STORAGE;
    if(status!=DL_ATTACHMENT_OK)execute(s,"ROLLBACK");return status;
}
dl_attachment_status dl_attachments_bind(dl_attachment_store *s,dl_attachment_owner_kind kind,const char *owner,const char *id,int64_t seq,const char *hash) {
    if(!s||!owner_ok(kind,owner)||!ascii_id(id,32,32,1)||seq<0||!ascii_id(hash,64,64,1))return DL_ATTACHMENT_INVALID;
    if(!s->writable)return DL_ATTACHMENT_STORAGE;
    if(sqlite3_get_autocommit(s->db)||sqlite3_txn_state(s->db,"main")!=SQLITE_TXN_WRITE)return DL_ATTACHMENT_TRANSACTION;
    dl_attachment_draft value;void *p=NULL;size_t n=0;
    dl_attachment_status status=dl_attachments_read(s,kind,owner,-1,id,NULL,&p,&n,&value);free(p);
    if(status!=DL_ATTACHMENT_OK)return status==DL_ATTACHMENT_NOT_FOUND?DL_ATTACHMENT_STALE:status;
    sqlite3_stmt *q=prepare(s,"SELECT 1 FROM attachment_messages WHERE scope=? AND owner=? AND seq=?");
    if(!q)return DL_ATTACHMENT_STORAGE;scope(q,1,kind,owner);sqlite3_bind_int64(q,3,seq);int row=sqlite3_step(q);sqlite3_finalize(q);
    if(row==SQLITE_ROW)return DL_ATTACHMENT_STALE;if(row!=SQLITE_DONE)return DL_ATTACHMENT_STORAGE;
    q=prepare(s,"INSERT INTO attachment_messages(scope,owner,seq,id,sha,name,kind,source_hash) SELECT scope,owner,?,id,sha,name,kind,? FROM attachment_drafts WHERE scope=? AND owner=? AND id=?");
    if(!q)return DL_ATTACHMENT_STORAGE;sqlite3_bind_int64(q,1,seq);text(q,2,hash);scope(q,3,kind,owner);text(q,5,id);
    int ok=sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;sqlite3_finalize(q);if(!ok)return DL_ATTACHMENT_STORAGE;
    q=prepare(s,"DELETE FROM attachment_drafts WHERE scope=? AND owner=? AND id=?");
    if(!q)return DL_ATTACHMENT_STORAGE;scope(q,1,kind,owner);text(q,3,id);
    ok=sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;sqlite3_finalize(q);return ok?DL_ATTACHMENT_OK:DL_ATTACHMENT_STORAGE;
}
dl_attachment_status dl_attachments_delete_owner(dl_attachment_store *s,dl_attachment_owner_kind kind,const char *owner) {
    if(!s||!owner_ok(kind,owner))return DL_ATTACHMENT_INVALID;
    if(!s->writable)return DL_ATTACHMENT_STORAGE;
    if(sqlite3_get_autocommit(s->db)||sqlite3_txn_state(s->db,"main")!=SQLITE_TXN_WRITE)return DL_ATTACHMENT_TRANSACTION;
    const char *sql[]={"DELETE FROM attachment_drafts WHERE scope=? AND owner=?","DELETE FROM attachment_messages WHERE scope=? AND owner=?"};
    for(unsigned i=0;i<2;i++){sqlite3_stmt *q=prepare(s,sql[i]);if(!q)return DL_ATTACHMENT_STORAGE;scope(q,1,kind,owner);int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);if(!ok)return DL_ATTACHMENT_STORAGE;}
    return collect(s)?DL_ATTACHMENT_OK:DL_ATTACHMENT_STORAGE;
}
