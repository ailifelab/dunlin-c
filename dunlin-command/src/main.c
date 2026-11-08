#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dunlin/compact/runtime.h"
#include "dunlin/compact/session.h"
#include "input.h"
#include "policy.h"
#include "view.h"
#include "../../dunlin-compact/platform/random.h"
#include "../../dunlin-compact/platform/clock.h"
#include <dirent.h>
#include <strings.h>
#include <json-c/json.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>

static volatile sig_atomic_t interrupted;
static void interrupt(int signo) { (void)signo; interrupted=1; }
static int cancelled(void *unused) { (void)unused; return interrupted; }
typedef struct {
    int interactive,events,json,default_yes,reasoning_open;
    uint64_t deadline;
    struct json_object *grants;
    cli_tools tools;
    cli_view view;
    dunlin_runtime *runtime;
} terminal;
static void display(FILE *,const char *);
static void display_n(FILE *,const char *,size_t);
static void end_reasoning(terminal *t) {
    cli_view_clear(&t->view);
    if(t->reasoning_open) {fputc('\n',stderr);fflush(stderr);t->reasoning_open=0;}
}

static void print_json(struct json_object *o) {
    puts(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));
    fflush(stdout);
}
static int approve(void *ctx,const char *name,const char *args) {
    terminal *t=ctx;
    end_reasoning(t);
    if(!t->interactive || interrupted || !t->grants) return 0;
    struct json_object *grant=NULL;
    if(json_object_object_get_ex(t->grants,name,&grant) && json_object_get_boolean(grant)) return 1;
    cli_view_heading(&t->view,"需要审批");
    fprintf(stderr,"Approval required for tool %s with exactly these arguments?\n",name);
    /* Preserve readable UTF-8/JSON, escape terminal-control and bidi bytes. */
    if(t->view.enabled) fputs("  ",stderr);
    display(stderr,args);
    fprintf(stderr,"\nAllow once? [%s] (y/n, a=allow ALL %s arguments for this chat process)\n",t->default_yes?"Yes/no":"yes/No",name);
    fputs("Type yes to allow once: ",stderr); fflush(stderr);
    uint64_t now=ah_os_now_ms(NULL);
    if(now>=t->deadline) return 0;
    struct pollfd fd={STDIN_FILENO,POLLIN,0};
    if(poll(&fd,1,(int)(t->deadline-now))!=1 || interrupted) return 0;
    char line[32];
    if(!fgets(line,sizeof(line),stdin)) return 0;
    if(!strchr(line,'\n')) {int c; while((c=getchar())!='\n' && c!=EOF) {}}
    line[strcspn(line,"\r\n")]=0;
    if(interrupted) return 0;
    if(!strcasecmp(line,"a") || !strcasecmp(line,"all") || !strcasecmp(line,"session")) {
        struct json_object *value=json_object_new_boolean(1);
        if(!value) {fputs("Cannot record chat grant. Denied.\n",stderr);return 0;}
        json_object_object_add(t->grants,name,value);
        if(!json_object_object_get_ex(t->grants,name,&grant) || !json_object_get_boolean(grant)) {
            fputs("Cannot record chat grant. Denied.\n",stderr);return 0;
        }
        fprintf(stderr,"Allowed %s for this chat process. /permissions reset revokes it.\n",name);
        return 1;
    }
    return !strcasecmp(line,"y") || !strcasecmp(line,"yes") || (!*line && t->default_yes);
}
static void observe(void *ctx,const char *type,const char *data,size_t n) {
    terminal *t=ctx;
    int kind=!strcmp(type,"reasoning.delta")?1:!strcmp(type,"reasoning.summary")?2:0;
    if(t->view.enabled) {
        if(kind && n && t->runtime && dunlin_show_reasoning(t->runtime)) cli_view_reason(&t->view,data,n);
        else if(!strcmp(type,"exec.progress") || !strcmp(type,"model.progress")) cli_view_wait(&t->view);
        return;
    }
    if(!t->json && kind && n && t->runtime && dunlin_show_reasoning(t->runtime)) {
        if(t->reasoning_open!=kind) {
            end_reasoning(t);
            fputs(kind==1?"[Reasoning] ":"[Reasoning summary] ",stderr);
            t->reasoning_open=kind;
        }
        /* Provider adapters emit complete UTF-8 strings, borrowed for n bytes. */
        display_n(stderr,data,n);fflush(stderr);
    } else if(!kind) end_reasoning(t);
    if(!t->events) return;
    struct json_object *o=json_object_new_object();
    json_object_object_add(o,"event",json_object_new_string(type));
    json_object_object_add(o,"data",json_object_new_string_len(data,(int)n));
    print_json(o); json_object_put(o);
}
static const char help_text[]=
    "Dunlin Command\n"
    "Usage: dunlin [OPTIONS] COMMAND [PROMPT]\n"
    "Commands: help, version, init, validate, doctor, plugins, tools, sessions, run, chat, resume\n"
    "Options: --config FILE, --session ID, --model ID, --image PNG, --require-tool NAME, --json, --events\n"
    "Permissions: --permissions request|agent|full-access (legacy read-only|strict accepted)\n"
    "             --approval-default yes|no (default no; noninteractive risky calls deny)\n"
    "Chat commands (local, no model request):\n"
    "  /help, /h, /?       Show this help\n"
    "  /pwd                Full configured tool workspace\n"
    "  /files              Actual workspace entries with full paths\n"
    "  /last               Last executed tool, actual arguments and result\n"
    "  /models             Configured model aliases (no credentials or URLs)\n"
    "  /model [ID]         Show or switch model at the idle prompt\n"
    "  /reasoning [on|off]  Show or hide provider-returned reasoning/summary\n"
    "  /effort [LEVEL]     default, none, low, medium, high, xhigh; supported models only\n"
    "  /permissions        Current native mode, grants and approval default\n"
    "  /permissions mode request|agent|full-access\n"
    "  /permissions reset  Revoke current chat grants\n"
    "  /permissions yes|no Set approval default for this chat\n"
    "  /tool NAME REQUEST  Require a tool for one turn\n"
    "  /q, /quit, /exit    Leave chat; Ctrl-D also exits\n"
    "Editing: arrows, Home/End, Ctrl-A/E, Ctrl-U (erase before cursor), Ctrl-W, Backspace/Delete.\n"
    "Approval: y/yes once, n/no deny, a/all allow this tool with ALL arguments for this chat process.\n"
    "          Enter chooses the capitalized Yes/no or yes/No default. Restart resets grants.\n"
    "Read-only automatic execution uses fixed system programs and literal arguments only.\n"
    "Code execution, writes and unrecognized commands require approval. /permissions reset revokes grants.\n"
    "Inline source via -c is not itself a saved script. Use /last, /pwd and /files for actual evidence.\n"
    "Each launch creates a new session unless --session ID is supplied; resume requires an ID.\n";
