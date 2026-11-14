#include "dunlin/agent_scheduler.h"
#include <sqlite3.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dl_agent_job job(const char *id,const char *chat,const char *resource,int write,const char *parent){
    dl_agent_job j={0};snprintf(j.id,sizeof j.id,"%s",id);snprintf(j.chat,sizeof j.chat,"%s",chat);snprintf(j.resource,sizeof j.resource,"%s",resource);snprintf(j.parent,sizeof j.parent,"%s",parent);strcpy(j.project,"project");strcpy(j.model,"model");j.task="保留原意；不要覆盖其他 Agent 的文件。";j.write_access=write;return j;
}
static unsigned external_load(void *raw){return *(unsigned*)raw;}
int main(int argc,char **argv){
    sqlite3 *db=NULL,*other=NULL;const char *path=argc>1?argv[1]:":memory:";assert(sqlite3_open(path,&db)==SQLITE_OK);
    dl_agent_scheduler *s=dl_agent_scheduler_open(db,"owner",2);assert(s);
    assert(!dl_agent_scheduler_open(db,"another-owner",2));assert(!dl_agent_scheduler_open(db,"owner",1));
    dl_agent_job a=job("a","chat-a","dev:123",0,""),b=job("b","chat-b","dev:123",0,"a"),w=job("writer","chat-c","dev:123",1,""),otherjob=job("other","chat-d","dev:456",1,""),out={0};
    assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);a.task="different";assert(dl_agent_enqueue(s,&a)==DL_SCHED_CONFLICT);
    assert(dl_agent_enqueue(s,&b)==DL_SCHED_OK);assert(dl_agent_enqueue(s,&w)==DL_SCHED_OK);assert(dl_agent_enqueue(s,&otherjob)==DL_SCHED_OK);
    assert(dl_agent_claim(s,&out)==DL_SCHED_OK&&!strcmp(out.id,"a"));int64_t first=out.generation;dl_agent_job_clear(&out);
    assert(dl_agent_claim(s,&out)==DL_SCHED_OK&&!strcmp(out.id,"b"));int64_t second=out.generation;assert(second>first);dl_agent_job_clear(&out);
    assert(dl_agent_claim(s,&out)==DL_SCHED_EMPTY);assert(!out.task);
    assert(dl_agent_finish(s,"a",second,"succeeded")==DL_SCHED_STALE);
    assert(dl_agent_finish(s,"a",first,"succeeded")==DL_SCHED_OK);
    assert(dl_agent_finish(s,"a",first,"succeeded")==DL_SCHED_STALE);
    /* One reader still owns dev:123: independent resource proceeds. */
    assert(dl_agent_claim(s,&out)==DL_SCHED_OK&&!strcmp(out.id,"other"));int64_t third=out.generation;dl_agent_job_clear(&out);
    assert(dl_agent_cancel(s,"b")==DL_SCHED_OK);assert(dl_agent_claim(s,&out)==DL_SCHED_EMPTY);
    assert(dl_agent_job_get(s,"b",&out)==DL_SCHED_OK&&!strcmp(out.state,"cancel_requested"));dl_agent_job_clear(&out);
    assert(dl_agent_finish(s,"b",second,"succeeded")==DL_SCHED_OK);
    assert(dl_agent_job_get(s,"b",&out)==DL_SCHED_OK&&!strcmp(out.state,"cancelled"));dl_agent_job_clear(&out);
    assert(dl_agent_claim(s,&out)==DL_SCHED_OK&&!strcmp(out.id,"writer"));int64_t fourth=out.generation;dl_agent_job_clear(&out);
    dl_agent_job child=job("child","chat-e","dev:789",0,"writer"),grand=job("grand","chat-f","dev:789",0,"child");assert(dl_agent_enqueue(s,&child)==DL_SCHED_OK);assert(dl_agent_enqueue(s,&grand)==DL_SCHED_OK);
    assert(dl_agent_cancel(s,"writer")==DL_SCHED_OK);assert(dl_agent_job_get(s,"grand",&out)==DL_SCHED_OK&&!strcmp(out.state,"cancelled"));dl_agent_job_clear(&out);
    assert(dl_agent_finish(s,"writer",fourth,"failed")==DL_SCHED_OK);assert(dl_agent_finish(s,"other",third,"succeeded")==DL_SCHED_OK);
    /* Chat exclusion is independent of resource identity. */
    a=job("same1","same-chat","one",0,"");b=job("same2","same-chat","two",0,"");assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);assert(dl_agent_enqueue(s,&b)==DL_SCHED_OK);
    assert(dl_agent_claim(s,&out)==DL_SCHED_OK&&!strcmp(out.id,"same1"));first=out.generation;dl_agent_job_clear(&out);assert(dl_agent_claim(s,&out)==DL_SCHED_EMPTY);
    assert(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)==SQLITE_OK);assert(dl_agent_cancel(s,"same1")==DL_SCHED_TRANSACTION);assert(sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL)==SQLITE_OK);
    /* Opening another handle must not mark a live worker interrupted. */
    dl_agent_scheduler *reopened=dl_agent_scheduler_open(db,"owner",2);assert(reopened);assert(dl_agent_job_get(reopened,"same1",&out)==DL_SCHED_OK&&!strcmp(out.state,"running"));dl_agent_job_clear(&out);dl_agent_scheduler_close(reopened);
    if(argc>1){assert(sqlite3_open(path,&other)==SQLITE_OK);reopened=dl_agent_scheduler_open(other,"owner",2);assert(reopened);
        assert(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)==SQLITE_OK);assert(dl_agent_claim(reopened,&out)==DL_SCHED_STORAGE);assert(sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL)==SQLITE_OK);
        assert(dl_agent_claim(reopened,&out)==DL_SCHED_EMPTY);dl_agent_scheduler_close(reopened);sqlite3_close(other);}
    child=job("recovery-child","recover-chat","three",0,"same1");assert(dl_agent_enqueue(s,&child)==DL_SCHED_OK);
    assert(dl_agent_scheduler_recover(s)==DL_SCHED_OK);assert(dl_agent_job_get(s,"same1",&out)==DL_SCHED_OK&&!strcmp(out.state,"interrupted"));dl_agent_job_clear(&out);
    assert(dl_agent_job_get(s,"recovery-child",&out)==DL_SCHED_OK&&!strcmp(out.state,"cancelled"));dl_agent_job_clear(&out);
    assert(dl_agent_finish(s,"same1",first,"succeeded")==DL_SCHED_STALE);
    assert(dl_agent_claim(s,&out)==DL_SCHED_OK&&!strcmp(out.id,"same2"));first=out.generation;dl_agent_job_clear(&out);assert(dl_agent_finish(s,"same2",first,"succeeded")==DL_SCHED_OK);
    a=job("depth0","depthchat","depth",0,"");assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);
    for(int i=1;i<=9;i++){char id[32],parent[32];snprintf(id,sizeof id,"depth%d",i);snprintf(parent,sizeof parent,"depth%d",i-1);a=job(id,id,"depth",0,parent);assert(dl_agent_enqueue(s,&a)==(i<=8?DL_SCHED_OK:DL_SCHED_LIMIT));}
    a=job("cross-project","cross","cross",0,"depth0");strcpy(a.project,"other-project");assert(dl_agent_enqueue(s,&a)==DL_SCHED_CONFLICT);
    a=job("invalid","invalid","invalid",0,"");a.task="\xed\xa0\x80";assert(dl_agent_enqueue(s,&a)==DL_SCHED_INVALID);a.task="ok";memset(a.id,'x',sizeof a.id);assert(dl_agent_enqueue(s,&a)==DL_SCHED_INVALID);
    assert(dl_agent_cancel(s,"depth0")==DL_SCHED_OK);assert(dl_agent_claim(s,&out)==DL_SCHED_EMPTY);
    a=job("parent-wait","parent-chat","shared-write",1,"");assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);assert(dl_agent_claim(s,&out)==DL_SCHED_OK);first=out.generation;dl_agent_job_clear(&out);
    child=job("child-wait","child-chat","shared-write",1,"parent-wait");assert(dl_agent_enqueue(s,&child)==DL_SCHED_OK);assert(dl_agent_claim(s,&out)==DL_SCHED_EMPTY);
    assert(dl_agent_park(s,"parent-wait",first+1)==DL_SCHED_STALE);assert(dl_agent_park(s,"parent-wait",first)==DL_SCHED_OK);assert(dl_agent_claim(s,&out)==DL_SCHED_OK&&!strcmp(out.id,"child-wait"));second=out.generation;dl_agent_job_clear(&out);
    assert(dl_agent_wake(s,"parent-wait",first)==DL_SCHED_LIMIT);assert(dl_agent_finish(s,"child-wait",second,"succeeded")==DL_SCHED_OK);assert(dl_agent_wake(s,"parent-wait",first)==DL_SCHED_OK);
    assert(dl_agent_park(s,"parent-wait",first)==DL_SCHED_OK);assert(dl_agent_cancel(s,"parent-wait")==DL_SCHED_OK);assert(dl_agent_wake(s,"parent-wait",first)==DL_SCHED_STALE);
    a=job("explicit-first","explicit-chat-1","explicit-resource",1,"");b=job("explicit-second","explicit-chat-2","explicit-resource",1,"");
    assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);assert(dl_agent_enqueue(s,&b)==DL_SCHED_OK);
    assert(dl_agent_claim_id(s,"missing",&out)==DL_SCHED_EMPTY);
    assert(dl_agent_claim_id(s,"invalid/id",&out)==DL_SCHED_INVALID);
    assert(dl_agent_claim_id(s,"explicit-second",&out)==DL_SCHED_OK&&!strcmp(out.id,"explicit-second"));second=out.generation;dl_agent_job_clear(&out);
    assert(dl_agent_claim_id(s,"explicit-first",&out)==DL_SCHED_EMPTY);
    assert(dl_agent_finish(s,"explicit-second",second,"succeeded")==DL_SCHED_OK);
    assert(dl_agent_claim_id(s,"explicit-first",&out)==DL_SCHED_OK&&!strcmp(out.id,"explicit-first"));first=out.generation;dl_agent_job_clear(&out);
    assert(dl_agent_finish(s,"explicit-first",first,"succeeded")==DL_SCHED_OK);
    a=job("remove-queued","remove-chat","remove-resource",1,"");assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);
    child=job("remove-child","remove-child-chat","remove-child-resource",1,"remove-queued");assert(dl_agent_enqueue(s,&child)==DL_SCHED_OK);
    assert(dl_agent_cancel_queued(s,"remove-queued")==DL_SCHED_OK);assert(dl_agent_job_get(s,"remove-child",&out)==DL_SCHED_OK&&!strcmp(out.state,"cancelled"));dl_agent_job_clear(&out);
    assert(dl_agent_cancel_queued(s,"remove-queued")==DL_SCHED_STALE);assert(dl_agent_cancel_queued(s,"missing")==DL_SCHED_EMPTY);
    a=job("keep-running","keep-chat","keep-resource",1,"");assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);assert(dl_agent_claim_id(s,"keep-running",&out)==DL_SCHED_OK);first=out.generation;dl_agent_job_clear(&out);
    child=job("keep-child","keep-child-chat","keep-child-resource",1,"keep-running");assert(dl_agent_enqueue(s,&child)==DL_SCHED_OK);
    assert(dl_agent_cancel_queued(s,"keep-running")==DL_SCHED_STALE);assert(dl_agent_job_get(s,"keep-running",&out)==DL_SCHED_OK&&!strcmp(out.state,"running"));dl_agent_job_clear(&out);assert(dl_agent_job_get(s,"keep-child",&out)==DL_SCHED_OK&&!strcmp(out.state,"queued"));dl_agent_job_clear(&out);
    assert(dl_agent_park(s,"keep-running",first)==DL_SCHED_OK);assert(dl_agent_cancel_queued(s,"keep-running")==DL_SCHED_STALE);assert(dl_agent_job_get(s,"keep-running",&out)==DL_SCHED_OK&&!strcmp(out.state,"waiting"));dl_agent_job_clear(&out);assert(dl_agent_cancel(s,"keep-running")==DL_SCHED_OK);
    /* Shared host executor consumes the same capacity under claim/wake TX. */
    unsigned external=2;dl_agent_scheduler_external_load(s,external_load,&external);
    a=job("external-first","external-chat","external-resource",1,"");assert(dl_agent_enqueue(s,&a)==DL_SCHED_OK);
    assert(dl_agent_claim_id(s,a.id,&out)==DL_SCHED_EMPTY);
    external=1;assert(dl_agent_claim_id(s,a.id,&out)==DL_SCHED_OK);first=out.generation;dl_agent_job_clear(&out);
    assert(dl_agent_park(s,a.id,first)==DL_SCHED_OK);external=2;assert(dl_agent_wake(s,a.id,first)==DL_SCHED_LIMIT);
    external=1;assert(dl_agent_wake(s,a.id,first)==DL_SCHED_OK);assert(dl_agent_finish(s,a.id,first,"succeeded")==DL_SCHED_OK);
    dl_agent_scheduler_external_load(s,NULL,NULL);
    dl_agent_scheduler_close(s);assert(sqlite3_close(db)==SQLITE_OK);puts("PASS scheduler claims, resource/chat exclusion, stale completion, nested cancellation, explicit recovery, queued-only retraction, isolation and limits");return 0;
}
