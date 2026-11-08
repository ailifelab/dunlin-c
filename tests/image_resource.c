#include "dunlin/image_resource.h"
#include "dunlin/content_hash.h"
#include <sqlite3.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
typedef struct { unsigned char *p; size_t n; } buffer;
static char source_hash[65];
static void put32(unsigned char *p,unsigned long x) {
    p[0]=(unsigned char)(x>>24);p[1]=(unsigned char)(x>>16);p[2]=(unsigned char)(x>>8);p[3]=(unsigned char)x;
}
static void chunk(buffer *b,const char *type,const void *p,size_t n) {
    b->p=realloc(b->p,b->n+n+12);CHECK(b->p);unsigned char *d=b->p+b->n;
    put32(d,(unsigned long)n);memcpy(d+4,type,4);if(n)memcpy(d+8,p,n);
    uLong crc=crc32(0L,Z_NULL,0);crc=crc32(crc,d+4,(uInt)n+4);put32(d+8+n,crc);b->n+=n+12;
}
static buffer png(unsigned w,unsigned h,unsigned depth,unsigned color,unsigned interlace,
                  const unsigned char *raw,size_t size,int split) {
    buffer b={malloc(8),8};CHECK(b.p);memcpy(b.p,"\211PNG\r\n\032\n",8);
    unsigned char header[13]={0};put32(header,w);put32(header+4,h);header[8]=(unsigned char)depth;header[9]=(unsigned char)color;header[12]=(unsigned char)interlace;
    chunk(&b,"IHDR",header,13);
    if(color==3){unsigned char palette[]={255,0,0,0,255,0};chunk(&b,"PLTE",palette,sizeof palette);}
    uLongf n=compressBound((uLong)size);unsigned char *compressed=malloc(n);CHECK(compressed);
    CHECK(compress2(compressed,&n,raw,(uLong)size,Z_BEST_SPEED)==Z_OK);
    if(split){chunk(&b,"IDAT",compressed,n/2);chunk(&b,"IDAT",compressed+n/2,n-n/2);}
    else chunk(&b,"IDAT",compressed,n);
    chunk(&b,"IEND",NULL,0);free(compressed);return b;
}
static void invalid(buffer b,dl_image_status expected) {
    dl_image_info info;memset(&info,0x7f,sizeof info);
    CHECK(dl_png_inspect(b.p,b.n,&info)==expected);
    CHECK(info.bytes==0&&info.width==0&&info.height==0&&!info.sha256[0]);
}
static void sql(sqlite3 *db,const char *s) {CHECK(sqlite3_exec(db,s,NULL,NULL,NULL)==SQLITE_OK);}
static sqlite3_int64 scalar(sqlite3 *db,const char *s) {
    sqlite3_stmt *q=NULL;CHECK(sqlite3_prepare_v2(db,s,-1,&q,NULL)==SQLITE_OK);CHECK(sqlite3_step(q)==SQLITE_ROW);
    sqlite3_int64 n=sqlite3_column_int64(q,0);CHECK(sqlite3_step(q)==SQLITE_DONE);sqlite3_finalize(q);return n;
}
static void readable(dl_image_store *s,const char *owner,int64_t seq,const char *id,
                     const buffer *b,dl_image_status expected) {
    void *p=(void*)1;size_t n=123;dl_image_draft meta;memset(&meta,0x7f,sizeof meta);
    CHECK(dl_images_read(s,owner,seq,id,seq>=0?source_hash:NULL,&p,&n,&meta)==expected);
    if(expected==DL_IMAGE_OK){CHECK(p&&n==b->n&&!memcmp(p,b->p,n));CHECK(meta.image.bytes==n);dl_image_free(p);}
    else CHECK(!p&&!n&&!meta.id[0]&&!meta.image.sha256[0]);
}
int main(int argc,char **argv) {
    CHECK(argc<=2);
    const char *path=argc==2?argv[1]:":memory:";
    dc_sha256("original source",strlen("original source"),source_hash);
    if(argc==2) {FILE *f=fopen(path,"wx");CHECK(f);CHECK(!fclose(f));}
    unsigned char raw[]={0,255,0,0,255};buffer red=png(1,1,8,6,0,raw,sizeof raw,1);
    raw[1]=0;raw[2]=255;buffer green=png(1,1,8,6,0,raw,sizeof raw,0);
    dl_image_info info;CHECK(dl_png_inspect(red.p,red.n,&info)==DL_IMAGE_OK&&info.width==1&&info.height==1&&info.bytes==red.n);
    char sha[65];dc_sha256(red.p,red.n,sha);CHECK(!strcmp(info.sha256,sha));
    char *url=NULL;CHECK(dl_png_data_url(red.p,red.n,&url)==DL_IMAGE_OK);
    const char *prefix="data:image/png;base64,iVBORw0KGgo";
    CHECK(!strncmp(url,prefix,strlen(prefix))&&strlen(url)==22+(red.n+2)/3*4);dl_image_free(url);
    CHECK(dl_png_data_url(red.p,red.n,NULL)==DL_IMAGE_INVALID);
    CHECK(dl_png_inspect(NULL,0,&info)==DL_IMAGE_INVALID);
    CHECK(dl_png_inspect(red.p,red.n,NULL)==DL_IMAGE_INVALID);
    invalid((buffer){red.p,red.n-1},DL_IMAGE_INVALID);
    red.p[red.n-1]^=1;invalid(red,DL_IMAGE_INVALID);red.p[red.n-1]^=1;
    unsigned char *larger=calloc(DL_IMAGE_MAX_BYTES+1,1);CHECK(larger);invalid((buffer){larger,DL_IMAGE_MAX_BYTES+1},DL_IMAGE_LIMIT);free(larger);
    buffer trailing={malloc(red.n+1),red.n+1};CHECK(trailing.p);memcpy(trailing.p,red.p,red.n);trailing.p[red.n]=0;invalid(trailing,DL_IMAGE_INVALID);free(trailing.p);
    buffer huge=png(1025,1,8,6,0,raw,sizeof raw,0);invalid(huge,DL_IMAGE_LIMIT);free(huge.p);
    buffer zero=png(0,1,8,6,0,raw,sizeof raw,0);invalid(zero,DL_IMAGE_LIMIT);free(zero.p);
    raw[0]=5;buffer filter=png(1,1,8,6,0,raw,sizeof raw,0);invalid(filter,DL_IMAGE_INVALID);free(filter.p);raw[0]=0;
    buffer short_rows=png(1,1,8,6,0,raw,sizeof raw-1,0);invalid(short_rows,DL_IMAGE_INVALID);free(short_rows.p);
    buffer long_rows=png(1,1,8,2,0,raw,sizeof raw,0);invalid(long_rows,DL_IMAGE_INVALID);free(long_rows.p);
    unsigned char packed[]={0,128};buffer indexed=png(1,1,1,3,0,packed,sizeof packed,0);CHECK(dl_png_inspect(indexed.p,indexed.n,&info)==DL_IMAGE_OK);free(indexed.p);
    buffer adam=png(1,1,8,6,1,raw,sizeof raw,0);CHECK(dl_png_inspect(adam.p,adam.n,&info)==DL_IMAGE_OK);free(adam.p);
    buffer animated={malloc(red.n),red.n-12};CHECK(animated.p);memcpy(animated.p,red.p,red.n-12);unsigned char actl[8]={0,0,0,1,0,0,0,0};chunk(&animated,"acTL",actl,sizeof actl);chunk(&animated,"IEND",NULL,0);invalid(animated,DL_IMAGE_INVALID);free(animated.p);
    buffer critical={malloc(red.n),red.n-12};CHECK(critical.p);memcpy(critical.p,red.p,red.n-12);chunk(&critical,"TEST",NULL,0);chunk(&critical,"IEND",NULL,0);invalid(critical,DL_IMAGE_INVALID);free(critical.p);
    /* Exhaustive single-bit changes in the complete resource, including CRCs,
     * must never silently decode a different unverified supplied byte stream. */
    for(size_t i=0;i<red.n;i++)for(unsigned bit=0;bit<8;bit++){red.p[i]^=(unsigned char)(1u<<bit);CHECK(dl_png_inspect(red.p,red.n,&info)!=DL_IMAGE_OK);red.p[i]^=(unsigned char)(1u<<bit);}
    puts("PASS PNG CRC/order/size/dimensions/zlib/scanlines/Adam7/indexed/static-only and exact data URL");
    sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    sql(db,"PRAGMA journal_mode=WAL; CREATE TABLE canonical(owner TEXT,seq INTEGER,body TEXT,PRIMARY KEY(owner,seq));");
    dl_image_store *s=dl_images_open(db,red.n+green.n);CHECK(s);
    CHECK(!dl_images_open(db,0));dl_image_draft a,b,c,tmp;
    CHECK(dl_images_stage(s,"chatA",NULL,"原图.png","file",red.p,red.n,&a)==DL_IMAGE_OK);
    CHECK(dl_images_stage(s,"chatA",NULL,"stale.png","file",green.p,green.n,&tmp)==DL_IMAGE_STALE&&!tmp.id[0]);
    CHECK(dl_images_stage(s,"chatB",NULL,"clip.png","clipboard",red.p,red.n,&b)==DL_IMAGE_OK);
    uint64_t used=0;CHECK(dl_images_usage(s,&used)==DL_IMAGE_OK&&used==red.n);
    CHECK(strcmp(a.id,b.id)&&!strcmp(a.image.sha256,b.image.sha256));
    readable(s,"chatA",-1,a.id,&red,DL_IMAGE_OK);
    readable(s,"chatC",-1,a.id,&red,DL_IMAGE_NOT_FOUND);
    CHECK(dl_images_stage(s,"chatA",a.id,"same.png","screenshot",red.p,red.n,&c)==DL_IMAGE_OK);
    CHECK(strcmp(c.id,a.id)&&!strcmp(c.image.sha256,a.image.sha256));
    CHECK(dl_images_remove_draft(s,"chatA",a.id)==DL_IMAGE_STALE);
    readable(s,"chatA",-1,c.id,&red,DL_IMAGE_OK);
    CHECK(dl_images_stage(s,"chatA",c.id,"new.png","file",green.p,green.n,&a)==DL_IMAGE_OK);
    CHECK(dl_images_bind(s,"chatA",a.id,1,source_hash)==DL_IMAGE_TRANSACTION);
    sql(db,"BEGIN IMMEDIATE; INSERT INTO canonical VALUES('chatA',1,'original source');");
    CHECK(dl_images_stage(s,"chatC",NULL,"nested.png","file",red.p,red.n,&tmp)==DL_IMAGE_TRANSACTION);
    CHECK(dl_images_bind(s,"chatA",a.id,1,source_hash)==DL_IMAGE_OK);
    CHECK(dl_images_bind(s,"chatA",a.id,1,source_hash)==DL_IMAGE_STALE);
    sql(db,"ROLLBACK;");CHECK(scalar(db,"SELECT count(*) FROM canonical")==0);
    readable(s,"chatA",-1,a.id,&green,DL_IMAGE_OK);readable(s,"chatA",1,NULL,&green,DL_IMAGE_NOT_FOUND);
    sql(db,"BEGIN IMMEDIATE; INSERT INTO canonical VALUES('chatA',1,'original source');");
    CHECK(dl_images_bind(s,"chatA",a.id,1,source_hash)==DL_IMAGE_OK);sql(db,"COMMIT;");
    CHECK(dl_images_draft(s,"chatA",&tmp)==DL_IMAGE_NOT_FOUND);
    readable(s,"chatA",1,NULL,&green,DL_IMAGE_OK);
    void *untrusted=NULL;size_t untrusted_size=0;
    CHECK(dl_images_read(s,"chatA",1,NULL,sha,&untrusted,&untrusted_size,&tmp)==DL_IMAGE_STALE&&!untrusted&&!untrusted_size);
    CHECK(dl_images_stage(s,"chatA",NULL,"pending.png","file",red.p,red.n,&c)==DL_IMAGE_OK);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_images_bind(s,"chatA",c.id,1,source_hash)==DL_IMAGE_STALE);sql(db,"ROLLBACK;");
    readable(s,"chatA",-1,c.id,&red,DL_IMAGE_OK);
    CHECK(dl_images_remove_draft(s,"chatA",c.id)==DL_IMAGE_OK);
    readable(s,"chatA",1,NULL,&green,DL_IMAGE_OK);
    if(argc==2) {
        dl_images_close(s);CHECK(sqlite3_close(db)==SQLITE_OK);
        CHECK(sqlite3_open(path,&db)==SQLITE_OK);s=dl_images_open(db,red.n+green.n);CHECK(s);
        readable(s,"chatA",1,NULL,&green,DL_IMAGE_OK);readable(s,"chatB",-1,b.id,&red,DL_IMAGE_OK);
        puts("PASS committed source and immutable resources survive actual database close/reopen");
    }
    /* Quota rejection rolls back new bytes and old binding; old referenced
     * shared payload is retained. Replacing unreferenced bytes reclaims space. */
    CHECK(dl_images_stage(s,"chatC",NULL,"shared.png","file",red.p,red.n,&c)==DL_IMAGE_OK);
    dl_image_store *small=dl_images_open(db,red.n);CHECK(small);
    CHECK(dl_images_stage(small,"chatB",b.id,"too-many.png","file",green.p,green.n,&tmp)==DL_IMAGE_LIMIT);
    readable(s,"chatB",-1,b.id,&red,DL_IMAGE_OK);
    dl_images_close(small);
    sql(db,"CREATE TRIGGER image_write_fault BEFORE INSERT ON image_drafts BEGIN SELECT RAISE(ABORT,'fixture disk failure'); END;");
    CHECK(dl_images_stage(s,"chatB",b.id,"fail.png","file",green.p,green.n,&tmp)==DL_IMAGE_STORAGE);
    readable(s,"chatB",-1,b.id,&red,DL_IMAGE_OK);sql(db,"DROP TRIGGER image_write_fault;");
    CHECK(dl_images_stage(s,"chatB",b.id,"bad\nname.png","file",red.p,red.n,&tmp)==DL_IMAGE_INVALID);
    CHECK(dl_images_stage(s,"chatB",b.id,"\xc0\xaf.png","file",red.p,red.n,&tmp)==DL_IMAGE_INVALID);
    CHECK(dl_images_stage(s,"chatB",b.id,"a.png","remote",red.p,red.n,&tmp)==DL_IMAGE_INVALID);
    CHECK(dl_images_stage(s,"../other",NULL,"a.png","file",red.p,red.n,&tmp)==DL_IMAGE_INVALID);
    CHECK(dl_images_delete_owner(s,"chatB")==DL_IMAGE_TRANSACTION);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_images_delete_owner(s,"chatB")==DL_IMAGE_OK);sql(db,"COMMIT;");
    readable(s,"chatC",-1,c.id,&red,DL_IMAGE_OK);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_images_delete_owner(s,"chatA")==DL_IMAGE_OK);sql(db,"COMMIT;");
    CHECK(dl_images_usage(s,&used)==DL_IMAGE_OK&&used==red.n);
    /* On-disk corruption is detected and cannot be hidden by deduplication. */
    sql(db,"UPDATE image_resources SET data=zeroblob(length(data));");
    readable(s,"chatC",-1,c.id,&red,DL_IMAGE_STORAGE);
    CHECK(dl_images_stage(s,"chatC",c.id,"repair-must-not-overwrite.png","file",red.p,red.n,&tmp)==DL_IMAGE_STORAGE);
    sql(db,"BEGIN IMMEDIATE;");CHECK(dl_images_delete_owner(s,"chatC")==DL_IMAGE_OK);sql(db,"COMMIT;");
    CHECK(dl_images_usage(s,&used)==DL_IMAGE_OK&&used==0);
    CHECK(dl_images_stage(s,"chatA",NULL,"reused.png","file",green.p,green.n,&a)==DL_IMAGE_OK);
    readable(s,"chatA",1,NULL,&green,DL_IMAGE_NOT_FOUND);
    readable(s,"chatA",-1,c.id,&green,DL_IMAGE_NOT_FOUND);
    dl_images_close(s);CHECK(sqlite3_close(db)==SQLITE_OK);
    if(argc==2) {
        CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);
        CHECK(!dl_images_open(db,red.n+green.n));CHECK(sqlite3_close(db)==SQLITE_OK);
    }
    free(red.p);free(green.p);
    puts("PASS durable drafts, exact CAS, owner-scoped reads, atomic source transaction rollback, quota/fault preservation, shared GC and corrupt blob rejection");
    return 0;
}