static int usage(void) {fputs(help_text,stderr);return 64;}
static int issue(int json,const char *text,int code) {
    if(json) {
        struct json_object *o=json_object_new_object();
        json_object_object_add(o,"status",json_object_new_string("error"));
        json_object_object_add(o,"text",json_object_new_string(text));
        print_json(o); json_object_put(o);
    } else fprintf(stderr,"%s\n",text);
    return code;
}
static void ack(const char *command,const char *value,int json) {
    if(!json) {puts(value);return;}
    struct json_object *o=json_object_new_object();
    json_object_object_add(o,command,json_object_new_string(value)); print_json(o); json_object_put(o);
}
static int fresh_id(char id[35]) {
    unsigned char random[16];
    if(!ah_os_random(random,sizeof(random))) return 0;
    id[0]='s'; for(unsigned i=0;i<sizeof(random);i++) snprintf(id+1+2*i,3,"%02x",random[i]);
    return 1;
}
/* Render untrusted text without terminal commands or bidi display overrides.
 * Keep ordinary multilingual text intact. Machine output is JSON encoded. */
static void display_n(FILE *file,const char *message,size_t n) {
    const unsigned char *p=(const unsigned char *)message;
    if(!isatty(fileno(file))) {fwrite(message,1,n,file); return;}
    for(size_t i=0;i<n;i++) {
        unsigned ch=p[i];
        if((ch<32 && ch!='\n' && ch!='\t') || ch==127) {
            fprintf(file,"\\x%02x",ch);
        } else if(ch==0xc2 && i+1<n && p[i+1]>=0x80 && p[i+1]<=0x9f) {
            fprintf(file,"\\u%04x",p[++i]);
        } else if(ch==0xe2 && i+2<n &&
            ((p[i+1]==0x80 && p[i+2]>=0xaa && p[i+2]<=0xae) ||
             (p[i+1]==0x81 && p[i+2]>=0xa6 && p[i+2]<=0xa9))) {
            unsigned cp=0x2000 | ((p[i+1]&63)<<6) | (p[i+2]&63);
            fprintf(file,"\\u%04x",cp); i+=2;
        } else fputc(ch,file);
    }
}
static void display(FILE *file,const char *message) {display_n(file,message,strlen(message));}
static int turn(dunlin_runtime *r,const ah_hooks *hooks,const char *session,const char *input,int json,const ah_png *image) {
    ((terminal *)hooks->user)->deadline=ah_os_now_ms(NULL)+dunlin_limits(r)->task_timeout_ms;
    terminal *t=hooks->user;
    cli_tools_begin_turn(&t->tools,input);
    cli_view_clear(&t->view);t->view.started=0;t->view.last_draw=0;t->view.used=0;t->view.executing=0;
    t->view.partial[0]=t->view.latest[0]=0;
    cli_view_heading(&t->view,"助手");
    ah_result result=dunlin_turn_for_model(r,hooks,session,input,image,&t->tools.service);
    end_reasoning(t);
    const char *message=result.text?result.text:"";
    if(json) {
        struct json_object *o=json_object_new_object();
        json_object_object_add(o,"status",json_object_new_string(ah_status_name(result.status)));
        json_object_object_add(o,"session",json_object_new_string(session));
        json_object_object_add(o,"text",json_object_new_string(message));
        print_json(o); json_object_put(o);
    } else {
        FILE *dest=result.status==AH_DONE?stdout:stderr;
        if(result.status!=AH_DONE) fprintf(dest,"%s ",ah_status_name(result.status));
        cli_view_heading(&t->view,result.status==AH_DONE?"答复":"未完成");
        if(t->view.enabled && isatty(fileno(dest))) {
            const char *line=message;
            do {
                size_t size=strcspn(line,"\n");fputs("  ",dest);display_n(dest,line,size);fputc('\n',dest);
                line+=size;
                if(!*line) break;
                line++;
            } while(*line);
        } else {display(dest,message);fputc('\n',dest);}
    }
    free(result.text);
    return result.status;
}
/* Standalone commands belong to the interactive terminal. Preserve all other
 * input verbatim, including whitespace in ordinary conversation and scripts. */
