#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "dunlin/terminal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#ifdef __APPLE__
#include <spawn.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#ifdef __APPLE__
#include <util.h>
#include <libproc.h>
#else
#include <pty.h>
#include <dirent.h>
#endif
#endif
#define TERMINAL_COUNT 8
#define OUTPUT_LIMIT (4u*1024u*1024u)
typedef struct terminal {
 char owner[161],chat[65],content[257],lease[129];dl_terminal_info info;
 unsigned char *output;size_t capacity;
#ifndef _WIN32
 int fd;pid_t pid;
#endif
} terminal;
struct dl_terminal_pool {terminal slots[TERMINAL_COUNT];uint64_t next;char nonce[33];};
static int bounded(const char *s,size_t n){return s&&*s&&strlen(s)<n;}
static terminal *find(dl_terminal_pool *p,const char *owner,const char *chat,const char *id){
 if(!p||!owner||!chat||!id)return NULL;
 for(size_t i=0;i<TERMINAL_COUNT;i++){terminal *t=p->slots+i;if(*t->owner&&!strcmp(t->owner,owner)&&!strcmp(t->chat,chat)&&!strcmp(t->info.id,id))return t;}return NULL;
}
#ifndef _WIN32
static void signal_session(pid_t session,int sig){
 /* Include interactive shell job-control groups, never unrelated sessions. */
#ifdef __APPLE__
 int count=proc_listallpids(NULL,0);if(count>0&&count<1000000){size_t capacity=(size_t)count+1024;pid_t *pids=calloc(capacity,sizeof *pids);if(pids){int n=proc_listallpids(pids,(int)(capacity*sizeof *pids));for(int i=0;i<n&&i<(int)capacity;i++)if(pids[i]>0&&getsid(pids[i])==session)kill(pids[i],sig);free(pids);}}
#else
 DIR *dir=opendir("/proc");if(dir){struct dirent *entry;while((entry=readdir(dir))){char *end;long pid=strtol(entry->d_name,&end,10);if(*entry->d_name&&!*end&&pid>0&&pid<=INT_MAX&&getsid((pid_t)pid)==session)kill((pid_t)pid,sig);}closedir(dir);}
#endif
 kill(-session,sig);
}
static void terminate(terminal *t){
 if(t->pid>0){siginfo_t initial={0};int owned=waitid(P_PID,(id_t)t->pid,&initial,WEXITED|WNOHANG|WNOWAIT)==0;if(owned){signal_session(t->pid,SIGTERM);struct timespec delay={0,10000000};int status;siginfo_t observed={0};
  for(int i=0;i<10;i++){if(waitid(P_PID,(id_t)t->pid,&observed,WEXITED|WNOHANG|WNOWAIT)<0||observed.si_pid)break;nanosleep(&delay,NULL);}signal_session(t->pid,SIGKILL);kill(t->pid,SIGKILL);
  pid_t r=0;for(int i=0;i<50;i++){r=waitpid(t->pid,&status,WNOHANG);if(r>0||(r<0&&errno!=EINTR))break;nanosleep(&delay,NULL);}if(r>0||(r<0&&errno==ECHILD))t->pid=0;else snprintf(t->info.error,sizeof t->info.error,"terminal_process_cleanup_pending");}else t->pid=0;
 }
 if(t->fd>=0){close(t->fd);t->fd=-1;}t->info.running=0;
}
static void collect(terminal *t){
 if(!*t->owner)return;
 if(t->fd>=0){unsigned char bytes[32768];for(int i=0;i<128;i++){
   ssize_t n=read(t->fd,bytes,sizeof bytes);if(n<=0){if(n<0&&errno==EINTR){i--;continue;}break;}
   if((size_t)n>OUTPUT_LIMIT-t->info.bytes){snprintf(t->info.error,sizeof t->info.error,"terminal_output_limit");terminate(t);break;}
   size_t wanted=t->info.bytes+(size_t)n;if(wanted>t->capacity){size_t capacity=t->capacity?t->capacity*2:65536;if(capacity<wanted)capacity=wanted;if(capacity>OUTPUT_LIMIT)capacity=OUTPUT_LIMIT;void *next=realloc(t->output,capacity);if(!next){snprintf(t->info.error,sizeof t->info.error,"terminal_output_storage_failed");terminate(t);break;}t->output=next;t->capacity=capacity;}
   memcpy(t->output+t->info.bytes,bytes,(size_t)n);t->info.bytes=wanted;
  }}
 if(t->pid>0){int status;siginfo_t observed={0};int rc=waitid(P_PID,(id_t)t->pid,&observed,WEXITED|WNOHANG|WNOWAIT);if(rc==0&&observed.si_pid){/* The zombie pins its group id until cleanup finishes. */signal_session(t->pid,SIGKILL);pid_t r=waitpid(t->pid,&status,WNOHANG);if(r>0)t->pid=0;t->info.running=0;t->info.exit_code=r>0?(WIFEXITED(status)?WEXITSTATUS(status):WIFSIGNALED(status)?128+WTERMSIG(status):-1):-1;}else if(rc<0&&errno==ECHILD){t->pid=0;t->info.running=0;t->info.exit_code=-1;}}
}
#else
static void terminate(terminal *t){t->info.running=0;}
static void collect(terminal *t){(void)t;}
#endif
static int clear(terminal *t){terminate(t);
#ifndef _WIN32
 if(t->pid>0)return 0;
#endif
 free(t->output);memset(t,0,sizeof *t);
#ifndef _WIN32
 t->fd=-1;
#endif
 return 1;
}
dl_terminal_pool *dl_terminal_pool_open(void){dl_terminal_pool *p=calloc(1,sizeof *p);if(p){
#ifndef _WIN32
 unsigned char random[16];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);size_t used=0;while(fd>=0&&used<sizeof random){ssize_t n=read(fd,random+used,sizeof random-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}if(fd>=0)close(fd);if(used!=sizeof random){free(p);return NULL;}for(size_t i=0;i<sizeof random;i++)snprintf(p->nonce+i*2,3,"%02x",random[i]);
 for(size_t i=0;i<TERMINAL_COUNT;i++)p->slots[i].fd=-1;
#else
 free(p);return NULL;
#endif
 }return p;}
