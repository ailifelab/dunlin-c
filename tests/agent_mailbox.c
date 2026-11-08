#include "dunlin/agent_mailbox.h"
#include "dunlin/content_hash.h"
#include <sqlite3.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void sql(sqlite3 *db,const char *s){assert(sqlite3_exec(db,s,NULL,NULL,NULL)==SQLITE_OK);}
static int64_t number(sqlite3 *db,const char *s){
    sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(db,s,-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_ROW);int64_t n=sqlite3_column_int64(q,0);sqlite3_finalize(q);return n;
}
static const char body[]="不要删除 A；只修改 B。\r\nKeep exact source.\n";
typedef struct {dl_agent_mailbox *other;int fired;} revoke_fixture;
static int revoke_during_read(unsigned kind,void *ctx,void *statement,void *unused){
    (void)unused;revoke_fixture *f=ctx;
    const char *query=kind==SQLITE_TRACE_STMT?sqlite3_sql(statement):NULL;
    if(!f->fired&&query&&strstr(query,"SELECT sequence,project,sender,receiver")==query){
        f->fired=1;
        assert(dl_agent_mailbox_member(f->other,"p","b",0)==DL_MAIL_OK);
    }
    return 0;
}
static void append_source(sqlite3 *db,int64_t mail){
    sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(db,"INSERT INTO canonical VALUES(5,?,?)",-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_bind_int64(q,1,mail)==SQLITE_OK);
    assert(sqlite3_bind_blob(q,2,body,sizeof body-1,SQLITE_TRANSIENT)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_DONE);sqlite3_finalize(q);
}
static void empty(dl_agent_mailbox *m,const char *project,const char *receiver){
    dl_agent_message *a=(void *)(uintptr_t)1;size_t n=99;int more=9;int64_t cursor=-1;
    assert(dl_agent_mailbox_pending(m,project,receiver,0,10,&a,&n,&more,&cursor)==DL_MAIL_OK);
    assert(!a&&!n&&!more&&cursor==0);
}
static void check(const char *path){
    sqlite3 *db=NULL;assert(sqlite3_open(path,&db)==SQLITE_OK);
    if(strcmp(path,":memory:"))sql(db,"PRAGMA journal_mode=WAL");
    assert(number(db,"SELECT count(*) FROM sqlite_master")==0);
    assert(!dl_agent_mailbox_reader(db,"owner",128,2));
    assert(!dl_agent_mailbox_open(db,"owner",0,2));
    assert(!dl_agent_mailbox_open(db,"owner",128,0));
    assert(!dl_agent_mailbox_open(db,"bad/owner",128,2));
    assert(number(db,"SELECT count(*) FROM sqlite_master")==0);
    dl_agent_mailbox *m=dl_agent_mailbox_open(db,"owner",128,2);assert(m);
    assert(!dl_agent_mailbox_open(db,"another",128,2));
    assert(!dl_agent_mailbox_reader(db,"another",128,2));
    assert(dl_agent_mailbox_member(m,"p","a",1)==DL_MAIL_OK);
    assert(dl_agent_mailbox_member(m,"p","b",1)==DL_MAIL_OK);
    assert(dl_agent_mailbox_member(m,"p","c",1)==DL_MAIL_OK);
    assert(dl_agent_mailbox_member(m,"q","b",1)==DL_MAIL_OK);
    assert(dl_agent_mailbox_member(m,"q","a",1)==DL_MAIL_OK);
    assert(dl_agent_mailbox_member(m,"p","a",2)==DL_MAIL_INVALID);
    assert(dl_agent_mailbox_member(m,"p' OR 1=1 --","b",1)==DL_MAIL_INVALID);
    int64_t first=99,second=99,again=99;
    assert(dl_agent_mailbox_send(m,"p","a","b","one",NULL,body,sizeof body-1,&first)==DL_MAIL_OK&&first>0);
    assert(dl_agent_mailbox_send(m,"p","a","b","two","one","second",6,&second)==DL_MAIL_OK&&second>first);
    assert(dl_agent_mailbox_send(m,"p","a","b","one",NULL,body,sizeof body-1,&again)==DL_MAIL_OK&&again==first);
    assert(dl_agent_mailbox_send(m,"p","a","b","one",NULL,"changed",7,&again)==DL_MAIL_STALE&&again==0);
    assert(dl_agent_mailbox_send(m,"p","a","c","one",NULL,body,sizeof body-1,&again)==DL_MAIL_STALE&&again==0);
    assert(dl_agent_mailbox_send(m,"p","a","b","one","another",body,sizeof body-1,&again)==DL_MAIL_STALE);
    assert(dl_agent_mailbox_send(m,"p","a","b","three",NULL,"full",4,&again)==DL_MAIL_LIMIT&&again==0);
    assert(number(db,"SELECT count(*) FROM agent_mailbox_messages")==2);
    assert(dl_agent_mailbox_send(m,"p","stranger","b","x",NULL,"x",1,&again)==DL_MAIL_DENIED);
    assert(dl_agent_mailbox_send(m,"unknown","a","b","x",NULL,"x",1,&again)==DL_MAIL_DENIED);
    const char bad[]={ (char)0xc0,(char)0xaf };const char nul[]={'a',0,'b'};char big[129];memset(big,'x',sizeof big);
    assert(dl_agent_mailbox_send(m,"p","a","b","x",NULL,bad,sizeof bad,&again)==DL_MAIL_INVALID);
    assert(dl_agent_mailbox_send(m,"p","a","b","x",NULL,nul,sizeof nul,&again)==DL_MAIL_INVALID);
    assert(dl_agent_mailbox_send(m,"p","a","b","x",NULL,"",0,&again)==DL_MAIL_INVALID);
    assert(dl_agent_mailbox_send(m,"p","a","b","x",NULL,big,sizeof big,&again)==DL_MAIL_LIMIT);
    dl_agent_message *a=NULL;size_t n=0;int more=0;int64_t cursor=0;
    assert(dl_agent_mailbox_pending(m,"p","b",0,1,&a,&n,&more,&cursor)==DL_MAIL_OK&&n==1&&more&&cursor==first);
    char hash[65];dc_sha256(body,sizeof body-1,hash);
    assert(a[0].sequence==first&&!strcmp(a[0].project,"p")&&!strcmp(a[0].sender,"a")&&!strcmp(a[0].receiver,"b"));
    assert(a[0].bytes==sizeof body-1&&!memcmp(a[0].text,body,sizeof body-1)&&!strcmp(a[0].payload_sha256,hash));
    dl_agent_messages_free(a,n);
    assert(dl_agent_mailbox_pending(m,"p","b",cursor,1,&a,&n,&more,&cursor)==DL_MAIL_OK&&n==1&&!more&&cursor==second&&!strcmp(a[0].reply_to,"one"));
    dl_agent_messages_free(a,n);
    empty(m,"p","c");empty(m,"q","b");
    a=(void *)(uintptr_t)1;n=9;more=9;cursor=9;
    assert(dl_agent_mailbox_pending(m,"none","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_DENIED&&!a&&!n&&!more&&!cursor);
    assert(dl_agent_mailbox_pending(m,"p","b",-1,10,&a,&n,&more,&cursor)==DL_MAIL_INVALID);
    assert(dl_agent_mailbox_pending(m,"p","b",0,201,&a,&n,&more,&cursor)==DL_MAIL_INVALID);
    assert(dl_agent_mailbox_member(m,"p","b",0)==DL_MAIL_OK);
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_DENIED&&!a&&!n);
    assert(dl_agent_mailbox_send(m,"p","a","b","one",NULL,body,sizeof body-1,&again)==DL_MAIL_DENIED);
    assert(dl_agent_mailbox_member(m,"p","b",1)==DL_MAIL_OK);
    sql(db,"CREATE TABLE canonical(seq INTEGER PRIMARY KEY,mail INTEGER UNIQUE,text BLOB NOT NULL)");
    assert(dl_agent_mailbox_ack(m,"p","b",first,5,hash)==DL_MAIL_TRANSACTION);
    sql(db,"BEGIN");assert(number(db,"SELECT count(*) FROM canonical")==0);
    assert(dl_agent_mailbox_ack(m,"p","b",first,5,hash)==DL_MAIL_TRANSACTION);sql(db,"ROLLBACK");
    sql(db,"BEGIN IMMEDIATE");append_source(db,first);
    assert(dl_agent_mailbox_ack(m,"p","c",first,5,hash)==DL_MAIL_NOT_FOUND);
    assert(dl_agent_mailbox_ack(m,"p","b",first,5,hash)==DL_MAIL_OK);
    sql(db,"ROLLBACK");
    assert(number(db,"SELECT count(*) FROM canonical")==0&&number(db,"SELECT count(*) FROM agent_mailbox_messages WHERE ack_sequence IS NOT NULL")==0);
    sql(db,"BEGIN IMMEDIATE");append_source(db,first);
    assert(dl_agent_mailbox_ack(m,"p","b",first,5,hash)==DL_MAIL_OK);
    assert(dl_agent_mailbox_ack(m,"p","b",first,5,hash)==DL_MAIL_OK);
    assert(dl_agent_mailbox_ack(m,"p","b",first,6,hash)==DL_MAIL_STALE);
    sql(db,"COMMIT");
    assert(number(db,"SELECT count(*) FROM canonical")==1&&number(db,"SELECT count(*) FROM agent_mailbox_messages WHERE ack_sequence=5")==1);
    sqlite3_stmt *source=NULL;assert(sqlite3_prepare_v2(db,"SELECT text FROM canonical WHERE seq=5",-1,&source,NULL)==SQLITE_OK);
    assert(sqlite3_step(source)==SQLITE_ROW&&sqlite3_column_bytes(source,0)==sizeof body-1&&!memcmp(sqlite3_column_blob(source,0),body,sizeof body-1));sqlite3_finalize(source);
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_OK&&n==1&&a[0].sequence==second);
    dl_agent_messages_free(a,n);
    assert(dl_agent_mailbox_send(m,"p","a","b","three",NULL,"after ack",9,&again)==DL_MAIL_OK&&again>second);
    dl_agent_mailbox *reader=dl_agent_mailbox_reader(db,"owner",128,2);assert(reader);
    assert(dl_agent_mailbox_member(reader,"p","b",0)==DL_MAIL_DENIED);
    assert(dl_agent_mailbox_send(reader,"p","a","c","reader",NULL,"denied",6,&again)==DL_MAIL_DENIED);
    sql(db,"BEGIN IMMEDIATE");assert(dl_agent_mailbox_ack(reader,"p","b",second,6,hash)==DL_MAIL_DENIED);sql(db,"ROLLBACK");
    assert(dl_agent_mailbox_pending(reader,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_OK&&n==2);dl_agent_messages_free(a,n);
    dl_agent_mailbox_close(reader);
    /* A failed write must leave the exact existing resource and transaction state. */
    sql(db,"CREATE TRIGGER mailbox_fault BEFORE INSERT ON agent_mailbox_messages BEGIN SELECT RAISE(ABORT,'fixed storage failure');END");
    assert(dl_agent_mailbox_send(m,"p","a","c","fault",NULL,"x",1,&again)==DL_MAIL_STORAGE&&again==0&&sqlite3_get_autocommit(db));
    sql(db,"DROP TRIGGER mailbox_fault");
    sql(db,"BEGIN IMMEDIATE");
    assert(dl_agent_mailbox_member(m,"p","b",0)==DL_MAIL_TRANSACTION);
    assert(dl_agent_mailbox_send(m,"p","a","c","nested",NULL,"x",1,&again)==DL_MAIL_TRANSACTION);
    assert(!sqlite3_get_autocommit(db));sql(db,"ROLLBACK");
    sql(db,"UPDATE agent_mailbox_messages SET payload=X'63' WHERE message_id='two'");
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_STORAGE&&!a&&!n&&!more&&!cursor);
    sql(db,"UPDATE agent_mailbox_messages SET payload=X'7365636F6E64' WHERE message_id='two'");
    sql(db,"UPDATE agent_mailbox_messages SET payload=X'63' WHERE message_id='three'");
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_STORAGE&&!a&&!n&&!more&&!cursor);
    sql(db,"UPDATE agent_mailbox_messages SET payload=X'61667465722061636b' WHERE message_id='three'");
    sql(db,"UPDATE agent_mailbox_messages SET sender='c' WHERE message_id='two'");
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_STORAGE&&!a&&!n);
    sql(db,"UPDATE agent_mailbox_messages SET sender='a' WHERE message_id='two'");
    sql(db,"UPDATE agent_mailbox_messages SET receiver='c' WHERE message_id='two'");
    assert(dl_agent_mailbox_pending(m,"p","c",0,10,&a,&n,&more,&cursor)==DL_MAIL_STORAGE&&!a&&!n);
    sql(db,"BEGIN IMMEDIATE");
    assert(dl_agent_mailbox_ack(m,"p","c",second,7,hash)==DL_MAIL_STORAGE);
    sql(db,"ROLLBACK");
    sql(db,"UPDATE agent_mailbox_messages SET receiver='b' WHERE message_id='two'");
    sql(db,"PRAGMA ignore_check_constraints=ON;UPDATE agent_mailbox_messages SET ack_hash='invalid' WHERE message_id='two';PRAGMA ignore_check_constraints=OFF");
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_STORAGE&&!a&&!n);
    sql(db,"PRAGMA ignore_check_constraints=ON;UPDATE agent_mailbox_messages SET ack_hash=NULL WHERE message_id='two';PRAGMA ignore_check_constraints=OFF");
    sql(db,"INSERT INTO agent_mailbox_meta VALUES(2,'owner')");
    assert(!dl_agent_mailbox_reader(db,"owner",128,2));
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_STORAGE&&!a&&!n);
    sql(db,"DELETE FROM agent_mailbox_meta WHERE rowid=(SELECT max(rowid) FROM agent_mailbox_meta)");
    sql(db,"UPDATE agent_mailbox_meta SET version=3");
    assert(!dl_agent_mailbox_reader(db,"owner",128,2)&&!dl_agent_mailbox_open(db,"owner",128,2));
    assert(dl_agent_mailbox_member(m,"p","b",1)==DL_MAIL_STORAGE&&sqlite3_get_autocommit(db));
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_STORAGE&&!a&&!n);
    sql(db,"UPDATE agent_mailbox_meta SET version=2,principal='another'");
    assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_STORAGE&&!a&&!n);
    sql(db,"UPDATE agent_mailbox_meta SET principal='owner'");
    sql(db,"UPDATE agent_mailbox_meta SET version=1");
    assert(!dl_agent_mailbox_open(db,"owner",128,2)&&!dl_agent_mailbox_reader(db,"owner",128,2));
    sql(db,"UPDATE agent_mailbox_meta SET version=2");
    if(strcmp(path,":memory:")){
        sqlite3 *second_db=NULL;assert(sqlite3_open(path,&second_db)==SQLITE_OK);
        dl_agent_mailbox *second_box=dl_agent_mailbox_open(second_db,"owner",128,2);assert(second_box);
        revoke_fixture fixture={second_box,0};
        assert(sqlite3_trace_v2(db,SQLITE_TRACE_STMT,revoke_during_read,&fixture)==SQLITE_OK);
        assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_OK&&n==2&&fixture.fired);
        dl_agent_messages_free(a,n);
        assert(sqlite3_trace_v2(db,0,NULL,NULL)==SQLITE_OK);
        // The in-flight read uses one authorized snapshot; the next read sees
        // revocation. No second membership/message snapshot is spliced in.
        assert(dl_agent_mailbox_pending(m,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_DENIED&&!a&&!n);
        assert(dl_agent_mailbox_member(second_box,"p","b",1)==DL_MAIL_OK);
        sql(second_db,"BEGIN IMMEDIATE");
        assert(dl_agent_mailbox_send(m,"p","a","c","competing-writer",NULL,"later",5,&again)==DL_MAIL_STORAGE&&again==0&&sqlite3_get_autocommit(db));
        sql(second_db,"ROLLBACK");
        assert(dl_agent_mailbox_send(m,"p","a","c","competing-writer",NULL,"later",5,&again)==DL_MAIL_OK);
        dl_agent_mailbox_close(second_box);assert(sqlite3_close(second_db)==SQLITE_OK);
    }
    dl_agent_mailbox_close(m);assert(sqlite3_close(db)==SQLITE_OK);
    if(strcmp(path,":memory:")){
        assert(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);
        assert(!dl_agent_mailbox_open(db,"owner",128,2));reader=dl_agent_mailbox_reader(db,"owner",128,2);assert(reader);
        assert(dl_agent_mailbox_pending(reader,"p","b",0,10,&a,&n,&more,&cursor)==DL_MAIL_OK&&n==2&&a[0].sequence==second&&!strcmp(a[0].text,"second"));
        dl_agent_messages_free(a,n);dl_agent_mailbox_close(reader);assert(sqlite3_close(db)==SQLITE_OK);
    }
    assert(sqlite3_open(":memory:",&db)==SQLITE_OK);
    sql(db,"CREATE TABLE agent_mailbox_meta(version INTEGER,principal TEXT);INSERT INTO agent_mailbox_meta VALUES(2,'owner')");
    assert(!dl_agent_mailbox_open(db,"owner",128,2)&&!dl_agent_mailbox_reader(db,"owner",128,2));
    assert(number(db,"SELECT count(*) FROM sqlite_master")==1&&sqlite3_get_autocommit(db));
    assert(sqlite3_close(db)==SQLITE_OK);
}
int main(int argc,char **argv){
    assert(argc==1||argc==2);check(argc==2?argv[1]:":memory:");
    puts("PASS independent C agent mailbox: exact source/dedup/scopes/quota/read-only/durable/corruption and canonical-transaction acknowledgement; no agent execution claim");return 0;
}
