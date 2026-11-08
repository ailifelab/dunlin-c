#include "dunlin/agent_scheduler.h"
#include <sqlite3.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {pthread_mutex_t mutex;pthread_cond_t condition;int ready,start;} gate;
typedef struct {dl_agent_scheduler *scheduler;gate *gate;dl_sched_status status;dl_agent_job job;} worker;
static void *claim(void *raw){worker *w=raw;gate *g=w->gate;pthread_mutex_lock(&g->mutex);g->ready++;pthread_cond_broadcast(&g->condition);while(!g->start)pthread_cond_wait(&g->condition,&g->mutex);pthread_mutex_unlock(&g->mutex);w->status=dl_agent_claim(w->scheduler,&w->job);return NULL;}
static void race(dl_agent_scheduler *a,dl_agent_scheduler *b,int expected){
    gate g={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0,0};worker w[2]={{.scheduler=a,.gate=&g},{.scheduler=b,.gate=&g}};pthread_t threads[2];
    for(int i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,claim,&w[i]));
    pthread_mutex_lock(&g.mutex);while(g.ready<2)pthread_cond_wait(&g.condition,&g.mutex);g.start=1;pthread_cond_broadcast(&g.condition);pthread_mutex_unlock(&g.mutex);
    int successes=0;for(int i=0;i<2;i++){assert(!pthread_join(threads[i],NULL));assert(w[i].status==DL_SCHED_OK||w[i].status==DL_SCHED_EMPTY);successes+=w[i].status==DL_SCHED_OK;}
    assert(successes==expected);if(expected==2){assert(strcmp(w[0].job.id,w[1].job.id));assert(w[0].job.generation!=w[1].job.generation);}
    for(int i=0;i<2;i++){if(w[i].status==DL_SCHED_OK)assert(dl_agent_finish(w[i].scheduler,w[i].job.id,w[i].job.generation,"succeeded")==DL_SCHED_OK);dl_agent_job_clear(&w[i].job);}
    pthread_cond_destroy(&g.condition);pthread_mutex_destroy(&g.mutex);
}
static void enqueue(dl_agent_scheduler *s,const char *id,const char *chat,const char *resource,int write){dl_agent_job j={0};snprintf(j.id,sizeof j.id,"%s",id);snprintf(j.chat,sizeof j.chat,"%s",chat);snprintf(j.resource,sizeof j.resource,"%s",resource);strcpy(j.project,"project");strcpy(j.model,"model");j.task="Concurrent task";j.write_access=write;assert(dl_agent_enqueue(s,&j)==DL_SCHED_OK);}
int main(int argc,char **argv){
    assert(argc==2);sqlite3 *db[2]={0};dl_agent_scheduler *s[2]={0};for(int i=0;i<2;i++){assert(sqlite3_open(argv[1],&db[i])==SQLITE_OK);sqlite3_busy_timeout(db[i],3000);s[i]=dl_agent_scheduler_open(db[i],"owner",2);assert(s[i]);}
    enqueue(s[0],"one","one","shared",0);enqueue(s[0],"two","two","shared",0);race(s[0],s[1],2);
    enqueue(s[0],"same-one","same","one",0);enqueue(s[0],"same-two","same","two",0);race(s[0],s[1],1);race(s[0],s[1],1);
    enqueue(s[0],"writer","writer","exclusive",1);enqueue(s[0],"reader","reader","exclusive",0);race(s[0],s[1],1);race(s[0],s[1],1);
    race(s[0],s[1],0);for(int i=0;i<2;i++){dl_agent_scheduler_close(s[i]);assert(sqlite3_close(db[i])==SQLITE_OK);}puts("PASS simultaneous two-connection claims, readers, writer exclusion, chat exclusion and unique generations");return 0;
}
