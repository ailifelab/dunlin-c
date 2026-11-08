#define _GNU_SOURCE
#include "dunlin/compact/tools.h"
#include "http_get.h"
#include <json-c/json.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/random.h>
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <sys/wait.h>
#include "closefrom.h"
#include <time.h>
#include <unistd.h>
#include <limits.h>

typedef struct json_object J;
typedef enum { ECHO, CALC, READ_FILE, WRITE_FILE, EXEC, HTTP_GET, SHELL, COUNT } tool_id;
typedef struct { int dir; unsigned enabled; char *child_path, *child_home; } tool_state;
static const struct {
    const char *name, *desc, *params;
    unsigned count;
} catalog[COUNT] = {
    {"echo", "Return text verbatim.", "{\"text\":{\"type\":\"string\"}}", 1},
    {"calc", "Compute a binary arithmetic operation.", "{\"op\":{\"type\":\"string\",\"enum\":[\"add\",\"sub\",\"mul\",\"div\"]},\"a\":{\"type\":\"number\"},\"b\":{\"type\":\"number\"}}", 3},
    {"read_file", "Read a UTF-8 regular file by basename in the authorized directory.", "{\"path\":{\"type\":\"string\"}}", 1},
    {"write_file", "Atomically replace a UTF-8 regular file by basename. Requires approval.", "{\"path\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}}", 2},
    {"exec", "High risk. Run an absolute executable with literal argv (excluding argv[0]) in the workspace. Requires approval.", "{\"executable\":{\"type\":\"string\"},\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}", 2},
    {"http_get", "Read bounded UTF-8 text from a public HTTPS URL. GET only; no credentials, headers, body or redirects; no approval. Use this instead of shell/curl for public web reads.", "{\"url\":{\"type\":\"string\"}}", 1},
    {"shell", "High risk. Execute the exact command string with /bin/sh -c in the workspace, after approval. "
        "For environment inventories, check each tool independently with command -v inside an if statement, "
        "and print its path or an explicit missing-from-PATH line. Missing tools are valid inventory results. "
        "Query versions only for observed executables; do not chain independent checks with &&. "
        "A pipeline reports the last command status, so exit_code=0 with empty output does not prove tools exist. "
        "After an empty inventory result, change the diagnostic command or summarize the observation; "
        "never repeat the same inventory command unchanged. Report the actual execution environment, including containers.", "{\"command\":{\"type\":\"string\"}}", 1}
};
static int find_tool(const char *name) {
    for (int i=0; i<COUNT; ++i) if (name && !strcmp(name,catalog[i].name)) return i;
    return -1;
}
static J *schemas(void *p) {
    tool_state *s = p;
    J *array = json_object_new_array();
    for (int i=0; i<COUNT; ++i) {
        if (!(s->enabled & (1u<<i))) continue;
        J *entry=json_object_new_object(), *fn=json_object_new_object(), *par=json_object_new_object();
        J *props=json_tokener_parse(catalog[i].params), *req=json_object_new_array();
        json_object_object_foreach(props, key, value) {
            (void)value;
            json_object_array_add(req,json_object_new_string(key));
        }
        json_object_object_add(par,"type",json_object_new_string("object"));
        json_object_object_add(par,"properties",props);
        json_object_object_add(par,"required",req);
        json_object_object_add(par,"additionalProperties",json_object_new_boolean(0));
        json_object_object_add(fn,"name",json_object_new_string(catalog[i].name));
        json_object_object_add(fn,"description",json_object_new_string(catalog[i].desc));
        json_object_object_add(fn,"parameters",par);
        json_object_object_add(entry,"type",json_object_new_string("function"));
        json_object_object_add(entry,"function",fn);
        json_object_array_add(array,entry);
    }
    return array;
}
static const char *string(J *obj) {
    if (!json_object_is_type(obj,json_type_string)) return NULL;
    const char *s=json_object_get_string(obj);
    return s && strlen(s)==(size_t)json_object_get_string_len(obj) ? s : NULL;
}
static J *get(J *o,const char *k) {
    J *value=NULL;
    if(json_object_is_type(o,json_type_object)) json_object_object_get_ex(o,k,&value);
    return value;
}
static const char *field(J *obj,const char *key) { return string(get(obj,key)); }
static int basename_ok(const char *path) {
    return path && *path && !strchr(path,'/') && strcmp(path,".") && strcmp(path,"..") &&
        strncmp(path,".dunlin-tmp-",10) && strlen(path)<=NAME_MAX;
}
static int number(J *obj) { return (json_object_is_type(obj,json_type_int) ||
    json_object_is_type(obj,json_type_double)) && isfinite(json_object_get_double(obj)); }
