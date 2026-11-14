#define _XOPEN_SOURCE 700
#include "../../desktop/knowledge.h"
#include "dunlin/knowledge.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/agent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int text(const char *p,size_t cap,int empty){return p&&(empty||*p)&&strlen(p)<=cap&&ah_utf8_text(p,strlen(p));}
static int ident(const char *p){return text(p,128,0)&&strspn(p,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/@+")==strlen(p);}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int i,const char *p){sqlite3_bind_text(q,i,p?p:"",-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int sql(dc_store *s,const char *query){return sqlite3_exec(s->db,query,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(r,"error",json_object_new_string(error));if(data)json_object_object_add(r,"data",data);return r;}
static J *fail(const char *e){return reply(0,e,NULL);}
int dc_knowledge_schema(dc_store *s){return sql(s,
 "CREATE TABLE IF NOT EXISTS knowledge_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS graph_nodes(seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT NOT NULL UNIQUE,owner TEXT NOT NULL,kind TEXT NOT NULL,scope TEXT NOT NULL,scope_id TEXT NOT NULL,name TEXT NOT NULL,body TEXT NOT NULL,revision INTEGER NOT NULL DEFAULT 1,state TEXT NOT NULL DEFAULT 'active',source_chat TEXT NOT NULL DEFAULT '',source_seq INTEGER NOT NULL DEFAULT -1,source_hash TEXT NOT NULL DEFAULT '',independent INTEGER NOT NULL DEFAULT 0,created INTEGER NOT NULL DEFAULT(unixepoch()),updated INTEGER NOT NULL DEFAULT(unixepoch()));"
 "CREATE TABLE IF NOT EXISTS graph_node_history(node TEXT NOT NULL,revision INTEGER NOT NULL,body TEXT NOT NULL,state TEXT NOT NULL,modified INTEGER NOT NULL DEFAULT(unixepoch()),PRIMARY KEY(node,revision));CREATE TRIGGER IF NOT EXISTS graph_history BEFORE UPDATE OF body,state ON graph_nodes WHEN NEW.revision>OLD.revision BEGIN INSERT OR IGNORE INTO graph_node_history(node,revision,body,state) VALUES(OLD.id,OLD.revision,OLD.body,OLD.state);END;"
 "CREATE INDEX IF NOT EXISTS graph_list ON graph_nodes(owner,kind,scope,scope_id,state,seq);CREATE INDEX IF NOT EXISTS graph_source ON graph_nodes(source_chat,source_seq);"
 "CREATE TABLE IF NOT EXISTS graph_edges(source TEXT NOT NULL,type TEXT NOT NULL,target TEXT NOT NULL,owner TEXT NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()),PRIMARY KEY(source,type,target));CREATE INDEX IF NOT EXISTS graph_incoming ON graph_edges(owner,target,type,source);"
 "CREATE VIRTUAL TABLE IF NOT EXISTS knowledge_fts USING fts5(node UNINDEXED,terms);"
 "CREATE TABLE IF NOT EXISTS paused_memory_sources(chat TEXT PRIMARY KEY);"
 "CREATE TABLE IF NOT EXISTS memory_subjects(id TEXT PRIMARY KEY,name TEXT NOT NULL,kind TEXT NOT NULL DEFAULT 'person');");}
int dc_bind_identity(dc_store *s,const char *principal){
 if(!s||!ident(principal)||!sql(s,"BEGIN IMMEDIATE"))return 0;sqlite3_stmt *q=prep(s,"INSERT OR IGNORE INTO knowledge_meta(key,value) VALUES('owner',?)");if(q)bind(q,1,principal);int ok=step(q);q=prep(s,"SELECT value FROM knowledge_meta WHERE key='owner'");if(q&&sqlite3_step(q)==SQLITE_ROW)ok=ok&&eq((const char*)sqlite3_column_text(q,0),principal);else ok=0;sqlite3_finalize(q);if(ok){q=prep(s,"INSERT OR IGNORE INTO memory_subjects(id,name) VALUES(?,?)");if(q){bind(q,1,principal);bind(q,2,"当前身份");}ok=step(q);}if(!ok){sql(s,"ROLLBACK");return 0;}if(!sql(s,"COMMIT")){sql(s,"ROLLBACK");return 0;}return 1;
}
static char *owner(dc_store *s){sqlite3_stmt *q=prep(s,"SELECT value FROM knowledge_meta WHERE key='owner'");char *p=q&&sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);return p;}
static int exists(dc_store *s,const char *query,const char *id){sqlite3_stmt *q=prep(s,query);if(q)bind(q,1,id);int found=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);return found;}
static int scope_valid(dc_store *s,const char *scope,const char *id){if(!scope||!id)return 0;if(eq(scope,"global"))return !*id;if(eq(scope,"project"))return ident(id)&&exists(s,"SELECT 1 FROM projects WHERE id=?",id);if(eq(scope,"chat"))return ident(id)&&exists(s,"SELECT 1 FROM chats WHERE id=?",id);return 0;}
static J *node(sqlite3_stmt *q){
 J *o=json_object_new_object();const char *keys[]={"sequence","id","owner","kind","scope","scopeID","name","body","revision","state","sourceChat","sourceSequence","sourceHash","independent","created","updated"};for(int i=0;i<16;i++){J *v=NULL;if(i==0||i==8||i==11||i==14||i==15)v=json_object_new_int64(sqlite3_column_int64(q,i));else if(i==7){const char *raw=(const char*)sqlite3_column_text(q,i);v=raw?dunlin_json_parse(raw,strlen(raw)):NULL;}else if(i==13)v=json_object_new_boolean(sqlite3_column_int(q,i));else v=json_object_new_string((const char*)sqlite3_column_text(q,i));json_object_object_add(o,keys[i],v);}return o;
}
static const char *columns="seq,id,owner,kind,scope,scope_id,name,body,revision,state,source_chat,source_seq,source_hash,independent,created,updated";
static J *find_node(dc_store *s,const char *principal,const char *id){char query[512];snprintf(query,sizeof query,"SELECT %s FROM graph_nodes WHERE owner=? AND id=? AND state<>'deleted'",columns);sqlite3_stmt *q=prep(s,query);if(q){bind(q,1,principal);bind(q,2,id);}J *o=q&&sqlite3_step(q)==SQLITE_ROW?node(q):NULL;sqlite3_finalize(q);return o;}
static int index_node(dc_store *s,const char *id,const char *text){sqlite3_stmt *q=prep(s,"DELETE FROM knowledge_fts WHERE node=?");if(q)bind(q,1,id);if(!step(q))return 0;if(!text||!*text)return 1;char *terms=dl_index_terms(text,0);if(!terms)return 0;q=prep(s,"INSERT INTO knowledge_fts(node,terms) VALUES(?,?)");if(q){bind(q,1,id);bind(q,2,terms);}int ok=step(q);free(terms);return ok;}
static int insert_node(dc_store *s,const char *principal,const char *id,const char *kind,const char *scope,const char *scope_id,const char *name,J *body,const char *source_chat,int source_seq,const char *hash,int independent){sqlite3_stmt *q=prep(s,"INSERT INTO graph_nodes(id,owner,kind,scope,scope_id,name,body,source_chat,source_seq,source_hash,independent) VALUES(?,?,?,?,?,?,?,?,?,?,?)");if(q){const char *v[]={id,principal,kind,scope,scope_id,name,json_object_to_json_string_ext(body,JSON_C_TO_STRING_PLAIN),source_chat};for(int i=0;i<8;i++)bind(q,i+1,v[i]);sqlite3_bind_int(q,9,source_seq);bind(q,10,hash);sqlite3_bind_int(q,11,independent);}return step(q);}
static int edge(dc_store *s,const char *principal,const char *source,const char *type,const char *target){sqlite3_stmt *q=prep(s,"INSERT OR IGNORE INTO graph_edges(source,type,target,owner) VALUES(?,?,?,?)");if(q){bind(q,1,source);bind(q,2,type);bind(q,3,target);bind(q,4,principal);}return step(q);}
static J *list_nodes(dc_store *s,J *r,const char *principal,const char *kind){
 const char *scope=str(r,"scope"),*scope_id=str(r,"scopeID");int all=eq(scope,"all")&&eq(scope_id,"");if(!all&&!scope_valid(s,scope,scope_id))return fail("invalid_knowledge_scope");int64_t after=json_object_get_int64(get(r,"after"));if(after<0)return fail("invalid_cursor");int limit=json_object_get_int(get(r,"limit"));if(limit<=0)limit=50;if(limit>100)return fail("knowledge_page_limit_exceeded");char query[1024];snprintf(query,sizeof query,"SELECT %s FROM graph_nodes WHERE owner=? AND kind=? AND (?='all' OR (scope=? AND scope_id=?)) AND state<>'deleted' AND seq>? ORDER BY seq LIMIT ?",columns);sqlite3_stmt *q=prep(s,query);if(q){bind(q,1,principal);bind(q,2,kind);bind(q,3,scope);bind(q,4,scope);bind(q,5,scope_id);sqlite3_bind_int64(q,6,after);sqlite3_bind_int(q,7,limit+1);}if(!q)return fail("storage_error");J *rows=json_object_new_array();int rc,more=0;int64_t cursor=after;while((rc=sqlite3_step(q))==SQLITE_ROW){if(json_object_array_length(rows)==(size_t)limit){more=1;break;}cursor=sqlite3_column_int64(q,0);json_object_array_add(rows,node(q));}sqlite3_finalize(q);if(rc!=SQLITE_DONE&&!more){json_object_put(rows);return fail("storage_error");}J *data=json_object_new_object();json_object_object_add(data,"rows",rows);json_object_object_add(data,"next",json_object_new_int64(cursor));json_object_object_add(data,"hasMore",json_object_new_boolean(more));return reply(1,NULL,data);
}
static J *source_message(dc_store *s,const char *chat,int seq){sqlite3_stmt *q=prep(s,"SELECT json_extract(m.value,'$._dunlin_display'),json_extract(m.value,'$.content'),json_extract(m.value,'$.role') FROM chats c,json_each(c.state,'$.messages') m WHERE c.id=? AND CAST(m.key AS INTEGER)=? AND json_type(m.value,'$.content')='text'");if(q){bind(q,1,chat);sqlite3_bind_int(q,2,seq);}J *out=NULL;if(q&&sqlite3_step(q)==SQLITE_ROW){const char *display=(const char*)sqlite3_column_text(q,0),*body=(const char*)sqlite3_column_text(q,1),*role=(const char*)sqlite3_column_text(q,2);out=json_object_new_object();json_object_object_add(out,"text",json_object_new_string(display?display:body));json_object_object_add(out,"role",json_object_new_string(role));}sqlite3_finalize(q);return out;}
static int source_enabled(dc_store *s,const char *chat){
 sqlite3_stmt *q=prep(s,"SELECT NOT EXISTS(SELECT 1 FROM paused_memory_sources WHERE chat=?)");
 if(q)bind(q,1,chat);
 int enabled=q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_int(q,0);
 sqlite3_finalize(q);return enabled;
}
static J *source_policy(dc_store *s,J *r){
 const char *chat=str(r,"chat");J *enabled=get(r,"enabled");
 if(!scope_valid(s,"chat",chat))return fail("invalid_knowledge_scope");
 if(enabled){
  if(!json_object_is_type(enabled,json_type_boolean))return fail("invalid_source_policy");
  sqlite3_stmt *q=prep(s,json_object_get_boolean(enabled)?"DELETE FROM paused_memory_sources WHERE chat=?":"INSERT OR IGNORE INTO paused_memory_sources(chat) VALUES(?)");
  if(q)bind(q,1,chat);
  if(!step(q))return fail("storage_error");
 }
 J *data=json_object_new_object();
 json_object_object_add(data,"enabled",json_object_new_boolean(source_enabled(s,chat)));
 return reply(1,NULL,data);
}
static J *memory_create(dc_store *s,J *r,const char *principal){
 const char *id=str(r,"id"),*body=str(r,"text"),*subject=str(r,"subject"),*kind=str(r,"kind"),*scope=str(r,"scope"),*scope_id=str(r,"scopeID"),*chat=str(r,"sourceChat");int seq=json_object_get_int(get(r,"sourceSequence"));if(!ident(id)||!text(body,4096,0)||!ident(subject)||(!eq(kind,"fact")&&!eq(kind,"preference")&&!eq(kind,"inference"))||!scope_valid(s,scope,scope_id)||!exists(s,"SELECT 1 FROM memory_subjects WHERE id=?",subject))return fail("invalid_memory_assertion");
 if(get(r,"expiresAt")&&(!json_object_is_type(get(r,"expiresAt"),json_type_int)||json_object_get_int64(get(r,"expiresAt"))<0))return fail("invalid_memory_expiry");
 J *payload=json_object_new_object();json_object_object_add(payload,"text",json_object_new_string(body));json_object_object_add(payload,"subject",json_object_new_string(subject));json_object_object_add(payload,"kind",json_object_new_string(kind));json_object_object_add(payload,"status",json_object_new_string("proposed"));json_object_object_add(payload,"speaker",json_object_new_string(principal));json_object_object_add(payload,"speakerType",json_object_new_string("user"));json_object_object_add(payload,"expiresAt",json_object_new_int64(json_object_get_int64(get(r,"expiresAt"))));char hash[65]="";J *message=NULL;int independent=json_object_get_boolean(get(r,"independent"));
 if(chat&&*chat){if(!ident(chat)||seq<0||(message=source_message(s,chat,seq))==NULL){json_object_put(payload);return fail("memory_source_missing");}const char *original=str(message,"text");dc_sha256(original,strlen(original),hash);if(!eq(str(r,"sourceHash"),hash)){json_object_put(payload);json_object_put(message);return fail("memory_source_changed");}json_object_object_add(payload,"sourceRole",json_object_get(get(message,"role")));if(!eq(str(message,"role"),"user")){json_object_object_add(payload,"speaker",json_object_new_string("agent"));json_object_object_add(payload,"speakerType",json_object_get(get(message,"role")));}if(strlen(original)<=8192)json_object_object_add(payload,"sourceQuote",json_object_new_string(original));else json_object_object_add(payload,"sourceQuoteOmitted",json_object_new_boolean(1));}
 else {chat="";seq=-1;independent=1;}
 if(!sql(s,"BEGIN IMMEDIATE")){json_object_put(payload);json_object_put(message);return fail("storage_error");}
 if(*chat&&!source_enabled(s,chat)){sql(s,"ROLLBACK");json_object_put(payload);json_object_put(message);return fail("memory_source_paused");}
 int ok=insert_node(s,principal,id,"memory",scope,scope_id,kind,payload,chat,seq,hash,independent);if(ok&&*chat){char evidence_hash[65],target[96];J *evidence=json_object_new_object();json_object_object_add(evidence,"chat",json_object_new_string(chat));json_object_object_add(evidence,"sequence",json_object_new_int(seq));json_object_object_add(evidence,"sourceHash",json_object_new_string(hash));json_object_object_add(evidence,"scope",json_object_new_string(scope));json_object_object_add(evidence,"scopeID",json_object_new_string(scope_id));const char *raw=json_object_to_json_string_ext(evidence,JSON_C_TO_STRING_PLAIN);dc_sha256(raw,strlen(raw),evidence_hash);snprintf(target,sizeof target,"evidence:%s",evidence_hash);json_object_object_add(evidence,"role",json_object_get(get(message,"role")));if(get(payload,"sourceQuote"))json_object_object_add(evidence,"quote",json_object_get(get(payload,"sourceQuote")));J *existing=find_node(s,principal,target);if(!existing)ok=insert_node(s,principal,target,"evidence",scope,scope_id,"聊天原文",evidence,chat,seq,hash,independent);json_object_put(existing);json_object_put(evidence);if(ok)ok=edge(s,principal,id,"derivedFrom",target);}if(ok)ok=sql(s,"COMMIT");else sql(s,"ROLLBACK");json_object_put(payload);json_object_put(message);return ok?reply(1,NULL,find_node(s,principal,id)):fail("memory_conflict_or_storage_error");
}
static J *memory_update(dc_store *s,J *r,const char *principal){
 const char *id=str(r,"id"),*status=str(r,"status");if(!ident(id)||(!eq(status,"confirmed")&&!eq(status,"rejected")&&!eq(status,"proposed")))return fail("invalid_memory_status");J *n=find_node(s,principal,id);if(!n||!eq(str(n,"kind"),"memory")){json_object_put(n);return fail("memory_not_found");}if(json_object_get_int64(get(n,"revision"))!=json_object_get_int64(get(r,"revision"))){json_object_put(n);return fail("revision_conflict");}J *body=get(n,"body");const char *corrected=str(r,"text");if(corrected&&!text(corrected,4096,0)){json_object_put(n);return fail("invalid_memory_text");}if(corrected)json_object_object_add(body,"text",json_object_new_string(corrected));json_object_object_add(body,"status",json_object_new_string(status));
 if(!sql(s,"BEGIN IMMEDIATE")){json_object_put(n);return fail("storage_error");}sqlite3_stmt *q=prep(s,"UPDATE graph_nodes SET body=?,revision=revision+1,updated=unixepoch() WHERE id=? AND owner=? AND revision=? AND state='active'");if(q){bind(q,1,json_object_to_json_string_ext(body,JSON_C_TO_STRING_PLAIN));bind(q,2,id);bind(q,3,principal);sqlite3_bind_int64(q,4,json_object_get_int64(get(n,"revision")));}int ok=step(q)&&sqlite3_changes(s->db)==1;if(ok)ok=index_node(s,id,eq(status,"confirmed")?str(body,"text"):NULL);if(ok)ok=sql(s,"COMMIT");else sql(s,"ROLLBACK");json_object_put(n);return ok?reply(1,NULL,find_node(s,principal,id)):fail("revision_conflict_or_storage_error");
}
static size_t boundary(const char *text,size_t at){while(at&&((unsigned char)text[at]&192)==128)at--;return at;}
static J *document_create(dc_store *s,J *r,const char *principal){
 const char *id=str(r,"id"),*name=str(r,"name"),*content=str(r,"text"),*scope=str(r,"scope"),*scope_id=str(r,"scopeID");if(!ident(id)||strlen(id)>96||!text(name,256,0)||!text(content,262144,0)||!scope_valid(s,scope,scope_id)||eq(scope,"chat"))return fail("invalid_knowledge_document");size_t len=strlen(content);char hash[65];dc_sha256(content,len,hash);J *body=json_object_new_object();json_object_object_add(body,"text",json_object_new_string(content));json_object_object_add(body,"sha256",json_object_new_string(hash));if(!sql(s,"BEGIN IMMEDIATE")){json_object_put(body);return fail("storage_error");}int ok=insert_node(s,principal,id,"document",scope,scope_id,name,body,"",-1,hash,1);json_object_put(body);
 size_t start=0;int chunks=0;while(ok&&start<len){size_t end=len-start>2048?boundary(content,start+2048):len;char chunk_id[256];snprintf(chunk_id,sizeof chunk_id,"%s:chunk:%d",id,chunks++);J *b=json_object_new_object();json_object_object_add(b,"text",json_object_new_string_len(content+start,(int)(end-start)));json_object_object_add(b,"document",json_object_new_string(id));json_object_object_add(b,"start",json_object_new_int64((int64_t)start));json_object_object_add(b,"end",json_object_new_int64((int64_t)end));json_object_object_add(b,"sourceHash",json_object_new_string(hash));ok=insert_node(s,principal,chunk_id,"chunk",scope,scope_id,name,b,"",-1,hash,1)&&index_node(s,chunk_id,str(b,"text"))&&edge(s,principal,chunk_id,"partOf",id);json_object_put(b);if(end==len)break;start=boundary(content,end-256);}
 if(ok)ok=sql(s,"COMMIT");else sql(s,"ROLLBACK");return ok?reply(1,NULL,find_node(s,principal,id)):fail("document_conflict_or_storage_error");
}
/* Relocating a document also changes each chunk's revision; a running or
 * recovered request must revalidate every previously selected source. */