static int chat_exit_requested(const char *line) {
    static const char *const names[]={"/q","/quit","/exit"};
    const char *word=line+strspn(line," \t\r");
    size_t len=strcspn(word," \t\r");
    if(word[len+strspn(word+len," \t\r")]) return 0;
    for(size_t i=0;i<sizeof names/sizeof *names;i++)
        if(strlen(names[i])==len && !strncmp(word,names[i],len)) return 1;
    return 0;
}
static void files(const char *workspace) {
    int fd=open(workspace,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR *dir=fd>=0?fdopendir(fd):NULL;
    if(!dir) {if(fd>=0) close(fd);fputs("Cannot list the configured workspace.\n",stderr);return;}
    fprintf(stderr,"Workspace entries in %s\n",workspace);
    size_t count=0;
    struct dirent *entry;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        if(count++==100) {fputs("(first 100 entries shown)\n",stderr);break;}
        size_t size=strlen(workspace)+strlen(entry->d_name)+2;
        char *path=malloc(size);
        if(!path) break;
        snprintf(path,size,"%s/%s",workspace,entry->d_name);
        display(stderr,path);
        fputc('\n',stderr);free(path);
    }
    if(!count) fputs("(empty)\n",stderr);
    closedir(dir);
}
static const char *command_value(const char *line,const char *command) {
    size_t n=strlen(command);
    if(strncmp(line,command,n) || (line[n] && line[n]!=' ' && line[n]!='\t')) return NULL;
    return line+n+strspn(line+n," \t");
}
static void show_model(dunlin_runtime *r) {
    fputs("Model: ",stderr);display(stderr,dunlin_current_model(r));fputc('\n',stderr);
}
static void models(dunlin_runtime *r) {
    struct json_object *list=dunlin_models(r);
    if(!list) {fputs("Model inventory unavailable.\n",stderr);return;}
    size_t count=json_object_array_length(list);
    for(size_t i=0;i<count;i++) {
        struct json_object *entry=json_object_array_get_idx(list,i),*value;
        int active=json_object_object_get_ex(entry,"active",&value) && json_object_get_boolean(value);
        fputs(active?"* ":"  ",stderr);
        static const char *const fields[]={"id","model","protocol"};
        for(size_t j=0;j<sizeof fields/sizeof *fields;j++) {
            if(j) fputs("  ",stderr);
            if(json_object_object_get_ex(entry,fields[j],&value)) display(stderr,json_object_get_string(value));
        }
        fputc('\n',stderr);
    }
    json_object_put(list);
}
static int local_command(dunlin_runtime *r,terminal *t,const char *line) {
    const char *value;
    if(!strcmp(line,"/help") || !strcmp(line,"/h") || !strcmp(line,"/?"))
        fputs(help_text,stderr);
    else if(!strcmp(line,"/pwd")) {display(stderr,dunlin_workspace(r));fputc('\n',stderr);}
    else if(!strcmp(line,"/files")) files(dunlin_workspace(r));
    else if((value=command_value(line,"/models"))) {
        if(*value) fputs("Use /models\n",stderr); else models(r);
    } else if((value=command_value(line,"/model"))) {
        if(!*value || !strcmp(value,"status")) show_model(r);
        else {
            char error[180];
            if(!dunlin_select_model(r,value,error,sizeof error)) {display(stderr,error);fputc('\n',stderr);}
            else {
                show_model(r);
                fputs("Each model uses its own history. First use starts fresh; switching back resumes that model.\n",stderr);
            }
        }
    } else if((value=command_value(line,"/reasoning"))) {
        if(*value && strcmp(value,"on") && strcmp(value,"off")) {
            fputs("Use /reasoning [on|off]\n",stderr);return 1;
        }
        if(*value && !dunlin_set_show_reasoning(r,!strcmp(value,"on"))) {
            fputs("Cannot change reasoning display while a turn is active.\n",stderr);return 1;
        }
        fprintf(stderr,"Reasoning display: %s (only provider-returned text or summaries)\n",
            dunlin_show_reasoning(r)?"on":"off");
    } else if((value=command_value(line,"/effort"))) {
        char error[180];
        if(*value && !dunlin_set_reasoning_effort(r,value,error,sizeof error)) {
            display(stderr,error);fputc('\n',stderr);
        }
        fputs("Reasoning effort: ",stderr);display(stderr,dunlin_reasoning_effort(r));fputc('\n',stderr);
    }
    else if(!strcmp(line,"/last")) {
        if(!t->tools.last_name) fputs("No tool has executed in this process.\n",stderr);
        else {
            fprintf(stderr,"Last executed tool: %s; status: %s\nWorkspace: %s\nActual arguments: ",
                t->tools.last_name,ah_status_name(t->tools.last_status),dunlin_workspace(r));
            display(stderr,t->tools.last_args?t->tools.last_args:"unavailable");
            fputs("\nActual result: ",stderr);
            display(stderr,t->tools.last_result?t->tools.last_result:"unavailable");
            fputs("\nUse /files to verify saved files; executing inline source does not imply a script was saved.\n",stderr);
        }
    } else if(!strncmp(line,"/permissions",12) && (!line[12] || line[12]==' ')) {
        const char *value=line+12;value+=strspn(value," ");
        if(!strncmp(value,"mode ",5)) {
            dunlin_permission_mode mode;
            if(!dunlin_permission_mode_parse(value+5,&mode)||!dunlin_set_permission_mode(r,mode)) {
                fputs("Use /permissions mode request|agent|full-access\n",stderr);return 1;
            }
        }
        else if(!strcmp(value,"reset")) {json_object_put(t->grants);t->grants=json_object_new_object();}
        else if(!strcmp(value,"yes")) t->default_yes=1;
        else if(!strcmp(value,"no")) t->default_yes=0;
        else if(*value) {fputs("Use /permissions [mode request|agent|full-access|reset|yes|no]\n",stderr);return 1;}
        fprintf(stderr,"Mode: %s; read-only shell policy: %s; approval [%s]; current process grants: %s\n",
            dunlin_permission_mode_name(dunlin_get_permission_mode(r)),t->tools.automatic?"automatic":"strict",t->default_yes?"Yes/no":"yes/No",
            json_object_to_json_string_ext(t->grants,JSON_C_TO_STRING_PLAIN));
    } else return 0;
    return 1;
}
static int chat(dunlin_runtime *r,const ah_hooks *hooks,const char *session,int json,const char *required) {
    size_t size=dunlin_limits(r)->max_message_bytes;
    char *line=malloc(size+1);
    if(!line) return AH_FAILED;
    int tty=isatty(STDIN_FILENO);
    cli_input editor={0};
    if(tty && !cli_input_open(&editor,&interrupted)) {free(line);return AH_FAILED;}
    if(tty) {
        fprintf(stderr,"Workspace: %s\n",dunlin_workspace(r));show_model(r);
        fputs("Type /help for commands, /q to leave.\n",stderr);
    }
    int code=0,ch;
    for(;;) {
        if(interrupted) {code=AH_CANCELLED;break;}
        size_t n=0;
        if(tty) {
            char *edited=NULL;
            int status=cli_input_read(&editor,size,&edited);
            if(status<=0) {
                free(edited);
                if(interrupted) {code=AH_CANCELLED;break;}
                if(!status) break;
                issue(json,status==-2?"invalid UTF-8 input. Please re-enter this line":
                    status==-3?"input exceeds byte limit or contains NUL":"terminal input failed",
                    status==-3?AH_BUDGET:AH_FAILED);
                continue;
            }
            n=strlen(edited);memcpy(line,edited,n+1);free(edited);
            ch='\n';
        } else {
            while((ch=getchar())!=EOF && ch!='\n') {
                if(n==size || !ch) {code=issue(json,"input exceeds byte limit or contains NUL",AH_BUDGET);goto done;}
                line[n++]=(char)ch;
            }
        }
        if(interrupted) {code=AH_CANCELLED;break;}
        if(!n && ch==EOF) break;
        if(!n) continue;
        line[n]=0;
        if(tty && !ah_utf8_text(line,n)) {
            issue(json,"invalid UTF-8 input. Please re-enter this line",AH_FAILED);
            continue;
        }
        const char *operator_line=line+strspn(line," \t");
        if(tty && chat_exit_requested(operator_line)) break;
        if(tty && local_command(r,hooks->user,operator_line)) continue;
        char *input=line;
        int scoped=tty && !strncmp(operator_line,"/tool",5) && (!operator_line[5] || operator_line[5]==' ' || operator_line[5]=='\t');
        if(scoped) {
            char *name=line+(operator_line-line)+5;
            name+=strspn(name," \t");
            char *end=name+strcspn(name," \t");
            if(*end) *end++=0;
            input=end+strspn(end," \t");
            if(!*input || (required && strcmp(name,required)) || !dunlin_require_tool(r,name)) {
                issue(json,"expected /tool ENABLED_NAME REQUEST within the current tool policy",64);
                continue;
            }
        }
        if(tty && operator_line[0]=='/' && !scoped) {
            issue(json,"unknown local command; use /help",64);
            continue;
        }
        code=turn(r,hooks,session,input,json,NULL);
        if(scoped && !required) dunlin_require_tool(r,NULL);
        if(code && !tty) break;
        /* The turn has reported its own outcome. A TTY stays open for fresh input. */
        code=0;
    }
done:
    if(tty) cli_input_close(&editor);
    free(line); return code;
}
static int inventory(dunlin_runtime *r,const char *cmd,int json) {
    ak_scope *scope=dunlin_scope(r);
    struct json_object *v=NULL;
    if(!strcmp(cmd,"sessions")) v=dunlin_conversations(r);
    else if(!strcmp(cmd,"tools")) {
        const ah_tool_service *tools=ak_get(scope,"tools",1);
        if(tools) v=tools->schema(tools->ctx);
    } else if(!strcmp(cmd,"plugins")) v=dunlin_plugins(r);
    else {
        const dunlin_model_v1 *model=ak_get(scope,"model",1);
        v=json_object_new_object();
        json_object_object_add(v,"version",json_object_new_string(DUNLIN_VERSION));
        json_object_object_add(v,"curl",json_object_new_string(curl_version()));
        json_object_object_add(v,"plugins",dunlin_plugins(r));
        json_object_object_add(v,"model_configured",json_object_new_boolean(model!=NULL));
        json_object_object_add(v,"key_available",json_object_new_boolean(model &&
            (!model->config.key_env || (getenv(model->config.key_env) && *getenv(model->config.key_env)))));
        json_object_object_add(v,"network_probed",json_object_new_boolean(0));
    }
    if(!v) return issue(json,"requested service unavailable",AH_FAILED);
    print_json(v); json_object_put(v); return 0;
}
static unsigned char *read_image(const char *path,size_t cap,ah_png *image) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    if(fd<0) return NULL;
    struct stat st;
    unsigned char *data=NULL;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0 && (uintmax_t)st.st_size<=cap && st.st_size<=1024*1024) {
        size_t n=(size_t)st.st_size,used=0;
        data=malloc(n);
        while(data && used<n) {
            ssize_t got=read(fd,data+used,n-used);
            if(got<0 && errno==EINTR) continue;
            if(got<=0) {free(data); data=NULL; break;}
            used+=(size_t)got;
        }
        if(data) *image=(ah_png){data,n};
    }
    close(fd);
    return data;
}
int main(int argc,char **argv) {
    const char *cfg="dunlin.json",*session=NULL,*image_path=NULL,*required=NULL,*model=NULL;
    int i=1,json=0,automatic=1;
    dunlin_permission_mode permission_mode=DUNLIN_PERMISSION_REQUEST;
    terminal t={0};
    for(;i<argc && (!strncmp(argv[i],"--",2) || !strcmp(argv[i],"-h"));i++) {
        if(!strcmp(argv[i],"--config") && i+1<argc) cfg=argv[++i];
        else if(!strcmp(argv[i],"--require-tool") && i+1<argc) required=argv[++i];
        else if(!strcmp(argv[i],"--image") && i+1<argc) image_path=argv[++i];
        else if(!strcmp(argv[i],"--permissions") && i+1<argc) {
            const char *mode=argv[++i];
            if(!strcmp(mode,"strict")){automatic=0;permission_mode=DUNLIN_PERMISSION_REQUEST;}
            else if(!strcmp(mode,"read-only")){automatic=1;permission_mode=DUNLIN_PERMISSION_REQUEST;}
            else if(!dunlin_permission_mode_parse(mode,&permission_mode))return usage();
        }
        else if(!strcmp(argv[i],"--approval-default") && i+1<argc) {
            const char *value=argv[++i];
            if(strcasecmp(value,"yes") && strcasecmp(value,"no")) return usage();
            t.default_yes=!strcasecmp(value,"yes");
        }
        else if(!strcmp(argv[i],"--session") && i+1<argc) session=argv[++i];
        else if(!strcmp(argv[i],"--model") && i+1<argc) model=argv[++i];
        else if(!strcmp(argv[i],"--json")) json=1;
        else if(!strcmp(argv[i],"--events")) {t.events=1;json=1;}
        else if(!strcmp(argv[i],"--version")) {ack("version",DUNLIN_VERSION,json);return 0;}
        else if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) {ack("help",help_text,json);return 0;}
        else return usage();
    }
    if(i>=argc) return usage();
    const char *cmd=argv[i++];
    int run=!strcmp(cmd,"run"), resume=!strcmp(cmd,"resume"), interact=!strcmp(cmd,"chat");
    int args=argc-i;
    if(required && !run && !resume && !interact) return usage();
    if(image_path && ((!run && !resume) || args!=1)) return usage();
    if((run && args!=1) || (resume && (!session || args>1)) || (!run && !resume && args!=0)) return usage();
    char error[180],id[35];
    if(session && (!*session || strlen(session)>64 || session[strspn(session,
       "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")]))
        return issue(json,"invalid session id",64);
    if(!strcmp(cmd,"help")) {if(args) return usage();ack("help",help_text,json);return 0;}
    if(!strcmp(cmd,"version")) {ack("version",DUNLIN_VERSION,json);return 0;}
    if(!strcmp(cmd,"init")) {
        if(!dunlin_init_config(cfg,error,sizeof(error))) return issue(json,error,64);
        ack("config",cfg,json);return 0;
    }
    if(!run && !resume && !interact && strcmp(cmd,"validate") && strcmp(cmd,"doctor") &&
        strcmp(cmd,"sessions") && strcmp(cmd,"tools") && strcmp(cmd,"plugins")) return usage();
    struct sigaction sa={0};
    sa.sa_handler=interrupt; sigemptyset(&sa.sa_mask); sigaction(SIGINT,&sa,NULL);
    t.interactive=(interact || resume) && isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
    setvbuf(stdin,NULL,_IONBF,0);
    ah_hooks hooks={.cancelled=cancelled,.approve=approve,.event=observe,.user=&t};
    dunlin_runtime *r=dunlin_open(cfg,&hooks,error,sizeof(error));
    if(!r) return issue(json,error,64);
    if(!dunlin_set_permission_mode(r,permission_mode)){dunlin_close(r);return issue(json,"cannot set permission mode",64);}
    t.runtime=r;t.json=json;
    cli_view_init(&t.view,t.interactive && !json && !t.events);
    if(model && !dunlin_select_model(r,model,error,sizeof error)) {dunlin_close(r);return issue(json,error,64);}
    if(required && !dunlin_require_tool(r,required)) {dunlin_close(r); return issue(json,"required tool unavailable",64);}
    int rc=0;
    if(run || resume || interact) {
        t.grants=json_object_new_object();
        const ah_tool_service *base=ak_get(dunlin_scope(r),"tools",1);
        if(!t.grants || !cli_tools_open(&t.tools,base,dunlin_workspace(r),automatic)) {
            json_object_put(t.grants);dunlin_close(r);return issue(json,"cannot initialize CLI tool adapter",AH_FAILED);
        }

        t.tools.activity=cli_view_activity;t.tools.activity_user=&t.view;
        if(!session && fresh_id(id)) session=id;
        if(!session) rc=issue(json,"cannot create session id",AH_FAILED);
        else {
            if(!json) fprintf(stderr,"session %s\n",session);
            ah_png image={0};
            unsigned char *bytes=NULL;
            if(image_path) bytes=read_image(image_path,dunlin_limits(r)->max_message_bytes,&image);
            if(image_path && !bytes) rc=issue(json,"cannot read PNG within configured byte limit",AH_FAILED);
            else rc=interact?chat(r,&hooks,session,json,required):turn(r,&hooks,session,args?argv[i]:NULL,json,bytes?&image:NULL);
            free(bytes);
        }
    } else if(!strcmp(cmd,"validate")) ack("status","valid",json);
    else rc=inventory(r,cmd,json);
    if(run || resume || interact) {cli_tools_close(&t.tools);json_object_put(t.grants);}
    dunlin_close(r);
    return rc;
}
