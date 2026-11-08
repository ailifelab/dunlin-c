#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "dunlin/trajectory.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
typedef struct json_object J;
struct dl_trajectory {sqlite3 *db;int enabled,capture,readonly,lock;int64_t quota;};
struct dl_trace_span {
    dl_trajectory *owner;char id[33];const char *secret;
    uint64_t started,model_started;unsigned attempt;int in_model,partial,first_observed;
    char delta[65537];size_t delta_used;int delta_partial;
};
static uint64_t monotonic(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;}
static int64_t wall(void){struct timespec t;if(clock_gettime(CLOCK_REALTIME,&t))return 0;return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int sql(dl_trajectory *t,const char *s){return t&&t->db&&sqlite3_exec(t->db,s,NULL,NULL,NULL)==SQLITE_OK;}
static sqlite3_stmt *prep(dl_trajectory *t,const char *s){sqlite3_stmt *q=NULL;return t&&t->db&&sqlite3_prepare_v2(t->db,s,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int i,const char *v){sqlite3_bind_text(q,i,v?v:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static J *reply(int ok,const char *error,J *data){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(r,"error",json_object_new_string(error));if(data)json_object_object_add(r,"data",data);return r;}
static int64_t scalar(dl_trajectory *t,const char *s){sqlite3_stmt *q=prep(t,s);int64_t n=q&&sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int64(q,0):-1;sqlite3_finalize(q);return n;}
static int safe_file(const char *p){struct stat s;return lstat(p,&s)?errno==ENOENT:S_ISREG(s.st_mode)&&s.st_nlink==1&&s.st_uid==geteuid();}
static dl_trajectory *open_mode(const char *directory,int readonly){
    if(!directory||strlen(directory)>3968)return NULL;
    char parent[4096],path[4096];struct stat info;if(!realpath(directory,parent)||strlen(parent)>3968||stat(parent,&info)||!S_ISDIR(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&077))return NULL;snprintf(path,sizeof path,"%s/trajectory.sqlite",parent);if(!safe_file(path))return NULL;
    dl_trajectory *t=calloc(1,sizeof *t);if(!t)return NULL;t->lock=-1;
    t->readonly=readonly;
    if(!readonly){char lock[4096];snprintf(lock,sizeof lock,"%s/trajectory.lock",parent);t->lock=open(lock,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);if(t->lock<0||fstat(t->lock,&info)||!S_ISREG(info.st_mode)||info.st_nlink!=1||info.st_uid!=geteuid()||flock(t->lock,LOCK_EX|LOCK_NB))goto failed;}
    if(sqlite3_open_v2(path,&t->db,(readonly?SQLITE_OPEN_READONLY:SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE)|SQLITE_OPEN_NOFOLLOW,NULL)!=SQLITE_OK)goto failed;
    if(!readonly)chmod(path,0600);sqlite3_busy_timeout(t->db,1000);
    if(!readonly&&!sql(t,"PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
       "CREATE TABLE IF NOT EXISTS settings(id INTEGER PRIMARY KEY CHECK(id=1),enabled INTEGER NOT NULL DEFAULT 1,capture INTEGER NOT NULL DEFAULT 1,quota INTEGER NOT NULL DEFAULT 67108864);INSERT OR IGNORE INTO settings(id) VALUES(1);"
       "CREATE TABLE IF NOT EXISTS segments(id TEXT PRIMARY KEY,chat TEXT NOT NULL,run TEXT NOT NULL,model TEXT NOT NULL,protocol TEXT NOT NULL,started INTEGER NOT NULL,duration INTEGER,first_text INTEGER,source_start INTEGER NOT NULL,source_end INTEGER,status TEXT NOT NULL,partial INTEGER NOT NULL DEFAULT 0);"
       "CREATE INDEX IF NOT EXISTS segment_chat ON segments(chat,started DESC,id);CREATE INDEX IF NOT EXISTS segment_run ON segments(chat,run);"
       "CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY,segment TEXT NOT NULL REFERENCES segments(id) ON DELETE CASCADE,kind TEXT NOT NULL,ms INTEGER NOT NULL,attempt INTEGER NOT NULL,body TEXT NOT NULL,partial INTEGER NOT NULL DEFAULT 0);"
       "CREATE INDEX IF NOT EXISTS event_segment ON events(segment,id);"
       /* A process crash supplies no duration or trustworthy completion. */
       "UPDATE segments SET status='interrupted',partial=1 WHERE status='recording';"))goto failed;
    if(!readonly){
        sqlite3_stmt *columns=prep(t,"PRAGMA table_info(settings)");int found=0,rc;
        if(!columns)goto failed;while((rc=sqlite3_step(columns))==SQLITE_ROW)if(!strcmp((const char*)sqlite3_column_text(columns,1),"used"))found=1;sqlite3_finalize(columns);if(rc!=SQLITE_DONE)goto failed;
        if(!found&&!sql(t,"ALTER TABLE settings ADD COLUMN used INTEGER NOT NULL DEFAULT 0"))goto failed;
        if(!sql(t,"BEGIN IMMEDIATE;UPDATE settings SET used=(SELECT coalesce(sum(length(CAST(body AS BLOB))+128),0) FROM events)+(SELECT count(*)*1024 FROM segments);"
            "CREATE TRIGGER IF NOT EXISTS trace_event_insert AFTER INSERT ON events BEGIN UPDATE settings SET used=used+length(CAST(NEW.body AS BLOB))+128 WHERE id=1;END;"
            "CREATE TRIGGER IF NOT EXISTS trace_event_delete AFTER DELETE ON events BEGIN UPDATE settings SET used=used-length(CAST(OLD.body AS BLOB))-128 WHERE id=1;END;"
            "CREATE TRIGGER IF NOT EXISTS trace_segment_insert AFTER INSERT ON segments BEGIN UPDATE settings SET used=used+1024 WHERE id=1;END;"
            "CREATE TRIGGER IF NOT EXISTS trace_segment_delete AFTER DELETE ON segments BEGIN UPDATE settings SET used=used-1024 WHERE id=1;END;COMMIT;"))goto failed;
    }
    sqlite3_stmt *q=prep(t,"SELECT enabled,capture,quota FROM settings WHERE id=1");
    if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);goto failed;}
    t->enabled=sqlite3_column_int(q,0);t->capture=sqlite3_column_int(q,1);t->quota=sqlite3_column_int64(q,2);sqlite3_finalize(q);
    return t;
failed:dl_trajectory_close(t);return NULL;
}
dl_trajectory *dl_trajectory_open(const char *directory){return open_mode(directory,0);}
dl_trajectory *dl_trajectory_open_reader(const char *directory){return open_mode(directory,1);}
dl_trajectory *dl_trajectory_open_worker(dl_trajectory *owner){
    if(!owner||owner->readonly||owner->lock<0)return NULL;
    const char *path=sqlite3_db_filename(owner->db,"main");
    dl_trajectory *t=calloc(1,sizeof *t);if(!t)return NULL;t->lock=dup(owner->lock);
    if(t->lock<0||!path||!safe_file(path)||sqlite3_open_v2(path,&t->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOFOLLOW,NULL)!=SQLITE_OK)goto failed;
    fcntl(t->lock,F_SETFD,FD_CLOEXEC);sqlite3_busy_timeout(t->db,3000);
    if(!sql(t,"PRAGMA foreign_keys=ON;PRAGMA synchronous=NORMAL;"))goto failed;
    sqlite3_stmt *q=prep(t,"SELECT enabled,capture,quota FROM settings WHERE id=1");
    if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);goto failed;}
    t->enabled=sqlite3_column_int(q,0);t->capture=sqlite3_column_int(q,1);t->quota=sqlite3_column_int64(q,2);sqlite3_finalize(q);return t;
failed:dl_trajectory_close(t);return NULL;
}
void dl_trajectory_close(dl_trajectory *t){if(t){sqlite3_close(t->db);if(t->lock>=0)close(t->lock);free(t);}}
static int sensitive(const char *key){
    const char *keys[]={"authorization","proxy-authorization","api_key","apikey","api-key","x-api-key","key_env","password","secret","access_token","refresh_token","_dunlin_provider","encrypted_content","signature","reasoning_content","reasoning_details","thinking"};
    for(size_t i=0;i<sizeof keys/sizeof *keys;i++)if(!strcasecmp(key,keys[i]))return 1;
    return !strncmp(key,"_dunlin_",8);
}
/* Redact the currently resolved model credential even when it appears in model
 * text or nested JSON tool arguments. Arbitrary user secrets are not inferred. */
