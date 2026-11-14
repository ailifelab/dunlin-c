/* Explicit native application shutdown. Desired bindings remain enabled.
 * A crash still has no disposal receipt and retains unknown/recovery rules. */
static J *quiesce(dc_store *s,J *request,const dc_hooks *hooks){
 extension_state *e=state(s);if(!e||e->users!=1)return fail("extension_workers_still_open");
 const char *raw=json_object_to_json_string_ext(e->active,JSON_C_TO_STRING_PLAIN);J *active=dunlin_json_parse(raw,strlen(raw));if(!active)return fail("out_of_memory");size_t count=0;
 json_object_object_foreach(active,identity,item){
  const char *source=str(item,"source"),*package=str(item,"package"),*version=str(item,"version"),*host=str(item,"host");dc_worker *worker=eq(host,"node-dsh")?e->dsh:e->python_worker;char call[65]="";
  if(!source||!package||!version||!dc_worker_alive(worker)||!lifecycle_begin(s,source,package,version,request,call)){json_object_put(active);return fail("extension_shutdown_admission_failed");}
  int ok=native_jobs_revoke(s,source,package);J *args=json_object_new_object();json_object_object_add(args,"id",json_object_new_string(identity));J *stopped=ok?worker_request(s,worker,"plugins.deactivate",args,hooks):NULL;json_object_put(args);ok=ok&&json_object_get_boolean(get(stopped,"ok"));json_object_put(stopped);
  // Only an actual successful disposer may settle the retained HTTP leases.
  if(ok)ok=pending_close(s,source,package,NULL,"cancelled_effects_possible");
  if(ok){dl_terminal_close_owner(e->terminals,identity);sqlite3_stmt *q=prep(s,"UPDATE extension_terminal_owners SET state='stopped' WHERE source=? AND package=? AND state='running'");if(q){bind(q,1,source);bind(q,2,package);}ok=step(q);}
  if(ok){sqlite3_stmt *q=prep(s,"UPDATE extension_bindings SET runtime_instance='' WHERE source=? AND package=?");if(q){bind(q,1,source);bind(q,2,package);}ok=step(q);}
  if(ok){sqlite3_stmt *q=prep(s,"UPDATE extension_calls SET status='reported',finished=unixepoch() WHERE id=? AND status='running'");if(q)bind(q,1,call);ok=step(q);}
  if(!ok){json_object_put(active);return fail("extension_shutdown_not_confirmed");}
  json_object_object_del(e->active,identity);count++;
 }
 json_object_put(active);dc_worker_close(e->dsh);e->dsh=NULL;dc_worker_close(e->python_worker);e->python_worker=NULL;
 J *result=json_object_new_object();json_object_object_add(result,"closed",json_object_new_int64((int64_t)count));return reply(1,NULL,result);
}
