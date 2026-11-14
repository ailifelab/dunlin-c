#define _POSIX_C_SOURCE 200809L
#include "dunlin/plugin_port.h"
#include "dunlin/compact/json.h"
#include "../../dunlin-compact/platform/random.h"
#include <json-c/json.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
typedef struct json_object J;
#define FRAME_LIMIT 1048576
struct dc_worker {pid_t pid;int socket,diagnostic,native_socket;uint64_t seq,native_seq;unsigned native_count;dc_worker_native_fn native_fn;void *native_user;int native_active;char *native_buffer;size_t native_used;int timeout,dead;char nonce[65];J *capabilities;char *buffer;size_t used;size_t diagnostic_bytes;};
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){J *v=get(o,k);const char *text=json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;return text&&strlen(text)==(size_t)json_object_get_string_len(v)?text:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static uint64_t now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int cancelled(const dc_hooks *h){return h&&h->cancelled&&h->cancelled(h->user);}
static char *reply(int ok,const char *error,J *data,const char *effect){J *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(ok));if(error)json_object_object_add(o,"error",json_object_new_string(error));if(data)json_object_object_add(o,"data",data);json_object_object_add(o,"effectState",json_object_new_string(effect));char *raw=strdup(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));json_object_put(o);return raw;}
static void stop(dc_worker *w){
 if(!w||w->dead)return;w->dead=1;if(w->socket>=0){close(w->socket);w->socket=-1;}if(w->diagnostic>=0){close(w->diagnostic);w->diagnostic=-1;}if(w->native_socket>=0){close(w->native_socket);w->native_socket=-1;}if(w->pid<=0)return;
 kill(-w->pid,SIGTERM);int status;uint64_t until=now()+300;for(;;){pid_t r=waitpid(w->pid,&status,WNOHANG);if(r==w->pid||r<0){if(kill(-w->pid,0)==0)kill(-w->pid,SIGKILL);break;}if(now()>=until){kill(-w->pid,SIGKILL);while(waitpid(w->pid,&status,0)<0&&errno==EINTR){}break;}struct timespec pause={0,10000000};nanosleep(&pause,NULL);}w->pid=0;
}
#include "native_channel.inc"
static const char *wait_io(dc_worker *w,short events,uint64_t deadline,const dc_hooks *hooks){
 for(;;){if(cancelled(hooks))return "cancelled";uint64_t time=now();if(time>=deadline)return "timeout";struct pollfd fds[3]={{w->socket,events,0},{w->diagnostic,POLLIN,0},{w->native_socket,POLLIN,0}};int n=poll(fds,3,50);if(n<0&&errno==EINTR)continue;if(n<0)return "worker_poll_failed";
  if(fds[1].revents&(POLLIN|POLLHUP)){char diagnostic[4096];ssize_t bytes=read(w->diagnostic,diagnostic,sizeof diagnostic);if(bytes>0){w->diagnostic_bytes+=(size_t)bytes;if(w->diagnostic_bytes>262144)return "worker_diagnostic_flood";}else if(!bytes){close(w->diagnostic);w->diagnostic=-1;}}
  if(fds[2].revents&POLLIN){const char *error=native_read(w,deadline,hooks);if(error)return error;}else if(fds[2].revents&(POLLHUP|POLLERR|POLLNVAL))return "native_channel_disconnected";
  if(fds[0].revents&events)return NULL;if(fds[0].revents&(POLLHUP|POLLERR|POLLNVAL))return "worker_disconnected";
 }
}
static const char *send_frame(dc_worker *w,const char *p,size_t n,uint64_t deadline,const dc_hooks *hooks){
 for(size_t used=0;used<n;){const char *error=wait_io(w,POLLOUT,deadline,hooks);if(error)return error;
#ifdef MSG_NOSIGNAL
  ssize_t bytes=send(w->socket,p+used,n-used,MSG_NOSIGNAL);
#else
  ssize_t bytes=send(w->socket,p+used,n-used,0);
#endif
  if(bytes<0&&(errno==EAGAIN||errno==EINTR))continue;if(bytes<=0)return "worker_write_failed";used+=(size_t)bytes;
 }return NULL;
}
static J *read_frame(dc_worker *w,uint64_t deadline,const dc_hooks *hooks,const char **error){
 for(;;){char *newline=memchr(w->buffer,'\n',w->used);if(newline){size_t length=(size_t)(newline-w->buffer);J *out=dunlin_json_parse(w->buffer,length);size_t consumed=length+1;memmove(w->buffer,w->buffer+consumed,w->used-consumed);w->used-=consumed;if(!out)*error="worker_invalid_json";return out;}
  if(w->used>=FRAME_LIMIT){*error="worker_frame_exceeds_limit";return NULL;}*error=wait_io(w,POLLIN,deadline,hooks);if(*error)return NULL;ssize_t n=read(w->socket,w->buffer+w->used,FRAME_LIMIT-w->used);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;if(n<=0){*error="worker_disconnected";return NULL;}w->used+=(size_t)n;
 }
}
static int safe_string(const char *s,size_t cap){return s&&strlen(s)<=cap&&!strpbrk(s,"\r\n");}
dc_worker *dc_worker_open(const char *raw,const dc_hooks *hooks,char **failure){
 if(failure)*failure=NULL;const char *error="invalid_worker_configuration";J *c=raw&&strlen(raw)<=65536?dunlin_json_parse(raw,strlen(raw)):NULL,*args=get(c,"args");const char *executable=str(c,"executable"),*home=str(c,"home");dc_worker *w=NULL;char **argv=NULL;int sockets[2]={-1,-1},diagnostic[2]={-1,-1},native[2]={-1,-1};int native_enabled=0;
 if(!safe_string(executable,PATH_MAX-1)||executable[0]!='/'||!safe_string(home,PATH_MAX-1)||home[0]!='/'||!json_object_is_type(args,json_type_array)||json_object_array_length(args)>64)goto done;
 struct stat st;if(stat(executable,&st)||!S_ISREG(st.st_mode)||access(executable,X_OK)||lstat(home,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))goto done;
 J *native_option=get(c,"nativeBridge");if(native_option&&!json_object_is_type(native_option,json_type_boolean))goto done;native_enabled=json_object_get_boolean(native_option);
 int timeout=json_object_get_int(get(c,"timeoutMs"));if(timeout<=0)timeout=15000;if(timeout>120000)goto done;
 w=calloc(1,sizeof *w);if(!w){error="out_of_memory";goto done;}w->socket=-1;w->diagnostic=-1;w->native_socket=-1;w->timeout=timeout;w->buffer=malloc(FRAME_LIMIT);unsigned char entropy[32];if(!w->buffer||!ah_os_random(entropy,sizeof entropy)){error="worker_entropy_or_allocation_failed";goto done;}for(size_t i=0;i<32;i++)snprintf(w->nonce+i*2,3,"%02x",entropy[i]);
 size_t count=json_object_array_length(args);argv=calloc(count+2,sizeof *argv);if(!argv){error="out_of_memory";goto done;}argv[0]=(char*)executable;size_t total=strlen(executable);for(size_t i=0;i<count;i++){J *arg=json_object_array_get_idx(args,i);const char *value=json_object_is_type(arg,json_type_string)?json_object_get_string(arg):NULL;if(!safe_string(value,8192)||(total+=strlen(value))>32768)goto done;argv[i+1]=(char*)value;}
 if(native_enabled){w->native_buffer=malloc(FRAME_LIMIT);if(!w->native_buffer){error="out_of_memory";goto done;}}
 if(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)||pipe(diagnostic)||(native_enabled&&socketpair(AF_UNIX,SOCK_STREAM,0,native))){error="worker_pipe_failed";goto done;}for(int i=0;i<2;i++){fcntl(sockets[i],F_SETFD,FD_CLOEXEC);fcntl(diagnostic[i],F_SETFD,FD_CLOEXEC);}
 if(native_enabled){int *fds[]={&sockets[0],&sockets[1],&diagnostic[0],&diagnostic[1],&native[0],&native[1]};for(size_t i=0;i<6;i++){int next=fcntl(*fds[i],F_DUPFD_CLOEXEC,10);if(next<0){error="worker_native_pipe_failed";goto done;}close(*fds[i]);*fds[i]=next;}}
