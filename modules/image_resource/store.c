#include "dunlin/image_resource.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct dl_image_store { sqlite3 *db; uint64_t quota; };
static int execute(dl_image_store *s,const char *sql) {
    return sqlite3_exec(s->db,sql,NULL,NULL,NULL)==SQLITE_OK;
}
static sqlite3_stmt *prepare(dl_image_store *s,const char *sql) {
    sqlite3_stmt *q=NULL;
    if(sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)!=SQLITE_OK) return NULL;
    return q;
}
static void bind(sqlite3_stmt *q,int index,const char *text) {
    sqlite3_bind_text(q,index,text,-1,SQLITE_TRANSIENT);
}
static int ascii_id(const char *v,size_t min,size_t max,int hex) {
    if(!v) return 0;
    size_t i=0;
    for(;v[i] && i<=max;++i) {
        unsigned char c=(unsigned char)v[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<=(hex?'f':'z'))
            ||(!hex&&((c>='A'&&c<='Z')||c=='_'||c=='-')))) return 0;
    }
    return i>=min && i<=max;
}
static int display_name(const char *v) {
    if(!v) return 0;
    size_t n=0;while(n<=256&&v[n])++n;
    if(!n||n>256) return 0;
    const unsigned char *p=(const unsigned char*)v;
    for(size_t i=0;i<n;) {
        unsigned char first=p[i++];
        if(first<32||first==127) return 0;
        if(first<128) continue;
        unsigned count;uint32_t value,minimum;
        if(first>=0xc2&&first<=0xdf){count=1;value=first&31;minimum=0x80;}
        else if(first>=0xe0&&first<=0xef){count=2;value=first&15;minimum=0x800;}
        else if(first>=0xf0&&first<=0xf4){count=3;value=first&7;minimum=0x10000;}
        else return 0;
        if(count>n-i) return 0;
        for(unsigned j=0;j<count;j++){if((p[i]&0xc0)!=0x80)return 0;value=value<<6|(p[i++]&63);}
        if(value<minimum||value>0x10ffff||(value>=0xd800&&value<=0xdfff)) return 0;
    }
    return 1;
}
static int column(sqlite3_stmt *q,int i,char *out,size_t max,int hex) {
    if(sqlite3_column_type(q,i)!=SQLITE_TEXT) return 0;
    const char *p=(const char*)sqlite3_column_text(q,i);int n=sqlite3_column_bytes(q,i);
    if(!p||n<1||(size_t)n>max||strlen(p)!=(size_t)n) return 0;
    if(hex&&!ascii_id(p,max,max,1)) return 0;
    memcpy(out,p,(size_t)n+1);return 1;
}
static int collect(dl_image_store *s) {
    return execute(s,"DELETE FROM image_resources WHERE NOT EXISTS(SELECT 1 FROM image_drafts d WHERE d.sha=image_resources.sha) AND NOT EXISTS(SELECT 1 FROM image_messages m WHERE m.sha=image_resources.sha)");
}
dl_image_store *dl_images_open(sqlite3 *db,uint64_t quota) {
    if(!db||!quota||quota>INT64_MAX||!sqlite3_get_autocommit(db)||sqlite3_db_readonly(db,"main")!=0) return NULL;
    dl_image_store *s=calloc(1,sizeof *s);if(!s)return NULL;s->db=db;s->quota=quota;
    if(!execute(s,"BEGIN IMMEDIATE")) {free(s);return NULL;}
    int ok=execute(s,
        "CREATE TABLE IF NOT EXISTS image_resources(sha TEXT PRIMARY KEY CHECK(length(sha)=64),width INTEGER NOT NULL CHECK(width BETWEEN 1 AND 1024),height INTEGER NOT NULL CHECK(height BETWEEN 1 AND 1024),data BLOB NOT NULL CHECK(length(data) BETWEEN 1 AND 1048576));"
        "CREATE TABLE IF NOT EXISTS image_drafts(owner TEXT PRIMARY KEY,id TEXT NOT NULL UNIQUE CHECK(length(id)=32),sha TEXT NOT NULL,name TEXT NOT NULL,kind TEXT NOT NULL CHECK(kind IN('file','clipboard','screenshot')));"
        "CREATE INDEX IF NOT EXISTS image_draft_sha ON image_drafts(sha);"
        "CREATE TABLE IF NOT EXISTS image_messages(owner TEXT NOT NULL,seq INTEGER NOT NULL CHECK(seq>=0),id TEXT NOT NULL CHECK(length(id)=32),sha TEXT NOT NULL,name TEXT NOT NULL,kind TEXT NOT NULL CHECK(kind IN('file','clipboard','screenshot')),source_hash TEXT NOT NULL CHECK(length(source_hash)=64),PRIMARY KEY(owner,seq));"
        "CREATE INDEX IF NOT EXISTS image_message_sha ON image_messages(sha);");
    if(ok)ok=execute(s,"COMMIT");
    if(!ok){execute(s,"ROLLBACK");free(s);return NULL;}
    return s;
}
void dl_images_close(dl_image_store *s) {free(s);}
dl_image_status dl_images_usage(dl_image_store *s,uint64_t *bytes) {
    if(bytes)*bytes=0;if(!s||!bytes)return DL_IMAGE_INVALID;
    sqlite3_stmt *q=prepare(s,"SELECT COALESCE(sum(length(data)),0),COALESCE(sum(CASE WHEN typeof(data)!='blob' OR length(data)<1 OR length(data)>1048576 THEN 1 ELSE 0 END),0) FROM image_resources");
    dl_image_status result=DL_IMAGE_STORAGE;
    if(q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_type(q,0)==SQLITE_INTEGER
       && sqlite3_column_int64(q,0)>=0 && sqlite3_column_int64(q,1)==0) {
        *bytes=(uint64_t)sqlite3_column_int64(q,0);result=DL_IMAGE_OK;
    }
    sqlite3_finalize(q);return result;
}
/* Selected row: draft ID, name, kind, sha, width, height, payload size. */
static dl_image_status metadata(sqlite3_stmt *q,dl_image_draft *out) {
    dl_image_draft value={0};
    if(!column(q,0,value.id,32,1)||!column(q,1,value.name,256,0)
       ||!display_name(value.name)||!column(q,2,value.kind,15,0)
       ||(strcmp(value.kind,"file")&&strcmp(value.kind,"clipboard")&&strcmp(value.kind,"screenshot"))
       ||!column(q,3,value.image.sha256,64,1)) return DL_IMAGE_STORAGE;
    for(int i=4;i<=6;i++) if(sqlite3_column_type(q,i)!=SQLITE_INTEGER) return DL_IMAGE_STORAGE;
    sqlite3_int64 width=sqlite3_column_int64(q,4),height=sqlite3_column_int64(q,5),size=sqlite3_column_int64(q,6);
    if(width<1||width>DL_IMAGE_MAX_DIMENSION||height<1||height>DL_IMAGE_MAX_DIMENSION||size<1||size>DL_IMAGE_MAX_BYTES) return DL_IMAGE_STORAGE;
    value.image.width=(unsigned)width;value.image.height=(unsigned)height;value.image.bytes=(size_t)size;*out=value;return DL_IMAGE_OK;
}
dl_image_status dl_images_draft(dl_image_store *s,const char *owner,dl_image_draft *out) {
    if(out)memset(out,0,sizeof *out);
    if(!s||!out||!ascii_id(owner,1,64,0))return DL_IMAGE_INVALID;
    sqlite3_stmt *q=prepare(s,"SELECT d.id,d.name,d.kind,r.sha,r.width,r.height,length(r.data) FROM image_drafts d LEFT JOIN image_resources r ON r.sha=d.sha WHERE d.owner=?");
    if(!q)return DL_IMAGE_STORAGE;bind(q,1,owner);int row=sqlite3_step(q);
    dl_image_status status=row==SQLITE_ROW?metadata(q,out):row==SQLITE_DONE?DL_IMAGE_NOT_FOUND:DL_IMAGE_STORAGE;
    sqlite3_finalize(q);return status;
}
dl_image_status dl_images_read(dl_image_store *s,const char *owner,int64_t seq,
    const char *draft_id,const char *hash,void **bytes,size_t *size,dl_image_draft *out) {
    if(bytes)*bytes=NULL;if(size)*size=0;if(out)memset(out,0,sizeof *out);
    if(!s||!bytes||!size||!out||!ascii_id(owner,1,64,0)||seq< -1
       ||(seq==-1&&!ascii_id(draft_id,32,32,1))||(seq>=0&&!ascii_id(hash,64,64,1)))return DL_IMAGE_INVALID;
    sqlite3_stmt *q=prepare(s,seq<0?
        "SELECT d.id,d.name,d.kind,r.sha,r.width,r.height,length(r.data),r.data FROM image_drafts d LEFT JOIN image_resources r ON r.sha=d.sha WHERE d.owner=? AND d.id=?":
        "SELECT m.id,m.name,m.kind,r.sha,r.width,r.height,length(r.data),r.data,m.source_hash FROM image_messages m LEFT JOIN image_resources r ON r.sha=m.sha WHERE m.owner=? AND m.seq=?");
    if(!q)return DL_IMAGE_STORAGE;bind(q,1,owner);
    if(seq<0)bind(q,2,draft_id);else sqlite3_bind_int64(q,2,seq);
    int row=sqlite3_step(q);dl_image_status status=DL_IMAGE_STORAGE;dl_image_draft value;
    if(row!=SQLITE_ROW) {status=row==SQLITE_DONE?DL_IMAGE_NOT_FOUND:DL_IMAGE_STORAGE;goto done;}
    if(seq>=0) {
        char source[65];if(!column(q,8,source,64,1))goto done;
        if(strcmp(source,hash)){status=DL_IMAGE_STALE;goto done;}
    }
    if(metadata(q,&value)!=DL_IMAGE_OK||sqlite3_column_type(q,7)!=SQLITE_BLOB)goto done;
    const void *p=sqlite3_column_blob(q,7);int n=sqlite3_column_bytes(q,7);
    if(!p||n!=(int)value.image.bytes)goto done;
    dl_image_info info;
    if(dl_png_inspect(p,(size_t)n,&info)!=DL_IMAGE_OK||strcmp(info.sha256,value.image.sha256)
       ||info.width!=value.image.width||info.height!=value.image.height)goto done;
    void *copy=malloc((size_t)n);if(!copy){status=DL_IMAGE_NOMEM;goto done;}
    memcpy(copy,p,(size_t)n);*bytes=copy;*size=(size_t)n;*out=value;status=DL_IMAGE_OK;
done:
    sqlite3_finalize(q);return status;
}
dl_image_status dl_images_stage(dl_image_store *s,const char *owner,const char *expected,const char *name,
    const char *kind,const void *bytes,size_t size,dl_image_draft *out) {
    if(out)memset(out,0,sizeof *out);
    if(!s||!out||!ascii_id(owner,1,64,0)||(expected&&!ascii_id(expected,32,32,1))||!display_name(name)||!kind
       ||(strcmp(kind,"file")&&strcmp(kind,"clipboard")&&strcmp(kind,"screenshot")))return DL_IMAGE_INVALID;
    if(!sqlite3_get_autocommit(s->db))return DL_IMAGE_TRANSACTION;
    dl_image_info info;dl_image_status status=dl_png_inspect(bytes,size,&info);
    if(status!=DL_IMAGE_OK)return status;
    if(size>s->quota)return DL_IMAGE_LIMIT;
    if(!execute(s,"BEGIN IMMEDIATE"))return DL_IMAGE_STORAGE;
    dl_image_draft prior;status=dl_images_draft(s,owner,&prior);
    if(status!=DL_IMAGE_OK&&status!=DL_IMAGE_NOT_FOUND){execute(s,"ROLLBACK");return status;}
    if((expected&&(status==DL_IMAGE_NOT_FOUND||strcmp(expected,prior.id)))||(!expected&&status!=DL_IMAGE_NOT_FOUND)) {
        execute(s,"ROLLBACK");return DL_IMAGE_STALE;
    }
    status=DL_IMAGE_OK;
    sqlite3_stmt *q=prepare(s,"INSERT OR IGNORE INTO image_resources(sha,width,height,data) VALUES(?,?,?,?)");
    if(!q)goto failed;
    bind(q,1,info.sha256);sqlite3_bind_int(q,2,(int)info.width);sqlite3_bind_int(q,3,(int)info.height);sqlite3_bind_blob(q,4,bytes,(int)size,SQLITE_TRANSIENT);
    int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);q=NULL;if(!ok)goto failed;
    /* INSERT OR IGNORE never treats a mismatched/corrupt preexisting blob as
     * the supplied image. Verify exact bytes and metadata before binding it. */
    q=prepare(s,"SELECT 1 FROM image_resources WHERE sha=? AND width=? AND height=? AND typeof(data)='blob' AND data=?");
    if(!q)goto failed;bind(q,1,info.sha256);sqlite3_bind_int(q,2,(int)info.width);sqlite3_bind_int(q,3,(int)info.height);sqlite3_bind_blob(q,4,bytes,(int)size,SQLITE_TRANSIENT);
    ok=sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);q=NULL;if(!ok)goto failed;
    q=prepare(s,"INSERT INTO image_drafts(owner,id,sha,name,kind) VALUES(?,lower(hex(randomblob(16))),?,?,?) ON CONFLICT(owner) DO UPDATE SET id=excluded.id,sha=excluded.sha,name=excluded.name,kind=excluded.kind");
    if(!q)goto failed;bind(q,1,owner);bind(q,2,info.sha256);bind(q,3,name);bind(q,4,kind);
    ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);q=NULL;if(!ok||!collect(s))goto failed;
    uint64_t used=0;
    status=dl_images_usage(s,&used);if(status!=DL_IMAGE_OK)goto failed;
    if(used>s->quota){status=DL_IMAGE_LIMIT;goto failed;}
    dl_image_draft value;status=dl_images_draft(s,owner,&value);if(status!=DL_IMAGE_OK)goto failed;
    if(!execute(s,"COMMIT")){status=DL_IMAGE_STORAGE;goto failed;}
    *out=value;return DL_IMAGE_OK;
