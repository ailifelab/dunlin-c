#include "dunlin/agent_scheduler.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct dl_agent_scheduler {sqlite3 *db;unsigned (*external)(void *);void *external_user;};
static int ident(const char *s,size_t max,int empty,int resource){
    if(!s)return 0;size_t n=0;while(n<=max&&s[n])n++;if(n>max||(!n&&!empty))return 0;
    for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||(resource&&c==':')))return 0;}return 1;
}
static int utf8(const char *s){
    if(!s||!*s)return 0;size_t n=strlen(s);if(n>65536)return 0;
    for(size_t i=0;i<n;){unsigned c=(unsigned char)s[i++],v=c,min=0;int more=0;
        if(c<128)continue;
        if(c>=194&&c<=223){more=1;v=c&31;min=128;}else if(c>=224&&c<=239){more=2;v=c&15;min=2048;}else if(c>=240&&c<=244){more=3;v=c&7;min=65536;}else return 0;
        while(more--){if(i>=n)return 0;c=(unsigned char)s[i++];if((c&192)!=128)return 0;v=(v<<6)|(c&63);}
        if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
    }return 1;
}
static int sql(dl_agent_scheduler *s,const char *q){return sqlite3_exec(s->db,q,NULL,NULL,NULL)==SQLITE_OK;}
static sqlite3_stmt *prepare(dl_agent_scheduler *s,const char *q){sqlite3_stmt *p=NULL;if(sqlite3_prepare_v2(s->db,q,-1,&p,NULL)!=SQLITE_OK)return NULL;return p;}
static void bind(sqlite3_stmt *p,int i,const char *v){sqlite3_bind_text(p,i,v,-1,SQLITE_TRANSIENT);}
static dl_sched_status begin(dl_agent_scheduler *s){if(!s)return DL_SCHED_INVALID;if(!sqlite3_get_autocommit(s->db))return DL_SCHED_TRANSACTION;return sql(s,"BEGIN IMMEDIATE")?DL_SCHED_OK:DL_SCHED_STORAGE;}
static dl_sched_status end(dl_agent_scheduler *s,dl_sched_status status){if(status==DL_SCHED_OK&&sql(s,"COMMIT"))return status;sql(s,"ROLLBACK");return status==DL_SCHED_OK?DL_SCHED_STORAGE:status;}
dl_agent_scheduler *dl_agent_scheduler_open(sqlite3 *db,const char *principal,unsigned capacity){
    if(!db||!ident(principal,64,0,0)||!capacity||capacity>64||sqlite3_db_readonly(db,"main")!=0||!sqlite3_get_autocommit(db))return NULL;
    dl_agent_scheduler *s=calloc(1,sizeof *s);if(!s)return NULL;s->db=db;
    if(begin(s)!=DL_SCHED_OK){free(s);return NULL;}
    int ok=sql(s,"CREATE TABLE IF NOT EXISTS agent_scheduler_meta(id INTEGER PRIMARY KEY CHECK(id=1),version INTEGER NOT NULL,principal TEXT NOT NULL,capacity INTEGER NOT NULL,next_generation INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS agent_jobs(seq INTEGER PRIMARY KEY,id TEXT NOT NULL UNIQUE,project TEXT NOT NULL,chat TEXT NOT NULL,parent TEXT NOT NULL,model TEXT NOT NULL,resource TEXT NOT NULL,write_access INTEGER NOT NULL,depth INTEGER NOT NULL,task TEXT NOT NULL,state TEXT NOT NULL,generation INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS agent_jobs_state ON agent_jobs(state,seq);CREATE INDEX IF NOT EXISTS agent_jobs_parent ON agent_jobs(parent);");
    sqlite3_stmt *p=ok?prepare(s,"INSERT OR IGNORE INTO agent_scheduler_meta VALUES(1,1,?,?,0)"):NULL;
    if(p){bind(p,1,principal);sqlite3_bind_int(p,2,(int)capacity);ok=sqlite3_step(p)==SQLITE_DONE;}else ok=0;sqlite3_finalize(p);
    p=ok?prepare(s,"SELECT version,principal,capacity,next_generation FROM agent_scheduler_meta WHERE id=1 AND typeof(version)='integer' AND typeof(principal)='text' AND length(CAST(principal AS BLOB))=length(principal) AND typeof(capacity)='integer' AND typeof(next_generation)='integer'"):NULL;
    ok=p&&sqlite3_step(p)==SQLITE_ROW&&sqlite3_column_int(p,0)==1&&!strcmp((const char*)sqlite3_column_text(p,1),principal)&&sqlite3_column_int(p,2)==(int)capacity&&sqlite3_column_int64(p,3)>=0;sqlite3_finalize(p);
    if(end(s,ok?DL_SCHED_OK:DL_SCHED_STORAGE)!=DL_SCHED_OK){free(s);return NULL;}return s;
}
void dl_agent_scheduler_close(dl_agent_scheduler *s){free(s);}
void dl_agent_scheduler_external_load(dl_agent_scheduler *s,unsigned (*load)(void *),void *user){if(s){s->external=load;s->external_user=user;}}
void dl_agent_job_clear(dl_agent_job *j){if(j){free(j->task);memset(j,0,sizeof *j);}}
static int job_row(sqlite3_stmt *p,dl_agent_job *j){
    memset(j,0,sizeof *j);char *dest[]={j->id,j->project,j->chat,j->parent,j->model,j->resource,j->state};size_t cap[]={65,65,65,65,65,129,24};
    for(int i=0;i<7;i++){const char *v=(const char*)sqlite3_column_text(p,i);int n=sqlite3_column_bytes(p,i);if(!v||n<0||(size_t)n>=cap[i]||strlen(v)!=(size_t)n)return 0;memcpy(dest[i],v,(size_t)n+1);}
    if(!ident(j->id,64,0,0)||!ident(j->project,64,0,0)||!ident(j->chat,64,0,0)||!ident(j->parent,64,1,0)||!ident(j->model,64,0,0)||!ident(j->resource,128,0,1))return 0;
    const char *states[]={"queued","running","waiting","cancel_requested","succeeded","failed","cancelled","interrupted"};int known=0;for(size_t i=0;i<sizeof states/sizeof *states;i++)if(!strcmp(j->state,states[i]))known=1;if(!known)return 0;
    j->write_access=sqlite3_column_int(p,7);j->depth=sqlite3_column_int(p,8);j->generation=sqlite3_column_int64(p,9);
    if((j->write_access!=0&&j->write_access!=1)||j->depth<0||j->depth>8||j->generation<0)return 0;
    const char *task=(const char*)sqlite3_column_text(p,10);int n=sqlite3_column_bytes(p,10);if(!task||n<1||n>65536||strlen(task)!=(size_t)n||!utf8(task))return 0;
    j->task=malloc((size_t)n+1);if(!j->task)return 0;memcpy(j->task,task,(size_t)n+1);return 1;
}
dl_sched_status dl_agent_job_get(dl_agent_scheduler *s,const char *id,dl_agent_job *j){
    if(j)memset(j,0,sizeof *j);if(!s||!j||!ident(id,64,0,0))return DL_SCHED_INVALID;
    sqlite3_stmt *p=prepare(s,"SELECT id,project,chat,parent,model,resource,state,write_access,depth,generation,task FROM agent_jobs WHERE id=?");if(!p)return DL_SCHED_STORAGE;bind(p,1,id);int rc=sqlite3_step(p);dl_sched_status status=rc==SQLITE_ROW?(job_row(p,j)?DL_SCHED_OK:DL_SCHED_STORAGE):rc==SQLITE_DONE?DL_SCHED_EMPTY:DL_SCHED_STORAGE;sqlite3_finalize(p);return status;
}
dl_sched_status dl_agent_enqueue(dl_agent_scheduler *s,const dl_agent_job *j){
    if(!j||!ident(j->id,64,0,0)||!ident(j->project,64,0,0)||!ident(j->chat,64,0,0)||!ident(j->parent,64,1,0)||!ident(j->model,64,0,0)||!ident(j->resource,128,0,1)||!utf8(j->task)||(j->write_access!=0&&j->write_access!=1))return DL_SCHED_INVALID;
    dl_sched_status status=begin(s);if(status!=DL_SCHED_OK)return status;
    dl_agent_job old={0};status=dl_agent_job_get(s,j->id,&old);
    if(status==DL_SCHED_OK){int same=!strcmp(j->project,old.project)&&!strcmp(j->chat,old.chat)&&!strcmp(j->parent,old.parent)&&!strcmp(j->model,old.model)&&!strcmp(j->resource,old.resource)&&!strcmp(j->task,old.task)&&j->write_access==old.write_access;dl_agent_job_clear(&old);return end(s,same?DL_SCHED_OK:DL_SCHED_CONFLICT);}
    if(status!=DL_SCHED_EMPTY)return end(s,status);
    int depth=0;
    if(*j->parent){status=dl_agent_job_get(s,j->parent,&old);if(status!=DL_SCHED_OK)return end(s,status==DL_SCHED_EMPTY?DL_SCHED_INVALID:status);
        int allowed=!strcmp(j->project,old.project)&&(!strcmp(old.state,"queued")||!strcmp(old.state,"running")||!strcmp(old.state,"waiting"));depth=old.depth+1;dl_agent_job_clear(&old);if(!allowed)return end(s,DL_SCHED_CONFLICT);if(depth>8)return end(s,DL_SCHED_LIMIT);}
    sqlite3_stmt *p=prepare(s,"SELECT count(*) FROM agent_jobs WHERE state IN('queued','running','cancel_requested','waiting')");if(!p)return end(s,DL_SCHED_STORAGE);int rc=sqlite3_step(p);int full=rc==SQLITE_ROW&&sqlite3_column_int64(p,0)>=4096;sqlite3_finalize(p);if(rc!=SQLITE_ROW||full)return end(s,full?DL_SCHED_LIMIT:DL_SCHED_STORAGE);
    p=prepare(s,"INSERT INTO agent_jobs(id,project,chat,parent,model,resource,write_access,depth,task,state) VALUES(?,?,?,?,?,?,?,?,?,'queued')");if(!p)return end(s,DL_SCHED_STORAGE);
    const char *values[]={j->id,j->project,j->chat,j->parent,j->model,j->resource};for(int i=0;i<6;i++)bind(p,i+1,values[i]);sqlite3_bind_int(p,7,j->write_access);sqlite3_bind_int(p,8,depth);bind(p,9,j->task);int ok=sqlite3_step(p)==SQLITE_DONE;sqlite3_finalize(p);return end(s,ok?DL_SCHED_OK:DL_SCHED_STORAGE);
}
static dl_sched_status claim_job(dl_agent_scheduler *s,const char *wanted,dl_agent_job *j){
    if(j)memset(j,0,sizeof *j);if(!j)return DL_SCHED_INVALID;dl_sched_status status=begin(s);if(status!=DL_SCHED_OK)return status;
    sqlite3_stmt *p=prepare(s,"SELECT q.id FROM agent_jobs q WHERE q.state='queued' AND (?='' OR q.id=?) AND (SELECT count(*) FROM agent_jobs WHERE state IN('running','cancel_requested'))+?3<(SELECT capacity FROM agent_scheduler_meta WHERE id=1) AND NOT EXISTS(SELECT 1 FROM agent_jobs a WHERE a.state IN('running','cancel_requested') AND (a.chat=q.chat OR (a.resource=q.resource AND (a.write_access=1 OR q.write_access=1)))) ORDER BY q.seq LIMIT 1");
    if(!p)return end(s,DL_SCHED_STORAGE);bind(p,1,wanted);bind(p,2,wanted);sqlite3_bind_int64(p,3,s->external?s->external(s->external_user):0);int rc=sqlite3_step(p);char id[65]="";if(rc==SQLITE_ROW){const char *v=(const char*)sqlite3_column_text(p,0);if(!ident(v,64,0,0)){sqlite3_finalize(p);return end(s,DL_SCHED_STORAGE);}snprintf(id,sizeof id,"%s",v);}sqlite3_finalize(p);if(rc!=SQLITE_ROW)return end(s,rc==SQLITE_DONE?DL_SCHED_EMPTY:DL_SCHED_STORAGE);
    if(!sql(s,"UPDATE agent_scheduler_meta SET next_generation=next_generation+1 WHERE id=1 AND next_generation<9223372036854775807")||sqlite3_changes(s->db)!=1)return end(s,DL_SCHED_LIMIT);
    p=prepare(s,"UPDATE agent_jobs SET state='running',generation=(SELECT next_generation FROM agent_scheduler_meta WHERE id=1) WHERE id=? AND state='queued'");if(!p)return end(s,DL_SCHED_STORAGE);bind(p,1,id);int ok=sqlite3_step(p)==SQLITE_DONE&&sqlite3_changes(s->db)==1;sqlite3_finalize(p);if(!ok)return end(s,DL_SCHED_STORAGE);
    status=dl_agent_job_get(s,id,j);status=end(s,status);if(status!=DL_SCHED_OK)dl_agent_job_clear(j);return status;
}
dl_sched_status dl_agent_claim(dl_agent_scheduler *s,dl_agent_job *j){return claim_job(s,"",j);}
dl_sched_status dl_agent_claim_id(dl_agent_scheduler *s,const char *id,dl_agent_job *j){if(!ident(id,64,0,0)){if(j)memset(j,0,sizeof *j);return DL_SCHED_INVALID;}return claim_job(s,id,j);}
dl_sched_status dl_agent_finish(dl_agent_scheduler *s,const char *id,int64_t generation,const char *outcome){
    if(!ident(id,64,0,0)||generation<=0||!outcome||(strcmp(outcome,"succeeded")&&strcmp(outcome,"failed")))return DL_SCHED_INVALID;
    dl_sched_status status=begin(s);if(status!=DL_SCHED_OK)return status;
    sqlite3_stmt *p=prepare(s,"UPDATE agent_jobs SET state=CASE WHEN state='cancel_requested' THEN 'cancelled' ELSE ? END WHERE id=? AND generation=? AND state IN('running','cancel_requested')");if(!p)return end(s,DL_SCHED_STORAGE);bind(p,1,outcome);bind(p,2,id);sqlite3_bind_int64(p,3,generation);int rc=sqlite3_step(p),changed=sqlite3_changes(s->db);sqlite3_finalize(p);return end(s,rc!=SQLITE_DONE?DL_SCHED_STORAGE:changed==1?DL_SCHED_OK:DL_SCHED_STALE);
}
static dl_sched_status cancel_job(dl_agent_scheduler *s,const char *id,int queued_only){
    if(!ident(id,64,0,0))return DL_SCHED_INVALID;dl_sched_status status=begin(s);if(status!=DL_SCHED_OK)return status;
    dl_agent_job job={0};status=dl_agent_job_get(s,id,&job);if(status==DL_SCHED_OK&&queued_only&&strcmp(job.state,"queued"))status=DL_SCHED_STALE;dl_agent_job_clear(&job);if(status!=DL_SCHED_OK)return end(s,status);
    sqlite3_stmt *p=prepare(s,"WITH RECURSIVE family(id) AS (SELECT id FROM agent_jobs WHERE id=? UNION SELECT j.id FROM agent_jobs j JOIN family f ON j.parent=f.id) UPDATE agent_jobs SET state=CASE WHEN state IN('queued','waiting') THEN 'cancelled' ELSE 'cancel_requested' END WHERE id IN(SELECT id FROM family) AND state IN('queued','running','waiting')");if(!p)return end(s,DL_SCHED_STORAGE);bind(p,1,id);int ok=sqlite3_step(p)==SQLITE_DONE;sqlite3_finalize(p);return end(s,ok?DL_SCHED_OK:DL_SCHED_STORAGE);
}
dl_sched_status dl_agent_cancel(dl_agent_scheduler *s,const char *id){return cancel_job(s,id,0);}
dl_sched_status dl_agent_cancel_queued(dl_agent_scheduler *s,const char *id){return cancel_job(s,id,1);}
static dl_sched_status lease_transition(dl_agent_scheduler *s,const char *id,int64_t generation,int wake){
    if(!ident(id,64,0,0)||generation<=0)return DL_SCHED_INVALID;dl_sched_status status=begin(s);if(status!=DL_SCHED_OK)return status;
    dl_agent_job job={0};status=dl_agent_job_get(s,id,&job);int valid=status==DL_SCHED_OK&&job.generation==generation&&!strcmp(job.state,wake?"waiting":"running");dl_agent_job_clear(&job);if(!valid)return end(s,status==DL_SCHED_OK||status==DL_SCHED_EMPTY?DL_SCHED_STALE:status);
    const char *query=wake?"UPDATE agent_jobs AS q SET state='running' WHERE id=? AND generation=? AND state='waiting' AND (SELECT count(*) FROM agent_jobs WHERE state IN('running','cancel_requested'))+?3<(SELECT capacity FROM agent_scheduler_meta WHERE id=1) AND NOT EXISTS(SELECT 1 FROM agent_jobs a WHERE a.state IN('running','cancel_requested') AND (a.chat=q.chat OR (a.resource=q.resource AND (a.write_access=1 OR q.write_access=1))))":"UPDATE agent_jobs SET state='waiting' WHERE id=? AND generation=? AND state='running'";
    sqlite3_stmt *p=prepare(s,query);if(!p)return end(s,DL_SCHED_STORAGE);bind(p,1,id);sqlite3_bind_int64(p,2,generation);if(wake)sqlite3_bind_int64(p,3,s->external?s->external(s->external_user):0);int rc=sqlite3_step(p),changed=sqlite3_changes(s->db);sqlite3_finalize(p);return end(s,rc!=SQLITE_DONE?DL_SCHED_STORAGE:changed?DL_SCHED_OK:DL_SCHED_LIMIT);
}
dl_sched_status dl_agent_park(dl_agent_scheduler *s,const char *id,int64_t generation){return lease_transition(s,id,generation,0);}
dl_sched_status dl_agent_wake(dl_agent_scheduler *s,const char *id,int64_t generation){return lease_transition(s,id,generation,1);}
dl_sched_status dl_agent_scheduler_recover(dl_agent_scheduler *s){
    dl_sched_status status=begin(s);if(status!=DL_SCHED_OK)return status;
    int ok=sql(s,"WITH RECURSIVE affected(id) AS (SELECT id FROM agent_jobs WHERE state IN('running','cancel_requested','waiting') UNION SELECT j.id FROM agent_jobs j JOIN affected a ON j.parent=a.id) UPDATE agent_jobs SET state=CASE WHEN state='queued' THEN 'cancelled' ELSE 'interrupted' END WHERE id IN(SELECT id FROM affected) AND state IN('queued','running','cancel_requested','waiting')");
    return end(s,ok?DL_SCHED_OK:DL_SCHED_STORAGE);
}
