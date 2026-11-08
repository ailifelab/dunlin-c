#define _XOPEN_SOURCE 700
#include "../../desktop/package_store.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/agent.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static char *read_fd(int fd,size_t *length){struct stat a,b;if(fstat(fd,&a)||!S_ISREG(a.st_mode)||a.st_nlink!=1||a.st_size<0||a.st_size>1048576)return NULL;size_t n=(size_t)a.st_size;char *p=malloc(n+1);if(!p)return NULL;for(size_t used=0;used<n;){ssize_t k=read(fd,p+used,n-used);if(k<0&&errno==EINTR)continue;if(k<=0){free(p);return NULL;}used+=(size_t)k;}if(fstat(fd,&b)||a.st_size!=b.st_size||a.st_mtime!=b.st_mtime){free(p);return NULL;}p[n]=0;*length=n;return p;}
static int copy_bytes(int dir,const char *name,const char *bytes,size_t n){int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return 0;size_t used=0;while(used<n){ssize_t k=write(fd,bytes+used,n-used);if(k<0&&errno==EINTR)continue;if(k<=0)break;used+=(size_t)k;}int ok=used==n&&!fsync(fd);if(close(fd))ok=0;return ok;}
typedef struct{J *files,*manifest;size_t count,total;} scan_state;
static int scan(scan_state *s,int source,int dest,const char *prefix,int depth){
 if(depth>8)return 0;DIR *dir=fdopendir(dup(source));if(!dir)return 0;int ok=1;struct dirent *e;while(ok&&(e=readdir(dir))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||!strcmp(e->d_name,".git")||!strcmp(e->d_name,"node_modules")||!strcmp(e->d_name,"__pycache__"))continue;
  char path[513];if(snprintf(path,sizeof path,"%s%s%s",prefix,*prefix?"/":"",e->d_name)>=(int)sizeof path||strchr(e->d_name,'\\')||!ah_utf8_text(path,strlen(path))){ok=0;break;}struct stat st;if(fstatat(source,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){ok=0;break;}
  if(S_ISDIR(st.st_mode)){if(mkdirat(dest,e->d_name,0700)){ok=0;break;}int a=openat(source,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),b=openat(dest,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);ok=a>=0&&b>=0&&scan(s,a,b,path,depth+1);if(a>=0)close(a);if(b>=0){if(fsync(b))ok=0;close(b);}continue;}
  if(!S_ISREG(st.st_mode)||st.st_nlink!=1||++s->count>128){ok=0;break;}int fd=openat(source,e->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);size_t n=0;char *bytes=fd>=0?read_fd(fd,&n):NULL;if(fd>=0)close(fd);if(!bytes||s->total+n>8388608){free(bytes);ok=0;break;}s->total+=n;char hash[65];dc_sha256(bytes,n,hash);if(!strcmp(path,"dunlin.json")){s->manifest=n<=32768?dunlin_json_parse(bytes,n):NULL;if(!s->manifest){free(bytes);ok=0;break;}}ok=copy_bytes(dest,e->d_name,bytes,n);free(bytes);J *f=json_object_new_object();json_object_object_add(f,"path",json_object_new_string(path));json_object_object_add(f,"digest",json_object_new_string(hash));json_object_object_add(f,"size",json_object_new_int64((int64_t)n));json_object_array_add(s->files,f);
 }closedir(dir);return ok;
}
static int compare(const void *a,const void *b){return strcmp(str(*(J *const*)a,"path"),str(*(J *const*)b,"path"));}
static int open_file(int root,const char *relative){if(!relative||!*relative||relative[0]=='/'||strlen(relative)>512||strchr(relative,'\\'))return -1;char path[513];snprintf(path,sizeof path,"%s",relative);char *at=path,*slash;int dir=dup(root);if(dir<0)return -1;while((slash=strchr(at,'/'))){*slash=0;if(!*at||!strcmp(at,".")||!strcmp(at,"..")){close(dir);return -1;}int next=openat(dir,at,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(dir);if(next<0)return -1;dir=next;at=slash+1;}int fd=-1;if(*at&&strcmp(at,".")&&strcmp(at,".."))fd=openat(dir,at,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);close(dir);return fd;}
static int exact_tree(int fd,const char *prefix,int depth,J *files,size_t *seen){
 if(depth>8)return 0;DIR *d=fdopendir(dup(fd));if(!d)return 0;int ok=1;struct dirent *e;while(ok&&(e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char path[513];struct stat st;if(snprintf(path,sizeof path,"%s%s%s",prefix,*prefix?"/":"",e->d_name)>=(int)sizeof path||fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){ok=0;break;}if(S_ISDIR(st.st_mode)){int child=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);ok=child>=0&&exact_tree(child,path,depth+1,files,seen);if(child>=0)close(child);}else{int found=0;for(size_t i=0;i<json_object_array_length(files);i++){const char *expected=str(json_object_array_get_idx(files,i),"path");if(expected&&!strcmp(path,expected)){found=1;break;}}ok=found&&S_ISREG(st.st_mode)&&st.st_nlink==1&&++*seen<=128;}}closedir(d);return ok;
}
int dc_package_verify(const char *directory,J *files){
 if(!directory||!json_object_is_type(files,json_type_array)||json_object_array_length(files)>128)return 0;int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return 0;size_t seen=0;int ok=exact_tree(dir,"",0,files,&seen)&&seen==json_object_array_length(files);for(size_t i=0;ok&&i<json_object_array_length(files);i++){J *f=json_object_array_get_idx(files,i);int fd=open_file(dir,str(f,"path"));size_t n=0;char *bytes=fd>=0?read_fd(fd,&n):NULL;if(fd>=0)close(fd);char hash[65];if(bytes)dc_sha256(bytes,n,hash);ok=bytes&&str(f,"digest")&&!strcmp(hash,str(f,"digest"))&&n==(uint64_t)json_object_get_int64(get(f,"size"));free(bytes);}close(dir);return ok;
}
char *dc_package_read_text(const char *directory,J *files,const char *relative){
 if(!directory||!relative||!json_object_is_type(files,json_type_array))return NULL;
 J *expected=NULL;for(size_t i=0;i<json_object_array_length(files);i++){J *f=json_object_array_get_idx(files,i);const char *p=str(f,"path");if(p&&!strcmp(p,relative)){expected=f;break;}}
 if(!expected)return NULL;int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return NULL;
 int fd=open_file(dir,relative);close(dir);size_t n=0;char *bytes=fd>=0?read_fd(fd,&n):NULL;if(fd>=0)close(fd);
 if(!bytes)return NULL;char hash[65];dc_sha256(bytes,n,hash);
 if(!str(expected,"digest")||strcmp(hash,str(expected,"digest"))||n!=(uint64_t)json_object_get_int64(get(expected,"size"))||!ah_utf8_text(bytes,n)||memchr(bytes,0,n)){free(bytes);return NULL;}
 return bytes;
}
static void clear(int fd){DIR *d=fdopendir(dup(fd));if(!d)return;struct dirent *e;while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;struct stat st;if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;if(S_ISDIR(st.st_mode)){int child=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(child>=0){clear(child);close(child);unlinkat(fd,e->d_name,AT_REMOVEDIR);}}else unlinkat(fd,e->d_name,0);}closedir(d);}
J *dc_package_snapshot(dc_store *s,const char *source){
 char canonical[PATH_MAX],parent[PATH_MAX],staging[PATH_MAX],final[PATH_MAX];if(!source||!realpath(source,canonical))return NULL;int input=open(canonical,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(input<0)return NULL;snprintf(parent,sizeof parent,"%s/extension-store",s->directory);if(mkdir(parent,0700)&&errno!=EEXIST){close(input);return NULL;}struct stat st;if(lstat(parent,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){close(input);return NULL;}snprintf(staging,sizeof staging,"%s/staging-XXXXXX",parent);if(!mkdtemp(staging)){close(input);return NULL;}int dest=open(staging,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);scan_state state={json_object_new_array(),NULL,0,0};int ok=dest>=0&&scan(&state,input,dest,"",0)&&state.manifest;close(input);if(dest>=0&&fsync(dest))ok=0;
 size_t count=json_object_array_length(state.files);J **rows=calloc(count?count:1,sizeof *rows);if(!rows)ok=0;J *sorted=json_object_new_array();if(rows){for(size_t i=0;i<count;i++)rows[i]=json_object_array_get_idx(state.files,i);qsort(rows,count,sizeof *rows,compare);for(size_t i=0;i<count;i++)json_object_array_add(sorted,json_object_get(rows[i]));free(rows);}json_object_put(state.files);state.files=sorted;
 char digest[65];const char *raw=json_object_to_json_string_ext(state.files,JSON_C_TO_STRING_PLAIN);dc_sha256(raw,strlen(raw),digest);snprintf(final,sizeof final,"%s/%s",parent,digest);
 if(ok){if(!rename(staging,final)){int p=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(p<0||fsync(p))ok=0;if(p>=0)close(p);}else if(errno!=EEXIST&&errno!=ENOTEMPTY)ok=0;else ok=dc_package_verify(final,state.files);}
 if(dest>=0){if(access(staging,F_OK)==0){clear(dest);rmdir(staging);}close(dest);}J *out=NULL;if(ok){out=json_object_new_object();json_object_object_add(out,"path",json_object_new_string(final));json_object_object_add(out,"digest",json_object_new_string(digest));json_object_object_add(out,"manifest",json_object_get(state.manifest));json_object_object_add(out,"files",json_object_get(state.files));}json_object_put(state.files);json_object_put(state.manifest);return out;
}

int dc_package_collect(dc_store *s,const char *directory){
 char base[PATH_MAX];int n=snprintf(base,sizeof base,"%s/extension-store/",s->directory);if(n<0||n>=(int)sizeof base||!directory||strncmp(directory,base,(size_t)n))return 0;
 const char *digest=directory+n;if(strlen(digest)!=64||strspn(digest,"0123456789abcdef")!=64)return 0;
 sqlite3_stmt *q=NULL;const char *query="SELECT 1 WHERE EXISTS(SELECT 1 FROM extension_versions WHERE path=?) OR EXISTS(SELECT 1 FROM catalog_packages WHERE path=?)";
 if(sqlite3_prepare_v2(s->db,query,-1,&q,NULL)!=SQLITE_OK)return 0;sqlite3_bind_text(q,1,directory,-1,SQLITE_TRANSIENT);sqlite3_bind_text(q,2,directory,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(q);sqlite3_finalize(q);if(rc==SQLITE_ROW)return 1;if(rc!=SQLITE_DONE)return 0;
 char parent[PATH_MAX];memcpy(parent,base,(size_t)n);parent[n-1]=0;int fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return errno==ENOENT;struct stat st;if(fstat(fd,&st)||st.st_uid!=geteuid()||(st.st_mode&077)){close(fd);return 0;}
 int child=openat(fd,digest,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(child<0){int absent=errno==ENOENT;close(fd);return absent;}clear(child);close(child);int ok=unlinkat(fd,digest,AT_REMOVEDIR)==0;if(ok)ok=fsync(fd)==0;close(fd);return ok;
}
