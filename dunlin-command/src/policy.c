#define _POSIX_C_SOURCE 200809L
#include "policy.h"
#include <dunlin/compact/json.h>
#include <json-c/json.h>
#include <histedit.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct json_object J;
static J *get(J *o,const char *key) {
    J *v=NULL; json_object_object_get_ex(o,key,&v); return v;
}
static const char *text(J *o,const char *key) {
    J *v=get(o,key);
    if(!json_object_is_type(v,json_type_string)) return NULL;
    const char *s=json_object_get_string(v);
    return strlen(s)==(size_t)json_object_get_string_len(v)?s:NULL;
}
/* Parse literal argv only. Never interpret substitutions, control operators,
 * comments, redirections or pipelines as evidence of a read-only operation. */
static J *literal(const char *s) {
    if(!s || strlen(s)>16384) return NULL;
    char quote=0;
    for(size_t i=0;s[i];i++) {
        char ch=s[i];
        if(ch=='\n' || ch=='\r' || ((unsigned char)ch<32 && ch!='\t')) return NULL;
        if(quote=='\'') {if(ch=='\'') quote=0;continue;}
        if(ch=='\\') {if(!s[++i] || s[i]=='\n' || s[i]=='\r') return NULL;continue;}
        if(ch=='$' || ch=='`') return NULL;
        if(quote=='"') {if(ch=='"') quote=0;continue;}
        if(ch=='\'' || ch=='"') quote=ch;
        else if(strchr(";&|<>()#*?[]{}~",ch)) return NULL;
    }
    if(quote) return NULL;
    Tokenizer *tok=tok_init(NULL);
    if(!tok) return NULL;
    int n=0;const char **argv=NULL;
    J *out=NULL;
    if(!tok_str(tok,s,&n,&argv) && n>0 && n<=66) {
        out=json_object_new_array();
        for(int i=0;i<n;i++) json_object_array_add(out,json_object_new_string(argv[i]));
    }
    tok_end(tok);return out;
}
static const char *arg(J *a,size_t i) {return json_object_get_string(json_object_array_get_idx(a,i));}
/* Diagnostics only: observe available executable names, never run or select one.
 * Match the requested basename or a numeric version suffix, not unrelated tools. */