void dl_terminal_pool_close(dl_terminal_pool *p){if(!p)return;for(size_t i=0;i<TERMINAL_COUNT;i++)if(*p->slots[i].owner){if(!clear(p->slots+i))free(p->slots[i].output);}free(p);}
int dl_terminal_open(dl_terminal_pool *p,const char *owner,const char *chat,const char *content,const char *cwd,const char *shell,int cols,int rows,dl_terminal_info *info){
 if(!p||!info||!bounded(owner,161)||!bounded(chat,65)||!bounded(content,257)||!bounded(cwd,4096)||!bounded(shell,4096)||cwd[0]!='/'||shell[0]!='/'||cols<1||cols>4096||rows<1||rows>4096)return 0;
 terminal *slot=NULL;
 for(size_t i=0;i<TERMINAL_COUNT;i++){terminal *t=p->slots+i;if(*t->owner&&!strcmp(t->owner,owner)&&!strcmp(t->chat,chat)&&!strcmp(t->content,content)){if(strcmp(t->info.cwd,cwd)||strcmp(t->info.shell,shell))return 0;collect(t);*info=t->info;return 1;}if(!*t->owner&&!slot)slot=t;}
 if(!slot){for(size_t i=0;i<TERMINAL_COUNT;i++){collect(p->slots+i);if(!p->slots[i].info.running&&clear(p->slots+i)){slot=p->slots+i;break;}}}if(!slot)return 0;
#ifdef _WIN32
 return 0; /* Native ConPTY provider required; no pipe masquerades as a PTY. */
#else
 if(access(shell,X_OK))return 0;int master=-1,slave=-1;struct winsize size={(unsigned short)rows,(unsigned short)cols,0,0};
 if(openpty(&master,&slave,NULL,NULL,&size))return 0;
 int flags=fcntl(master,F_GETFL,0);if(flags<0||fcntl(master,F_SETFL,flags|O_NONBLOCK)<0||fcntl(master,F_SETFD,FD_CLOEXEC)<0){close(master);close(slave);return 0;}
 extern char **environ;pid_t child=0;size_t count=0;while(environ[count]&&count<65536)count++;if(count==65536){close(master);close(slave);return 0;}
 char **env=calloc(count+2,sizeof *env);if(!env){close(master);close(slave);return 0;}size_t used=0;for(size_t i=0;i<count;i++)if(strncmp(environ[i],"TERM=",5))env[used++]=environ[i];env[used]="TERM=xterm-256color";
#ifdef __APPLE__
 char slave_path[4096];if(ttyname_r(slave,slave_path,sizeof slave_path)){free(env);close(master);close(slave);return 0;}
 posix_spawn_file_actions_t actions;posix_spawnattr_t attributes;int action_ready=posix_spawn_file_actions_init(&actions)==0,attribute_ready=posix_spawnattr_init(&attributes)==0,rc=action_ready&&attribute_ready?0:ENOMEM;
 sigset_t mask,defaults;sigemptyset(&mask);sigemptyset(&defaults);sigaddset(&defaults,SIGPIPE);sigaddset(&defaults,SIGINT);sigaddset(&defaults,SIGTERM);sigaddset(&defaults,SIGHUP);
 if(!rc)rc=posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSID|POSIX_SPAWN_CLOEXEC_DEFAULT|POSIX_SPAWN_SETSIGDEF|POSIX_SPAWN_SETSIGMASK);
 if(!rc)rc=posix_spawnattr_setsigmask(&attributes,&mask);if(!rc)rc=posix_spawnattr_setsigdefault(&attributes,&defaults);
 if(!rc)rc=posix_spawn_file_actions_addchdir_np(&actions,cwd);
 if(!rc)rc=posix_spawn_file_actions_addopen(&actions,0,slave_path,O_RDWR,0600);
 if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,0,1);if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,0,2);
 char *const argv[]={(char*)shell,"-i",NULL};if(!rc)rc=posix_spawn(&child,shell,&actions,&attributes,argv,env);
 if(action_ready)posix_spawn_file_actions_destroy(&actions);if(attribute_ready)posix_spawnattr_destroy(&attributes);if(rc){free(env);close(master);close(slave);return 0;}
