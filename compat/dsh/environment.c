#define _XOPEN_SOURCE 700
#include "environment.h"
#include "linux_runtime.h"
#include "dunlin/content_hash.h"
#include "dunlin/compact/json.h"
#include <openssl/evp.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <zlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <time.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int stopped(const dc_hooks *h){return h&&h->cancelled&&h->cancelled(h->user);}
static void event(const dc_hooks *h,const char *phase){if(h&&h->event)h->event(h->user,"compat.phase",phase);}
static int private_dir(const char *p){if(mkdir(p,0700)&&errno!=EEXIST)return 0;struct stat st;return !lstat(p,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&077);}
static char *read_file(const char *p,size_t cap,size_t *n){int fd=open(p,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;char *data=NULL;if(fd>=0&&!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_nlink==1&&st.st_size>=0&&(uint64_t)st.st_size<=cap){size_t len=(size_t)st.st_size;data=malloc(len+1);size_t used=0;while(data&&used<len){ssize_t k=read(fd,data+used,len-used);if(k<0&&errno==EINTR)continue;if(k<=0){free(data);data=NULL;break;}used+=(size_t)k;}if(data){data[len]=0;if(n)*n=len;}}if(fd>=0)close(fd);return data;}
static int asset_path(const char *root,const char *relative,char out[PATH_MAX]){
 if(!relative||!*relative||relative[0]=='/'||strchr(relative,'\\')||strstr(relative,"../")||strstr(relative,"/..")||!strcmp(relative,".."))return 0;char candidate[PATH_MAX],canonical[PATH_MAX];if(snprintf(candidate,sizeof candidate,"%s/%s",root,relative)>=(int)sizeof candidate||!realpath(candidate,canonical))return 0;size_t n=strlen(root);if(strncmp(root,canonical,n)||canonical[n]!='/')return 0;snprintf(out,PATH_MAX,"%s",canonical);return 1;
}
static int verify_assets(const char *root){
 char path[PATH_MAX];if(!asset_path(root,"assets.json",path))return 0;size_t n=0;char *raw=read_file(path,4194304,&n);J *manifest=raw?dunlin_json_parse(raw,n):NULL;free(raw);J *files=get(manifest,"files");int ok=eq(str(manifest,"protocol"),"dunlin.dsh.assets")&&eq(str(manifest,"commit"),"1f81c047fc48fa190a683e469daa699857e9afe8")&&json_object_is_type(files,json_type_array)&&json_object_array_length(files)>10&&json_object_array_length(files)<=16384;
 for(size_t i=0;ok&&i<json_object_array_length(files);i++){J *f=json_object_array_get_idx(files,i);size_t size=0;char *bytes=asset_path(root,str(f,"path"),path)?read_file(path,67108864,&size):NULL;char hash[65];if(bytes)dc_sha256(bytes,size,hash);ok=bytes&&eq(hash,str(f,"sha256"));free(bytes);}json_object_put(manifest);return ok;
}
static uint64_t milliseconds(void){
 struct timespec now={0};clock_gettime(CLOCK_MONOTONIC,&now);
 return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
}
typedef struct{int fd;size_t bytes;EVP_MD_CTX *hash;const dc_hooks *hooks;int percent;uint64_t deadline;} download;
static size_t receive(char *p,size_t a,size_t b,void *ctx){download *d=ctx;if(a&&b>SIZE_MAX/a)return 0;size_t n=a*b;if(n>67108864-d->bytes||stopped(d->hooks))return 0;for(size_t used=0;used<n;){ssize_t k=write(d->fd,p+used,n-used);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;used+=(size_t)k;}if(EVP_DigestUpdate(d->hash,p,n)!=1)return 0;d->bytes+=n;return n;}
static int progress(void *ctx,curl_off_t total,curl_off_t current,curl_off_t sent_total,curl_off_t sent){
 (void)sent_total;(void)sent;
 download *d=ctx;
 if(total>0&&current>=0){
  int percent=(int)((double)current*100.0/(double)total);
  if(percent!=d->percent){
   d->percent=percent;
   if(d->hooks&&d->hooks->event){
    char text[160];snprintf(text,sizeof text,"{\"received\":%lld,\"total\":%lld}",(long long)current,(long long)total);
    uint64_t before=milliseconds();
    d->hooks->event(d->hooks->user,"compat.progress",text);
    /* The UI may hold this synchronous callback until the user resumes. */
    d->deadline+=milliseconds()-before;
   }
  }
 }
 return stopped(d->hooks)||milliseconds()>=d->deadline;
}
static int fetch(const char *url,const char *sha,const char *destination,const dc_hooks *hooks){
 int fd=open(destination,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return 0;CURL *c=curl_easy_init();EVP_MD_CTX *hash=EVP_MD_CTX_new();int ok=c&&hash&&EVP_DigestInit_ex(hash,EVP_sha256(),NULL)==1;download d={fd,0,hash,hooks,-1,milliseconds()+300000};
#define SET(k,v) do{if(ok&&curl_easy_setopt(c,k,v)!=CURLE_OK)ok=0;}while(0)
 SET(CURLOPT_URL,url);SET(CURLOPT_FOLLOWLOCATION,0L);SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_SSL_VERIFYHOST,2L);SET(CURLOPT_NOSIGNAL,1L);SET(CURLOPT_CONNECTTIMEOUT_MS,10000L);SET(CURLOPT_WRITEFUNCTION,receive);SET(CURLOPT_WRITEDATA,&d);SET(CURLOPT_NOPROGRESS,0L);SET(CURLOPT_XFERINFOFUNCTION,progress);SET(CURLOPT_XFERINFODATA,&d);
#if LIBCURL_VERSION_NUM >= 0x075500
 SET(CURLOPT_PROTOCOLS_STR,"https");
#else
 SET(CURLOPT_PROTOCOLS,(long)CURLPROTO_HTTPS);
#endif
 if(ok)ok=curl_easy_perform(c)==CURLE_OK;long status=0;if(c)curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);unsigned char digest[32];unsigned length=0;char hex[65];if(ok)ok=status==200&&EVP_DigestFinal_ex(hash,digest,&length)==1&&length==32;for(size_t i=0;ok&&i<32;i++)snprintf(hex+i*2,3,"%02x",digest[i]);if(ok)ok=eq(hex,sha)&&!fsync(fd);if(close(fd))ok=0;EVP_MD_CTX_free(hash);if(c)curl_easy_cleanup(c);if(!ok)unlink(destination);return ok;
#undef SET
}
static int exact(gzFile archive,void *bytes,size_t n){size_t used=0;while(used<n){int k=gzread(archive,(char*)bytes+used,(unsigned)(n-used));if(k<=0)return 0;used+=(size_t)k;}return 1;}
static int octal(const unsigned char *p,size_t n,uint64_t *value){uint64_t v=0;size_t i=0;while(i<n&&p[i]==' ')i++;int digits=0;for(;i<n&&p[i]&&p[i]!=' ';i++){if(p[i]<'0'||p[i]>'7'||v>(UINT64_MAX-7)/8)return 0;v=v*8+(p[i]-'0');digits=1;}*value=v;return digits;}
static int extract_node(const char *archive_path,const char *expected,const char *destination,const dc_hooks *hooks){
 gzFile a=gzopen(archive_path,"rb");if(!a)return 0;unsigned char header[512],buffer[65536];int found=0,ok=1,fd=-1;uint64_t total=0;
 while(ok&&!stopped(hooks)){if(!exact(a,header,sizeof header)){ok=0;break;}int empty=1;for(size_t i=0;i<512;i++)if(header[i]){empty=0;break;}if(empty)break;uint64_t size=0,checksum=0,sum=0;if(!octal(header+124,12,&size)||size>268435456||!octal(header+148,8,&checksum)){ok=0;break;}for(size_t i=0;i<512;i++)sum+=i>=148&&i<156?' ':header[i];if(sum!=checksum){ok=0;break;}
  char name[257];size_t name_len=strnlen((const char*)header,100),prefix_len=strnlen((const char*)header+345,155);if(prefix_len)snprintf(name,sizeof name,"%.*s/%.*s",(int)prefix_len,header+345,(int)name_len,header);else snprintf(name,sizeof name,"%.*s",(int)name_len,header);
  int selected=!strcmp(name,expected);if(selected){if(found||(header[156]!=0&&header[156]!='0')){ok=0;break;}fd=open(destination,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0700);if(fd<0){ok=0;break;}found=1;}
  uint64_t blocks=(size+511)/512*512;if(blocks>536870912-total){ok=0;break;}total+=blocks;uint64_t remain=blocks,content=size;
  while(ok&&remain){size_t bytes=remain>sizeof buffer?sizeof buffer:(size_t)remain;if(!exact(a,buffer,bytes)){ok=0;break;}size_t write_bytes=content>bytes?bytes:(size_t)content;if(selected){for(size_t used=0;used<write_bytes;){ssize_t n=write(fd,buffer+used,write_bytes-used);if(n<0&&errno==EINTR)continue;if(n<=0){ok=0;break;}used+=(size_t)n;}}content-=write_bytes;remain-=bytes;}
  if(fd>=0){if(fsync(fd))ok=0;if(close(fd))ok=0;fd=-1;}
 }
 if(fd>=0)close(fd);if(gzclose(a)!=Z_OK)ok=0;if(!ok||!found||stopped(hooks)){unlink(destination);return 0;}return 1;
}
static char *configuration(const char *node,const char *home,const char *assets,int probe,const dc_dsh_node_abi *abi){
 J *c=json_object_new_object(),*args=json_object_new_array();json_object_object_add(c,"executable",json_object_new_string(abi->loader[0]?abi->loader:node));json_object_object_add(c,"home",json_object_new_string(home));json_object_object_add(c,"timeoutMs",json_object_new_int(15000));
 if(abi->loader[0]){
  json_object_array_add(args,json_object_new_string("--argv0"));
  json_object_array_add(args,json_object_new_string(abi->launcher));
  json_object_array_add(args,json_object_new_string("--library-path"));
  json_object_array_add(args,json_object_new_string(abi->directory));
  json_object_array_add(args,json_object_new_string(node));
  json_object_array_add(args,json_object_new_string("--require"));
  json_object_array_add(args,json_object_new_string(abi->preload));
 }
 if(probe){json_object_array_add(args,json_object_new_string("-e"));json_object_array_add(args,json_object_new_string("process.stdout.write(JSON.stringify({protocol:'dunlin.worker',major:1,nonce:process.env.DUNLIN_WORKER_NONCE,methods:[],runtime:{node:process.versions.node}})+'\\n');process.stdin.resume()"));}
 else {json_object_object_add(c,"nativeBridge",json_object_new_boolean(1));char host[PATH_MAX],compat[PATH_MAX],toolchain[PATH_MAX];snprintf(host,sizeof host,"%s/hosts/node-dsh/worker.mjs",assets);snprintf(compat,sizeof compat,"%s/compat/dsh",assets);snprintf(toolchain,sizeof toolchain,"%s/toolchain",assets);json_object_array_add(args,json_object_new_string(host));json_object_array_add(args,json_object_new_string(compat));json_object_array_add(args,json_object_new_string(toolchain));}
 json_object_object_add(c,"args",args);char *raw=strdup(json_object_to_json_string_ext(c,JSON_C_TO_STRING_PLAIN));json_object_put(c);return raw;
}
static dc_worker *prepare(const char *directory,const char *assets,const char *selected,int upgrade,int detect_only,const dc_hooks *hooks,char **failure,char **actual){
 if(failure)*failure=NULL;if(actual)*actual=NULL;const char *error="compatibility_assets_invalid";char asset_root[PATH_MAX],home[PATH_MAX],node[PATH_MAX],runtime[PATH_MAX],staged[PATH_MAX]={0};dc_worker *worker=NULL;
 if(!assets||!realpath(assets,asset_root)||!verify_assets(asset_root))goto done;
 snprintf(home,sizeof home,"%s/Compatibility",directory);if(!private_dir(home)){error="compatibility_directory_unavailable";goto done;}snprintf(home,sizeof home,"%s/Compatibility/dsh",directory);if(!private_dir(home)){error="compatibility_directory_unavailable";goto done;}
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
 const char *platform="darwin-arm64",*sha="d38bd5e5a2ddce6bbb5ce45e46e208337da4d51fffe611e567fd32073d6439f6";
#elif defined(__linux__) && defined(__aarch64__)
 const char *platform="linux-arm64",*sha="439e9c438acdf6a6ffe47418da709153d66c4bed812e30a473faacb107dd2791";
#elif defined(__linux__) && defined(__x86_64__)
 const char *platform="linux-x64",*sha="5848553868b402d3fabb5b09270649977780a390f832155721025a08b5104455";
#else
 const char *platform=NULL,*sha=NULL;
#endif
 if(!platform){error="compatibility_platform_unverified";goto done;}
 snprintf(runtime,sizeof runtime,"%s/node-26.13.0-%s",home,platform);if(!private_dir(runtime)){error="runtime_directory_unavailable";goto done;}snprintf(node,sizeof node,"%s/node",runtime);
 const char *candidate=selected&&*selected?selected:access(node,X_OK)?NULL:node;
 if(!candidate){const char *known[]={"/opt/homebrew/bin/node","/usr/local/bin/node","/usr/bin/node"};for(size_t i=0;i<3;i++)if(!access(known[i],X_OK)){candidate=known[i];break;}}
 dc_dsh_node_abi abi={0};
 event(hooks,"probing");
#ifdef __linux__
 if(candidate){
  error=dc_dsh_linux_node_abi(candidate,home,detect_only,hooks,fetch,&abi);
  if(!error&&abi.loader[0])error=dc_dsh_linux_child_launcher(candidate,home,detect_only,&abi);
  if(error)goto done;
 }
#endif
 if(candidate){char *probe=configuration(candidate,home,asset_root,1,&abi),*detail=NULL;worker=probe?dc_worker_open(probe,hooks,&detail):NULL;free(probe);free(detail);char *cap=worker?dc_worker_capabilities(worker):NULL;J *hello=cap?dunlin_json_parse(cap,strlen(cap)):NULL;free(cap);const char *version=str(get(hello,"runtime"),"node");int supported=eq(version,"26.13.0");json_object_put(hello);dc_worker_close(worker);worker=NULL;
  if(stopped(hooks)){error="cancelled";goto done;}
  if(supported){if(!realpath(candidate,node)){error="node_unavailable";goto done;}}
  else {if(!upgrade){error="engine_upgrade_confirmation_required";goto done;}candidate=NULL;snprintf(node,sizeof node,"%s/node",runtime);}
 }
 if(stopped(hooks)){error="cancelled";goto done;}
 if(!candidate){
  if(detect_only){error="engine_download_required";goto done;}
  char archive[PATH_MAX],binary[PATH_MAX],url[256],entry[128];snprintf(archive,sizeof archive,"%s/download-XXXXXX",home);int fd=mkstemp(archive);if(fd<0){error="download_staging_failed";goto done;}close(fd);unlink(archive);
  snprintf(url,sizeof url,"https://nodejs.org/dist/v26.13.0/node-v26.13.0-%s.tar.gz",platform);event(hooks,"downloading");if(!fetch(url,sha,archive,hooks)){error=stopped(hooks)?"cancelled":"engine_download_or_digest_failed";goto done;}
  snprintf(binary,sizeof binary,"%s/node-XXXXXX",runtime);fd=mkstemp(binary);if(fd<0){unlink(archive);error="engine_staging_failed";goto done;}close(fd);unlink(binary);snprintf(entry,sizeof entry,"node-v26.13.0-%s/bin/node",platform);event(hooks,"verifying");int ok=extract_node(archive,entry,binary,hooks);unlink(archive);if(!ok){unlink(binary);error=stopped(hooks)?"cancelled":"engine_archive_invalid";goto done;}snprintf(staged,sizeof staged,"%s",binary);
 }
#ifdef __linux__
 if(staged[0]){
  memset(&abi,0,sizeof abi);
  error=dc_dsh_linux_node_abi(staged,home,detect_only,hooks,fetch,&abi);
  if(!error&&abi.loader[0])error=dc_dsh_linux_child_launcher(node,home,detect_only,&abi);
  if(error)goto done;
 }
#endif
 event(hooks,"starting");{char *config=configuration(staged[0]?staged:node,home,asset_root,0,&abi),*detail=NULL;worker=config?dc_worker_open(config,hooks,&detail):NULL;free(config);error=worker?NULL:"compatibility_handshake_failed";free(detail);if(worker){char *raw=dc_worker_capabilities(worker);J *hello=raw?dunlin_json_parse(raw,strlen(raw)):NULL;free(raw);J *runtime_info=get(hello,"runtime");if(!eq(str(runtime_info,"node"),"26.13.0")||!eq(str(runtime_info,"commit"),"1f81c047fc48fa190a683e469daa699857e9afe8")||!eq(str(runtime_info,"cordis"),"4.0.5")){error="compatibility_version_mismatch";dc_worker_close(worker);worker=NULL;}json_object_put(hello);}}
 if(worker&&staged[0]){
  if(stopped(hooks)||rename(staged,node)){
   error=stopped(hooks)?"cancelled":"engine_staging_failed";
   dc_worker_close(worker);worker=NULL;
  }else staged[0]=0;
 }
 if(worker&&actual)*actual=strdup(node);
 done:if(staged[0])unlink(staged);if(error&&failure)*failure=strdup(error);return worker;
}

dc_worker *dc_dsh_prepare(const char *directory,const char *assets,const char *selected,int upgrade,const dc_hooks *hooks,char **failure,char **actual){
 return prepare(directory,assets,selected,upgrade,0,hooks,failure,actual);
}
int dc_dsh_detect(const char *directory,const char *assets,const char *selected,const dc_hooks *hooks,char **failure,char **actual){
 dc_worker *worker=prepare(directory,assets,selected,0,1,hooks,failure,actual);
 if(!worker)return 0;
 dc_worker_close(worker);
 return 1;
}