static char *redact(const char *p,size_t n,const char *secret){
    size_t k=secret?strlen(secret):0,count=0;if(k)for(size_t i=0;i+k<=n;){if(!memcmp(p+i,secret,k)){count++;i+=k;}else i++;}
    if(count && k<10 && count>(SIZE_MAX-n-1)/(10-k))return NULL;
    size_t length=count?(k>10?n-count*(k-10):n+count*(10-k)):n;
    char *out=malloc(length+1);if(!out)return NULL;size_t at=0;
    for(size_t i=0;i<n;){if(k&&i+k<=n&&!memcmp(p+i,secret,k)){memcpy(out+at,"[REDACTED]",10);at+=10;i+=k;}else out[at++]=p[i++];}out[at]=0;return out;
}
typedef struct {size_t remaining;unsigned nodes;int partial;const char *secret;} scrubber;
static J *scrub(J *v,scrubber *s,unsigned depth){
    if(!v)return NULL;
    if(depth>32||++s->nodes>8192||s->remaining<32){s->partial=1;return json_object_new_string("[omitted: local trace detail limit]");}
    s->remaining-=16;
    if(json_object_is_type(v,json_type_object)){
        J *out=json_object_new_object();json_object_object_foreach(v,key,value){
            if(sensitive(key))continue;
            if(s->remaining<32||s->nodes>8192){s->partial=1;break;}
            size_t key_length=strlen(key);if(key_length>s->remaining/2){s->partial=1;break;}s->remaining-=key_length;
            char *safe_key=redact(key,key_length,s->secret);if(!safe_key){s->partial=1;break;}
            if(!strcmp(key,"arguments")&&json_object_is_type(value,json_type_string)){
                const char *raw=json_object_get_string(value);J *args=json_tokener_parse(raw);
                if(args){J *clean=scrub(args,s,depth+1);const char *text=json_object_to_json_string_ext(clean,JSON_C_TO_STRING_PLAIN);json_object_object_add(out,safe_key,json_object_new_string(text));json_object_put(clean);json_object_put(args);free(safe_key);continue;}
            }
            json_object_object_add(out,safe_key,scrub(value,s,depth+1));free(safe_key);
        }return out;
    }
    if(json_object_is_type(v,json_type_array)){
        J *out=json_object_new_array();size_t n=json_object_array_length(v);
        for(size_t i=0;i<n;i++){if(s->remaining<32||s->nodes>8192){s->partial=1;break;}json_object_array_add(out,scrub(json_object_array_get_idx(v,i),s,depth+1));}return out;
    }
    if(json_object_is_type(v,json_type_string)){
        size_t n=(size_t)json_object_get_string_len(v),cap=s->remaining>32?s->remaining-32:0;
        char *copy=redact(json_object_get_string(v),n,s->secret);if(!copy){s->partial=1;return NULL;}n=strlen(copy);
        if(n>cap){n=cap;while(n&&((unsigned char)copy[n]&0xc0)==0x80)n--;s->partial=1;}
        s->remaining-=n;J *out=json_object_new_string_len(copy,(int)n);free(copy);return out;
    }
    return json_object_get(v);
}
static void partial(dl_trace_span *span){span->partial=1;sqlite3_stmt *q=prep(span->owner,"UPDATE segments SET partial=1 WHERE id=?");if(q)bind(q,1,span->id);step(q);}
static int room(dl_trajectory *t,size_t bytes){
    /* Logical payload budget; SQLite pages/WAL and temporary files are additional.
     * Only ended segments can be evicted; an active trace becomes partial. */
    int64_t used=scalar(t,"SELECT used FROM settings WHERE id=1");if(used<0)return 0;
    while((uint64_t)used+bytes+4096>(uint64_t)t->quota){
        if(!sql(t,"DELETE FROM segments WHERE id=(SELECT id FROM segments WHERE status!='recording' ORDER BY started,id LIMIT 1)")||!sqlite3_changes(t->db))return 0;
        used=scalar(t,"SELECT used FROM settings WHERE id=1");if(used<0)return 0;
    }return 1;
}
dl_trace_span *dl_trajectory_begin(dl_trajectory *t,const char *chat,const char *run,const char *model,const char *protocol,long long start,const char *secret){
    if(!t||t->readonly||!t->enabled||!chat||!run||strlen(chat)>64||strlen(run)>64||!model||strlen(model)>1024||!protocol||strlen(protocol)>32)return NULL;
    if(!room(t,2048))return NULL;
    dl_trace_span *span=calloc(1,sizeof *span);if(!span)return NULL;span->owner=t;span->secret=secret;span->started=monotonic();
    unsigned char nonce[16];sqlite3_randomness(sizeof nonce,nonce);for(size_t i=0;i<sizeof nonce;i++)snprintf(span->id+i*2,3,"%02x",nonce[i]);
    char *safe_model=redact(model,strlen(model),secret);
    sqlite3_stmt *q=safe_model?prep(t,"INSERT INTO segments(id,chat,run,model,protocol,started,source_start,status) VALUES(?,?,?,?,?,?,?,'recording')"):NULL;
    if(q){bind(q,1,span->id);bind(q,2,chat);bind(q,3,run);bind(q,4,safe_model);bind(q,5,protocol);sqlite3_bind_int64(q,6,wall());sqlite3_bind_int64(q,7,start);}free(safe_model);
    if(!step(q)){free(span);return NULL;}return span;
}
static void append(dl_trace_span *span,const char *type,J *value,int omitted){
    dl_trajectory *t=span->owner;scrubber s={.remaining=262144,.secret=span->secret};J *clean=scrub(value,&s,0);
    const char *text=clean?json_object_to_json_string_ext(clean,JSON_C_TO_STRING_PLAIN):"null";size_t length=strlen(text);
    if(length>1048576||!room(t,length)){partial(span);json_object_put(clean);return;}
    sqlite3_stmt *q=prep(t,"INSERT INTO events(segment,kind,ms,attempt,body,partial) VALUES(?,?,?,?,?,?)");
    if(q){bind(q,1,span->id);bind(q,2,type);sqlite3_bind_int64(q,3,(int64_t)(monotonic()-span->started));sqlite3_bind_int(q,4,(int)span->attempt);bind(q,5,text);sqlite3_bind_int(q,6,s.partial||omitted);}
    if(!step(q)||s.partial||omitted)partial(span);json_object_put(clean);
}
static void flush_delta(dl_trace_span *span){
    if(!span->delta_used&&!span->delta_partial)return;
    /* The aggregation limit may cut through a model key. Drop any matching key
     * prefix at that boundary before a bounded copy can retain part of it. */
    if(span->delta_partial&&span->secret&&*span->secret){size_t k=strlen(span->secret);size_t n=k>span->delta_used?span->delta_used:k-1;for(;n;n--)if(!memcmp(span->delta+span->delta_used-n,span->secret,n)){span->delta_used-=n;break;}}
    J *o=json_object_new_object();json_object_object_add(o,"text",json_object_new_string_len(span->delta,(int)span->delta_used));
    append(span,"model.visible_text",o,span->delta_partial);json_object_put(o);span->delta_used=0;span->delta_partial=0;
}
void dl_trajectory_event(dl_trace_span *span,const char *type,const char *data,size_t length){
    if(!span||!type||strlen(type)>80||!data)return;
    /* Private reasoning deltas never enter the ledger, regardless of UI toggles. */
    if(!strcmp(type,"reasoning.delta")||!strncmp(type,"reasoning.",10))return;
    if(!strcmp(type,"delta")){
        if(!span->in_model)return;
        if(!span->first_observed){
            span->first_observed=1;uint64_t now=monotonic();sqlite3_stmt *q=prep(span->owner,"UPDATE segments SET first_text=coalesce(first_text,?) WHERE id=?");if(q){sqlite3_bind_int64(q,1,(int64_t)(now-span->started));bind(q,2,span->id);}step(q);
            J *first=json_object_new_object();json_object_object_add(first,"latency_ms",json_object_new_int64((int64_t)(now-span->model_started)));append(span,"model.first_visible_text",first,0);json_object_put(first);
        }
        if(!span->owner->capture)return;
        size_t n=length;if(n>65536-span->delta_used){n=65536-span->delta_used;while(n&&((unsigned char)data[n]&0xc0)==0x80)n--;span->delta_partial=1;}
        memcpy(span->delta+span->delta_used,data,n);span->delta_used+=n;return;
    }
    int start=eq(type,"model.request")||eq(type,"context.model.request");
    int end=eq(type,"model.request.end")||eq(type,"context.model.end");
    if(start){flush_delta(span);span->attempt++;span->in_model=1;span->first_observed=0;span->model_started=monotonic();}
    if(end)flush_delta(span);
    int detail=start||eq(type,"model.response")||eq(type,"context.model.response")||eq(type,"source.committed")||eq(type,"approval.request");
    J *value=NULL;if(length<=8388608){struct json_tokener *tok=json_tokener_new();if(tok){value=json_tokener_parse_ex(tok,data,(int)length);if(json_tokener_get_error(tok)!=json_tokener_success){json_object_put(value);value=NULL;}json_tokener_free(tok);}}
    int omitted=0;
    if(detail&&!span->owner->capture){json_object_put(value);value=json_object_new_object();json_object_object_add(value,"omitted",json_object_new_string("detail capture disabled"));omitted=1;}
    else if(!value){if(length>262144){length=262144;while(length&&((unsigned char)data[length]&0xc0)==0x80)length--;omitted=1;}value=json_object_new_string_len(data,(int)length);}
    append(span,type,value,omitted);json_object_put(value);if(end)span->in_model=0;
}
void dl_trajectory_finish(dl_trace_span *span,const char *status,long long end){
    if(!span)return;flush_delta(span);
    sqlite3_stmt *q=prep(span->owner,"UPDATE segments SET status=?,duration=?,source_end=?,partial=max(partial,?) WHERE id=?");
    if(q){bind(q,1,status?status:"unknown");sqlite3_bind_int64(q,2,(int64_t)(monotonic()-span->started));sqlite3_bind_int64(q,3,end);sqlite3_bind_int(q,4,span->partial||span->in_model);bind(q,5,span->id);}step(q);free(span);
}
int dl_trajectory_forget(dl_trajectory *t,const char *chat){if(!t||t->readonly)return 0;sqlite3_stmt *q=prep(t,"DELETE FROM segments WHERE chat=?");if(q)bind(q,1,chat);return step(q);}
static J *status(dl_trajectory *t){J *o=json_object_new_object();json_object_object_add(o,"available",json_object_new_boolean(t!=NULL));if(!t)return o;json_object_object_add(o,"enabled",json_object_new_boolean(t->enabled));json_object_object_add(o,"capture",json_object_new_boolean(t->capture));json_object_object_add(o,"quota",json_object_new_int64(t->quota));json_object_object_add(o,"payloadBytes",json_object_new_int64(scalar(t,"SELECT used FROM settings WHERE id=1")));json_object_object_add(o,"segments",json_object_new_int64(scalar(t,"SELECT count(*) FROM segments")));return o;}
static J *segments(dl_trajectory *t,J *r){
    const char *chat=str(r,"chat"),*run=str(r,"run");if(run&&strlen(run)>64)return reply(0,"invalid_trace_query",NULL);int64_t before=json_object_get_int64(get(r,"before"));if(!before)before=INT64_MAX;
    sqlite3_stmt *q=prep(t,"SELECT rowid,id,run,model,protocol,started,duration,first_text,source_start,source_end,status,partial FROM segments WHERE chat=? AND rowid<? AND (?='' OR run=?) ORDER BY rowid DESC LIMIT 51");
    if(!q)return reply(0,"trace_storage_error",NULL);bind(q,1,chat);sqlite3_bind_int64(q,2,before);bind(q,3,run);bind(q,4,run);J *a=json_object_new_array();int rc,count=0;int64_t next=0;int more=0;
    const char *keys[]={"cursor","id","run","model","protocol","started","duration","firstText","sourceStart","sourceEnd","status","partial"};
    while((rc=sqlite3_step(q))==SQLITE_ROW){if(count++==50){more=1;break;}J *v=json_object_new_object();for(int i=0;i<12;i++){if(sqlite3_column_type(q,i)==SQLITE_NULL)continue;json_object_object_add(v,keys[i],i==1||i==2||i==3||i==4||i==10?json_object_new_string((const char*)sqlite3_column_text(q,i)):i==11?json_object_new_boolean(sqlite3_column_int(q,i)):json_object_new_int64(sqlite3_column_int64(q,i)));}next=sqlite3_column_int64(q,0);json_object_array_add(a,v);}
    sqlite3_finalize(q);if(rc!=SQLITE_DONE&&rc!=SQLITE_ROW){json_object_put(a);return reply(0,"trace_storage_error",NULL);}J *o=json_object_new_object();json_object_object_add(o,"items",a);json_object_object_add(o,"more",json_object_new_boolean(more));json_object_object_add(o,"nextCursor",json_object_new_int64(next));return reply(1,NULL,o);
}
static J *events(dl_trajectory *t,J *r,int exporting){
    const char *chat=str(r,"chat"),*segment=str(r,"segment"),*kind=str(r,"kind"),*query=str(r,"query");
    if(!segment||strlen(segment)!=32||(kind&&strlen(kind)>80)||(query&&strlen(query)>256))return reply(0,"invalid_trace_query",NULL);
    sqlite3_stmt *q=prep(t,"SELECT partial,run,model,protocol,status,started,duration,source_start,source_end,first_text FROM segments WHERE id=? AND chat=?");if(q){bind(q,1,segment);bind(q,2,chat);}int row=q?sqlite3_step(q):SQLITE_ERROR,exists=row==SQLITE_ROW,segment_partial=exists?sqlite3_column_int(q,0):0;
    J *metadata=NULL;if(exporting&&exists){metadata=json_object_new_object();const char *keys[]={"partial","run","model","protocol","status","started","duration","sourceStart","sourceEnd","firstText"};for(int i=0;i<10;i++){if(sqlite3_column_type(q,i)==SQLITE_NULL)continue;json_object_object_add(metadata,keys[i],i==0?json_object_new_boolean(segment_partial):i>=1&&i<=4?json_object_new_string((const char*)sqlite3_column_text(q,i)):json_object_new_int64(sqlite3_column_int64(q,i)));}}
    sqlite3_finalize(q);if(!exists){json_object_put(metadata);return reply(0,"trace_not_found",NULL);}
    int64_t before=json_object_get_int64(get(r,"before"));if(!before||exporting)before=INT64_MAX;
    q=prep(t,"SELECT id,kind,ms,attempt,body,partial FROM events WHERE segment=? AND id<? AND (?='' OR instr(kind,?)=1) AND (?='' OR instr(body,?)>0) ORDER BY id DESC LIMIT ?");
    if(!q){json_object_put(metadata);return reply(0,"trace_storage_error",NULL);}bind(q,1,segment);sqlite3_bind_int64(q,2,before);bind(q,3,exporting?"":kind);bind(q,4,exporting?"":kind);bind(q,5,exporting?"":query);bind(q,6,exporting?"":query);sqlite3_bind_int(q,7,exporting?4097:51);
    J *a=json_object_new_array();int rc,count=0,more=0;size_t used=0;int64_t next=0;
    while((rc=sqlite3_step(q))==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,4);size_t n=raw?strlen(raw):0;
        if(count++==(exporting?4096:50)||(exporting&&used+n>8388608)){more=1;break;}used+=n;J *v=json_object_new_object();
        json_object_object_add(v,"id",json_object_new_int64(sqlite3_column_int64(q,0)));json_object_object_add(v,"kind",json_object_new_string((const char*)sqlite3_column_text(q,1)));json_object_object_add(v,"ms",json_object_new_int64(sqlite3_column_int64(q,2)));json_object_object_add(v,"attempt",json_object_new_int(sqlite3_column_int(q,3)));json_object_object_add(v,"body",raw?json_tokener_parse(raw):NULL);json_object_object_add(v,"partial",json_object_new_boolean(sqlite3_column_int(q,5)));next=sqlite3_column_int64(q,0);json_object_array_add(a,v);
    }
    sqlite3_finalize(q);if(rc!=SQLITE_DONE&&rc!=SQLITE_ROW){json_object_put(a);json_object_put(metadata);return reply(0,"trace_storage_error",NULL);}J *o=json_object_new_object();json_object_object_add(o,"items",a);json_object_object_add(o,"more",json_object_new_boolean(more));json_object_object_add(o,"nextCursor",json_object_new_int64(next));if(metadata)json_object_object_add(o,"metadata",metadata);
    if(exporting){json_object_object_add(o,"format",json_object_new_string("dunlin.local-trajectory.v1"));json_object_object_add(o,"segment",json_object_new_string(segment));json_object_object_add(o,"chat",json_object_new_string(chat));json_object_object_add(o,"order",json_object_new_string("newest-first"));json_object_object_add(o,"partial",json_object_new_boolean(more||segment_partial));json_object_object_add(o,"notice",json_object_new_string("Local observed events only. Missing data is not evidence of absent effects. Known model key and structured secret fields removed; arbitrary text may still contain private data. Review before sharing."));}
    return reply(1,NULL,o);
}
J *dl_trajectory_request(dl_trajectory *t,J *r){
    const char *op=str(r,"op");if(eq(op,"trace.status"))return reply(1,NULL,status(t));if(!t)return reply(0,"trace_cache_unavailable",NULL);
    if(t->readonly&&(eq(op,"trace.config")||eq(op,"trace.clear")))return reply(0,"trace_read_only",NULL);
    if(eq(op,"trace.config")){
        J *enabled=get(r,"enabled"),*capture=get(r,"capture"),*quota=get(r,"quota");
        if((enabled&&!json_object_is_type(enabled,json_type_boolean))||(capture&&!json_object_is_type(capture,json_type_boolean))||(quota&&(!json_object_is_type(quota,json_type_int)||json_object_get_int64(quota)<1048576||json_object_get_int64(quota)>4294967296LL)))return reply(0,"invalid_trace_settings",NULL);
        int e=enabled?json_object_get_boolean(enabled):t->enabled,c=capture?json_object_get_boolean(capture):t->capture;int64_t bytes=quota?json_object_get_int64(quota):t->quota;
        sqlite3_stmt *q=prep(t,"UPDATE settings SET enabled=?,capture=?,quota=? WHERE id=1");if(q){sqlite3_bind_int(q,1,e);sqlite3_bind_int(q,2,c);sqlite3_bind_int64(q,3,bytes);}if(!step(q))return reply(0,"trace_storage_error",NULL);t->enabled=e;t->capture=c;t->quota=bytes;room(t,0);return reply(1,NULL,status(t));
    }
    if(eq(op,"trace.segments"))return segments(t,r);
    if(eq(op,"trace.events")||eq(op,"trace.export"))return events(t,r,eq(op,"trace.export"));
    if(eq(op,"trace.clear")){const char *chat=str(r,"chat");if(!chat)return reply(0,"invalid_chat",NULL);sqlite3_stmt *q=prep(t,"DELETE FROM segments WHERE chat=? AND status!='recording'");if(q)bind(q,1,chat);return step(q)?reply(1,NULL,NULL):reply(0,"trace_storage_error",NULL);}
    return reply(0,"unsupported_trace_operation",NULL);
}
