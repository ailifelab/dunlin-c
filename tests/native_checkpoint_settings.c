#define _XOPEN_SOURCE 700
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include "../desktop/internal.h"
#include "../desktop/chat_workspace.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
typedef struct json_object J;
static J *get(J *o,const char *key){J *v=NULL;if(o)json_object_object_get_ex(o,key,&v);return v;}
static const char *str(J *o,const char *key){J *v=get(o,key);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int id(const char *s){return s&&*s&&strlen(s)<=64;}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v,-1,SQLITE_TRANSIENT);}
static int step(sqlite3_stmt *q){int done=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return done;}
static int sql(dc_store *s,const char *text){return sqlite3_exec(s->db,text,NULL,NULL,NULL)==SQLITE_OK;}
static J *reply(int ok,const char *error,J *data){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(o,"error",json_object_new_string(error));if(data)json_object_object_add(o,"data",data);return o;}
static J *fail(const char *e){return reply(0,e,NULL);}
#include "../desktop/extension_native_settings.inc"
static void expect(J *value,int success){assert(json_object_get_boolean(get(value,"ok"))==success);json_object_put(value);}
static void unchanged(dc_store *s,int wanted){sqlite3_stmt *q=prep(s,"SELECT json_extract(config,'$.listLimit') FROM extension_configuration WHERE source='owned' AND package='dsh-checkpoint-rewind' AND version='owned'");assert(q&&sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_int(q,0)==wanted);sqlite3_finalize(q);}
static void invocation(dc_store *s,const char *call,int grant){
 sqlite3_stmt *q=prep(s,"INSERT INTO extension_calls(id,source,package,version,method,args_hash,status) VALUES(?,'owned','dsh-checkpoint-rewind','owned','slash:rewind','owned','running')");bind(q,1,call);assert(step(q));q=prep(s,"INSERT INTO extension_native_scopes(call,instance,chat,surface,operation) VALUES(?,'owned','owned','','none')");bind(q,1,call);assert(step(q));q=prep(s,"INSERT INTO extension_questions(call,id,question_hash,decision) VALUES(?,'owned','owned','allowed_once')");bind(q,1,call);assert(step(q));if(grant){q=prep(s,"UPDATE extension_native_scopes SET operation='interactive-config-restore' WHERE call=?");bind(q,1,call);assert(step(q));}
}
int main(int argc,char **argv){
 assert(argc==2);dc_store *s=dc_open(argv[1]);assert(s);assert(sql(s,"INSERT INTO extension_configuration(source,package,version,config) VALUES('owned','dsh-checkpoint-rewind','owned','{\"listLimit\":7}')"));invocation(s,"test",0);
 J *hash=native_settings_hash(s,"owned","dsh-checkpoint-rewind","owned"),*args=json_object_new_object(),*config=json_object_new_object();json_object_object_add(config,"listLimit",json_object_new_int(10));json_object_object_add(args,"namespace",json_object_new_string("checkpoint-rewind"));json_object_object_add(args,"config",config);json_object_object_add(args,"expectedHash",json_object_get(get(get(hash,"data"),"hash")));
 expect(native_settings_update(s,"settings.commit",args,"owned","dsh-checkpoint-rewind","owned","test",1),0);unchanged(s,7);assert(sql(s,"UPDATE extension_native_scopes SET operation='interactive-config-restore' WHERE call='test'"));
 expect(native_settings_update(s,"settings.commit",args,"owned","foreign","owned","test",1),0);unchanged(s,7);
 json_object_object_add(args,"namespace",json_object_new_string("models"));expect(native_settings_update(s,"settings.commit",args,"owned","dsh-checkpoint-rewind","owned","test",1),0);json_object_object_add(args,"namespace",json_object_new_string("checkpoint-rewind"));
 json_object_object_add(args,"expectedHash",json_object_new_string("0000000000000000000000000000000000000000000000000000000000000000"));expect(native_settings_update(s,"settings.commit",args,"owned","dsh-checkpoint-rewind","owned","test",1),0);unchanged(s,7);json_object_object_add(args,"expectedHash",json_object_get(get(get(hash,"data"),"hash")));
 assert(sql(s,"CREATE TRIGGER deny_owned_config BEFORE UPDATE ON extension_configuration BEGIN SELECT RAISE(ABORT,'owned storage fault');END"));expect(native_settings_update(s,"settings.commit",args,"owned","dsh-checkpoint-rewind","owned","test",1),0);unchanged(s,7);assert(native_settings_settled(s,"test"));assert(sql(s,"DROP TRIGGER deny_owned_config"));
 J *committed=native_settings_update(s,"settings.commit",args,"owned","dsh-checkpoint-rewind","owned","test",1);assert(json_object_get_boolean(get(committed,"ok")));unchanged(s,10);assert(!native_settings_settled(s,"test"));J *ack=json_object_new_object();json_object_object_add(ack,"namespace",json_object_new_string("checkpoint-rewind"));json_object_object_add(ack,"hash",json_object_get(get(get(committed,"data"),"hash")));
 expect(native_settings_update(s,"settings.applied",ack,"owned","dsh-checkpoint-rewind","owned","test",2),0);assert(!native_settings_settled(s,"test"));assert(sql(s,"INSERT INTO extension_native_effects(call,sequence,method,args_hash,status) VALUES('test',1,'settings.commit','owned','reported')"));expect(native_settings_update(s,"settings.applied",ack,"owned","dsh-checkpoint-rewind","owned","test",2),1);assert(native_settings_settled(s,"test"));expect(native_settings_update(s,"settings.applied",ack,"owned","dsh-checkpoint-rewind","owned","test",2),0);
 json_object_put(hash);json_object_put(args);json_object_put(committed);json_object_put(ack);dc_close(s);puts("PASS C checkpoint settings helper: config-specific consent/owner/namespace/CAS/storage rollback/desired commit/exact acknowledgement/no duplicate ack");return 0;
}
