/* Human terminal UI -> independent C PTY; never a second agent/model loop. */
static char *terminal_base64(const unsigned char *bytes,size_t n){
 static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";char *out=malloc((n+2)/3*4+1);if(!out)return NULL;size_t at=0;
 for(size_t i=0;i<n;i+=3){unsigned v=(unsigned)bytes[i]<<16;if(i+1<n)v|=(unsigned)bytes[i+1]<<8;if(i+2<n)v|=bytes[i+2];out[at++]=alphabet[(v>>18)&63];out[at++]=alphabet[(v>>12)&63];out[at++]=i+1<n?alphabet[(v>>6)&63]:'=';out[at++]=i+2<n?alphabet[v&63]:'=';}out[at]=0;return out;
}
static J *terminal_info(const dl_terminal_info *info){
 J *out=json_object_new_object();json_object_object_add(out,"id",json_object_new_string(info->id));json_object_object_add(out,"title",json_object_new_string(strrchr(info->shell,'/')?strrchr(info->shell,'/')+1:info->shell));json_object_object_add(out,"cwd",json_object_new_string(info->cwd));json_object_object_add(out,"cols",json_object_new_int(info->cols));json_object_object_add(out,"rows",json_object_new_int(info->rows));json_object_object_add(out,"state",json_object_new_string(*info->error?"failed":info->running?"running":"exited"));json_object_object_add(out,"exitCode",info->running?NULL:json_object_new_int(info->exit_code));if(*info->error)json_object_object_add(out,"error",json_object_new_string(info->error));return out;
}
static int terminal_record(dc_store *s,J *r,J *active,const char *chat,const dl_terminal_info *info,const char *status){
 sqlite3_stmt *q=prep(s,"INSERT INTO extension_terminal_owners(id,source,package,chat,instance,state) VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET state=excluded.state WHERE extension_terminal_owners.source=excluded.source AND extension_terminal_owners.package=excluded.package AND extension_terminal_owners.chat=excluded.chat AND extension_terminal_owners.instance=excluded.instance");
 if(q){const char *v[]={info->id,str(r,"source"),str(r,"package"),chat,str(active,"instance"),status};for(int i=0;i<6;i++)bind(q,i+1,v[i]);}return step(q);
}
static int terminal_audit_begin(dc_store *s,J *r,const char *method,char call[65]){
 const char *raw=json_object_to_json_string_ext(get(r,"arguments"),JSON_C_TO_STRING_PLAIN);char digest[65];dc_sha256(raw,strlen(raw),digest);sqlite3_stmt *q=prep(s,"SELECT lower(hex(randomblob(16)))");if(!q||sqlite3_step(q)!=SQLITE_ROW){sqlite3_finalize(q);return 0;}snprintf(call,65,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);
 q=prep(s,"INSERT INTO extension_calls(id,source,package,version,method,args_hash,status) VALUES(?,?,?,?,?,?,'running')");if(q){const char *v[]={call,str(r,"source"),str(r,"package"),str(r,"version"),method,digest};for(int i=0;i<6;i++)bind(q,i+1,v[i]);}return step(q);
}
static J *client_terminal(dc_store *s,J *r,J *active,const char *identity){
 const char *method=str(r,"method");J *args=get(r,"arguments");const char *chat=str(args,"sessionId"),*lease=str(args,"_surface"),*tid=str(args,"id");
 if(!id(chat)||!lease||!*lease||strlen(lease)>128)return fail("invalid_terminal_owner");
 J *workspace=dc_chat_workspace_view(s,chat);const char *path=str(workspace,"path");int cleanup=eq(method,"terminal.close")||eq(method,"terminal.detach")||eq(method,"terminal.recover");if((!path||!json_object_get_boolean(get(workspace,"available")))&&!cleanup){json_object_put(workspace);return fail("terminal_workspace_unavailable");}
 extension_state *e=state(s);if(!e->terminals)e->terminals=dl_terminal_pool_open();if(!e->terminals){json_object_put(workspace);return fail("terminal_storage_unavailable");}
 char owner[161];snprintf(owner,sizeof owner,"%s:%s",identity,str(active,"instance"));J *value=NULL;dl_terminal_info info={0};int ok=0;char call[65]="";
 int read_only=eq(method,"terminal.read")||eq(method,"terminal.recover")||eq(method,"terminal.detach")||eq(method,"terminal.attach");
 if(!read_only&&!terminal_audit_begin(s,r,method,call)){json_object_put(workspace);return fail("invocation_audit_unavailable");}
 if(eq(method,"terminal.open")){
  const char *shell=str(args,"shellPath");if(!shell||!*shell)shell="/bin/sh";const char *content=str(args,"contentId");
  unsigned char probe;size_t got=0;int opened=tid?dl_terminal_read(e->terminals,owner,chat,tid,0,&probe,1,&got,&info)&&!strcmp(path,info.cwd):content&&dl_terminal_open(e->terminals,owner,chat,content,path,shell,80,24,&info);
  if(opened){value=json_object_new_object();json_object_object_add(value,"info",terminal_info(&info));J *environment=json_object_new_object();json_object_object_add(environment,"cwd",json_object_new_string(path));json_object_object_add(environment,"maxInputBytes",json_object_new_int(16384));json_object_object_add(environment,"maxCols",json_object_new_int(4096));json_object_object_add(environment,"maxRows",json_object_new_int(4096));json_object_object_add(environment,"scrollback",json_object_new_int(10000));json_object_object_add(value,"environment",environment);ok=1;}
 }else if(eq(method,"terminal.recover")){
  value=json_object_new_array();for(size_t i=0;dl_terminal_list(e->terminals,owner,chat,i,&info);i++)json_object_array_add(value,terminal_info(&info));ok=1;
 }else if(eq(method,"terminal.close")&&!tid){const char *content=str(args,"contentId");if(content&&*content&&strlen(content)<=256){dl_terminal_find_content(e->terminals,owner,chat,content,&info);ok=dl_terminal_close_content(e->terminals,owner,chat,content);}
 }else{
  unsigned char bytes[32768];size_t length=0;J *offset=get(args,"offset");int valid_offset=!offset||(json_object_is_type(offset,json_type_int)&&json_object_get_int64(offset)>=0&&json_object_get_int64(offset)<=4194304);
  if(id(tid)&&valid_offset&&dl_terminal_read(e->terminals,owner,chat,tid,(size_t)json_object_get_int64(offset),bytes,sizeof bytes,&length,&info)&&(cleanup||(path&&!strcmp(path,info.cwd)))){
   if(eq(method,"terminal.read")){
    char *encoded=terminal_base64(bytes,length);if(encoded){value=json_object_new_object();json_object_object_add(value,"info",terminal_info(&info));json_object_object_add(value,"base64",json_object_new_string(encoded));json_object_object_add(value,"bytes",json_object_new_int64((int64_t)info.bytes));json_object_object_add(value,"writable",json_object_new_boolean(dl_terminal_writable(e->terminals,owner,chat,tid,lease)));free(encoded);ok=1;}
   }else if(eq(method,"terminal.attach"))ok=dl_terminal_attach(e->terminals,owner,chat,tid,lease);
   else if(eq(method,"terminal.detach")){dl_terminal_detach(e->terminals,owner,chat,tid,lease);ok=1;}
   else if(eq(method,"terminal.write")){
    J *data=get(args,"data");size_t written=0,n=json_object_is_type(data,json_type_array)?json_object_array_length(data):0;unsigned char input[16384];int valid=json_object_is_type(data,json_type_array)&&n>0&&n<=sizeof input;
    for(size_t i=0;valid&&i<n;i++){J *v=json_object_array_get_idx(data,i);int64_t b=json_object_get_int64(v);if(!json_object_is_type(v,json_type_int)||b<0||b>255)valid=0;else input[i]=(unsigned char)b;}
    if(valid&&dl_terminal_write(e->terminals,owner,chat,tid,lease,input,n,&written)){value=json_object_new_object();json_object_object_add(value,"written",json_object_new_int64((int64_t)written));ok=1;}
   }else if(eq(method,"terminal.resize")){
    J *cols=get(args,"cols"),*rows=get(args,"rows");if(json_object_is_type(cols,json_type_int)&&json_object_is_type(rows,json_type_int))ok=dl_terminal_resize(e->terminals,owner,chat,tid,lease,json_object_get_int(cols),json_object_get_int(rows));
   }else if(eq(method,"terminal.close"))ok=dl_terminal_close(e->terminals,owner,chat,tid);
  }
 }
 json_object_put(workspace);
 if(ok&&*info.id&&(eq(method,"terminal.open")||eq(method,"terminal.close")||(!info.running&&eq(method,"terminal.read")))){
  if(!terminal_record(s,r,active,chat,&info,eq(method,"terminal.close")?"stopped":info.running?"running":*info.error?"faulted":"exited")){dl_terminal_close_owner(e->terminals,identity);fault(s,str(r,"source"),str(r,"package"),str(r,"version"),"terminal","terminal_owner_audit_failed");json_object_put(value);return fail("terminal_owner_audit_failed");}
 }
 if(*call){sqlite3_stmt *q=prep(s,"UPDATE extension_calls SET status=?,finished=unixepoch() WHERE id=? AND status='running'");if(q){bind(q,1,ok?"reported":"reported_failure");bind(q,2,call);}if(!step(q)){dl_terminal_close_owner(e->terminals,identity);fault(s,str(r,"source"),str(r,"package"),str(r,"version"),"terminal","invocation_finalization_failed");json_object_put(value);return fail("invocation_finalization_failed");}}
 if(!ok){json_object_put(value);return fail("terminal_operation_refused");}J *out=json_object_new_object();json_object_object_add(out,"value",value?value:json_object_new_boolean(1));return reply(1,NULL,out);
}