#ifdef SO_NOSIGPIPE
 {int enabled=1;setsockopt(sockets[0],SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof enabled);if(native_enabled)setsockopt(native[0],SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof enabled);}
#endif
 char private_home[PATH_MAX+6],temp[PATH_MAX+8],nonce[100];snprintf(private_home,sizeof private_home,"HOME=%s",home);snprintf(temp,sizeof temp,"TMPDIR=%s",home);snprintf(nonce,sizeof nonce,"DUNLIN_WORKER_NONCE=%s",w->nonce);char *env[]={private_home,temp,nonce,"PATH=/usr/bin:/bin","LANG=en_US.UTF-8",native_enabled?"DUNLIN_NATIVE_FD=3":NULL,NULL};
 posix_spawn_file_actions_t actions;posix_spawnattr_t attr;int actions_ready=0,attr_ready=0;
 if(posix_spawn_file_actions_init(&actions)){error="worker_spawn_setup_failed";goto done;}actions_ready=1;if(posix_spawnattr_init(&attr)){posix_spawn_file_actions_destroy(&actions);error="worker_spawn_setup_failed";goto done;}attr_ready=1;
 int setup=posix_spawn_file_actions_adddup2(&actions,sockets[1],STDIN_FILENO)|posix_spawn_file_actions_adddup2(&actions,sockets[1],STDOUT_FILENO)|posix_spawn_file_actions_adddup2(&actions,diagnostic[1],STDERR_FILENO)|posix_spawn_file_actions_addclose(&actions,sockets[0])|posix_spawn_file_actions_addclose(&actions,diagnostic[0])|posix_spawn_file_actions_addclose(&actions,sockets[1])|posix_spawn_file_actions_addclose(&actions,diagnostic[1]);
 if(native_enabled)setup|=posix_spawn_file_actions_adddup2(&actions,native[1],3)|posix_spawn_file_actions_addclose(&actions,native[0])|posix_spawn_file_actions_addclose(&actions,native[1]);
 short flags=POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
 flags|=POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
 sigset_t mask,defaults;sigemptyset(&mask);sigemptyset(&defaults);sigaddset(&defaults,SIGPIPE);setup|=posix_spawnattr_setsigmask(&attr,&mask)|posix_spawnattr_setsigdefault(&attr,&defaults)|posix_spawnattr_setpgroup(&attr,0)|posix_spawnattr_setflags(&attr,flags);
 int spawned=setup?setup:posix_spawn(&w->pid,executable,&actions,&attr,argv,env);if(actions_ready)posix_spawn_file_actions_destroy(&actions);if(attr_ready)posix_spawnattr_destroy(&attr);if(spawned){w->pid=0;error="worker_spawn_failed";goto done;}
 close(sockets[1]);sockets[1]=-1;close(diagnostic[1]);diagnostic[1]=-1;w->socket=sockets[0];sockets[0]=-1;w->diagnostic=diagnostic[0];diagnostic[0]=-1;fcntl(w->socket,F_SETFL,O_NONBLOCK);fcntl(w->diagnostic,F_SETFL,O_NONBLOCK);
 if(native_enabled){close(native[1]);native[1]=-1;w->native_socket=native[0];native[0]=-1;fcntl(w->native_socket,F_SETFL,O_NONBLOCK);}
 error=NULL;w->capabilities=read_frame(w,now()+(uint64_t)timeout,hooks,&error);J *methods=get(w->capabilities,"methods");if(!error&&(!eq(str(w->capabilities,"protocol"),"dunlin.worker")||!eq(str(w->capabilities,"nonce"),w->nonce)||!json_object_is_type(get(w->capabilities,"major"),json_type_int)||json_object_get_int(get(w->capabilities,"major"))!=1||!json_object_is_type(methods,json_type_array)||json_object_array_length(methods)>64))error="worker_handshake_mismatch";
 for(size_t i=0;!error&&i<json_object_array_length(methods);i++){J *declaration=json_object_array_get_idx(methods,i);const char *method=json_object_is_type(declaration,json_type_string)?json_object_get_string(declaration):NULL;if(!safe_string(method,128)||!*method)error="worker_invalid_method_declaration";}
 done:json_object_put(c);free(argv);for(int i=0;i<2;i++){if(sockets[i]>=0)close(sockets[i]);if(diagnostic[i]>=0)close(diagnostic[i]);if(native[i]>=0)close(native[i]);}if(error){if(failure)*failure=strdup(error);dc_worker_close(w);return NULL;}return w;
}
int dc_worker_alive(dc_worker *w){
 if(!w||w->dead||w->pid<=0)return 0;int status;pid_t r=waitpid(w->pid,&status,WNOHANG);if(r==0)return 1;if(r<0&&errno==EINTR)return 1;kill(-w->pid,SIGKILL);w->pid=0;stop(w);return 0;
}
char *dc_worker_capabilities(dc_worker *w){return w&&w->capabilities?strdup(json_object_to_json_string_ext(w->capabilities,JSON_C_TO_STRING_PLAIN)):NULL;}
static char *call_inner(dc_worker *w,const char *method,const char *params,const dc_hooks *hooks){
 if(!w||w->dead)return reply(0,"worker_unavailable",NULL,"not_started");if(!method||!params||strlen(params)>FRAME_LIMIT-512)return reply(0,"invalid_worker_request",NULL,"not_started");J *methods=get(w->capabilities,"methods");int allowed=0;for(size_t i=0;i<json_object_array_length(methods);i++)if(eq(json_object_get_string(json_object_array_get_idx(methods,i)),method))allowed=1;if(!allowed)return reply(0,"worker_method_not_declared",NULL,"not_started");J *p=dunlin_json_parse(params,strlen(params));if(!p)return reply(0,"invalid_worker_params",NULL,"not_started");
 J *request=json_object_new_object();uint64_t id=++w->seq;if(id>INT64_MAX){json_object_put(p);json_object_put(request);return reply(0,"worker_sequence_exhausted",NULL,"not_started");}json_object_object_add(request,"jsonrpc",json_object_new_string("2.0"));json_object_object_add(request,"id",json_object_new_int64((int64_t)id));json_object_object_add(request,"method",json_object_new_string(method));json_object_object_add(request,"params",p);const char *raw=json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN);uint64_t deadline=now()+(uint64_t)w->timeout;
 const char *error=cancelled(hooks)?"cancelled":NULL;int sent=0;if(!error){sent=1;error=send_frame(w,raw,strlen(raw),deadline,hooks);if(!error)error=send_frame(w,"\n",1,deadline,hooks);}json_object_put(request);if(error){stop(w);return reply(0,error,NULL,sent?"unknown":"not_started");}
 J *response=read_frame(w,deadline,hooks,&error);if(!error&&(!eq(str(response,"jsonrpc"),"2.0")||!json_object_is_type(get(response,"id"),json_type_int)||json_object_get_int64(get(response,"id"))!=(int64_t)id||json_object_object_get_ex(response,"result",NULL)==json_object_object_get_ex(response,"error",NULL)))error="worker_response_mismatch";
 if(error){json_object_put(response);stop(w);return reply(0,error,NULL,"unknown");}J *result=get(response,"result"),*failure=get(response,"error");char *out=failure?reply(0,"worker_method_error",json_object_get(failure),"reported_failure"):reply(1,NULL,json_object_get(result),"reported");json_object_put(response);return out;
}
void dc_worker_close(dc_worker *w){if(!w)return;stop(w);json_object_put(w->capabilities);free(w->buffer);free(w->native_buffer);free(w);}

char *dc_worker_call_native(dc_worker *w,const char *method,const char *params,const dc_hooks *hooks,dc_worker_native_fn callback,void *user){
 if(!w)return reply(0,"worker_unavailable",NULL,"not_started");if(w->native_active)return reply(0,"recursive_worker_call_refused",NULL,"not_started");w->native_active=1;w->native_count=0;w->native_fn=callback;w->native_user=user;char *out=call_inner(w,method,params,hooks);w->native_fn=NULL;w->native_user=NULL;w->native_active=0;return out;
}
char *dc_worker_call(dc_worker *w,const char *method,const char *params,const dc_hooks *hooks){return dc_worker_call_native(w,method,params,hooks,NULL,NULL);}
