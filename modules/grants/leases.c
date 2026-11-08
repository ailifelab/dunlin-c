#define _POSIX_C_SOURCE 200809L
#include "dunlin/grant_broker.h"
#include <sqlite3.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
struct dl_grant_broker {sqlite3 *db;char *principal;dl_grant_adapter adapters[16];size_t count;};
static int valid(const char *s,size_t limit){return s&&*s&&strlen(s)<=limit;}
static sqlite3_stmt *prep(dl_grant_broker *b,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(b->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void text(sqlite3_stmt *q,int n,const char *s){sqlite3_bind_text(q,n,s,-1,SQLITE_TRANSIENT);}
static int done(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
dl_grant_broker *dl_grant_open(sqlite3 *db,const char *principal,int recover){
 if(!db||!valid(principal,128))return NULL;
 dl_grant_broker *b=calloc(1,sizeof *b);if(!b)return NULL;b->db=db;b->principal=strdup(principal);if(!b->principal){free(b);return NULL;}
 if(sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS capability_leases(id TEXT PRIMARY KEY,principal TEXT NOT NULL,chat TEXT NOT NULL,protocol TEXT NOT NULL,resource TEXT NOT NULL,scope TEXT NOT NULL,started INTEGER NOT NULL,expires INTEGER NOT NULL,state TEXT NOT NULL DEFAULT 'active',generation INTEGER NOT NULL DEFAULT 0,revoke_started INTEGER,revoked INTEGER);CREATE INDEX IF NOT EXISTS capability_owner ON capability_leases(principal,chat);",NULL,NULL,NULL)!=SQLITE_OK){dl_grant_close(b);return NULL;}
 if(recover){sqlite3_stmt *q=prep(b,"UPDATE capability_leases SET state=CASE WHEN state='revoking' THEN 'unknown' ELSE 'expired' END WHERE principal=? AND state IN('revoking','active')");if(q)text(q,1,principal);if(!done(q)){dl_grant_close(b);return NULL;}}
 return b;
}
void dl_grant_close(dl_grant_broker *b){if(b){for(size_t i=0;i<b->count;i++)free((void*)b->adapters[i].protocol);free(b->principal);free(b);}}
void dl_grant_free(void *p){free(p);}
int dl_grant_register(dl_grant_broker *b,const dl_grant_adapter *a){
 if(!b||!a||!valid(a->protocol,64)||!a->revoke||b->count==16)return 0;
 for(size_t i=0;i<b->count;i++)if(!strcmp(b->adapters[i].protocol,a->protocol))return 0;
 char *protocol=strdup(a->protocol);if(!protocol)return 0;b->adapters[b->count]=*a;b->adapters[b->count++].protocol=protocol;return 1;
}
int dl_grant_issue(dl_grant_broker *b,const char *id,const char *chat,const char *protocol,const char *resource,const char *scope,int64_t now,int64_t expires){
 if(!b||!valid(id,128)||!valid(chat,128)||!valid(protocol,64)||!valid(resource,1024)||!valid(scope,128)||now<0||expires<=now||expires-now>2592000)return 0;
 int supported=0;for(size_t i=0;i<b->count;i++)if(!strcmp(b->adapters[i].protocol,protocol))supported=1;if(!supported)return 0;
 sqlite3_stmt *q=prep(b,"INSERT INTO capability_leases(id,principal,chat,protocol,resource,scope,started,expires) VALUES(?,?,?,?,?,?,?,?)");if(q){text(q,1,id);text(q,2,b->principal);text(q,3,chat);text(q,4,protocol);text(q,5,resource);text(q,6,scope);sqlite3_bind_int64(q,7,now);sqlite3_bind_int64(q,8,expires);}return done(q);
}
int dl_grant_valid(dl_grant_broker *b,const char *id,const char *chat,const char *scope,int64_t now){
 if(!b||!valid(id,128)||!valid(chat,128)||!valid(scope,128)||now<0)return 0;
 sqlite3_stmt *expiry=prep(b,"UPDATE capability_leases SET state='expired' WHERE id=? AND principal=? AND state='active' AND expires<=?");
 if(expiry){text(expiry,1,id);text(expiry,2,b->principal);sqlite3_bind_int64(expiry,3,now);}if(!done(expiry))return 0;
 sqlite3_stmt *q=prep(b,"SELECT 1 FROM capability_leases WHERE id=? AND principal=? AND chat=? AND scope=? AND state='active' AND started<=? AND expires>?");if(q){text(q,1,id);text(q,2,b->principal);text(q,3,chat);text(q,4,scope);sqlite3_bind_int64(q,5,now);sqlite3_bind_int64(q,6,now);}int ok=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);return ok;
}
int dl_grant_revoke(dl_grant_broker *b,const char *id,int64_t now){
 if(!b||!valid(id,128)||now<0||!sqlite3_get_autocommit(b->db))return 0;
 if(sqlite3_exec(b->db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return 0;
 sqlite3_stmt *q=prep(b,"SELECT protocol,resource,state,generation FROM capability_leases WHERE id=? AND principal=?");if(q){text(q,1,id);text(q,2,b->principal);}int found=q&&sqlite3_step(q)==SQLITE_ROW;
 const char *state=found?(const char*)sqlite3_column_text(q,2):NULL;int revoked=state&&!strcmp(state,"revoked"),busy=state&&!strcmp(state,"revoking");
 char *protocol=found?strdup((const char*)sqlite3_column_text(q,0)):NULL,*resource=found?strdup((const char*)sqlite3_column_text(q,1)):NULL;int64_t generation=found?sqlite3_column_int64(q,3):0;if(generation==INT64_MAX)busy=1;else generation++;sqlite3_finalize(q);
 if(!protocol||!resource||busy||revoked){free(protocol);free(resource);sqlite3_exec(b->db,"ROLLBACK",NULL,NULL,NULL);return revoked;}
 q=prep(b,"UPDATE capability_leases SET state='revoking',generation=?,revoke_started=? WHERE id=? AND principal=?");if(q){sqlite3_bind_int64(q,1,generation);sqlite3_bind_int64(q,2,now);text(q,3,id);text(q,4,b->principal);}int ok=done(q)&&sqlite3_changes(b->db)==1;
 if(!ok||sqlite3_exec(b->db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(b->db,"ROLLBACK",NULL,NULL,NULL);free(protocol);free(resource);return 0;}
 dl_grant_revoke_result result=DL_GRANT_RETRY;for(size_t i=0;i<b->count;i++)if(!strcmp(protocol,b->adapters[i].protocol)){result=b->adapters[i].revoke(b->adapters[i].user,resource);break;}
 free(protocol);free(resource);const char *next=result==DL_GRANT_REVOKED?"revoked":result==DL_GRANT_NEEDS_USER?"needs_user":result==DL_GRANT_RETRY?"retry":"unknown";
 q=prep(b,"UPDATE capability_leases SET state=?,revoked=CASE WHEN ?='revoked' THEN ? ELSE NULL END WHERE id=? AND principal=? AND state='revoking' AND generation=?");if(q){text(q,1,next);text(q,2,next);sqlite3_bind_int64(q,3,now);text(q,4,id);text(q,5,b->principal);sqlite3_bind_int64(q,6,generation);}return done(q)&&sqlite3_changes(b->db)==1;
}
char *dl_grant_list(dl_grant_broker *b,const char *chat){
 if(!b||!chat||strlen(chat)>128)return NULL;sqlite3_stmt *q=prep(b,"SELECT id,chat,protocol,scope,state,started,expires,generation,COALESCE(revoke_started,0),COALESCE(revoked,0) FROM capability_leases WHERE principal=? AND (?='' OR chat=?) ORDER BY started DESC,rowid DESC LIMIT 200");if(!q)return NULL;text(q,1,b->principal);text(q,2,chat);text(q,3,chat);
 struct json_object *rows=json_object_new_array();const char *keys[]={"id","chat","protocol","scope","state","started","expires","generation","revokeStarted","revoked"};int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){struct json_object *r=json_object_new_object();for(int i=0;i<10;i++)json_object_object_add(r,keys[i],i<5?json_object_new_string((const char*)sqlite3_column_text(q,i)):json_object_new_int64(sqlite3_column_int64(q,i)));json_object_array_add(rows,r);}sqlite3_finalize(q);char *out=rc==SQLITE_DONE?strdup(json_object_to_json_string_ext(rows,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(rows);return out;
}
