/* A checkpoint config replacement changes only its owning plugin settings.
 * Desired state is durable before SDK hooks run; acknowledgement is mandatory. */
static int native_settings_settled(dc_store *s,const char *call){
 sqlite3_stmt *q=prep(s,"SELECT 1 FROM extension_native_settings WHERE call=? AND status<>'applied' LIMIT 1");if(q)bind(q,1,call);int rc=q?sqlite3_step(q):SQLITE_ERROR;sqlite3_finalize(q);return rc==SQLITE_DONE;
}
static J *native_settings_hash(dc_store *s,const char *source,const char *package,const char *version){
 sqlite3_stmt *q=prep(s,"SELECT coalesce((SELECT config FROM extension_configuration WHERE source=? AND package=? AND version=?),'{}')");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}char hash[65]="";if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);dc_sha256(raw,strlen(raw),hash);}sqlite3_finalize(q);if(!*hash)return fail("native_settings_unavailable");J *data=json_object_new_object();json_object_object_add(data,"hash",json_object_new_string(hash));return reply(1,NULL,data);
}
static J *native_settings_update(dc_store *s,const char *method,J *args,const char *source,const char *package,const char *version,const char *call,int64_t sequence){
 if(!eq(package,"dsh-checkpoint-rewind")||!eq(str(args,"namespace"),"checkpoint-rewind"))return fail("native_settings_owner_invalid");
 sqlite3_stmt *q=prep(s,"SELECT 1 FROM extension_calls c JOIN extension_questions q ON q.call=c.id WHERE c.id=? AND c.method='slash:rewind' AND c.status='running' AND q.decision='allowed_once' AND EXISTS(SELECT 1 FROM extension_native_scopes n WHERE n.call=c.id AND n.operation='interactive-config-restore') LIMIT 1");if(q)bind(q,1,call);int allowed=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);if(!allowed)return fail("native_settings_confirmation_required");
 if(eq(method,"settings.applied")){
  const char *hash=str(args,"hash");if(!hash||strlen(hash)!=64)return fail("native_settings_acknowledgement_invalid");
  q=prep(s,"UPDATE extension_native_settings SET status='applied' WHERE call=? AND config_hash=? AND status='pending' AND EXISTS(SELECT 1 FROM extension_native_effects e WHERE e.call=extension_native_settings.call AND e.sequence=extension_native_settings.sequence AND e.method='settings.commit' AND e.status='reported')");if(q){bind(q,1,call);bind(q,2,hash);}return step(q)&&sqlite3_changes(s->db)==1?reply(1,NULL,json_object_new_boolean(1)):fail("native_settings_acknowledgement_invalid");
 }
 J *config=get(args,"config");const char *expected=str(args,"expectedHash");if(!json_object_is_type(config,json_type_object)||!expected||strlen(expected)!=64)return fail("native_settings_configuration_invalid");const char *raw=json_object_to_json_string_ext(config,JSON_C_TO_STRING_PLAIN);if(strlen(raw)>32768)return fail("native_settings_configuration_invalid");
 if(!sqlite3_get_autocommit(s->db)||!sql(s,"BEGIN IMMEDIATE"))return fail("native_settings_storage_unavailable");J *current=native_settings_hash(s,source,package,version);int valid=json_object_get_boolean(get(current,"ok"))&&eq(str(get(current,"data"),"hash"),expected);json_object_put(current);if(!valid){sql(s,"ROLLBACK");return fail("native_settings_configuration_changed");}
 char hash[65];dc_sha256(raw,strlen(raw),hash);
 q=prep(s,"INSERT INTO extension_native_settings(call,sequence,config_hash,status) VALUES(?,?,?,'pending')");if(q){bind(q,1,call);sqlite3_bind_int64(q,2,sequence);bind(q,3,hash);}valid=step(q);
 if(valid){q=prep(s,"INSERT INTO extension_configuration(source,package,version,config) VALUES(?,?,?,?) ON CONFLICT(source,package,version) DO UPDATE SET config=excluded.config");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);bind(q,4,raw);}valid=step(q);}
 if(valid)valid=sql(s,"COMMIT");if(!valid){sql(s,"ROLLBACK");return fail("native_settings_commit_failed");}J *data=json_object_new_object();json_object_object_add(data,"hash",json_object_new_string(hash));return reply(1,NULL,data);
}