static int version_name(const char *base,const char *name) {
    size_t n=strlen(base);
    if(strncmp(base,name,n)) return 0;
    const char *p=name+n;
    if(!*p) return 1;
    if(*p=='-') p++;
    if(*p<'0' || *p>'9') return 0;
    for(;*p;p++) if((*p<'0' || *p>'9') && *p!='.') return 0;
    return 1;
}
static int executable(const char *path) {
    struct stat st;
    return !stat(path,&st) && S_ISREG(st.st_mode) && !access(path,X_OK);
}
static void candidates(const char *word,char *hint,size_t size) {
    *hint=0;
    const char *base=strrchr(word,'/');base=base?base+1:word;
    if(!*base || strlen(base)>64) return;
    for(const char *c=base;*c;c++)
        if(!((*c>='a' && *c<='z') || (*c>='A' && *c<='Z') ||
             (*c>='0' && *c<='9') || strchr("_+.-",*c))) return;
    const char *path=getenv("PATH");
    size_t length=path?strlen(path):0;
    if(length>16384) return;
    char *search=malloc(length+32);
    if(!search) return;
    snprintf(search,length+32,"%s:/usr/bin:/bin",path?path:"");
    char *save=NULL;
    size_t dirs=0,found=0;
    for(char *dir=strtok_r(search,":",&save);dir && dirs++<32 && found<2;dir=strtok_r(NULL,":",&save)) {
        if(*dir!='/') continue;
        int plain=1;
        for(const unsigned char *c=(const unsigned char *)dir;*c;c++)
            if(*c<=32 || *c>=127 || strchr("\"'`$\\",*c)) {plain=0;break;}
        if(!plain) continue;
        DIR *entries=opendir(dir);
        if(!entries) continue;
        struct dirent *entry;size_t scanned=0;
        while(found<2 && scanned++<4096 && (entry=readdir(entries))) {
            if(!version_name(base,entry->d_name)) continue;
            char full[4096];
            int n=snprintf(full,sizeof full,"%s/%s",dir,entry->d_name);
            if(n<0 || (size_t)n>=sizeof full || !executable(full)) continue;
            size_t used=strlen(hint),extra=used?2:0;
            if(used+extra+(size_t)n>=size || strstr(hint,full)) continue;
            if(extra) memcpy(hint+used,", ",2);
            memcpy(hint+used+extra,full,(size_t)n+1);found++;
        }
        closedir(entries);
    }
    free(search);
}
static int known_program(cli_tools *t,const char *name) {
    char full[4096];
    if(strchr(name,'/')) {
        int n=snprintf(full,sizeof full,"%s%s%s",*name=='/'?"":t->workspace,*name=='/'?"":"/",name);
        return n>=0 && (size_t)n<sizeof full && executable(full);
    }
    const char *path="/usr/bin:/bin";
#ifdef __APPLE__
    const char *host=getenv("PATH");
    if(host && *host) path=host;
#endif
    size_t length=strlen(path);
    if(length>16384) return 0;
    if(*path==':' || (length && path[length-1]==':') || strstr(path,"::")) {
        int n=snprintf(full,sizeof full,"%s/%s",t->workspace,name);
        if(n>=0 && (size_t)n<sizeof full && executable(full)) return 1;
    }
    char *copy=strdup(path),*save=NULL;
    if(!copy) return 0;
    int found=0;size_t count=0;
    for(char *dir=strtok_r(copy,":",&save);dir && count++<32;dir=strtok_r(NULL,":",&save)) {
        /* Relative PATH entries resolve in the tool's workspace. */
        int n=snprintf(full,sizeof full,"%s%s%s/%s",*dir=='/'?"":t->workspace,*dir=='/'?"":"/",dir,name);
        if(n>=0 && (size_t)n<sizeof full && executable(full)) {found=1;break;}
    }
    free(copy);return found;
}
static int quoted_command(cli_tools *t,J *args,char *error,size_t size) {
    const char *raw=text(args,"command");
    if(!raw) return 0;
    raw+=strspn(raw," \t");
    size_t length=strlen(raw);
    while(length && (raw[length-1]==' ' || raw[length-1]=='\t')) length--;
    if(length<2 || (*raw!='\"' && *raw!='\'') || raw[length-1]!=*raw) return 0;
    J *words=literal(raw);
    if(!words) return 0;
    const char *one=arg(words,0);
    int wrapped=json_object_array_length(words)==1 && strpbrk(one," \t")!=NULL;
    /* A real quoted executable path containing spaces is legitimate shell source. */
    if(!wrapped || known_program(t,one)) {json_object_put(words);return 0;}
    char first[128],hint[160];
    size_t len=strcspn(one," \t");
    if(len>=sizeof first) len=0;
    memcpy(first,one,len);first[len]=0;
    candidates(first,hint,sizeof hint);
    snprintf(error,size,"ONE executable: remove outer quotes; keep argument quotes. Paths (not run): %s",*hint?hint:"none in checked paths");
    json_object_put(words);return 1;
}
static void missing_executable_feedback(cli_tools *t,J *args,size_t cap,char **out) {
    if(!*out || strncmp(*out,"exit_code=127\n",14)) return;
    J *words=literal(text(args,"command"));
    if(!words) return;
    const char *program=arg(words,0);
    char hint[160]={0};
    if(!known_program(t,program)) candidates(program,hint,sizeof hint);
    json_object_put(words);
    if(!*hint) return;
    char note[400];
    int n=snprintf(note,sizeof note,"\nCLI observed executable paths (not executed): %s\nUse an observed path if appropriate. shell.command is shell source; do not put quotes around the entire command.",hint);
    size_t used=strlen(*out);
    if(n<0 || (size_t)n>=sizeof note || used>cap || (size_t)n>cap-used) return;
    char *combined=malloc(used+(size_t)n+1);
    if(!combined) return;
    memcpy(combined,*out,used);memcpy(combined+used,note,(size_t)n+1);
    free(*out);*out=combined;
}
static const char *system_program(const char *name,const char *program) {
    static char selected[256];
    const char *dirs[]={"/usr/bin","/bin"};
    for(size_t i=0;i<2;i++) {
        snprintf(selected,sizeof selected,"%s/%s",dirs[i],program);
        if(strcmp(name,program) && strcmp(name,selected)) continue;
        struct stat st;
        if(!stat(selected,&st) && S_ISREG(st.st_mode) && st.st_uid==0 &&
           !(st.st_mode&(S_IWGRP|S_IWOTH)) && !access(selected,X_OK)) return selected;
    }
    return NULL;
}
static int http_url(const char *s) {
    const char *host=NULL;
    if(!strncmp(s,"https://",8)) host=s+8;
    else if(!strncmp(s,"http://",7)) host=s+7;
    if(!host || !*host || *host=='/' || *host=='?' || *host=='#') return 0;
    size_t authority=strcspn(host,"/?#");
    if(memchr(host,'@',authority)) return 0;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++)
        if(*p<=32 || *p>=127) return 0;
    return 1;
}
static int options(J *a,const char *program) {
    size_t n=json_object_array_length(a);
    int urls=0;
    for(size_t i=1;i<n;i++) {
        const char *v=arg(a,i);
        if(!strcmp(program,"curl")) {
            if(!strcmp(v,"-s") || !strcmp(v,"-S") || !strcmp(v,"-sS") ||
               !strcmp(v,"-fsS") || !strcmp(v,"-f") || !strcmp(v,"--silent") ||
               !strcmp(v,"--show-error") || !strcmp(v,"--fail")) continue;
            if((!strcmp(v,"-X") || !strcmp(v,"--request")) && i+1<n && !strcmp(arg(a,i+1),"GET")) {i++;continue;}
            if(!http_url(v) || ++urls>1) return 0;
        } else if(!strcmp(program,"date")) {
            if(v[0]!='+' && strcmp(v,"-u") && strcmp(v,"-R")) return 0;
        } else if(!strcmp(program,"pwd")) {
            if(strcmp(v,"-L") && strcmp(v,"-P")) return 0;
        } else if(!strcmp(program,"ls")) {
            if(strcmp(v,".") && strcmp(v,"-l") && strcmp(v,"-a") && strcmp(v,"-la") &&
               strcmp(v,"-al") && strcmp(v,"-h") && strcmp(v,"-lh") &&
               strcmp(v,"-lah") && strcmp(v,"-1")) return 0;
        }
    }
    return strcmp(program,"curl") || urls==1;
}
/* The normalized command quotes every literal token and uses fixed system
 * executables. PATH shadows and user curl configuration cannot change its meaning. */