static J *document_maintain(dc_store *s,J *r,const char *principal,int move){
 const char *id=str(r,"id"),*scope=str(r,"scope"),*scope_id=str(r,"scopeID");
 if(!ident(id))return fail("knowledge_not_found");
 if(!sql(s,"BEGIN IMMEDIATE"))return fail("storage_error");
 const char *error=NULL;
 J *doc=find_node(s,principal,id);
 if(!doc||!eq(str(doc,"kind"),"document"))error="knowledge_not_found";
 else if(json_object_get_int64(get(r,"revision"))!=json_object_get_int64(get(doc,"revision")))error="revision_conflict";
 else if(move&&(!scope_valid(s,scope,scope_id)||eq(scope,"chat")))error="invalid_knowledge_scope";
 if(!error&&move){
  sqlite3_stmt *q=prep(s,"UPDATE graph_nodes SET scope=?,scope_id=?,revision=revision+1,updated=unixepoch() WHERE owner=? AND state='active' AND (id=? OR (kind='chunk' AND id IN(SELECT source FROM graph_edges WHERE target=? AND owner=? AND type='partOf')))");
  if(q){bind(q,1,scope);bind(q,2,scope_id);bind(q,3,principal);bind(q,4,id);bind(q,5,id);bind(q,6,principal);}
  if(!step(q)||sqlite3_changes(s->db)<1)error="storage_error";
 }else if(!error){
  sqlite3_stmt *q=prep(s,"SELECT n.id,json_extract(n.body,'$.text') FROM graph_nodes n JOIN graph_edges e ON e.source=n.id AND e.owner=n.owner WHERE n.owner=? AND n.state='active' AND n.kind='chunk' AND e.type='partOf' AND e.target=?");
  if(q){bind(q,1,principal);bind(q,2,id);}
  int rc=SQLITE_ERROR,count=0;
  while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){
   if(!index_node(s,(const char*)sqlite3_column_text(q,0),(const char*)sqlite3_column_text(q,1)))break;
   count++;
  }
  sqlite3_finalize(q);
  if(!count||rc!=SQLITE_DONE)error="storage_error";
 }
 json_object_put(doc);
 if(!error&&!sql(s,"COMMIT"))error="storage_error";
 if(error){sql(s,"ROLLBACK");return fail(error);}
 return reply(1,NULL,find_node(s,principal,id));
}
static J *remove_node(dc_store *s,J *r,const char *principal){const char *id=str(r,"id");J *n=ident(id)?find_node(s,principal,id):NULL;if(!n)return fail("knowledge_not_found");if(json_object_get_int64(get(n,"revision"))!=json_object_get_int64(get(r,"revision"))){json_object_put(n);return fail("revision_conflict");}if(!sql(s,"BEGIN IMMEDIATE")){json_object_put(n);return fail("storage_error");}sqlite3_stmt *q=prep(s,"DELETE FROM knowledge_fts WHERE node=? OR node IN(SELECT source FROM graph_edges WHERE target=? AND type='partOf' AND owner=?)");if(q){bind(q,1,id);bind(q,2,id);bind(q,3,principal);}int ok=step(q);q=prep(s,"UPDATE graph_nodes SET state='deleted',revision=revision+1,updated=unixepoch() WHERE owner=? AND (id=? OR id IN(SELECT source FROM graph_edges WHERE target=? AND type='partOf' AND owner=?))");if(q){bind(q,1,principal);bind(q,2,id);bind(q,3,id);bind(q,4,principal);}ok=step(q)&&ok;if(ok)ok=sql(s,"COMMIT");else sql(s,"ROLLBACK");json_object_put(n);return ok?reply(1,NULL,json_object_new_object()):fail("storage_error");}
int dc_knowledge_drop_chat(dc_store *s,const char *chat){
 /* A quote can be shared by independently saved memories and source-bound
    memories. Preserve its provenance if any surviving memory still uses it. */
 const char *steps[]={
  "UPDATE graph_nodes SET independent=1 WHERE source_chat=?1 AND kind='evidence' AND independent=0 "
  "AND EXISTS(SELECT 1 FROM graph_edges e JOIN graph_nodes m ON m.id=e.source "
  "WHERE e.target=graph_nodes.id AND e.type='derivedFrom' AND m.independent=1 AND m.state!='deleted')",
  "DELETE FROM knowledge_fts WHERE node IN(SELECT id FROM graph_nodes WHERE source_chat=?1 AND independent=0)",
  "DELETE FROM graph_node_history WHERE node IN(SELECT id FROM graph_nodes WHERE source_chat=?1 AND independent=0)",
  "DELETE FROM graph_edges WHERE source IN(SELECT id FROM graph_nodes WHERE source_chat=?1 AND independent=0) "
  "OR target IN(SELECT id FROM graph_nodes WHERE source_chat=?1 AND independent=0)",
  "DELETE FROM graph_nodes WHERE source_chat=?1 AND independent=0",
  "DELETE FROM paused_memory_sources WHERE chat=?1"
 };
 for(size_t i=0;i<sizeof steps/sizeof steps[0];i++){
  sqlite3_stmt *q=prep(s,steps[i]);if(q)bind(q,1,chat);if(!step(q))return 0;
 }
 return 1;
}
static J *search(dc_store *s,J *r,const char *principal){
 const char *query=str(r,"query"),*scope=str(r,"scope"),*scope_id=str(r,"scopeID");if(!text(query,4096,1)||!scope_valid(s,scope,scope_id))return fail("invalid_knowledge_query");char *terms=dl_index_terms(query,1);if(!terms)return fail("invalid_query_text");if(!*terms){free(terms);return reply(1,NULL,json_object_new_array());}char sqltext[2048];snprintf(sqltext,sizeof sqltext,"SELECT %s FROM graph_nodes WHERE id IN(SELECT node FROM knowledge_fts WHERE knowledge_fts MATCH ?) AND owner=? AND state='active' AND (scope='global' OR (scope=? AND scope_id=?)) AND (kind='chunk' OR (kind='memory' AND NOT EXISTS(SELECT 1 FROM paused_memory_sources paused WHERE paused.chat=graph_nodes.source_chat) AND json_extract(body,'$.status')='confirmed' AND (COALESCE(json_extract(body,'$.expiresAt'),0)=0 OR json_extract(body,'$.expiresAt')>unixepoch()))) ORDER BY seq DESC LIMIT 20",columns);sqlite3_stmt *q=prep(s,sqltext);if(q){bind(q,1,terms);bind(q,2,principal);bind(q,3,scope);bind(q,4,scope_id);}free(terms);if(!q)return fail("storage_error");J *a=json_object_new_array();int rc;while((rc=sqlite3_step(q))==SQLITE_ROW)json_object_array_add(a,node(q));sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return fail("storage_error");}return reply(1,NULL,a);
}
J *dc_knowledge_request(dc_store *s,J *r){
 char *principal=owner(s);if(!principal)return fail("identity_not_bound");const char *op=str(r,"op");J *out=NULL;
 if(eq(op,"memory.list"))out=list_nodes(s,r,principal,"memory");else if(eq(op,"knowledge.list"))out=list_nodes(s,r,principal,"document");
 else if(eq(op,"memory.source.policy"))out=source_policy(s,r);else if(eq(op,"memory.create"))out=memory_create(s,r,principal);else if(eq(op,"memory.update"))out=memory_update(s,r,principal);else if(eq(op,"knowledge.add"))out=document_create(s,r,principal);else if(eq(op,"knowledge.move")||eq(op,"knowledge.reindex"))out=document_maintain(s,r,principal,eq(op,"knowledge.move"));else if(eq(op,"knowledge.remove")||eq(op,"memory.remove"))out=remove_node(s,r,principal);else if(eq(op,"knowledge.search"))out=search(s,r,principal);
 else if(eq(op,"graph.get")){const char *id=str(r,"id");J *n=ident(id)?find_node(s,principal,id):NULL;out=n?reply(1,NULL,n):fail("knowledge_not_found");}
 else if(eq(op,"memory.subjects")){sqlite3_stmt *q=prep(s,"SELECT id,name,kind FROM memory_subjects ORDER BY name,id");J *a=json_object_new_array();int rc=SQLITE_ERROR;while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){J *o=json_object_new_object();json_object_object_add(o,"id",json_object_new_string((const char*)sqlite3_column_text(q,0)));json_object_object_add(o,"name",json_object_new_string((const char*)sqlite3_column_text(q,1)));json_object_object_add(o,"kind",json_object_new_string((const char*)sqlite3_column_text(q,2)));json_object_array_add(a,o);}sqlite3_finalize(q);if(rc==SQLITE_DONE)out=reply(1,NULL,a);else{json_object_put(a);out=fail("storage_error");}}
 else if(eq(op,"memory.subject.add")){const char *id=str(r,"id"),*name=str(r,"name"),*kind=str(r,"kind");if(!kind)kind="person";if(!ident(id)||!text(name,128,0)||(!eq(kind,"person")&&!eq(kind,"shared")))out=fail("invalid_subject");else{sqlite3_stmt *q=prep(s,"INSERT INTO memory_subjects(id,name,kind) VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,kind=excluded.kind");if(q){bind(q,1,id);bind(q,2,name);bind(q,3,kind);}out=step(q)?reply(1,NULL,json_object_new_object()):fail("storage_error");}}
 else if(eq(op,"graph.neighbors")){const char *id=str(r,"id"),*type=str(r,"type");J *root=ident(id)?find_node(s,principal,id):NULL;if(!root||!ident(type))out=fail("invalid_graph_root");else{sqlite3_stmt *q=prep(s,"SELECT target FROM graph_edges WHERE owner=? AND source=? AND type=? AND target IN(SELECT id FROM graph_nodes WHERE owner=? AND state='active') ORDER BY target LIMIT 100");if(q){bind(q,1,principal);bind(q,2,id);bind(q,3,type);bind(q,4,principal);}J *a=json_object_new_array();int rc=SQLITE_ERROR;while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){J *target=find_node(s,principal,(const char*)sqlite3_column_text(q,0));if(target)json_object_array_add(a,target);}sqlite3_finalize(q);if(rc==SQLITE_DONE)out=reply(1,NULL,a);else{json_object_put(a);out=fail("storage_error");}}json_object_put(root);}
 else out=fail("unsupported_knowledge_operation");free(principal);return out;
}
char *dc_knowledge_call(dc_store *s,const char *raw){if(!s||!raw||strlen(raw)>1048576)return NULL;J *r=dunlin_json_parse(raw,strlen(raw)),*out=json_object_is_type(r,json_type_object)?dc_knowledge_request(s,r):fail("invalid_knowledge_json");char *result=out?strdup(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(out);json_object_put(r);return result;}
static int add_reference(dc_store *s,J *references,J *n,size_t *bytes){
 J *body=get(n,"body"),*r=json_object_new_object();const char *keys[]={"id","revision","kind","sourceHash","sourceChat","sourceSequence","scope","scopeID","updated"};for(size_t i=0;i<sizeof keys/sizeof *keys;i++)if(get(n,keys[i]))json_object_object_add(r,keys[i],json_object_get(get(n,keys[i])));json_object_object_add(r,"text",json_object_get(get(body,"text")));if(eq(str(n,"kind"),"memory")){json_object_object_add(r,"subject",json_object_get(get(body,"subject")));json_object_object_add(r,"assertionKind",json_object_get(get(body,"kind")));json_object_object_add(r,"status",json_object_get(get(body,"status")));}
 const char *subject=str(r,"subject");if(subject){sqlite3_stmt *q=prep(s,"SELECT name FROM memory_subjects WHERE id=?");if(q)bind(q,1,subject);if(q&&sqlite3_step(q)==SQLITE_ROW)json_object_object_add(r,"subjectName",json_object_new_string((const char*)sqlite3_column_text(q,0)));sqlite3_finalize(q);}
 size_t nbytes=strlen(json_object_to_json_string_ext(r,JSON_C_TO_STRING_PLAIN));size_t separator=json_object_array_length(references)?1:0;if(nbytes+separator>8192-*bytes){json_object_put(r);return 0;}*bytes+=nbytes+separator;json_object_array_add(references,r);return 1;
}
J *dc_knowledge_snapshot(dc_store *s,const char *chat,const char *query,J *options,const char **error){
 if(error)*error=NULL;if(!json_object_is_type(options,json_type_object)||!json_object_is_type(get(options,"memory"),json_type_boolean)||!json_object_is_type(get(options,"rag"),json_type_boolean)){if(error)*error="invalid_knowledge_policy";return NULL;}
 char *principal=owner(s);if(!principal){if(error)*error="identity_not_bound";return NULL;}sqlite3_stmt *q=prep(s,"SELECT project FROM chat_properties WHERE chat=?");if(q)bind(q,1,chat);char *project=q&&sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);if(!project){free(principal);if(error)*error="knowledge_chat_missing";return NULL;}
 J *references=json_object_new_array();size_t bytes=2;int partial=0,memory_count=0,chunk_count=0,ok=1;
 const char *scope_clause="owner=? AND state='active' AND (scope='global' OR (scope='project' AND scope_id=?) OR (scope='chat' AND scope_id=?))";char sqltext[2048];
 if(json_object_get_boolean(get(options,"memory"))){snprintf(sqltext,sizeof sqltext,"SELECT %s FROM graph_nodes WHERE %s AND kind='memory' AND NOT EXISTS(SELECT 1 FROM paused_memory_sources paused WHERE paused.chat=graph_nodes.source_chat) AND json_extract(body,'$.status')='confirmed' AND (COALESCE(json_extract(body,'$.expiresAt'),0)=0 OR json_extract(body,'$.expiresAt')>unixepoch()) ORDER BY updated DESC,seq DESC LIMIT 17",columns,scope_clause);q=prep(s,sqltext);if(q){bind(q,1,principal);bind(q,2,project);bind(q,3,chat);}int rc=SQLITE_ERROR;while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){J *n=node(q);if(memory_count<8&&add_reference(s,references,n,&bytes))memory_count++;else partial=1;json_object_put(n);}sqlite3_finalize(q);if(rc!=SQLITE_DONE)ok=0;}
 if(ok&&query&&json_object_get_boolean(get(options,"rag"))){char *terms=dl_index_terms(query,1);if(!terms)ok=0;else if(*terms){snprintf(sqltext,sizeof sqltext,"SELECT %s FROM graph_nodes WHERE %s AND kind='chunk' AND id IN(SELECT node FROM knowledge_fts WHERE knowledge_fts MATCH ?) ORDER BY seq DESC LIMIT 9",columns,scope_clause);q=prep(s,sqltext);if(q){bind(q,1,principal);bind(q,2,project);bind(q,3,chat);bind(q,4,terms);}int rc=SQLITE_ERROR;while(q&&(rc=sqlite3_step(q))==SQLITE_ROW){J *n=node(q);if(chunk_count<4&&add_reference(s,references,n,&bytes))chunk_count++;else partial=1;json_object_put(n);}sqlite3_finalize(q);if(rc!=SQLITE_DONE)ok=0;}free(terms);}
 free(project);if(!ok){free(principal);json_object_put(references);if(error)*error="knowledge_snapshot_failed";return NULL;}
 J *snapshot=json_object_new_object();json_object_object_add(snapshot,"owner",json_object_new_string(principal));free(principal);json_object_object_add(snapshot,"references",references);json_object_object_add(snapshot,"partial",json_object_new_boolean(partial));json_object_object_add(snapshot,"memoryCount",json_object_new_int(memory_count));json_object_object_add(snapshot,"chunkCount",json_object_new_int(chunk_count));const char *raw=json_object_to_json_string_ext(references,JSON_C_TO_STRING_PLAIN);char hash[65];dc_sha256(raw,strlen(raw),hash);json_object_object_add(snapshot,"hash",json_object_new_string(hash));return snapshot;
}
int dc_knowledge_snapshot_valid(dc_store *s,J *snapshot){
 char *principal=owner(s);if(!principal)return 0;int valid=eq(principal,str(snapshot,"owner"));J *references=get(snapshot,"references");if(!json_object_is_type(references,json_type_array)||json_object_array_length(references)>12)valid=0;for(size_t i=0;valid&&i<json_object_array_length(references);i++){J *r=json_object_array_get_idx(references,i),*n=find_node(s,principal,str(r,"id"));valid=n&&eq(str(n,"state"),"active")&&json_object_get_int64(get(n,"revision"))==json_object_get_int64(get(r,"revision"))&&eq(str(n,"sourceHash"),str(r,"sourceHash"))&&eq(str(get(n,"body"),"text"),str(r,"text"))&&eq(str(n,"kind"),str(r,"kind"));if(valid&&eq(str(n,"kind"),"memory")){J *body=get(n,"body");int64_t expires=json_object_get_int64(get(body,"expiresAt"));valid=source_enabled(s,str(n,"sourceChat"))&&eq(str(body,"status"),"confirmed")&&(!expires||expires>(int64_t)time(NULL));}json_object_put(n);}free(principal);return valid;
}