failed:
    sqlite3_finalize(q);execute(s,"ROLLBACK");
    return status==DL_IMAGE_OK?DL_IMAGE_STORAGE:status;
}
dl_image_status dl_images_remove_draft(dl_image_store *s,const char *owner,const char *id) {
    if(!s||!ascii_id(owner,1,64,0)||!ascii_id(id,32,32,1))return DL_IMAGE_INVALID;
    if(!sqlite3_get_autocommit(s->db))return DL_IMAGE_TRANSACTION;
    if(!execute(s,"BEGIN IMMEDIATE"))return DL_IMAGE_STORAGE;
    sqlite3_stmt *q=prepare(s,"DELETE FROM image_drafts WHERE owner=? AND id=?");
    if(q){bind(q,1,owner);bind(q,2,id);}int ok=q&&sqlite3_step(q)==SQLITE_DONE;
    int changes=sqlite3_changes(s->db);sqlite3_finalize(q);
    dl_image_status status=!ok?DL_IMAGE_STORAGE:changes!=1?DL_IMAGE_STALE:DL_IMAGE_OK;
    if(status==DL_IMAGE_OK&&(!collect(s)||!execute(s,"COMMIT")))status=DL_IMAGE_STORAGE;
    if(status!=DL_IMAGE_OK)execute(s,"ROLLBACK");return status;
}
dl_image_status dl_images_bind(dl_image_store *s,const char *owner,const char *id,
    int64_t seq,const char *hash) {
    if(!s||!ascii_id(owner,1,64,0)||!ascii_id(id,32,32,1)||seq<0||!ascii_id(hash,64,64,1))return DL_IMAGE_INVALID;
    if(sqlite3_get_autocommit(s->db))return DL_IMAGE_TRANSACTION;
    dl_image_draft value;void *bytes=NULL;size_t size=0;
    dl_image_status status=dl_images_read(s,owner,-1,id,NULL,&bytes,&size,&value);free(bytes);
    if(status!=DL_IMAGE_OK)return status==DL_IMAGE_NOT_FOUND?DL_IMAGE_STALE:status;
    sqlite3_stmt *q=prepare(s,"SELECT 1 FROM image_messages WHERE owner=? AND seq=?");
    if(!q)return DL_IMAGE_STORAGE;bind(q,1,owner);sqlite3_bind_int64(q,2,seq);int row=sqlite3_step(q);sqlite3_finalize(q);
    if(row==SQLITE_ROW)return DL_IMAGE_STALE;if(row!=SQLITE_DONE)return DL_IMAGE_STORAGE;
    q=prepare(s,"INSERT INTO image_messages(owner,seq,id,sha,name,kind,source_hash) SELECT owner,?,id,sha,name,kind,? FROM image_drafts WHERE owner=? AND id=?");
    if(!q)return DL_IMAGE_STORAGE;sqlite3_bind_int64(q,1,seq);bind(q,2,hash);bind(q,3,owner);bind(q,4,id);
    int ok=sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;sqlite3_finalize(q);if(!ok)return DL_IMAGE_STORAGE;
    q=prepare(s,"DELETE FROM image_drafts WHERE owner=? AND id=?");
    if(!q)return DL_IMAGE_STORAGE;bind(q,1,owner);bind(q,2,id);
    ok=sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;sqlite3_finalize(q);return ok?DL_IMAGE_OK:DL_IMAGE_STORAGE;
}
dl_image_status dl_images_delete_owner(dl_image_store *s,const char *owner) {
    if(!s||!ascii_id(owner,1,64,0))return DL_IMAGE_INVALID;
    if(sqlite3_get_autocommit(s->db))return DL_IMAGE_TRANSACTION;
    const char *sql[]={"DELETE FROM image_drafts WHERE owner=?","DELETE FROM image_messages WHERE owner=?"};
    for(unsigned i=0;i<2;i++) {
        sqlite3_stmt *q=prepare(s,sql[i]);if(!q)return DL_IMAGE_STORAGE;bind(q,1,owner);
        int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);if(!ok)return DL_IMAGE_STORAGE;
    }
    return collect(s)?DL_IMAGE_OK:DL_IMAGE_STORAGE;
}
