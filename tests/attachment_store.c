#include "dunlin/attachment_store.h"
#include "dunlin/content_hash.h"
#include <sqlite3.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static const char *caption="原始附件消息";
static char hash[65],wrong[65];
static void sql(sqlite3 *db,const char *s){CHECK(sqlite3_exec(db,s,NULL,NULL,NULL)==SQLITE_OK);}
static sqlite3_int64 count(sqlite3 *db,const char *s) {
    sqlite3_stmt *q=NULL;CHECK(sqlite3_prepare_v2(db,s,-1,&q,NULL)==SQLITE_OK);
    CHECK(sqlite3_step(q)==SQLITE_ROW);sqlite3_int64 n=sqlite3_column_int64(q,0);
    CHECK(sqlite3_step(q)==SQLITE_DONE);sqlite3_finalize(q);return n;
}
static void read_resource(dl_attachment_store *s,dl_attachment_owner_kind k,const char *owner,
    int64_t seq,const char *id,const void *expected,size_t expected_size,dl_attachment_status status) {
    void *p=(void*)1;size_t n=99;dl_attachment_draft meta;memset(&meta,0xaa,sizeof meta);
    CHECK(dl_attachments_read(s,k,owner,seq,id,seq>=0?hash:NULL,&p,&n,&meta)==status);
    if(status==DL_ATTACHMENT_OK){CHECK(p&&n==expected_size);CHECK(!n||!memcmp(p,expected,n));CHECK(meta.content.bytes==n);dl_attachment_free(p);}
    else CHECK(!p&&!n&&!meta.id[0]&&!meta.content.sha256[0]);
}
static void be32(unsigned char *p,uLong x){p[0]=(unsigned char)(x>>24);p[1]=(unsigned char)(x>>16);p[2]=(unsigned char)(x>>8);p[3]=(unsigned char)x;}
static size_t chunk(unsigned char *p,const char *type,const void *data,size_t n) {
    be32(p,(uLong)n);memcpy(p+4,type,4);if(n)memcpy(p+8,data,n);
    be32(p+8+n,crc32(crc32(0L,Z_NULL,0),p+4,(uInt)n+4));return n+12;
}
int main(int argc,char **argv) {
    CHECK(argc<=2);const char *path=argc==2?argv[1]:":memory:";
    if(argc==2){FILE *f=fopen(path,"wx");CHECK(f);CHECK(!fclose(f));}
    dc_sha256(caption,strlen(caption),hash);dc_sha256("wrong source",12,wrong);
    const char *body="不要删除 A；只修改 B。\r\n变量 café、用户 👩‍💻 保持原样。";
    size_t size=strlen(body);dl_attachment_info info;
    CHECK(dl_attachment_inspect(DL_ATTACHMENT_TEXT,body,size,&info)==DL_ATTACHMENT_OK);
    CHECK(info.format==DL_ATTACHMENT_TEXT&&info.bytes==size&&!info.width&&!info.height);
    CHECK(dl_attachment_inspect(DL_ATTACHMENT_TEXT,NULL,0,&info)==DL_ATTACHMENT_OK);
    CHECK(!strcmp(info.sha256,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(dl_attachment_inspect(DL_ATTACHMENT_TEXT,"abc",3,&info)==DL_ATTACHMENT_OK);
    CHECK(!strcmp(info.sha256,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const unsigned char *invalid[]={ (const unsigned char*)"a\0b",(const unsigned char*)"\xc0\xaf",(const unsigned char*)"\xed\xa0\x80",(const unsigned char*)"\xf4\x90\x80\x80",(const unsigned char*)"\xe2\x82",(const unsigned char*)"\x80"};
    const size_t invalid_sizes[]={3,2,3,4,2,1};
    for(size_t i=0;i<sizeof invalid_sizes/sizeof *invalid_sizes;i++) {
        memset(&info,0xaa,sizeof info);CHECK(dl_attachment_inspect(DL_ATTACHMENT_TEXT,invalid[i],invalid_sizes[i],&info)==DL_ATTACHMENT_INVALID);CHECK(!info.sha256[0]&&!info.bytes);
    }
    CHECK(dl_attachment_inspect(DL_ATTACHMENT_TEXT,NULL,1,&info)==DL_ATTACHMENT_INVALID);
    CHECK(dl_attachment_inspect((dl_attachment_format)3,body,size,&info)==DL_ATTACHMENT_INVALID);
    CHECK(dl_attachment_inspect(DL_ATTACHMENT_TEXT,body,size,NULL)==DL_ATTACHMENT_INVALID);
    unsigned char *maximum=malloc(65537);CHECK(maximum);memset(maximum,'a',65537);
    CHECK(dl_attachment_inspect(DL_ATTACHMENT_TEXT,maximum,65536,&info)==DL_ATTACHMENT_OK);
    CHECK(dl_attachment_inspect(DL_ATTACHMENT_TEXT,maximum,65537,&info)==DL_ATTACHMENT_LIMIT);
    unsigned char image[128],header[13]={0},compressed[32],raw[]={0,23,42,99,255};
    memcpy(image,"\211PNG\r\n\032\n",8);be32(header,1);be32(header+4,1);header[8]=8;header[9]=6;
    size_t image_size=8+chunk(image+8,"IHDR",header,sizeof header);uLongf n=sizeof compressed;
    CHECK(compress2(compressed,&n,raw,sizeof raw,Z_BEST_SPEED)==Z_OK);
    image_size+=chunk(image+image_size,"IDAT",compressed,(size_t)n);image_size+=chunk(image+image_size,"IEND",NULL,0);
    CHECK(dl_attachment_inspect(DL_ATTACHMENT_PNG,image,image_size,&info)==DL_ATTACHMENT_OK&&info.width==1&&info.height==1);
    image[image_size-1]^=1;CHECK(dl_attachment_inspect(DL_ATTACHMENT_PNG,image,image_size,&info)==DL_ATTACHMENT_INVALID);image[image_size-1]^=1;
    CHECK(!strcmp(dl_attachment_mime(DL_ATTACHMENT_PNG),"image/png"));CHECK(!dl_attachment_mime(0));
    puts("PASS exact UTF8/CRLF/empty/Unicode text, known SHA vectors, bounded text and composed PNG validation");

    sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    sql(db,"PRAGMA journal_mode=WAL;CREATE TABLE canonical(scope INTEGER,owner TEXT,seq INTEGER,body TEXT,PRIMARY KEY(scope,owner,seq));");
    CHECK(!dl_attachments_open(db,0));dl_attachment_store *s=dl_attachments_open(db,1048576);CHECK(s);
    dl_attachment_draft a,b,c,tmp;
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CHAT,"same",NULL,"原始信息.txt","file",DL_ATTACHMENT_TEXT,body,size,&a)==DL_ATTACHMENT_OK);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CALL,"same",NULL,"通话.txt","file",DL_ATTACHMENT_TEXT,body,size,&b)==DL_ATTACHMENT_OK);
    CHECK(strcmp(a.id,b.id)&&!strcmp(a.content.sha256,b.content.sha256));
    uint64_t used=0;CHECK(dl_attachments_usage(s,&used)==DL_ATTACHMENT_OK&&used==size);
    read_resource(s,DL_ATTACHMENT_CHAT,"same",-1,b.id,body,size,DL_ATTACHMENT_NOT_FOUND);
    read_resource(s,DL_ATTACHMENT_CALL,"same",-1,a.id,body,size,DL_ATTACHMENT_NOT_FOUND);
    read_resource(s,DL_ATTACHMENT_CHAT,"other",-1,a.id,body,size,DL_ATTACHMENT_NOT_FOUND);
    read_resource(s,DL_ATTACHMENT_CHAT,"same",-1,a.id,body,size,DL_ATTACHMENT_OK);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CHAT,"same",NULL,"stale.txt","file",DL_ATTACHMENT_TEXT,"new",3,&tmp)==DL_ATTACHMENT_STALE&&!tmp.id[0]);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CHAT,"same",a.id,"same.txt","clipboard",DL_ATTACHMENT_TEXT,body,size,&c)==DL_ATTACHMENT_OK);
    CHECK(strcmp(c.id,a.id)&&!strcmp(c.content.sha256,a.content.sha256));
    CHECK(dl_attachments_remove(s,DL_ATTACHMENT_CHAT,"same",a.id)==DL_ATTACHMENT_STALE);a=c;
    CHECK(dl_attachments_bind(s,DL_ATTACHMENT_CHAT,"same",a.id,0,hash)==DL_ATTACHMENT_TRANSACTION);
    sql(db,"BEGIN;SELECT count(*) FROM canonical;");
    CHECK(dl_attachments_bind(s,DL_ATTACHMENT_CHAT,"same",a.id,0,hash)==DL_ATTACHMENT_TRANSACTION);sql(db,"ROLLBACK;");
    sql(db,"BEGIN IMMEDIATE;INSERT INTO canonical VALUES(1,'same',0,'原始附件消息');");
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CHAT,"other",NULL,"nested.txt","file",DL_ATTACHMENT_TEXT,body,size,&tmp)==DL_ATTACHMENT_TRANSACTION);
    CHECK(dl_attachments_bind(s,DL_ATTACHMENT_CHAT,"same",a.id,0,hash)==DL_ATTACHMENT_OK);
    CHECK(dl_attachments_bind(s,DL_ATTACHMENT_CHAT,"same",a.id,0,hash)==DL_ATTACHMENT_STALE);
    sql(db,"ROLLBACK;");CHECK(!count(db,"SELECT count(*) FROM canonical"));
    read_resource(s,DL_ATTACHMENT_CHAT,"same",-1,a.id,body,size,DL_ATTACHMENT_OK);
    read_resource(s,DL_ATTACHMENT_CHAT,"same",0,NULL,body,size,DL_ATTACHMENT_NOT_FOUND);
    sql(db,"BEGIN IMMEDIATE;INSERT INTO canonical VALUES(1,'same',0,'原始附件消息');");
    CHECK(dl_attachments_bind(s,DL_ATTACHMENT_CHAT,"same",a.id,0,hash)==DL_ATTACHMENT_OK);sql(db,"COMMIT;");
    CHECK(dl_attachments_draft(s,DL_ATTACHMENT_CHAT,"same",&tmp)==DL_ATTACHMENT_NOT_FOUND);
    CHECK(dl_attachments_info(s,DL_ATTACHMENT_CHAT,"same",0,hash,&tmp)==DL_ATTACHMENT_OK&&!strcmp(tmp.id,a.id));
    CHECK(dl_attachments_info(s,DL_ATTACHMENT_CHAT,"same",0,wrong,&tmp)==DL_ATTACHMENT_STALE&&!tmp.id[0]);
    void *p=(void*)1;size_t bytes=99;
    CHECK(dl_attachments_read(s,DL_ATTACHMENT_CHAT,"same",0,NULL,wrong,&p,&bytes,&tmp)==DL_ATTACHMENT_STALE&&!p&&!bytes&&!tmp.id[0]);
    read_resource(s,DL_ATTACHMENT_CHAT,"same",0,NULL,body,size,DL_ATTACHMENT_OK);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CHAT,"same",NULL,"next.png","screenshot",DL_ATTACHMENT_PNG,image,image_size,&c)==DL_ATTACHMENT_OK);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_attachments_bind(s,DL_ATTACHMENT_CHAT,"same",c.id,0,hash)==DL_ATTACHMENT_STALE);sql(db,"ROLLBACK;");
    read_resource(s,DL_ATTACHMENT_CHAT,"same",-1,c.id,image,image_size,DL_ATTACHMENT_OK);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_KNOWLEDGE,"empty",NULL,"empty.txt","file",DL_ATTACHMENT_TEXT,NULL,0,&tmp)==DL_ATTACHMENT_OK);
    read_resource(s,DL_ATTACHMENT_KNOWLEDGE,"empty",-1,tmp.id,NULL,0,DL_ATTACHMENT_OK);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_attachments_bind(s,DL_ATTACHMENT_KNOWLEDGE,"empty",tmp.id,INT64_MAX,hash)==DL_ATTACHMENT_OK);sql(db,"COMMIT;");
    read_resource(s,DL_ATTACHMENT_KNOWLEDGE,"empty",INT64_MAX,NULL,NULL,0,DL_ATTACHMENT_OK);
    if(argc==2) {
        dl_attachments_close(s);CHECK(sqlite3_close(db)==SQLITE_OK);
        CHECK(sqlite3_open(path,&db)==SQLITE_OK);s=dl_attachments_open(db,1048576);CHECK(s);
        read_resource(s,DL_ATTACHMENT_CHAT,"same",0,NULL,body,size,DL_ATTACHMENT_OK);
        read_resource(s,DL_ATTACHMENT_CHAT,"same",-1,c.id,image,image_size,DL_ATTACHMENT_OK);
        read_resource(s,DL_ATTACHMENT_KNOWLEDGE,"empty",INT64_MAX,NULL,NULL,0,DL_ATTACHMENT_OK);
        puts("PASS actual close/reopen preserves canonical associations, pending bytes and zero-byte resources");
    }
    dl_attachment_store *small=dl_attachments_open(db,size+image_size);CHECK(small);
    CHECK(dl_attachments_stage(small,DL_ATTACHMENT_CALL,"same",b.id,"too-many.txt","file",DL_ATTACHMENT_TEXT,maximum,65536,&tmp)==DL_ATTACHMENT_LIMIT);
    CHECK(dl_attachments_stage(small,DL_ATTACHMENT_CALL,"same",b.id,"total-quota.txt","file",DL_ATTACHMENT_TEXT,"unique replacement",18,&tmp)==DL_ATTACHMENT_LIMIT);
    read_resource(s,DL_ATTACHMENT_CALL,"same",-1,b.id,body,size,DL_ATTACHMENT_OK);dl_attachments_close(small);
    sql(db,"CREATE TRIGGER stage_fault BEFORE INSERT ON attachment_drafts BEGIN SELECT RAISE(ABORT,'fixture failure');END;");
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CALL,"same",b.id,"failure.txt","file",DL_ATTACHMENT_TEXT,"unique failed bytes",19,&tmp)==DL_ATTACHMENT_STORAGE&&!tmp.id[0]);
    CHECK(dl_attachments_usage(s,&used)==DL_ATTACHMENT_OK&&used==size+image_size);read_resource(s,DL_ATTACHMENT_CALL,"same",-1,b.id,body,size,DL_ATTACHMENT_OK);sql(db,"DROP TRIGGER stage_fault;");
    sql(db,"CREATE TRIGGER bind_fault BEFORE DELETE ON attachment_drafts BEGIN SELECT RAISE(ABORT,'fixture failure');END;BEGIN IMMEDIATE;INSERT INTO canonical VALUES(1,'same',1,'原始附件消息');");
    CHECK(dl_attachments_bind(s,DL_ATTACHMENT_CHAT,"same",c.id,1,hash)==DL_ATTACHMENT_STORAGE);sql(db,"ROLLBACK;DROP TRIGGER bind_fault;");
    CHECK(count(db,"SELECT count(*) FROM canonical")==1);read_resource(s,DL_ATTACHMENT_CHAT,"same",1,NULL,image,image_size,DL_ATTACHMENT_NOT_FOUND);read_resource(s,DL_ATTACHMENT_CHAT,"same",-1,c.id,image,image_size,DL_ATTACHMENT_OK);
    dl_attachment_store *reader=dl_attachments_reader(db);CHECK(reader);
    CHECK(dl_attachments_stage(reader,DL_ATTACHMENT_EXTENSION,"other",NULL,"no.txt","file",DL_ATTACHMENT_TEXT,body,size,&tmp)==DL_ATTACHMENT_STORAGE);
    CHECK(dl_attachments_remove(reader,DL_ATTACHMENT_CHAT,"same",c.id)==DL_ATTACHMENT_STORAGE);
    read_resource(reader,DL_ATTACHMENT_CHAT,"same",0,NULL,body,size,DL_ATTACHMENT_OK);dl_attachments_close(reader);
    CHECK(dl_attachments_stage(s,0,"same",NULL,"a.txt","file",DL_ATTACHMENT_TEXT,body,size,&tmp)==DL_ATTACHMENT_INVALID);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CHAT,"../same",NULL,"a.txt","file",DL_ATTACHMENT_TEXT,body,size,&tmp)==DL_ATTACHMENT_INVALID);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CALL,"same",b.id,"bad\nname.txt","file",DL_ATTACHMENT_TEXT,body,size,&tmp)==DL_ATTACHMENT_INVALID);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CALL,"same",b.id,"\xc0\xaf.txt","file",DL_ATTACHMENT_TEXT,body,size,&tmp)==DL_ATTACHMENT_INVALID);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CALL,"same",b.id,"a.txt","remote",DL_ATTACHMENT_TEXT,body,size,&tmp)==DL_ATTACHMENT_INVALID);
    sql(db,"PRAGMA ignore_check_constraints=ON;BEGIN IMMEDIATE;UPDATE attachment_resources SET format=4294967298 WHERE length(data)>0;");
    CHECK(dl_attachments_info(s,DL_ATTACHMENT_CHAT,"same",0,hash,&tmp)==DL_ATTACHMENT_STORAGE&&!tmp.id[0]);
    read_resource(s,DL_ATTACHMENT_CHAT,"same",0,NULL,body,size,DL_ATTACHMENT_STORAGE);
    sql(db,"ROLLBACK;BEGIN IMMEDIATE;UPDATE attachment_resources SET width=4294967296 WHERE format=2;");
    read_resource(s,DL_ATTACHMENT_CHAT,"same",0,NULL,body,size,DL_ATTACHMENT_STORAGE);
    sql(db,"ROLLBACK;PRAGMA ignore_check_constraints=OFF;");
    CHECK(dl_attachments_delete_owner(s,DL_ATTACHMENT_CHAT,"same")==DL_ATTACHMENT_TRANSACTION);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_attachments_delete_owner(s,DL_ATTACHMENT_CHAT,"same")==DL_ATTACHMENT_OK);sql(db,"ROLLBACK;");
    read_resource(s,DL_ATTACHMENT_CHAT,"same",0,NULL,body,size,DL_ATTACHMENT_OK);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_attachments_delete_owner(s,DL_ATTACHMENT_CHAT,"same")==DL_ATTACHMENT_OK);sql(db,"COMMIT;");
    read_resource(s,DL_ATTACHMENT_CALL,"same",-1,b.id,body,size,DL_ATTACHMENT_OK);CHECK(dl_attachments_usage(s,&used)==DL_ATTACHMENT_OK&&used==size);
    /* Corrupted payload is not silently repaired by immutable deduplication. */
    sql(db,"UPDATE attachment_resources SET data=zeroblob(length(data)) WHERE length(data)>0;");
    read_resource(s,DL_ATTACHMENT_CALL,"same",-1,b.id,body,size,DL_ATTACHMENT_STORAGE);
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_CALL,"same",b.id,"no-overwrite.txt","file",DL_ATTACHMENT_TEXT,body,size,&tmp)==DL_ATTACHMENT_STORAGE);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_attachments_delete_owner(s,DL_ATTACHMENT_CALL,"same")==DL_ATTACHMENT_OK);CHECK(dl_attachments_delete_owner(s,DL_ATTACHMENT_KNOWLEDGE,"empty")==DL_ATTACHMENT_OK);sql(db,"COMMIT;");
    CHECK(dl_attachments_usage(s,&used)==DL_ATTACHMENT_OK&&!used);CHECK(!count(db,"SELECT count(*) FROM attachment_resources"));
    CHECK(dl_attachments_stage(s,DL_ATTACHMENT_EXTENSION,"owner",NULL,"exact.txt","file",DL_ATTACHMENT_TEXT,body,size,&a)==DL_ATTACHMENT_OK);
    read_resource(s,DL_ATTACHMENT_EXTENSION,"owner",-1,a.id,body,size,DL_ATTACHMENT_OK);
    dl_attachments_close(s);CHECK(sqlite3_close(db)==SQLITE_OK);
    if(argc==2) {
        CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);CHECK(!dl_attachments_open(db,1048576));
        reader=dl_attachments_reader(db);CHECK(reader);read_resource(reader,DL_ATTACHMENT_EXTENSION,"owner",-1,a.id,body,size,DL_ATTACHMENT_OK);
        CHECK(dl_attachments_usage(reader,&used)==DL_ATTACHMENT_OK&&used==size);
        dl_attachments_close(reader);CHECK(sqlite3_close(db)==SQLITE_OK);
        CHECK(sqlite3_open(path,&db)==SQLITE_OK);sqlite3 *rival=NULL;CHECK(sqlite3_open(path,&rival)==SQLITE_OK);
        s=dl_attachments_open(db,1048576);CHECK(s);sql(rival,"BEGIN IMMEDIATE;");
        CHECK(dl_attachments_remove(s,DL_ATTACHMENT_EXTENSION,"owner",a.id)==DL_ATTACHMENT_STORAGE);
        read_resource(s,DL_ATTACHMENT_EXTENSION,"owner",-1,a.id,body,size,DL_ATTACHMENT_OK);sql(rival,"ROLLBACK;");CHECK(sqlite3_close(rival)==SQLITE_OK);
        CHECK(dl_attachments_remove(s,DL_ATTACHMENT_EXTENSION,"owner",a.id)==DL_ATTACHMENT_OK);dl_attachments_close(s);CHECK(sqlite3_close(db)==SQLITE_OK);
    }
    free(maximum);
    puts("PASS owner-kind namespaces, exact draft CAS, source-hash reads, canonical transaction rollback, quota/fault preservation, read-only access, competing writer and shared GC");
    return 0;
}