J *cli_readonly(const char *name,J *args) {
    if(strlen(json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN))>16384) return NULL;
    J *argv=NULL;
    if(!strcmp(name,"shell")) argv=literal(text(args,"command"));
    else if(!strcmp(name,"exec")) {
        const char *exe=text(args,"executable");J *values=get(args,"argv");
        if(!exe || !json_object_is_type(values,json_type_array) || json_object_array_length(values)>64) return NULL;
        argv=json_object_new_array();
        json_object_array_add(argv,json_object_new_string(exe));
        for(size_t i=0;i<(size_t)json_object_array_length(values);i++) {
            J *v=json_object_array_get_idx(values,i);
            if(!json_object_is_type(v,json_type_string) ||
               strlen(json_object_get_string(v))!=(size_t)json_object_get_string_len(v)) {
                json_object_put(argv);return NULL;
            }
            json_object_array_add(argv,json_object_get(v));
        }
    }
    if(!argv) return NULL;
    const char *programs[]={"date","pwd","uname","sw_vers","ls","echo","printf","curl"};
    const char *selected=NULL,*program=NULL;
    for(size_t i=0;i<sizeof programs/sizeof *programs;i++) {
        selected=system_program(arg(argv,0),programs[i]);
        if(selected) {program=programs[i];break;}
    }
    if(!selected || !options(argv,program)) {json_object_put(argv);return NULL;}
    size_t n=json_object_array_length(argv),size=128+strlen(selected)*4;
    for(size_t i=1;i<n;i++) size+=strlen(arg(argv,i))*4+4;
    char *command=malloc(size);
    if(!command) {json_object_put(argv);return NULL;}
    char *p=command;
    const char *prefix=!strcmp(program,"curl")?
        " --disable --globoff --proto '=http,https' --proto-redir '=http,https'":"";
    for(size_t i=0;i<n;i++) {
        if(i) *p++=' ';
        *p++='\'';
        for(const char *v=i?arg(argv,i):selected;*v;v++) {
            if(*v=='\'') {memcpy(p,"'\\''",4);p+=4;}
            else *p++=*v;
        }
        *p++='\'';
        if(!i) {memcpy(p,prefix,strlen(prefix));p+=strlen(prefix);}
    }
    *p=0;
    J *out=json_object_new_object();
    json_object_object_add(out,"command",json_object_new_string(command));
    free(command);json_object_put(argv);return out;
}
static void clear_checked(cli_tools *t) {
    json_object_put(t->checked);json_object_put(t->normalized);free(t->checked_name);
    t->checked=NULL;t->normalized=NULL;t->checked_name=NULL;
}
static J *schema(void *ctx) {
    cli_tools *t=ctx;
    J *source=t->base->schema(t->base->ctx);
    if(!t->automatic || !source) return source;
    const char *encoded=json_object_to_json_string_ext(source,JSON_C_TO_STRING_PLAIN);
    J *view=dunlin_json_parse(encoded,strlen(encoded));
    json_object_put(source);
    if(!view) return NULL;
    for(size_t i=0;i<(size_t)json_object_array_length(view);i++) {
        J *fn=get(json_object_array_get_idx(view,i),"function");
        const char *name=text(fn,"name");
        if(!name || (strcmp(name,"shell") && strcmp(name,"exec") && strcmp(name,"read_file") && strcmp(name,"write_file"))) continue;
        const char *original=text(fn,"description");
        const char *format="%s CLI workspace (actual absolute path): %s. "
            "If asked to write a script, save a real file in this workspace and verify its path. "
            "Code executed via -c is inline code, not by itself a saved script. "
            "Report only observed file paths. "
            "shell.command is shell source: quote individual arguments, never wrap the whole command in extra quotes. "
            "File-tool path arguments must be plain basenames without slashes or extra surrounding quote characters.";
        size_t size=strlen(format)+strlen(t->workspace)+(original?strlen(original):0)+1;
        char *description=malloc(size);
        if(!description) {json_object_put(view);return NULL;}
        snprintf(description,size,format,original?original:"",t->workspace);
        json_object_object_add(fn,"description",json_object_new_string(description));
        free(description);
    }
    return view;
}
static char *probe_key(const char *name,J *args) {
    if(strcmp(name,"shell")) return NULL;
    J *words=literal(text(args,"command"));
    if(!words) return NULL;
    size_t count=json_object_array_length(words);
    int probe=(count>=2 && !strcmp(arg(words,0),"which")) ||
        (count>=3 && !strcmp(arg(words,0),"command") && !strcmp(arg(words,1),"-v"));
    char *key=probe?strdup(json_object_to_json_string_ext(words,JSON_C_TO_STRING_PLAIN)):NULL;
    json_object_put(words);return key;
}
void cli_tools_begin_turn(cli_tools *tools,const char *input) {
    free(tools->last_probe);tools->last_probe=NULL;tools->guard_probes=tools->automatic;
    if(!input) return;
    const char *repeat[]={"重复","轮询","每隔","连续","两次","三次","四次","五次","十次","repeat","twice","poll"};
    int negative=strstr(input,"不要重复") || strstr(input,"不重复") || strstr(input,"别重复") || strstr(input,"避免重复");
    for(size_t i=0;!negative && i<sizeof repeat/sizeof *repeat;i++)
        if(strstr(input,repeat[i])) tools->guard_probes=0;
    for(const char *p=input;!negative && *p;p++) {
        if(*p<'0' || *p>'9') continue;
        char *end;unsigned long count=strtoul(p,&end,10);
        if(count>=2 && !strncmp(end,"次",strlen("次"))) tools->guard_probes=0;
        p=end-1;
    }
}
static ah_status check(void *ctx,const char *name,J *args,int *approval,int *effect,char *error,size_t size) {
    cli_tools *t=ctx;clear_checked(t);
    ah_status status=t->base->check(t->base->ctx,name,args,approval,effect,error,size);
    if(status!=AH_DONE) return status;
    char *probe=probe_key(name,args);
    int duplicate=t->guard_probes && probe && t->last_probe && !strcmp(probe,t->last_probe) && t->last_status==AH_DONE;
    free(probe);
    if(duplicate) {
        snprintf(error,size,"This lookup already succeeded. Observed result: %.96s. Use the observed executable for --version or continue the task; do not repeat this lookup.",t->last_result?t->last_result:"");
        if(t->activity) t->activity(t->activity_user,"duplicate",NULL,0);
        return AH_INVALID_ARGUMENTS;
    }
    if(t->automatic && !strcmp(name,"shell") && quoted_command(t,args,error,size)) return AH_INVALID_ARGUMENTS;
    if(t->automatic && (!strcmp(name,"read_file") || !strcmp(name,"write_file"))) {
        const char *path=text(args,"path");
        size_t n=path?strlen(path):0;
        if(n>=2 && (path[0]=='\"' || path[0]=='\'') && path[n-1]==path[0]) {
            snprintf(error,size,"%s.path must be a plain basename: remove the extra surrounding quote characters from the JSON string value",name);
            return AH_INVALID_ARGUMENTS;
        }
    }
    t->checked_name=strdup(name);
    if(!t->checked_name) return AH_FAILED;
    t->checked=json_object_get(args);
    if(t->automatic) t->normalized=cli_readonly(name,args);
    if(t->normalized) *approval=0;
    return status;
}
static ah_status call(void *ctx,const char *name,J *args,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out) {
    cli_tools *t=ctx;
    if(!t->checked_name || strcmp(name,t->checked_name) || args!=t->checked) {
        *out=strdup("CLI tool dispatch does not match its checked request");return AH_FAILED;
    }
    free(t->last_name);free(t->last_args);free(t->last_result);
    t->last_name=strdup(name);
    J *actual=t->normalized?t->normalized:args;
    t->last_args=strdup(json_object_to_json_string_ext(actual,JSON_C_TO_STRING_PLAIN));
    free(t->last_probe);t->last_probe=probe_key(name,args);
    const char *label=!strcmp(name,"shell")?text(actual,"command"):t->last_args;
    if(t->activity) t->activity(t->activity_user,"start",label?label:name,strlen(label?label:name));
    cli_live_scope scope;cli_live_enter(&scope,t->activity,t->activity_user);
    ah_status status;
    if(t->normalized) status=t->runner.call(t->runner.ctx,"shell",actual,hooks,cap,timeout,out);
    else if(t->native_commands && (!strcmp(name,"shell") || !strcmp(name,"exec")))
        status=t->runner.call(t->runner.ctx,name,args,hooks,cap,timeout,out);
    else status=t->base->call(t->base->ctx,name,args,hooks,cap,timeout,out);
    cli_live_leave(&scope);
    if(t->activity) t->activity(t->activity_user,"finish",ah_status_name(status),strlen(ah_status_name(status)));
    if(t->normalized && !strcmp(name,"exec") && status==AH_DONE && *out &&
       !strncmp(*out,"exit_code=0\n",12)) memmove(*out,*out+12,strlen(*out+12)+1);
    if(t->automatic && !strcmp(name,"shell") && status==AH_EXEC_FAILED) missing_executable_feedback(t,args,cap,out);
    t->last_status=status;t->last_result=strdup(*out?*out:ah_status_name(status));
    clear_checked(t);return status;
}
int cli_tools_open(cli_tools *t,const ah_tool_service *base,const char *workspace,int automatic) {
    memset(t,0,sizeof *t);t->base=base;t->automatic=automatic;t->workspace=workspace;
    if(!base) return 0;
    ah_tool_service reference;
    if(ah_linux_tools(&reference,workspace)) return 0;
    t->native_commands=base->call==reference.call;
    ah_linux_tools_close(&reference);
    if(cli_live_tools(&t->runner,workspace)) return 0;
    cli_live_tool_enable(&t->runner,"shell",1);
    t->service=(ah_tool_service){t,schema,check,call};return 1;
}
void cli_tools_close(cli_tools *t) {
    clear_checked(t);free(t->last_name);free(t->last_args);free(t->last_result);free(t->last_probe);
    cli_live_tools_close(&t->runner);
}
