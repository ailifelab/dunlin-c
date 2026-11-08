/* Private C catalog source configuration; catalog.c owns the JSON/SQL helpers.
 * The file is the source authority. SQLite retains signed payloads separately.
 * Existing source IDs never change their URL/key, even while disabled. */
#define SOURCE_CONFIG_LIMIT 262144u
static int source_path(dc_store *s,char path[PATH_MAX]){
 int n=snprintf(path,PATH_MAX,"%s/catalog-sources.json",s->directory);return n>0&&n<PATH_MAX;
}
static int source_fields(J *o,const char *const *fields){
 if(!json_object_is_type(o,json_type_object))return 0;
 json_object_object_foreach(o,k,v){(void)v;int found=0;for(int i=0;fields[i];i++)if(eq(k,fields[i]))found=1;if(!found)return 0;}return 1;
}
static int source_config_valid(J *root){
 const char *const top[]={"protocol","major","sources",NULL},*const fields[]={"id","title","url","publicKey","enabled",NULL};
 J *rows=get(root,"sources");
 if(!source_fields(root,top)||!eq(str(root,"protocol"),"dunlin.catalog.sources")||!json_object_is_type(get(root,"major"),json_type_int)||json_object_get_int64(get(root,"major"))!=1||!json_object_is_type(rows,json_type_array)||json_object_array_length(rows)>128)return 0;
 for(size_t i=0;i<json_object_array_length(rows);i++){
  J *r=json_object_array_get_idx(rows,i);
  if(!source_fields(r,fields)||!ident(str(r,"id"))||!text(str(r,"title"),256)||!https(str(r,"url"))||!hex(str(r,"publicKey"),64)||!json_object_is_type(get(r,"enabled"),json_type_boolean))return 0;
  for(size_t j=0;j<i;j++){J *p=json_object_array_get_idx(rows,j);if(eq(str(r,"id"),str(p,"id"))||eq(str(r,"url"),str(p,"url")))return 0;}
 }return 1;
}
static J *source_config_snapshot(dc_store *s){
 sqlite3_stmt *q=prep(s,"SELECT id,title,url,public_key,enabled FROM catalog_sources ORDER BY id");if(!q)return NULL;
 J *root=json_object_new_object(),*rows=json_object_new_array();const char *keys[]={"id","title","url","publicKey"};int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){J *r=json_object_new_object();for(int i=0;i<4;i++)json_object_object_add(r,keys[i],json_object_new_string((const char*)sqlite3_column_text(q,i)));json_object_object_add(r,"enabled",json_object_new_boolean(sqlite3_column_int(q,4)));json_object_array_add(rows,r);}
 sqlite3_finalize(q);json_object_object_add(root,"protocol",json_object_new_string("dunlin.catalog.sources"));json_object_object_add(root,"major",json_object_new_int(1));json_object_object_add(root,"sources",rows);
 if(rc!=SQLITE_DONE||!source_config_valid(root)){json_object_put(root);return NULL;}return root;
}
/* All bytes are staged before rename. Failure after publication is reported
 * separately: the next request reconciles SQLite from the valid file. */
static const char *source_config_write(dc_store *s,J *root){
 char path[PATH_MAX],temp[PATH_MAX];if(!source_path(s,path))return "source_config_path_invalid";
 const char *raw=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PRETTY);size_t len=raw?strlen(raw):0;if(!len||len>SOURCE_CONFIG_LIMIT||!source_config_valid(root))return "source_config_invalid";
 int n=snprintf(temp,sizeof temp,"%s/.catalog-sources-XXXXXX",s->directory);if(n<=0||n>=(int)sizeof temp)return "source_config_path_invalid";
 int fd=mkstemp(temp);if(fd<0)return "source_config_write_failed";
 size_t used=0;int ok=fchmod(fd,0600)==0;while(ok&&used<len){ssize_t wrote=write(fd,raw+used,len-used);if(wrote<0&&errno==EINTR)continue;if(wrote<=0){ok=0;break;}used+=(size_t)wrote;}
 if(ok)ok=fsync(fd)==0;if(close(fd))ok=0;
 if(!ok||rename(temp,path)){unlink(temp);return "source_config_write_failed";}
 int dir=open(s->directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);ok=dir>=0&&fsync(dir)==0;if(dir>=0)close(dir);return ok?NULL:"source_config_durability_unknown";
}
static const char *source_config_apply(dc_store *s,J *root){
 if(!sql(s,"BEGIN IMMEDIATE"))return "storage_error";
 const char *error=NULL;J *rows=get(root,"sources");
 if(!sql(s,"UPDATE catalog_sources SET enabled=0 WHERE enabled<>0"))error="storage_error";
 for(size_t i=0;!error&&i<json_object_array_length(rows);i++){
  J *r=json_object_array_get_idx(rows,i);sqlite3_stmt *q=prep(s,"SELECT url,public_key FROM catalog_sources WHERE id=?");if(q)db_bind(q,1,str(r,"id"));int rc=q?sqlite3_step(q):SQLITE_ERROR;
  if(rc==SQLITE_ROW&&(!eq((const char*)sqlite3_column_text(q,0),str(r,"url"))||!eq((const char*)sqlite3_column_text(q,1),str(r,"publicKey"))))error="source_identity_changed";
  else if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)error="storage_error";
  sqlite3_finalize(q);if(error)break;
  q=prep(s,"INSERT INTO catalog_sources(id,title,url,public_key,enabled) VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET title=excluded.title,enabled=excluded.enabled");
  if(q){db_bind(q,1,str(r,"id"));db_bind(q,2,str(r,"title"));db_bind(q,3,str(r,"url"));db_bind(q,4,str(r,"publicKey"));sqlite3_bind_int(q,5,json_object_get_boolean(get(r,"enabled")));}if(!step(q))error="source_config_conflict";
 }
 if(!error&&!sql(s,"COMMIT"))error="storage_error";if(error)sql(s,"ROLLBACK");return error;
}
static const char *source_config_sync(dc_store *s){
 char path[PATH_MAX];if(!source_path(s,path))return "source_config_path_invalid";
 int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
 if(fd<0){if(errno!=ENOENT)return "source_config_unreadable";J *root=source_config_snapshot(s);const char *error=root?source_config_write(s,root):"source_config_invalid";json_object_put(root);return error;}
 struct stat info;int valid=fstat(fd,&info)==0&&S_ISREG(info.st_mode)&&info.st_uid==geteuid()&&info.st_size>0&&(uint64_t)info.st_size<=SOURCE_CONFIG_LIMIT;
 if(!valid){close(fd);return "source_config_invalid";}
 size_t length=(size_t)info.st_size,used=0;char *raw=malloc(length+1);if(!raw){close(fd);return "out_of_memory";}
 while(used<length){ssize_t n=read(fd,raw+used,length-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 char extra;ssize_t tail=read(fd,&extra,1);close(fd);raw[used]=0;
 J *root=used==length&&tail==0?dunlin_json_parse(raw,length):NULL;free(raw);
 const char *error=source_config_valid(root)?source_config_apply(s,root):"source_config_invalid";json_object_put(root);return error;
}
static const char *source_config_finish(dc_store *s){
 J *root=source_config_snapshot(s);const char *error=root?source_config_write(s,root):"source_config_invalid";json_object_put(root);
 if(!error&&!sql(s,"COMMIT"))error="source_config_commit_pending";
 if(error)sql(s,"ROLLBACK");return error;
}
