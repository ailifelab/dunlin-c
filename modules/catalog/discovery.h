/* Data-only community discovery. Never forwards upstream installation commands
 * to the extension host. This is deliberately separate from signed catalogs. */
#define DISCOVERY_LIMIT 4194304u
static const char *discovery_defaults="{\"format\":1,\"sources\":[{\"id\":\"dunlin-common\",\"title\":\"Dunlin 常用 DSH 插件清单\",\"url\":\"https://raw.githubusercontent.com/ailifelab/dunlin-c/main/catalogs/dsh-plugins.json\"},{\"id\":\"dshseek\",\"title\":\"DSHSeek 社区索引（第三方）\",\"url\":\"https://dshseek.com/market/dsh-index.json\"}]}";
static int discovery_path(dc_store *s,char p[PATH_MAX]){int n=snprintf(p,PATH_MAX,"%s/plugin-index-sources.json",s->directory);return n>0&&n<PATH_MAX;}
static int discovery_valid(J *o){
 J *a=get(o,"sources");if(!json_object_is_type(get(o,"format"),json_type_int)||json_object_get_int64(get(o,"format"))!=1||!json_object_is_type(a,json_type_array)||json_object_array_length(a)>64)return 0;
 for(size_t i=0;i<json_object_array_length(a);i++){J *r=json_object_array_get_idx(a,i);if(!ident(str(r,"id"))||!text(str(r,"title"),256)||!https(str(r,"url")))return 0;for(size_t j=0;j<i;j++){J *q=json_object_array_get_idx(a,j);if(eq(str(r,"id"),str(q,"id"))||eq(str(r,"url"),str(q,"url")))return 0;}}
 return 1;
}
static int discovery_write(dc_store *s,J *o){
 char path[PATH_MAX],tmp[PATH_MAX];if(!discovery_path(s,path)||snprintf(tmp,sizeof tmp,"%s/.plugin-index-XXXXXX",s->directory)>=(int)sizeof tmp)return 0;
 const char *raw=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PRETTY);size_t len=strlen(raw);if(!discovery_valid(o)||len>SOURCE_CONFIG_LIMIT)return 0;
 int fd=mkstemp(tmp);if(fd<0)return 0;int ok=fchmod(fd,0600)==0;size_t used=0;
 while(ok&&used<len){ssize_t n=write(fd,raw+used,len-used);if(n<0&&errno==EINTR)continue;if(n<=0){ok=0;break;}used+=(size_t)n;}
 if(ok)ok=fsync(fd)==0;if(close(fd))ok=0;if(!ok||rename(tmp,path)){unlink(tmp);return 0;}return 1;
}
static J *discovery_read(dc_store *s){
 char path[PATH_MAX];if(!discovery_path(s,path))return NULL;int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
 if(fd<0){if(errno!=ENOENT)return NULL;J *o=dunlin_json_parse(discovery_defaults,strlen(discovery_defaults));if(!discovery_write(s,o)){json_object_put(o);return NULL;}return o;}
 struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_size<=0||st.st_size>SOURCE_CONFIG_LIMIT){close(fd);return NULL;}
 size_t len=(size_t)st.st_size,used=0;char *raw=malloc(len+1);if(!raw){close(fd);return NULL;}
 while(used<len){ssize_t n=read(fd,raw+used,len-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 char extra;ssize_t tail=read(fd,&extra,1);close(fd);J *o=used==len&&tail==0?dunlin_json_parse(raw,len):NULL;free(raw);if(!discovery_valid(o)){json_object_put(o);return NULL;}return o;
}
static const char *discovery_localized(J *o,const char *key){const char *p=str(o,key);J *v=get(o,key);if(!p)p=str(v,"zh");if(!p)p=str(v,"en");return p;}
static J *discovery_entries(const char *raw,size_t len){
 J *o=dunlin_json_parse(raw,len),*a=get(o,"plugins"),*out=NULL;
 if(!json_object_is_type(get(o,"format"),json_type_int)||json_object_get_int64(get(o,"format"))!=1||!json_object_is_type(a,json_type_array)||json_object_array_length(a)>2000)goto done;
 out=json_object_new_array();for(size_t i=0;i<json_object_array_length(a);i++){
  J *r=json_object_array_get_idx(a,i);const char *name=str(r,"name"),*url=str(get(r,"repository"),"url"),*title=discovery_localized(r,"title"),*desc=discovery_localized(r,"description");
  if(!text(name,256)||!https(url)||(title&&!text(title,512))||(desc&&strlen(desc)>4096)){json_object_put(out);out=NULL;break;}
  J *e=json_object_new_object();json_object_object_add(e,"name",json_object_new_string(name));json_object_object_add(e,"title",json_object_new_string(title?title:name));json_object_object_add(e,"description",json_object_new_string(desc?desc:""));json_object_object_add(e,"url",json_object_new_string(url));const char *reason=str(get(get(r,"metadata"),"dunlin"),"reason");if(reason&&strlen(reason)<=512)json_object_object_add(e,"compatibilityReason",json_object_new_string(reason));json_object_array_add(out,e);
 }
 done:json_object_put(o);return out;
}
static J *discovery_list(dc_store *s,J *config){
 J *rows=get(config,"sources"),*out=json_object_new_array();for(size_t i=0;i<json_object_array_length(rows);i++){
  J *r=json_object_array_get_idx(rows,i),*e=json_object_new_object();const char *keys[]={"id","title","url"};for(int k=0;k<3;k++)json_object_object_add(e,keys[k],json_object_new_string(str(r,keys[k])));
  sqlite3_stmt *q=prep(s,"SELECT payload,updated FROM community_indexes WHERE url=?");if(q)db_bind(q,1,str(r,"url"));J *a=NULL;int64_t at=0;
  if(q&&sqlite3_step(q)==SQLITE_ROW){const char *raw=(const char*)sqlite3_column_text(q,0);a=raw?dunlin_json_parse(raw,strlen(raw)):NULL;at=sqlite3_column_int64(q,1);}sqlite3_finalize(q);
  json_object_object_add(e,"entries",a?a:json_object_new_array());json_object_object_add(e,"updatedAt",json_object_new_int64(at));json_object_array_add(out,e);
 }return reply(1,NULL,out);
}
static J *discovery_request(dc_store *s,J *r,const dc_hooks *hooks){
 const char *op=str(r,"op"),*id=str(r,"id");J *config=discovery_read(s);if(!config)return fail("community_config_invalid");J *out=NULL,*rows=get(config,"sources");
 if(eq(op,"catalog.community.list"))out=discovery_list(s,config);
 else if(eq(op,"catalog.community.add")){
  J *row=json_object_new_object();const char *keys[]={"id","title","url"};for(int k=0;k<3;k++)json_object_object_add(row,keys[k],json_object_new_string(str(r,keys[k])?str(r,keys[k]):""));json_object_array_add(rows,row);
  out=discovery_valid(config)&&discovery_write(s,config)?discovery_list(s,config):fail("community_source_invalid_or_duplicate");
 }else{
  size_t i;for(i=0;i<json_object_array_length(rows);i++)if(eq(str(json_object_array_get_idx(rows,i),"id"),id))break;
  if(i==json_object_array_length(rows))out=fail("community_source_missing");
  else if(eq(op,"catalog.community.remove")){json_object_array_del_idx(rows,i,1);out=discovery_write(s,config)?discovery_list(s,config):fail("community_config_write_failed");}
  else if(eq(op,"catalog.community.sync")||eq(op,"catalog.community.import")){
   const char *url=str(json_object_array_get_idx(rows,i),"url");transfer t={.limit=DISCOVERY_LIMIT,.hooks=hooks};long status=0;J *entries=NULL;if(eq(op,"catalog.community.import")){const char *raw=str(r,"index");if(raw&&strlen(raw)<=DISCOVERY_LIMIT)entries=discovery_entries(raw,strlen(raw));}else{int ok=fetch(url,NULL,&t,&status);if(ok&&status==200)entries=discovery_entries(t.bytes,t.used);}free(t.bytes);
   if(!entries)out=fail(progress(&t,0,0,0,0)?"cancelled":"community_download_or_format_failed");
   else{sqlite3_stmt *q=prep(s,"INSERT INTO community_indexes(url,payload,updated) VALUES(?,?,unixepoch()) ON CONFLICT(url) DO UPDATE SET payload=excluded.payload,updated=excluded.updated");if(q){db_bind(q,1,url);db_bind(q,2,json_object_to_json_string_ext(entries,JSON_C_TO_STRING_PLAIN));}out=step(q)?discovery_list(s,config):fail("storage_error");json_object_put(entries);}
  }else out=fail("unsupported_catalog_operation");
 }json_object_put(config);return out;
}