static const char *calc(J *args, double *answer) {
    J *a=get(args,"a"), *b=get(args,"b");
    const char *op=field(args,"op");
    if (!number(a)) return "calc.a must be a finite JSON number";
    if (!number(b)) return "calc.b must be a finite JSON number";
    if (!op) return "calc.op must be a string: add, sub, mul, or div";
    double x=json_object_get_double(a), y=json_object_get_double(b);
    if (!strcmp(op,"add")) *answer=x+y;
    else if (!strcmp(op,"sub")) *answer=x-y;
    else if (!strcmp(op,"mul")) *answer=x*y;
    else if (!strcmp(op,"div")) {
        if (y==0) return "calc.b must be nonzero for div";
        *answer=x/y;
    } else return "calc.op must be exactly add, sub, mul, or div (no embedded quotes)";
    return isfinite(*answer)?NULL:"calc result must be finite";
}
static const char *argument_error(int id,J *a) {
    if (!json_object_is_type(a,json_type_object)) return "arguments must be a JSON object";
    switch (id) {
    case ECHO:
        if (!field(a,"text")) return "echo.text must be a string";
        break;
    case CALC: {
        double value;
        const char *why=calc(a,&value);
        if (why) return why;
        break;
    }
    case WRITE_FILE:
        if (!field(a,"text")) return "write_file.text must be a string";
        /* fall through */
    case READ_FILE:
        if (!basename_ok(field(a,"path"))) return "path must be an authorized basename without slashes, dot paths or reserved prefixes";
        break;
    case HTTP_GET:
        if(!field(a,"url")) return "http_get.url must be a string";
        break;
    case SHELL:
        if(!field(a,"command") || !*field(a,"command")) return "shell.command must be a nonempty command string";
        break;
    case EXEC: {
        const char *exe=field(a,"executable");
        J *argv=get(a,"argv");
        if (!exe || *exe!='/') return "exec.executable must be an absolute path";
        if (!json_object_is_type(argv,json_type_array) || json_object_array_length(argv)>64)
            return "exec.argv must be an array with at most 64 strings";
        for (size_t i=0;i<(size_t)json_object_array_length(argv);i++)
            if (!string(json_object_array_get_idx(argv,i))) return "every exec.argv item must be a string";
        break;
    }
    default: return "unknown tool";
    }
    return (unsigned)json_object_object_length(a)==catalog[id].count?NULL:"remove fields not listed in the tool schema";
}
static ah_status check(void *ctx,const char *name,J *a,int *approval,int *effect,char *err,size_t len) {
    int id=find_tool(name);
    tool_state *s=ctx;
    if (id<0 || !(s->enabled & (1u<<id))) {
        snprintf(err,len,"unknown or disabled tool"); return AH_FAILED;
    }
    const char *why=argument_error(id,a);
    if (why) {
        snprintf(err,len,"%s",why);
        /* A quoted enum is a common model error. Name the precise field edit,
         * using only schema literals. The proposed arguments remain untouched. */
        const char *op=id==CALC?field(a,"op"):NULL;
        if(op && number(get(a,"a")) && number(get(a,"b")) && strlen(op)==5 && op[0]=='"' && op[4]=='"') {
            static const char *const ops[]={"add","sub","mul","div"};
            for(size_t i=0;i<sizeof ops/sizeof *ops;i++) {
                if(!memcmp(op+1,ops[i],3)) {
                    snprintf(err,len,"calc.op contains extra quotes. Send the field as {\"op\":\"%s\"}. "
                        "Use %s as the plain value with no embedded quotes. Keep a and b numeric and unchanged",ops[i],ops[i]);
                    break;
                }
            }
        }
        return AH_INVALID_ARGUMENTS;
    }
    if(id==HTTP_GET && !ah_https_url_check(field(a,"url"),err,len)) return AH_INVALID_ARGUMENTS;
    *approval = *effect = id==WRITE_FILE || id==EXEC || id==SHELL;
    return AH_DONE;
}
static ah_status result(char **out,ah_status st,const char *message) {
    *out=strdup(message);
    return *out ? st : AH_FAILED;
}
static ah_status read_file(tool_state *s,const char *path,size_t cap,char **out) {
#ifdef __APPLE__
    struct stat st;
    if(fstatat(s->dir,path,&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_nlink!=1)
        return result(out,AH_FAILED,"expected a regular file with one hard link");
    int fd=openat(s->dir,path,O_NOFOLLOW|O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOCTTY);
    struct stat actual;
    if(fd>=0 && (fstat(fd,&actual) || actual.st_dev!=st.st_dev || actual.st_ino!=st.st_ino ||
                 !S_ISREG(actual.st_mode) || actual.st_nlink!=1)) { close(fd); fd=-1; }
#else
    int pinned=openat(s->dir,path,O_PATH|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if (pinned<0 || fstat(pinned,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1) {
        if(pinned>=0) close(pinned);
        return result(out,AH_FAILED,"expected a regular file with one hard link");
    }
    char proc_path[48];
    snprintf(proc_path,sizeof proc_path,"/proc/self/fd/%d",pinned);
    int fd=open(proc_path,O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    close(pinned);
#endif
    if(fd<0) return result(out,AH_FAILED,"cannot open pinned file");
    char *buf=malloc(cap+1);
    if (!buf) { close(fd); return AH_FAILED; }
    size_t used=0;
    int failed=0;
    while (used<=cap) {
        ssize_t n=read(fd,buf+used,cap+1-used);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { failed=n<0; break; }
        used+=(size_t)n;
    }
    close(fd);
    if (failed || used>cap || !ah_utf8_text(buf,used)) {
        free(buf);
        return result(out,used>cap?AH_BUDGET:AH_FAILED,used>cap?"file exceeds output limit":"invalid UTF-8 file");
    }
    buf[used]=0; *out=buf; return AH_DONE;
}
static ah_status write_file(tool_state *s,const char *path,const char *data,char **out) {
    struct stat st;
    if (fstatat(s->dir,path,&st,AT_SYMLINK_NOFOLLOW)==0) {
        if (!S_ISREG(st.st_mode) || st.st_nlink!=1) return result(out,AH_FAILED,"unsafe target");
    } else if(errno!=ENOENT) return result(out,AH_FAILED,"cannot inspect target");
    unsigned char nonce[12];
#ifdef __APPLE__
    arc4random_buf(nonce,sizeof nonce);
#else
    if (getrandom(nonce,sizeof nonce,0)!=(ssize_t)sizeof nonce) return result(out,AH_FAILED,"entropy unavailable");
#endif
    char temp[48]=".dunlin-tmp-";
    for(size_t i=0;i<sizeof nonce;i++) snprintf(temp+10+2*i,3,"%02x",nonce[i]);
    int fd=openat(s->dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if (fd<0) return result(out,AH_FAILED,"cannot create new file");
    size_t len=strlen(data),pos=0;
    while (pos<len) {
        ssize_t n=write(fd,data+pos,len-pos);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) break;
        pos+=(size_t)n;
    }
    int ok=pos==len && !fsync(fd);
    if(close(fd)) ok=0;
    if(!ok || renameat(s->dir,temp,s->dir,path)) {
        unlinkat(s->dir,temp,0); return result(out,AH_FAILED,"cannot replace file");
    }
    if(fsync(s->dir)) return result(out,AH_UNKNOWN,"rename observed; durability unknown");
    return result(out,AH_DONE,"written");
}

static _Noreturn void supervision_failed(void) {
    kill(getpid(),SIGKILL);
    _exit(125);
}
/* On Linux a per-command subreaper owns this process tree. The embedding process never
 * adopts plugin children or changes its SIGCHLD policy. EOF on control also
 * requests cleanup if the embedding process disappears. */
static _Noreturn void supervise(tool_state *s,int control,int output,const char *exe,char *const argv[]) {
#ifdef __linux__
    if(prctl(PR_SET_CHILD_SUBREAPER,1)) supervision_failed();
#endif
    if(setpgid(0,0) || signal(SIGCHLD,SIG_DFL)==SIG_ERR)
        supervision_failed();
    pid_t leader=fork();
    if(!leader) {
        if(setpgid(0,0) || fchdir(s->dir) || dup2(output,1)<0 || dup2(output,2)<0) _exit(126);
        int in=open("/dev/null",O_RDONLY);
        if(in<0 || dup2(in,0)<0) _exit(126);
        close_from(3);
        char *env[]={s->child_path,"LANG=C",s->child_home,NULL};
        execve(exe,argv,env);
        int missing=errno==ENOENT;
        const char *why=missing?"exec: executable or interpreter not found\n":"exec: executable could not be started\n";
        (void)write(2,why,strlen(why)); _exit(missing?127:126);
    }
    if(leader<0) supervision_failed();
    setpgid(leader,leader);
    close(output);
    if(control!=3) { if(dup2(control,3)<0) supervision_failed(); close(control); }
    /* Only the control read end stays open, including after a host crash. */
    int control_fd=3;
    close_from(4);
    for(;;) {
        siginfo_t si={0};
        if(waitid(P_PID,leader,&si,WEXITED|WNOHANG|WNOWAIT)) {
            if(errno==EINTR) continue;
            break;
        }
        if(si.si_pid) break;
        struct pollfd notice={control_fd,POLLIN,0};
        if(poll(&notice,1,10)>0) break;
    }
    kill(-leader,SIGKILL);
    kill(leader,SIGKILL);
    int status=0;
    pid_t w;
    do { w=waitpid(leader,&status,0); } while(w<0 && errno==EINTR);
    /* Adopted descendants remaining in the managed process group. */
    for(;;) {
        int ignored;
        pid_t child=waitpid(-leader,&ignored,0);
        if(child<0 && errno==EINTR) continue;
        if(child<0) break;
    }
    if(w!=leader || !WIFEXITED(status)) supervision_failed();
    _exit(WEXITSTATUS(status));
}
static int create_pipe(int fd[2]) {
#ifdef __APPLE__
    if(pipe(fd)) return -1;
    if(fcntl(fd[0],F_SETFD,FD_CLOEXEC)<0 || fcntl(fd[1],F_SETFD,FD_CLOEXEC)<0) {
        close(fd[0]); close(fd[1]); return -1;
    }
    return 0;
#else
    return pipe2(fd,O_CLOEXEC);
#endif
}
static ah_status execute(tool_state *s,char *const argv[],int include_status,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out) {
    /* A completed shell may report a marked preview. Bound the extra bytes
     * consumed while waiting for exit, independently of the retained result. */
    size_t capture=cap;
    char *data=malloc(capture+1);
    int fds[2];
    if (!data || create_pipe(fds)) { free(data); return result(out,AH_FAILED,"pipe allocation failed"); }
    int control[2];
    if(create_pipe(control)) {
        free(data); close(fds[0]); close(fds[1]); return result(out,AH_FAILED,"control pipe failed");
    }
    uint64_t start=hooks->now_ms(hooks->user);
    pid_t pid=fork();
    if(!pid) {
        close(control[1]); close(fds[0]);
        supervise(s,control[0],fds[1],argv[0],argv);
    }
    close(control[0]); close(fds[1]);
    if(pid<0) {
        close(control[1]); close(fds[0]); free(data); return result(out,AH_FAILED,"fork failed");
    }
    int flags=fcntl(fds[0],F_GETFL);
    int bad=flags<0 || fcntl(fds[0],F_SETFL,flags|O_NONBLOCK)<0;
    int exited=0,eof=0;
    size_t used=0;
    uint64_t last_progress=start;
    const char *reason=NULL;
    while (!bad) {
        if (hooks->cancelled && hooks->cancelled(hooks->user)) { reason="cancelled"; break; }
        if (hooks->now_ms(hooks->user)-start>=timeout) { reason="timeout"; break; }
        ssize_t n=read(fds[0],data+used,capture+1-used);
        if(n>0) {
            used+=(size_t)n;
            if(used>capture) {
                if(!include_status || capture!=cap) { reason="output limit exceeded"; break; }
                capture=cap+64*1024;
                char *expanded=realloc(data,capture+1);
                if(!expanded) { reason="capture allocation failed"; break; }
                data=expanded;
            }
        } else if (!n) eof=1;
        else if(errno!=EAGAIN && errno!=EINTR) { bad=1; break; }
        if(!exited) {
            siginfo_t si={0};
            if(waitid(P_PID,pid,&si,WEXITED|WNOHANG|WNOWAIT)) {
                if(errno==EINTR) continue;
                bad=1; break;
            }
            exited=si.si_pid!=0;

        }
        if(exited && eof) break;
        uint64_t now=hooks->now_ms(hooks->user);
        if(hooks->event && now-last_progress>=5000) {
            char progress[128];
            int length=snprintf(progress,sizeof progress,"elapsed_ms=%llu output_bytes=%zu",
                (unsigned long long)(now-start),used);
            hooks->event(hooks->user,"exec.progress",progress,(size_t)length);
            last_progress=now;
        }
        if(n<=0) {
            struct pollfd pfd={fds[0],POLLIN,0};
            poll(&pfd,eof?0:1,10);
        }
    }
    close(control[1]);
    int status=0;
    pid_t reaped;
    do { reaped=waitpid(pid,&status,0); } while(reaped<0 && errno==EINTR);
    close(fds[0]);
    if(reason || bad || reaped<0) {
        free(data);
        char detail[128];
        snprintf(detail,sizeof detail,"%s; process group terminated; external result unknown",reason?reason:"supervision failure");
        return result(out,AH_UNKNOWN,detail);
    }
    if(!ah_utf8_text(data,used)) { free(data); return result(out,AH_UNKNOWN,"process ran but output is not UTF-8; external result unknown"); }
    if (!WIFEXITED(status)) {
        free(data); return result(out,AH_UNKNOWN,"abnormal process exit; external result unknown");
    }
    data[used]=0;
    int exit_code=WEXITSTATUS(status);
    char label[48];
    int header=snprintf(label,sizeof label,"exit_code=%d\n",exit_code);
    static const char marker[]="\n[output truncated]\n";
    if(include_status && (size_t)header+used>cap && cap>=(size_t)header+sizeof marker-1) {
        size_t room=cap-(size_t)header-(sizeof marker-1);
        size_t head=room/2, tail=used-(room-head);
        while(head && ((unsigned char)data[head]&0xc0)==0x80) --head;
        while(tail<used && ((unsigned char)data[tail]&0xc0)==0x80) ++tail;
        memmove(data+head+sizeof marker-1,data+tail,used-tail);
        memcpy(data+head,marker,sizeof marker-1);
        used=head+sizeof marker-1+used-tail;
        data[used]=0;
    }
    char *observation=malloc((size_t)header+used+1);
    if(!observation) { free(data); return result(out,AH_UNKNOWN,"cannot retain command observation"); }
    memcpy(observation,label,(size_t)header);
    memcpy(observation+header,data,used+1);
    if((include_status || exit_code) && used+(size_t)header>cap) { free(data); free(observation); return result(out,AH_UNKNOWN,"exit status and output exceed budget"); }
    if(hooks->event) hooks->event(hooks->user,"exec.result",observation,(size_t)header+used);
    if(include_status || exit_code) {
        free(data);
        *out=observation;
    } else { free(observation); *out=data; }
    return exit_code ? AH_EXEC_FAILED : AH_DONE;
}
static ah_status call(void *ctx,const char *name,J *a,const ah_hooks *h,size_t cap,unsigned timeout,char **out) {
    int approve,effect; char error[80]={0};
    *out=NULL;
    if(check(ctx,name,a,&approve,&effect,error,sizeof error)!=AH_DONE)
        return result(out,AH_FAILED,error);
    if (!h || !h->now_ms || cap==0 || cap>16*1024*1024) return result(out,AH_FAILED,"invalid tool limits");
    if(h->cancelled && h->cancelled(h->user)) return result(out,AH_CANCELLED,"cancelled");
    char answer[64]; double value=0; const char *txt=NULL;
    switch(find_tool(name)) {
    case READ_FILE: return read_file(ctx,field(a,"path"),cap,out);
    case WRITE_FILE:
        if(cap < strlen("written")) return result(out,AH_BUDGET,"output limit exceeded");
        return write_file(ctx,field(a,"path"),field(a,"text"),out);
    case HTTP_GET: return ah_https_get(field(a,"url"),h,cap,timeout,out);
    case SHELL: {
        char *argv[]={"/bin/sh","-c",(char *)field(a,"command"),NULL};
        return execute(ctx,argv,1,h,cap,timeout,out);
    }
    case EXEC: {
        J *items=get(a,"argv");
        char *argv[66]={(char *)field(a,"executable")};
        for(size_t i=0;i<(size_t)json_object_array_length(items);i++)
            argv[i+1]=(char *)string(json_object_array_get_idx(items,i));
        return execute(ctx,argv,0,h,cap,timeout,out);
    }
    case CALC: calc(a,&value); snprintf(answer,sizeof answer,"%.17g",value); txt=answer; break;
    default: txt=field(a,"text"); break;
    }
    return strlen(txt)<=cap ? result(out,AH_DONE,txt) : result(out,AH_BUDGET,"output limit exceeded");
}
int ah_linux_tools(ah_tool_service *out,const char *workspace) {
    if(!out || !workspace) return -1;
    int dir=open(workspace,O_RDONLY|O_NOFOLLOW|O_DIRECTORY|O_CLOEXEC);
    if(dir<0) return -1;
    tool_state *s=malloc(sizeof(*s));
    if(!s) { close(dir); return -1; }
    const char *path="/usr/bin:/bin";
#ifdef __APPLE__
    const char *launch_path=getenv("PATH");
    if(launch_path && *launch_path) path=launch_path;
#endif
    char *child_path=malloc(strlen(path)+6);
    if(!child_path) {close(dir); free(s); return -1;}
    sprintf(child_path,"PATH=%s",path);
    const char *launch_home=getenv("HOME");
    char *child_home=NULL;
    if(launch_home && *launch_home) {
        child_home=malloc(strlen(launch_home)+6);
        if(!child_home) { close(dir); free(child_path); free(s); return -1; }
        sprintf(child_home,"HOME=%s",launch_home);
    }
    *s=(tool_state){dir,(1u<<SHELL)-1,child_path,child_home};
    *out=(ah_tool_service){s,schemas,check,call};
    return 0;
}
int ah_linux_tools_root_identity(const ah_tool_service *service,uint64_t *device,uint64_t *inode) {
    if(device)*device=0;if(inode)*inode=0;
    if(!service||!service->ctx||!device||!inode||service->schema!=schemas||service->check!=check||service->call!=call)return 0;
    const tool_state *s=service->ctx;struct stat st;
    if(fstat(s->dir,&st)||!S_ISDIR(st.st_mode))return 0;
    *device=(uint64_t)st.st_dev;*inode=(uint64_t)st.st_ino;return 1;
}
int ah_linux_tool_enable(ah_tool_service *service,const char *name,int enable) {
    int id=find_tool(name);
    if(!service || !service->ctx || id<0) return -1;
    tool_state *s=service->ctx;
    if(enable) s->enabled|=1u<<id; else s->enabled&=~(1u<<id);
    return 0;
}
void ah_linux_tools_close(ah_tool_service *out) {
    if(out && out->ctx) {
        tool_state *s=out->ctx;
        close(s->dir); free(s->child_path); free(s->child_home); free(s); *out=(ah_tool_service){0};
    }
}