#else
 long maximum=sysconf(_SC_OPEN_MAX);if(maximum<0)maximum=65536;if(maximum>INT_MAX){free(env);close(master);close(slave);return 0;}
 char *const argv[]={(char*)shell,"-i",NULL};
 child=fork();if(child<0){free(env);close(master);close(slave);return 0;}
 if(child==0){
  close(master);if(setsid()<0||ioctl(slave,TIOCSCTTY,0)<0||chdir(cwd)<0)_exit(126);
  for(int i=0;i<3;i++)if(dup2(slave,i)<0)_exit(126);
  for(int i=3;i<maximum;i++)close(i);
  signal(SIGPIPE,SIG_DFL);signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGHUP,SIG_DFL);
  execve(shell,argv,env);_exit(127);
 }
#endif
 free(env);close(slave);slot->fd=master;slot->pid=child;snprintf(slot->owner,sizeof slot->owner,"%s",owner);snprintf(slot->chat,sizeof slot->chat,"%s",chat);snprintf(slot->content,sizeof slot->content,"%s",content);
 snprintf(slot->info.id,sizeof slot->info.id,"terminal-%s-%llu",p->nonce,(unsigned long long)++p->next);snprintf(slot->info.cwd,sizeof slot->info.cwd,"%s",cwd);snprintf(slot->info.shell,sizeof slot->info.shell,"%s",shell);slot->info.cols=cols;slot->info.rows=rows;slot->info.running=1;slot->info.exit_code=-1;*info=slot->info;return 1;
