#define _XOPEN_SOURCE 700
#include "../../desktop/skills.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/agent.h"
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);if(!json_object_is_type(v,json_type_string))return NULL;const char *p=json_object_get_string(v);return strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int i,const char *v){sqlite3_bind_text(q,i,v?v:"",-1,SQLITE_TRANSIENT);}
static int sql(dc_store *s,const char *v){return sqlite3_exec(s->db,v,NULL,NULL,NULL)==SQLITE_OK;}
static int step(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static J *reply(int ok,const char *error,J *data){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(o,"error",json_object_new_string(error));if(data)json_object_object_add(o,"data",data);return o;}
static int valid(const char *p,size_t cap){return p&&*p&&strlen(p)<=cap&&ah_utf8_text(p,strlen(p));}
int dc_skills_schema(dc_store *s){return sql(s,"CREATE TABLE IF NOT EXISTS skill_versions(source TEXT NOT NULL,package TEXT NOT NULL,version TEXT NOT NULL,digest TEXT NOT NULL,name TEXT NOT NULL,description TEXT NOT NULL,body TEXT NOT NULL,resources TEXT NOT NULL,local_path TEXT NOT NULL DEFAULT '',quarantined INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(source,package,version));CREATE TABLE IF NOT EXISTS skill_bindings(scope TEXT NOT NULL,scope_id TEXT NOT NULL,source TEXT NOT NULL,package TEXT NOT NULL,mode TEXT NOT NULL,version TEXT NOT NULL DEFAULT '',PRIMARY KEY(scope,scope_id,source,package));CREATE TABLE IF NOT EXISTS skill_faults(source TEXT,package TEXT,version TEXT,phase TEXT,error TEXT,created INTEGER DEFAULT(unixepoch()));");}
static int scope_ok(dc_store *s,const char *scope,const char *id){
 if(!scope||!id)return 0;if(!strcmp(scope,"global"))return !*id;
 sqlite3_stmt *q=prep(s,!strcmp(scope,"project")?"SELECT 1 FROM projects WHERE id=?":!strcmp(scope,"chat")?"SELECT 1 FROM chats WHERE id=?":"SELECT 1 WHERE 0");if(!q)return 0;bind(q,1,id);int ok=sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);return ok;
}
static char *read_fd(int fd,size_t cap,size_t *length){struct stat st,after;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<0||(uint64_t)st.st_size>cap)return NULL;char *p=malloc((size_t)st.st_size+1);if(!p)return NULL;size_t used=0,n=(size_t)st.st_size;while(used<n){ssize_t r=read(fd,p+used,n-used);if(r<0&&errno==EINTR)continue;if(r<=0){free(p);return NULL;}used+=(size_t)r;}if(fstat(fd,&after)||after.st_size!=st.st_size||after.st_mtime!=st.st_mtime){free(p);return NULL;}p[n]=0;if(length)*length=n;return p;}
static int blob(dc_store *s,const char *digest,const char *bytes,size_t len){
 char path[PATH_MAX];snprintf(path,sizeof path,"%s/skill-blobs",s->directory);if(mkdir(path,0700)&&errno!=EEXIST)return 0;struct stat st;if(lstat(path,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))return 0;
 int dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return 0;int fd=openat(dir,digest,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 if(fd<0&&errno==EEXIST){fd=openat(dir,digest,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);size_t actual=0;char *p=fd>=0?read_fd(fd,1048576,&actual):NULL;char hash[65];if(p)dc_sha256(p,actual,hash);int ok=p&&actual==len&&!strcmp(hash,digest);free(p);if(fd>=0)close(fd);close(dir);return ok;}
 if(fd<0){close(dir);return 0;}size_t used=0;while(used<len){ssize_t n=write(fd,bytes+used,len-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}int ok=used==len&&!fsync(fd);if(close(fd))ok=0;if(!ok)unlinkat(dir,digest,0);else if(fsync(dir))ok=0;close(dir);return ok;
}
typedef struct {dc_store *store;J *resources;size_t count,total;char *body;} snapshot;
static int scan(snapshot *s,int dir,const char *prefix,int depth){
 if(depth>8)return 0;DIR *d=fdopendir(dup(dir));if(!d)return 0;struct dirent *e;int ok=1;
 while(ok&&(e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||!strcmp(e->d_name,".git")||!strcmp(e->d_name,"node_modules")||!strcmp(e->d_name,"__pycache__"))continue;
  char path[1024];int n=snprintf(path,sizeof path,"%s%s%s",prefix,*prefix?"/":"",e->d_name);struct stat st;if(n<0||n>=(int)sizeof path||!ah_utf8_text(path,strlen(path))||fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){ok=0;break;}
  if(S_ISDIR(st.st_mode)){int fd=openat(dir,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);ok=fd>=0&&scan(s,fd,path,depth+1);if(fd>=0)close(fd);continue;}
  if(!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<0||st.st_size>1048576||++s->count>128||s->total+(size_t)st.st_size>8388608){ok=0;break;}
  size_t len=0;int fd=openat(dir,e->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);char *data=fd>=0?read_fd(fd,1048576,&len):NULL;if(fd>=0)close(fd);if(!data||s->total+len>8388608){free(data);ok=0;break;}
  char digest[65];dc_sha256(data,len,digest);ok=blob(s->store,digest,data,len);s->total+=len;
  if(!strcmp(path,"SKILL.md")){if(!ah_utf8_text(data,len)||len>32768)ok=0;else s->body=strdup(data);}
  free(data);J *row=json_object_new_object();json_object_object_add(row,"path",json_object_new_string(path));json_object_object_add(row,"digest",json_object_new_string(digest));json_object_object_add(row,"size",json_object_new_int64((int64_t)len));json_object_array_add(s->resources,row);
 }
 closedir(d);return ok;
}
static int compare(const void *a,const void *b){J *x=*(J *const*)a,*y=*(J *const*)b;return strcmp(str(x,"path"),str(y,"path"));}
/* A deliberately data-only frontmatter subset. No YAML includes/tags/code. */
static char *metadata(const char *body,const char *key){
 if(strncmp(body,"---\n",4)&&strncmp(body,"---\r\n",5))return NULL;const char *p=strchr(body,'\n')+1;
 while(*p){const char *e=strchr(p,'\n');if(!e)e=p+strlen(p);size_t n=(size_t)(e-p);if(n>=3&&!memcmp(p,"---",3))break;size_t k=strlen(key);if(n>k+1&&!strncmp(p,key,k)&&p[k]==':'){const char *v=p+k+1;while(v<e&&(*v==' '||*v=='\t'))v++;while(e>v&&(e[-1]==' '||e[-1]=='\r'))e--;if(e>v&&((*v=='"'&&e[-1]=='"')||(*v=='\''&&e[-1]=='\''))){v++;e--;}return strndup(v,(size_t)(e-v));}p=*e?e+1:e;
 }return NULL;
}
static J *import_as(dc_store *s,J *request,const char *verified_source,const char *verified_package,const char *verified_version){
 const char *path=str(request,"path");if(!path||strlen(path)>=PATH_MAX)return reply(0,"invalid_path",NULL);char canonical[PATH_MAX];if(!realpath(path,canonical))return reply(0,"unreadable_skill_directory",NULL);
 int fd=open(canonical,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return reply(0,"unreadable_skill_directory",NULL);snapshot snap={.store=s,.resources=json_object_new_array()};int ok=scan(&snap,fd,"",0);close(fd);
 if(!ok||!snap.body){free(snap.body);json_object_put(snap.resources);return reply(0,"unsafe_or_oversized_skill_snapshot",NULL);}
 size_t n=json_object_array_length(snap.resources);J **rows=calloc(n?n:1,sizeof *rows);if(!rows){free(snap.body);json_object_put(snap.resources);return NULL;}for(size_t i=0;i<n;i++)rows[i]=json_object_array_get_idx(snap.resources,i);qsort(rows,n,sizeof *rows,compare);J *resources=json_object_new_array();for(size_t i=0;i<n;i++)json_object_array_add(resources,json_object_get(rows[i]));free(rows);json_object_put(snap.resources);
 const char *raw=json_object_to_json_string_ext(resources,JSON_C_TO_STRING_PLAIN);char digest[65],path_hash[65],source[256];dc_sha256(raw,strlen(raw),digest);dc_sha256(canonical,strlen(canonical),path_hash);snprintf(source,sizeof source,"local:%s",path_hash);
 char *name=metadata(snap.body,"name"),*description=metadata(snap.body,"description"),*version=metadata(snap.body,"version");if(!name)name=strdup(strrchr(canonical,'/')+1);if(!description)description=strdup("");if(!version){char v[64];snprintf(v,sizeof v,"0.0.0+%.16s",digest);version=strdup(v);}
 if(verified_version&&strcmp(version,verified_version)){free(name);free(description);free(version);free(snap.body);json_object_put(resources);return reply(0,"skill_version_mismatch",NULL);}
 if(verified_source)snprintf(source,sizeof source,"%s",verified_source);
 if(verified_package){free(name);name=strdup(verified_package);}
 if(!valid(name,128)||!valid(version,128)||!description||strlen(description)>2048){ok=0;}
 sqlite3_stmt *q=ok?prep(s,"SELECT digest FROM skill_versions WHERE source=? AND package=? AND version=?"):NULL;int found=0;
 if(q){bind(q,1,source);bind(q,2,name);bind(q,3,version);if(sqlite3_step(q)==SQLITE_ROW){found=1;ok=!strcmp((const char*)sqlite3_column_text(q,0),digest);}sqlite3_finalize(q);}else ok=0;
 if(ok&&!found){q=prep(s,"INSERT INTO skill_versions(source,package,version,digest,name,description,body,resources,local_path) VALUES(?,?,?,?,?,?,?,?,?)");if(q){const char *v[]={source,name,version,digest,name,description,snap.body,raw,verified_source?"":canonical};for(int i=0;i<9;i++)bind(q,i+1,v[i]);}ok=step(q);}
 J *data=NULL;if(ok){data=json_object_new_object();json_object_object_add(data,"source",json_object_new_string(source));json_object_object_add(data,"package",json_object_new_string(name));json_object_object_add(data,"version",json_object_new_string(version));json_object_object_add(data,"digest",json_object_new_string(digest));}
 free(name);free(description);free(version);free(snap.body);json_object_put(resources);return reply(ok,ok?NULL:found?"immutable_version_conflict":"invalid_skill_metadata_or_storage_error",data);
}
static J *import(dc_store *s,J *request){return import_as(s,request,NULL,NULL,NULL);}
J *dc_skills_import_verified(dc_store *s,const char *path,const char *source,const char *package,const char *version){
 if(!valid(source,255)||!valid(package,128)||!valid(version,128))return reply(0,"invalid_verified_reference",NULL);
 J *r=json_object_new_object();json_object_object_add(r,"path",json_object_new_string(path));J *result=import_as(s,r,source,package,version);json_object_put(r);return result;
}
static J *versions(dc_store *s,const char *source,const char *package){
 sqlite3_stmt *q=prep(s,"SELECT version,digest,name,description,body,resources,local_path,quarantined FROM skill_versions WHERE source=? AND package=? ORDER BY rowid DESC");if(!q)return NULL;bind(q,1,source);bind(q,2,package);J *a=json_object_new_array();int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){J *v=json_object_new_object();const char *keys[]={"version","digest","name","description","body","resources","localPath","quarantined"};for(int i=0;i<8;i++){const char *p=(const char*)sqlite3_column_text(q,i);json_object_object_add(v,keys[i],i==5?json_tokener_parse(p):i==7?json_object_new_boolean(sqlite3_column_int(q,i)):json_object_new_string(p?p:""));}json_object_array_add(a,v);}sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return NULL;}return a;
}
static int apply(dc_store *s,J *item,const char *scope,const char *id){
 sqlite3_stmt *q=prep(s,"SELECT mode,version FROM skill_bindings WHERE scope=? AND scope_id=? AND source=? AND package=?");if(!q)return 0;bind(q,1,scope);bind(q,2,id);bind(q,3,str(item,"source"));bind(q,4,str(item,"package"));int rc=sqlite3_step(q);if(rc==SQLITE_ROW){const char *mode=(const char*)sqlite3_column_text(q,0),*version=(const char*)sqlite3_column_text(q,1);if(strcmp(mode,"inherit")){json_object_object_add(item,"enabled",json_object_new_boolean(!strcmp(mode,"enable")));json_object_object_add(item,"origin",json_object_new_string(scope));if(*version)json_object_object_add(item,"version",json_object_new_string(version));}}
 sqlite3_finalize(q);return rc==SQLITE_ROW||rc==SQLITE_DONE;
}
int dc_skills_snapshot_allowed(dc_store *s,J *snapshot){
 if(!json_object_is_type(snapshot,json_type_array)||json_object_array_length(snapshot)>256)return 0;
 for(size_t i=0;i<json_object_array_length(snapshot);i++){J *v=json_object_array_get_idx(snapshot,i),*selected=get(v,"selected");const char *source=str(v,"source"),*package=str(v,"package"),*version=str(selected,"version"),*digest=str(selected,"digest");if(!source||!package||!version||!digest||!str(selected,"body")||!json_object_is_type(get(selected,"resources"),json_type_array))return 0;sqlite3_stmt *q=prep(s,"SELECT digest FROM skill_versions WHERE source=? AND package=? AND version=? AND quarantined=0");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}int valid=q&&sqlite3_step(q)==SQLITE_ROW&&!strcmp((const char*)sqlite3_column_text(q,0),digest);sqlite3_finalize(q);if(!valid)return 0;}return 1;
}
J *dc_skills_resolve(dc_store *s,const char *project,const char *chat){
 sqlite3_stmt *q=prep(s,"SELECT source,package FROM skill_versions GROUP BY source,package ORDER BY package,source");if(!q)return NULL;J *a=json_object_new_array();int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW){const char *source=(const char*)sqlite3_column_text(q,0),*package=(const char*)sqlite3_column_text(q,1);J *v=json_object_new_object(),*all=versions(s,source,package);if(!all||!json_object_array_length(all)){json_object_put(all);json_object_put(v);json_object_put(a);sqlite3_finalize(q);return NULL;}
  json_object_object_add(v,"source",json_object_new_string(source));json_object_object_add(v,"package",json_object_new_string(package));json_object_object_add(v,"enabled",json_object_new_boolean(0));json_object_object_add(v,"version",json_object_get(get(json_object_array_get_idx(all,0),"version")));json_object_object_add(v,"origin",json_object_new_string("none"));
  if(!apply(s,v,"global","")||(project&&*project&&!apply(s,v,"project",project))||(chat&&*chat&&!apply(s,v,"chat",chat))){json_object_put(all);json_object_put(v);json_object_put(a);sqlite3_finalize(q);return NULL;}
  const char *selected=str(v,"version");J *chosen=NULL;for(size_t i=0;i<json_object_array_length(all);i++){J *item=json_object_array_get_idx(all,i);if(!strcmp(str(item,"version"),selected))chosen=item;}
  int denied=!chosen||json_object_get_boolean(get(chosen,"quarantined"));json_object_object_add(v,"blocked",json_object_new_boolean(denied));if(denied)json_object_object_add(v,"enabled",json_object_new_boolean(0));json_object_object_add(v,"selected",chosen?json_object_get(chosen):NULL);json_object_object_add(v,"versions",all);json_object_array_add(a,v);
 }
 sqlite3_finalize(q);if(rc!=SQLITE_DONE){json_object_put(a);return NULL;}return a;
}
J *dc_skills_request(dc_store *s,J *r){
 const char *op=str(r,"op");if(!op)return reply(0,"invalid_request",NULL);
 if(!strcmp(op,"skills.import"))return import(s,r);
 const char *scope=str(r,"scope"),*id=str(r,"scopeID");if(!scope_ok(s,scope,id))return reply(0,"invalid_scope",NULL);
 char *project=NULL;if(!strcmp(scope,"chat")){sqlite3_stmt *q=prep(s,"SELECT project FROM chat_properties WHERE chat=?");if(q){bind(q,1,id);if(sqlite3_step(q)==SQLITE_ROW)project=strdup((const char*)sqlite3_column_text(q,0));sqlite3_finalize(q);}}
 if(!strcmp(op,"skills.list")){
  J *a=dc_skills_resolve(s,!strcmp(scope,"project")?id:project,!strcmp(scope,"chat")?id:NULL);free(project);if(!a)return reply(0,"storage_error",NULL);
  for(size_t i=0;i<json_object_array_length(a);i++){J *v=json_object_array_get_idx(a,i);sqlite3_stmt *q=prep(s,"SELECT mode,version FROM skill_bindings WHERE scope=? AND scope_id=? AND source=? AND package=?");const char *mode="inherit";if(q){bind(q,1,scope);bind(q,2,id);bind(q,3,str(v,"source"));bind(q,4,str(v,"package"));if(sqlite3_step(q)==SQLITE_ROW)mode=(const char*)sqlite3_column_text(q,0);json_object_object_add(v,"scopeMode",json_object_new_string(mode));sqlite3_finalize(q);}}
  return reply(1,NULL,a);
 }
 free(project);
 const char *source=str(r,"source"),*package=str(r,"package"),*mode=str(r,"mode"),*version=str(r,"version");
 if(!strcmp(op,"skills.bind")){
  if(!valid(source,256)||!valid(package,128)||!mode||(strcmp(mode,"inherit")&&strcmp(mode,"enable")&&strcmp(mode,"disable"))||(!strcmp(mode,"enable")&&!valid(version,128)))return reply(0,"invalid_binding",NULL);
  if(!strcmp(mode,"enable")){sqlite3_stmt *q=prep(s,"SELECT 1 FROM skill_versions WHERE source=? AND package=? AND version=? AND quarantined=0");if(q){bind(q,1,source);bind(q,2,package);bind(q,3,version);}int found=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);if(!found)return reply(0,"version_missing_or_quarantined",NULL);}
  sqlite3_stmt *q=prep(s,"INSERT INTO skill_bindings(scope,scope_id,source,package,mode,version) VALUES(?,?,?,?,?,?) ON CONFLICT(scope,scope_id,source,package) DO UPDATE SET mode=excluded.mode,version=excluded.version");if(q){const char *v[]={scope,id,source,package,mode,version?version:""};for(int i=0;i<6;i++)bind(q,i+1,v[i]);}int ok=step(q);return reply(ok,ok?NULL:"storage_error",NULL);
 }
 if(!strcmp(op,"skills.update")){
  sqlite3_stmt *q=prep(s,"SELECT local_path FROM skill_versions WHERE source=? AND package=? AND local_path<>'' ORDER BY rowid DESC LIMIT 1");if(q){bind(q,1,source);bind(q,2,package);}char *path=q&&sqlite3_step(q)==SQLITE_ROW?strdup((const char*)sqlite3_column_text(q,0)):NULL;sqlite3_finalize(q);if(!path)return reply(0,"source_unavailable",NULL);J *request=json_object_new_object();json_object_object_add(request,"path",json_object_new_string(path));J *result=import(s,request);json_object_put(request);free(path);return result;
 }
 return reply(0,"unsupported_operation",NULL);
}
char *dc_skill_resource(const char *directory,J *resolved,J *arguments,size_t cap){
 const char *source=str(arguments,"source"),*package=str(arguments,"package"),*path=str(arguments,"path");if(!directory||!source||!package||!path)return NULL;
 J *resource=NULL;
 for(size_t i=0;resolved&&i<json_object_array_length(resolved);i++){
  J *v=json_object_array_get_idx(resolved,i);if(!json_object_get_boolean(get(v,"enabled"))||strcmp(source,str(v,"source"))||strcmp(package,str(v,"package")))continue;
  J *resources=get(get(v,"selected"),"resources");for(size_t n=0;resources&&n<json_object_array_length(resources);n++){J *r=json_object_array_get_idx(resources,n);if(!strcmp(path,str(r,"path")))resource=r;}
 }
 if(!resource)return NULL;const char *digest=str(resource,"digest");if(!digest||strlen(digest)!=64||strspn(digest,"0123456789abcdef")!=64)return NULL;
 char dirpath[PATH_MAX];snprintf(dirpath,sizeof dirpath,"%s/skill-blobs",directory);int dir=open(dirpath,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return NULL;int fd=openat(dir,digest,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);close(dir);size_t len=0;char *p=fd>=0?read_fd(fd,cap,&len):NULL;if(fd>=0)close(fd);if(!p)return NULL;char hash[65];dc_sha256(p,len,hash);if(strcmp(hash,digest)||!ah_utf8_text(p,len)){free(p);return NULL;}return p;
}
