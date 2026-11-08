#define _XOPEN_SOURCE 700
#include "project_files.h"
#include "skills.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/tools.h"
#include <json-c/json.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
typedef struct json_object J;
typedef struct {int root;int mode;ah_tool_service commands;char *data_directory;J *skills;dc_file_journal_v1 journal;} files;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);if(!json_object_is_type(v,json_type_string))return NULL;const char *p=json_object_get_string(v);return strlen(p)==(size_t)json_object_get_string_len(v)?p:NULL;}
static int path_ok(const char *p,int empty){
 if(!p||strlen(p)>1024||(!*p&&!empty)||*p=='/'||strchr(p,'\\')||!ah_utf8_text(p,strlen(p)))return 0;
 for(const char *c=p;*c;){const char *e=strchr(c,'/');size_t n=e?(size_t)(e-c):strlen(c);if(!n||n>NAME_MAX||(n==1&&*c=='.')||(n==2&&!memcmp(c,"..",2))||!strncmp(c,".dunlin-",8)||!strncmp(c,".dunlin-tmp-",10))return 0;if(!e)break;c=e+1;if(!*c)return 0;}return 1;
}
/* Caller owns returned pinned parent; final basename is copied into base. */
static int parent(int root,const char *path,char base[NAME_MAX+1]){
 if(!path_ok(path,0))return -1;char *copy=strdup(path);if(!copy)return -1;int fd=dup(root);char *cursor=copy,*slash;
 while(fd>=0&&(slash=strchr(cursor,'/'))){*slash=0;int next=openat(fd,cursor,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(fd);fd=next;cursor=slash+1;}
 if(fd>=0)snprintf(base,NAME_MAX+1,"%s",cursor);free(copy);return fd;
}
static char *read_at(int dir,const char *base,size_t cap,struct stat *identity){
 int fd=openat(dir,base,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return NULL;struct stat st;char *buf=NULL;
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<0||(uint64_t)st.st_size>cap)goto end;
 buf=malloc(cap+1);if(!buf)goto end;size_t used=0;for(;;){ssize_t n=read(fd,buf+used,cap+1-used);if(n<0&&errno==EINTR)continue;if(n<0||used+(size_t)n>cap){free(buf);buf=NULL;break;}if(n==0){if(!ah_utf8_text(buf,used)){free(buf);buf=NULL;}else{buf[used]=0;if(identity)*identity=st;}break;}used+=(size_t)n;}
end:close(fd);return buf;
}
static int same(const struct stat *a,const struct stat *b){return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&a->st_mtime==b->st_mtime&&a->st_ctime==b->st_ctime&&a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_gid==b->st_gid;}
static ah_status result(char **out,ah_status status,const char *text){*out=strdup(text);return *out?status:AH_FAILED;}
static ah_status file_read(files *s,J *args,size_t cap,char **out){
 char base[NAME_MAX+1];int dir=parent(s->root,str(args,"path"),base);if(dir<0)return result(out,AH_FAILED,"unsafe or inaccessible path");char *data=read_at(dir,base,cap,NULL);close(dir);if(!data)return result(out,AH_FAILED,"file missing, unsafe, non-UTF8 or exceeds limit");char hash[65];dc_sha256(data,strlen(data),hash);J *o=json_object_new_object();json_object_object_add(o,"path",json_object_get(get(args,"path")));json_object_object_add(o,"sha256",json_object_new_string(hash));json_object_object_add(o,"text",json_object_new_string(data));free(data);char *raw=strdup(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));json_object_put(o);if(!raw)return AH_FAILED;if(strlen(raw)>cap){free(raw);return result(out,AH_BUDGET,"encoded file result exceeds output limit");}*out=raw;return AH_DONE;
}
static ah_status file_write(files *s,const char *name,J *args,char **out){
 const char *path=str(args,"path"),*expected=str(args,"expected_sha256");
 char base[NAME_MAX+1],temporary[96]={0},operation_id[65]={0},digest[65]={0};
 int dir=parent(s->root,path,base),fd=-1,locked=0,staged=0,applied=0,prepared=0;
 char *before=NULL,*patched=NULL;const char *after=NULL,*failure="unsafe or inaccessible parent directory";
 ah_status status=AH_FAILED;int remove=!strcmp(name,"restore_file")&&json_object_get_boolean(get(args,"remove"));
 if(dir<0)goto cleanup;
 /* Cooperative directory ownership spans source capture through publication.
  * External programs that do not take this lock still require the final CAS. */
 if(flock(dir,LOCK_EX|LOCK_NB)){failure="directory is busy with another controlled file operation";goto cleanup;}locked=1;
 struct stat original,now,root;int present=fstatat(dir,base,&original,AT_SYMLINK_NOFOLLOW)==0;
 if(!present&&errno!=ENOENT){failure="cannot inspect target";goto cleanup;}
 before=present?read_at(dir,base,1048576,NULL):strdup("");
 if(before)dc_sha256(before,strlen(before),digest);
 if(!before||(present&&(!expected||strcmp(expected,digest)))||(!present&&expected&&strcmp(expected,"new"))){failure="conflict: read current file, then provide its expected_sha256; use new for absent files";goto cleanup;}
 if(remove&&!present){failure="conflict: target is already absent";goto cleanup;}
 after=remove?NULL:str(args,"text");
 if(!strcmp(name,"patch_file")){
  const char *old=str(args,"old_text"),*replacement=str(args,"new_text");char *at=old&&*old?strstr(before,old):NULL;
  if(!at||!replacement||strstr(at+strlen(old),old)){failure="patch requires exactly one matching source span";goto cleanup;}
  size_t n=strlen(before)-strlen(old)+strlen(replacement);if(n>1048576){failure="file exceeds write limit";status=AH_BUDGET;goto cleanup;}
  patched=malloc(n+1);if(patched){size_t lead=(size_t)(at-before);memcpy(patched,before,lead);memcpy(patched+lead,replacement,strlen(replacement));strcpy(patched+lead+strlen(replacement),at+strlen(old));}after=patched;
 }
 if(!remove&&!after){failure="missing file contents";goto cleanup;}
 if(after&&strlen(after)>1048576){failure="file exceeds write limit";status=AH_BUDGET;goto cleanup;}
 int exists=fstatat(dir,base,&now,AT_SYMLINK_NOFOLLOW)==0;int missing_error=errno;
 char *current=exists?read_at(dir,base,1048576,NULL):NULL;char current_hash[65];if(current)dc_sha256(current,strlen(current),current_hash);
 int conflict=present?(!exists||!current||!same(&original,&now)||strcmp(digest,current_hash)):exists||missing_error!=ENOENT;
 free(current);if(conflict){failure="conflict: target changed; no overwrite performed";goto cleanup;}
 if(s->journal.prepare){
  char dev[32],ino[32];if(fstat(s->root,&root)){failure="cannot verify project directory identity";goto cleanup;}
  snprintf(dev,sizeof dev,"%ju",(uintmax_t)root.st_dev);snprintf(ino,sizeof ino,"%ju",(uintmax_t)root.st_ino);
  if(!s->journal.prepare(s->journal.user,name,path,args,present,before,after,dev,ino,operation_id)){failure="cannot durably prepare authorized file history; no change applied";goto cleanup;}
  prepared=1;
  if(strlen(operation_id)!=64||strspn(operation_id,"0123456789abcdef")!=64){failure="invalid durable file operation identity";goto cleanup;}
 }
 if(after){
  static _Atomic unsigned serial;for(int tries=0;tries<20&&fd<0;tries++){unsigned sequence=atomic_fetch_add(&serial,1)+1;if(prepared)snprintf(temporary,sizeof temporary,".dunlin-%s",operation_id);else snprintf(temporary,sizeof temporary,".dunlin-%ld-%u",(long)getpid(),sequence);fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);}
  if(fd<0){failure="cannot stage file";goto cleanup;}staged=1;
  size_t len=strlen(after),used=0;while(used<len){ssize_t n=write(fd,after+used,len-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
  int ok=used==len;if(present&&fchmod(fd,original.st_mode&0777))ok=0;if(fsync(fd))ok=0;if(close(fd))ok=0;fd=-1;
  if(!ok){failure="cannot durably stage file";goto cleanup;}
 }
 /* SQLite preparation may have waited; recheck source after the durable log. */
 exists=fstatat(dir,base,&now,AT_SYMLINK_NOFOLLOW)==0;missing_error=errno;
 current=exists?read_at(dir,base,1048576,NULL):NULL;if(current)dc_sha256(current,strlen(current),current_hash);
 conflict=present?(!exists||!current||!same(&original,&now)||strcmp(digest,current_hash)):exists||missing_error!=ENOENT;free(current);
 if(conflict){failure="conflict after history preparation; no overwrite performed";goto cleanup;}
 if(remove?unlinkat(dir,base,0):renameat(dir,temporary,dir,base)){failure="atomic file publication failed";goto cleanup;}
 applied=1;
 int durable=fsync(dir)==0;
 int settled=!prepared||s->journal.settle(s->journal.user,operation_id,durable?"applied":"unknown",durable?"File operation and directory sync completed.":"File changed, but directory durability is unknown.");
 J *receipt=json_object_new_object();json_object_object_add(receipt,"path",json_object_new_string(path));if(after)dc_sha256(after,strlen(after),digest);else snprintf(digest,sizeof digest,"absent");
 json_object_object_add(receipt,"sha256",json_object_new_string(digest));json_object_object_add(receipt,"status",json_object_new_string(!settled?"history_commit_unknown":!durable?"durability_unknown":remove?"removed":"written"));if(prepared)json_object_object_add(receipt,"operation_id",json_object_new_string(operation_id));
 *out=strdup(json_object_to_json_string_ext(receipt,JSON_C_TO_STRING_PLAIN));json_object_put(receipt);status=*out&&durable&&settled?AH_DONE:AH_UNKNOWN;
cleanup:
 if(fd>=0)close(fd);if(staged&&!applied)unlinkat(dir,temporary,0);
 if(!applied&&prepared&&!s->journal.settle(s->journal.user,operation_id,"failed",failure))status=AH_UNKNOWN;
 if(locked)flock(dir,LOCK_UN);if(dir>=0)close(dir);free(before);free(patched);
 if(!applied)return result(out,status,failure);return status;
}
static J *listing(files *s,const char *path){
 if(!path_ok(path,1))return NULL;int fd;
 if(!*path)fd=dup(s->root);else{char base[NAME_MAX+1];int d=parent(s->root,path,base);fd=d>=0?openat(d,base,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;if(d>=0)close(d);}
 if(fd<0)return NULL;DIR *d=fdopendir(fd);if(!d){close(fd);return NULL;}J *a=json_object_new_array();struct dirent *e;size_t n=0;
 while((e=readdir(d))&&n<1000){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||!strncmp(e->d_name,".dunlin-",8)||!strncmp(e->d_name,".dunlin-tmp-",10))continue;struct stat st;if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)))continue;char p[PATH_MAX];snprintf(p,sizeof p,"%s%s%s",path,*path?"/":"",e->d_name);if(!path_ok(p,0))continue;J *o=json_object_new_object();json_object_object_add(o,"path",json_object_new_string(p));json_object_object_add(o,"name",json_object_new_string(e->d_name));json_object_object_add(o,"directory",json_object_new_boolean(S_ISDIR(st.st_mode)));json_object_object_add(o,"size",json_object_new_int64(st.st_size));json_object_array_add(a,o);n++;}
 closedir(d);return a;
}
static J *schema(void *ctx){
 files *s=ctx;J *a=json_object_new_array();
 if(s->skills&&json_object_array_length(s->skills)){
  J *entry=json_tokener_parse("{\"type\":\"function\",\"function\":{\"name\":\"read_skill_resource\",\"description\":\"Read a UTF8 resource from the immutable version of an enabled Skill. Source and package must match the enabled skill list. No script execution.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"package\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"source\",\"package\",\"path\"],\"additionalProperties\":false}}}");json_object_array_add(a,entry);
 }
 if(s->mode==2)return a;
 const char *names[]={"read_file","list_files","write_file","patch_file"};
 const char *desc[]={"Read project-relative UTF8 regular file. Returns source text and SHA256 for conflict detection.","List up to 1000 direct children. path is empty for project root. Symlinks excluded.","Atomically write project-relative file. Existing file requires SHA256 from read_file. New file uses expected_sha256=new. Always requires approval.","Replace exactly one old_text span using read_file SHA256. Always requires approval."};
 const char *props[]={"{\"path\":{\"type\":\"string\"}}","{\"path\":{\"type\":\"string\"}}","{\"path\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"expected_sha256\":{\"type\":\"string\"}}","{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"},\"expected_sha256\":{\"type\":\"string\"}}"};
 for(int i=0;i<(s->mode==1?2:4);i++){J *fn=json_object_new_object(),*o=json_object_new_object(),*par=json_object_new_object(),*required=json_object_new_array(),*p=json_tokener_parse(props[i]);json_object_object_foreach(p,k,v){(void)v;if(i<2||strcmp(k,"expected_sha256"))json_object_array_add(required,json_object_new_string(k));}json_object_object_add(par,"type",json_object_new_string("object"));json_object_object_add(par,"properties",p);json_object_object_add(par,"required",required);json_object_object_add(par,"additionalProperties",json_object_new_boolean(0));json_object_object_add(fn,"name",json_object_new_string(names[i]));json_object_object_add(fn,"description",json_object_new_string(desc[i]));json_object_object_add(fn,"parameters",par);json_object_object_add(o,"type",json_object_new_string("function"));json_object_object_add(o,"function",fn);json_object_array_add(a,o);}
 if(!s->mode){J *base=s->commands.schema(s->commands.ctx);for(size_t i=0;i<json_object_array_length(base);i++)json_object_array_add(a,json_object_get(json_object_array_get_idx(base,i)));json_object_put(base);}return a;
}
static ah_status check(void *ctx,const char *name,J *args,int *approval,int *effect,char *error,size_t cap){
 files *s=ctx;int list=!strcmp(name,"list_files"),read=!strcmp(name,"read_file"),write=!strcmp(name,"write_file"),patch=!strcmp(name,"patch_file");
 if(!strcmp(name,"read_skill_resource")){
  if(!s->skills||!str(args,"source")||!str(args,"package")||!path_ok(str(args,"path"),0)){snprintf(error,cap,"invalid skill resource reference");return AH_INVALID_ARGUMENTS;}*approval=*effect=0;return AH_DONE;
 }
 if(s->mode==2||(s->mode==1&&!list&&!read)){snprintf(error,cap,"tool disabled by project policy");return AH_FAILED;}
 if(!list&&!read&&!write&&!patch)return s->commands.check(s->commands.ctx,name,args,approval,effect,error,cap);
 if(!path_ok(str(args,"path"),list)){snprintf(error,cap,"path must stay within project without dot components or symlinks");return AH_INVALID_ARGUMENTS;}
 if((write&&!str(args,"text"))||(patch&&(!str(args,"old_text")||!*str(args,"old_text")||!str(args,"new_text")||!str(args,"expected_sha256")))){snprintf(error,cap,"missing file contents or expected_sha256");return AH_INVALID_ARGUMENTS;}
 *approval=*effect=write||patch;return AH_DONE;
}
static ah_status call(void *ctx,const char *name,J *args,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out){
 files *s=ctx;int approval=0,effect=0;char err[256];ah_status checked=check(ctx,name,args,&approval,&effect,err,sizeof err);if(checked!=AH_DONE)return result(out,checked,err);
 if(!strcmp(name,"read_skill_resource")){*out=dc_skill_resource(s->data_directory,s->skills,args,cap);return *out?AH_DONE:result(out,AH_FAILED,"resource not enabled, changed, binary, unsafe or exceeds limit");}
 if(!strcmp(name,"read_file"))return file_read(s,args,cap,out);
 if(!strcmp(name,"write_file")||!strcmp(name,"patch_file"))return file_write(s,name,args,out);
 if(!strcmp(name,"list_files")){J *a=listing(s,str(args,"path"));if(!a)return result(out,AH_FAILED,"directory inaccessible");const char *raw=json_object_to_json_string_ext(a,JSON_C_TO_STRING_PLAIN);ah_status status=strlen(raw)<=cap?result(out,AH_DONE,raw):result(out,AH_BUDGET,"directory result exceeds limit; select a smaller subdirectory");json_object_put(a);return status;}
 return s->commands.call(s->commands.ctx,name,args,hooks,cap,timeout,out);
}
int dc_project_tools_open(ah_tool_service *out,const char *root,const char *policy){
 if(!out||!root||!policy||(strcmp(policy,"ask")&&strcmp(policy,"readOnly")&&strcmp(policy,"noTools")))return 0;*out=(ah_tool_service){0};files *s=calloc(1,sizeof *s);if(!s)return 0;s->root=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);s->mode=!strcmp(policy,"noTools")?2:!strcmp(policy,"readOnly")?1:0;
 if(s->root<0||ah_linux_tools(&s->commands,root)){if(s->root>=0)close(s->root);free(s);return 0;}const char *names[]={"echo","calc","read_file","write_file","exec","http_get","shell"};for(size_t i=0;i<7;i++)ah_linux_tool_enable(&s->commands,names[i],!strcmp(names[i],"exec")||!strcmp(names[i],"shell"));*out=(ah_tool_service){s,schema,check,call};return 1;
}
void dc_project_tools_close(ah_tool_service *tools){if(tools&&tools->ctx){files *s=tools->ctx;ah_linux_tools_close(&s->commands);close(s->root);free(s->data_directory);json_object_put(s->skills);free(s);*tools=(ah_tool_service){0};}}
static int root_matches(int fd,const char *device,const char *inode){
 struct stat st;char dev[32],ino[32];if(!device||!*device||!inode||!*inode||fstat(fd,&st)||!S_ISDIR(st.st_mode))return 0;
 snprintf(dev,sizeof dev,"%ju",(uintmax_t)st.st_dev);snprintf(ino,sizeof ino,"%ju",(uintmax_t)st.st_ino);return !strcmp(device,dev)&&!strcmp(inode,ino);
}
int dc_project_tools_identity(const ah_tool_service *tools,const char *device,const char *inode){
 if(!tools||!tools->ctx||tools->schema!=schema||tools->check!=check||tools->call!=call)return 0;
 const files *s=tools->ctx;uint64_t d=0,i=0;char dev[32],ino[32];
 if(!root_matches(s->root,device,inode)||!ah_linux_tools_root_identity(&s->commands,&d,&i))return 0;
 snprintf(dev,sizeof dev,"%" PRIu64,d);snprintf(ino,sizeof ino,"%" PRIu64,i);return !strcmp(device,dev)&&!strcmp(inode,ino);
}
void dc_project_tools_skills(ah_tool_service *tools,const char *directory,J *resolved){if(tools&&tools->ctx){files *s=tools->ctx;free(s->data_directory);json_object_put(s->skills);s->data_directory=strdup(directory);s->skills=json_object_get(resolved);}}
void dc_project_tools_journal(ah_tool_service *tools,const dc_file_journal_v1 *journal){if(tools&&tools->ctx){files *s=tools->ctx;s->journal=journal&&journal->prepare&&journal->settle?*journal:(dc_file_journal_v1){0};}}
ah_status dc_project_file_restore(const char *root,J *args,const dc_file_journal_v1 *journal,char **out){
 if(!out)return AH_FAILED;*out=NULL;if(!journal||!journal->prepare||!journal->settle||!path_ok(str(args,"path"),0)||!str(args,"expected_sha256")||(!json_object_get_boolean(get(args,"remove"))&&!str(args,"text")))return result(out,AH_FAILED,"invalid explicit restore request");
 ah_tool_service tools={0};if(!dc_project_tools_open(&tools,root,"ask"))return result(out,AH_FAILED,"project unavailable");dc_project_tools_journal(&tools,journal);ah_status status=file_write(tools.ctx,"restore_file",args,out);dc_project_tools_close(&tools);return status;
}
int dc_project_file_digest(const char *root,const char *path,char hash[65]){
 if(!root||!path||!hash)return -1;int fd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;char base[NAME_MAX+1];int dir=parent(fd,path,base);close(fd);if(dir<0)return -1;struct stat info;int state=fstatat(dir,base,&info,AT_SYMLINK_NOFOLLOW);if(state&&errno==ENOENT){close(dir);return 0;}if(state){close(dir);return -1;}char *text=read_at(dir,base,1048576,NULL);close(dir);if(!text)return -1;dc_sha256(text,strlen(text),hash);free(text);return 1;
}
int dc_project_file_discard_stage(const char *root,const char *path,const char *id,const char *intended){
 if(!root||!path||!id||strlen(id)!=64||strspn(id,"0123456789abcdef")!=64||!intended||strlen(intended)>1048576)return 0;
 int root_fd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root_fd<0)return 0;char base[NAME_MAX+1];int dir=parent(root_fd,path,base);close(root_fd);if(dir<0)return 0;
 if(flock(dir,LOCK_EX|LOCK_NB)){close(dir);return 0;}char stage[80];snprintf(stage,sizeof stage,".dunlin-%s",id);int fd=openat(dir,stage,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC),clean=0;
 if(fd<0){clean=errno==ENOENT&&fsync(dir)==0;goto end;}struct stat first,last;size_t total=0,limit=strlen(intended);
 if(fstat(fd,&first)||!S_ISREG(first.st_mode)||first.st_nlink!=1||first.st_uid!=geteuid()||first.st_size<0||(uint64_t)first.st_size>limit)goto end;
 for(;;){char bytes[8192];ssize_t n=read(fd,bytes,sizeof bytes);if(n<0&&errno==EINTR)continue;if(n<0||(size_t)n>limit-total)goto end;if(n==0)break;if(memcmp(bytes,intended+total,(size_t)n))goto end;total+=(size_t)n;}
 if(total!=(size_t)first.st_size||fstatat(dir,stage,&last,AT_SYMLINK_NOFOLLOW)||last.st_nlink!=1||!same(&first,&last))goto end;
 clean=unlinkat(dir,stage,0)==0;if(clean&&fsync(dir))clean=0;
end:if(fd>=0)close(fd);flock(dir,LOCK_UN);close(dir);return clean;
}
J *dc_project_file_query_bound(const char *root,const char *device,const char *inode,J *request){
 if(!root)return NULL;files s={.root=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)};if(s.root<0)return NULL;
 if((device||inode)&&!root_matches(s.root,device,inode)){close(s.root);return NULL;}
 const char *op=str(request,"op");J *r=NULL;if(op&&!strcmp(op,"files.list"))r=listing(&s,str(request,"path"));else if(op&&!strcmp(op,"files.read")){char *out=NULL;if(file_read(&s,request,65536,&out)==AH_DONE)r=json_tokener_parse(out);free(out);}close(s.root);return r;
}
J *dc_project_file_query(const char *root,J *request){return dc_project_file_query_bound(root,NULL,NULL,request);}
char *dc_preview_file(const char *root,const char *path){
 if(!root||!path)return NULL;J *q=json_object_new_object();json_object_object_add(q,"op",json_object_new_string("files.read"));json_object_object_add(q,"path",json_object_new_string(path));J *o=dc_project_file_query(root,q);json_object_put(q);char *out=o?strdup(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN)):NULL;json_object_put(o);return out;
}