#endif
}
int dl_terminal_list(dl_terminal_pool *p,const char *owner,const char *chat,size_t index,dl_terminal_info *info){if(!p||!owner||!chat||!info)return 0;for(size_t i=0;i<TERMINAL_COUNT;i++){terminal *t=p->slots+i;if(*t->owner&&!strcmp(t->owner,owner)&&!strcmp(t->chat,chat)){if(index--==0){collect(t);*info=t->info;return 1;}}}return 0;}
int dl_terminal_read(dl_terminal_pool *p,const char *owner,const char *chat,const char *id,size_t offset,void *buffer,size_t capacity,size_t *length,dl_terminal_info *info){terminal *t=find(p,owner,chat,id);if(!t||!buffer||!length||!info)return 0;collect(t);if(offset>t->info.bytes)return 0;*length=t->info.bytes-offset;if(*length>capacity)*length=capacity;memcpy(buffer,t->output? t->output+offset:(const unsigned char*)"",*length);*info=t->info;return 1;}
int dl_terminal_attach(dl_terminal_pool *p,const char *owner,const char *chat,const char *id,const char *lease){terminal *t=find(p,owner,chat,id);if(!t||!bounded(lease,129))return 0;collect(t);snprintf(t->lease,sizeof t->lease,"%s",lease);return 1;}
int dl_terminal_writable(dl_terminal_pool *p,const char *owner,const char *chat,const char *id,const char *lease){terminal *t=find(p,owner,chat,id);return t&&lease&&*lease&&!strcmp(t->lease,lease)&&t->info.running;}
int dl_terminal_chat_busy(dl_terminal_pool *p,const char *chat){if(!p||!chat)return 0;for(size_t i=0;i<TERMINAL_COUNT;i++)if(!strcmp(p->slots[i].chat,chat)){collect(p->slots+i);if(p->slots[i].info.running)return 1;}return 0;}
int dl_terminal_write(dl_terminal_pool *p,const char *owner,const char *chat,const char *id,const char *lease,const void *data,size_t length,size_t *written){terminal *t=find(p,owner,chat,id);if(!t||!lease||strcmp(t->lease,lease)||!data||!length||length>16384||!written)return 0;collect(t);if(!t->info.running)return 0;
#ifndef _WIN32
 ssize_t n=write(t->fd,data,length);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)){*written=0;return 1;}if(n<=0)return 0;*written=(size_t)n;return 1;
#else
 return 0;
#endif
}
int dl_terminal_resize(dl_terminal_pool *p,const char *owner,const char *chat,const char *id,const char *lease,int cols,int rows){terminal *t=find(p,owner,chat,id);if(!t||!lease||strcmp(t->lease,lease)||cols<1||cols>4096||rows<1||rows>4096)return 0;collect(t);if(!t->info.running)return 0;
#ifndef _WIN32
 struct winsize size={(unsigned short)rows,(unsigned short)cols,0,0};if(ioctl(t->fd,TIOCSWINSZ,&size))return 0;t->info.cols=cols;t->info.rows=rows;return 1;
#else
 return 0;
#endif
}
int dl_terminal_close(dl_terminal_pool *p,const char *owner,const char *chat,const char *id){terminal *t=find(p,owner,chat,id);if(!t)return 0;return clear(t);}
int dl_terminal_close_content(dl_terminal_pool *p,const char *owner,const char *chat,const char *content){if(!p||!owner||!chat||!content)return 0;for(size_t i=0;i<TERMINAL_COUNT;i++){terminal *t=p->slots+i;if(!strcmp(t->owner,owner)&&!strcmp(t->chat,chat)&&!strcmp(t->content,content)){return clear(t);}}return 1;}
int dl_terminal_find_content(dl_terminal_pool *p,const char *owner,const char *chat,const char *content,dl_terminal_info *info){if(!p||!owner||!chat||!content||!info)return 0;for(size_t i=0;i<TERMINAL_COUNT;i++){terminal *t=p->slots+i;if(!strcmp(t->owner,owner)&&!strcmp(t->chat,chat)&&!strcmp(t->content,content)){collect(t);*info=t->info;return 1;}}return 0;}
int dl_terminal_forget_chat(dl_terminal_pool *p,const char *chat){if(!p||!chat)return 1;if(dl_terminal_chat_busy(p,chat))return 0;int ok=1;for(size_t i=0;i<TERMINAL_COUNT;i++)if(!strcmp(p->slots[i].chat,chat)&&!clear(p->slots+i))ok=0;return ok;}
void dl_terminal_detach(dl_terminal_pool *p,const char *owner,const char *chat,const char *id,const char *lease){terminal *t=find(p,owner,chat,id);if(t&&lease&&!strcmp(t->lease,lease))t->lease[0]=0;}
void dl_terminal_release_surface(dl_terminal_pool *p,const char *owner,const char *lease){if(!p||!owner||!lease)return;for(size_t i=0;i<TERMINAL_COUNT;i++){terminal *t=p->slots+i;if(!strcmp(t->owner,owner)&&!strcmp(t->lease,lease))t->lease[0]=0;}}
void dl_terminal_close_owner(dl_terminal_pool *p,const char *prefix){if(!p||!prefix)return;size_t n=strlen(prefix);for(size_t i=0;i<TERMINAL_COUNT;i++){terminal *t=p->slots+i;if(!strncmp(t->owner,prefix,n)&&(t->owner[n]==':'||t->owner[n]==0))clear(t);}}
