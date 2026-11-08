#define _GNU_SOURCE
#include "dunlin/compact/session.h"
#include "dunlin/compact/tools.h"
#include <json-c/json.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

typedef struct json_object J;
static void ok(ah_result r,ah_status st,const char *text) {
    if(r.status!=st || !r.text || (text && !strstr(r.text,text))) {
        fprintf(stderr,"unexpected %s: %s (wanted %s, %s)\n",ah_status_name(r.status),
            r.text?r.text:"NULL",ah_status_name(st),text?text:"any");
        abort();
    }
    free(r.text);
}
static void put_file(const char *name,const char *text) {
    FILE *f=fopen(name,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static int rm_item(const char *path,const struct stat *st,int flag,struct FTW *walk) {
    (void)st; (void)flag; (void)walk; return remove(path);
}
static int approved,force_cancel;
static int approve(void *p,const char *name,const char *raw) {
    (void)p;
    J *review=json_tokener_parse(raw),*exe=NULL,*argv=NULL;
    assert(!strcmp(name,"exec") && json_object_object_get_ex(review,"executable",&exe));
    assert(!strcmp(json_object_get_string(exe),"/bin/sh"));
    assert(json_object_object_get_ex(review,"argv",&argv));
    assert(!strcmp(json_object_get_string(json_object_array_get_idx(argv,1)),
        "(sleep 0.5; echo escaped > late) & echo armed > armed; wait"));
    json_object_put(review);
    return approved;
}
static int cancel(void *p) {
    (void)p; return force_cancel && access("work/armed",F_OK)==0;
}
/* Cross the injected deadline only after the subprocess actually starts.
 * A real-time backstop makes failure to reach the marker a bounded failure. */
static uint64_t deadline_start;
static uint64_t armed_clock(void *ctx) {
    if(access("work/armed",F_OK)==0) return 100;
    return ah_linux_now_ms(ctx)-deadline_start>=5000?100:0;
}
static int model_requests;
static dunlin_model_status mock(const dunlin_model_config *cfg,J *msgs,J *tools,
        const dunlin_model_events *events,J **answer,char *error,size_t size) {
    (void)cfg;(void)tools;(void)events;(void)error;(void)size;
    model_requests++;
    int n=json_object_array_length(msgs);
    J *last=json_object_array_get_idx(msgs,n-1),*role=NULL;
    json_object_object_get_ex(last,"role",&role);
    if(!strcmp(json_object_get_string(role),"tool")) {
        *answer=json_tokener_parse("{\"role\":\"assistant\",\"content\":\"ok\"}");
        return DUNLIN_MODEL_OK;
    }
    *answer=json_tokener_parse("{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"effect1\",\"type\":\"function\",\"function\":{\"name\":\"exec\"}}]}");
    J *batch,*fn,*args=json_object_new_object(),*argv=json_object_new_array();
    json_object_object_get_ex(*answer,"tool_calls",&batch);
    json_object_object_get_ex(json_object_array_get_idx(batch,0),"function",&fn);
    json_object_object_add(args,"executable",json_object_new_string("/bin/sh"));
    json_object_array_add(argv,json_object_new_string("-c"));
    json_object_array_add(argv,json_object_new_string("(sleep 0.5; echo escaped > late) & echo armed > armed; wait"));
    json_object_object_add(args,"argv",argv);
    json_object_object_add(fn,"arguments",json_object_new_string(json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN)));
    json_object_put(args);
    return DUNLIN_MODEL_OK;
}
static void run_tool(ah_tool_service *tools,const ah_hooks *hooks,const char *name,
                     const char *args,size_t limit,ah_status status,const char *wanted) {
    J *a=json_tokener_parse(args);
    char *out=NULL;
    ah_status rc=tools->call(tools->ctx,name,a,hooks,limit,5000,&out);
    ok((ah_result){rc,out},status,wanted);
    json_object_put(a);
}
int main(void) {
    char temp[]="/tmp/dunlin-agent-test-XXXXXX";
    assert(mkdtemp(temp)); assert(!chdir(temp)); assert(!mkdir("work",0700));
    assert(!setenv("HOME",temp,1));
#ifdef __linux__
    /* Observe adopted descendants as well as direct children on Linux. */
    assert(!prctl(PR_SET_CHILD_SUBREAPER,1));
#endif
    ah_session_service sessions; ah_tool_service tools;
    assert(!ah_linux_sessions(&sessions,"sessions")); assert(!ah_linux_tools(&tools,"work"));
    ah_hooks hooks={.approve=approve,.now_ms=ah_linux_now_ms,.cancelled=cancel};
    dunlin_model_config model={.timeout_ms=5000};
    ah_agent a={mock,&model,&tools,&sessions,hooks,ah_default_limits()};
    void *lease,*second; J *state=NULL;
    assert(sessions.open(sessions.ctx,"locked",&lease,&state)==AH_DONE && !state);
    assert(sessions.open(sessions.ctx,"locked",&second,&state)==AH_BUSY);
    assert(sessions.open(sessions.ctx,"../locked",&second,&state)==AH_FAILED);
    ok(ah_agent_run(&a,"locked","test"),AH_BUSY,NULL);
    sessions.close(lease);

    ok(ah_agent_run(&a,"deny","bad \377"),AH_FAILED,"UTF-8");
    ok(ah_agent_run(&a,"deny","test"),AH_DENIED,"denied");
    assert(access("work/armed",F_OK));
    ok(ah_agent_run(&a,"deny",NULL),AH_DENIED,NULL);
    approved=1;
    a.limits.tool_timeout_ms=100;
    deadline_start=ah_linux_now_ms(NULL); a.hooks.now_ms=armed_clock;
    ok(ah_agent_run(&a,"timeout","test"),AH_UNKNOWN,"timeout");
    a.hooks.now_ms=ah_linux_now_ms;
    assert(!access("work/armed",F_OK));
    unlink("work/armed");
    int saved=model_requests;
    ok(ah_agent_run(&a,"timeout",NULL),AH_UNKNOWN,"unresolved");
    ok(ah_agent_run(&a,"timeout","again"),AH_UNKNOWN,"unresolved");
    assert(saved==model_requests);
    force_cancel=1; a.limits.tool_timeout_ms=10000;
    ok(ah_agent_run(&a,"cancel","test"),AH_UNKNOWN,"cancelled");
    force_cancel=0;
    usleep(650000);
    assert(access("work/late",F_OK));
    int leftover; assert(waitpid(-1,&leftover,WNOHANG)<0 && errno==ECHILD);
    ok(ah_agent_run(&a,"cancel",NULL),AH_UNKNOWN,NULL);
    puts("PASS review binding, exclusive session, tool deadline, cancellation, descendant kill and durable no replay");

    run_tool(&tools,&hooks,"write_file","{\"path\":\"ok\",\"text\":\"data\"}",100,AH_DONE,"written");
    run_tool(&tools,&hooks,"read_file","{\"path\":\"ok\"}",100,AH_DONE,"data");
    run_tool(&tools,&hooks,"read_file","{\"path\":\"ok\"}",3,AH_BUDGET,"limit");
    put_file("work/invalid","\xff");
    run_tool(&tools,&hooks,"read_file","{\"path\":\"invalid\"}",100,AH_FAILED,"UTF-8");
    put_file("outside","secret");
    assert(!symlink("../outside","work/sym")); assert(!link("outside","work/hard"));
    assert(!mkfifo("work/fifo",0600));
    const char *bad[]={"../outside","/etc/passwd","sym","hard","fifo",".","sub/file"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) {
        char args[256]; snprintf(args,sizeof args,"{\"path\":\"%s\"}",bad[i]);
        run_tool(&tools,&hooks,"read_file",args,100,AH_FAILED,NULL);
        snprintf(args,sizeof args,"{\"path\":\"%s\",\"text\":\"changed\"}",bad[i]);
        run_tool(&tools,&hooks,"write_file",args,100,AH_FAILED,NULL);
    }
    pid_t racer=fork();
    assert(racer>=0);
    if (!racer) {
        for(int i=0;i<400;i++) {
            unlink("work/next"); assert(!symlink("../outside","work/next"));
            assert(!rename("work/next","work/race"));
            put_file("work/next","public"); assert(!rename("work/next","work/race"));
        }
        _exit(0);
    }
    J *path=json_tokener_parse("{\"path\":\"race\"}");
    J *written=json_tokener_parse("{\"path\":\"race\",\"text\":\"public\"}");
    for(int i=0;i<400;i++) {
        char *out=NULL;
        ah_status st=tools.call(tools.ctx,"read_file",path,&hooks,100,1000,&out);
        assert(st==AH_FAILED || (st==AH_DONE && !strcmp(out,"public")));
        free(out); out=NULL;
        st=tools.call(tools.ctx,"write_file",written,&hooks,100,1000,&out);
        assert(st==AH_DONE || st==AH_FAILED); free(out);
    }
    json_object_put(path);json_object_put(written);
    int status; assert(waitpid(racer,&status,0)==racer && WIFEXITED(status) && !WEXITSTATUS(status));
    FILE *file=fopen("outside","r"); assert(file); char line[64]; assert(fgets(line,sizeof line,file));
    assert(!strcmp(line,"secret")); fclose(file);
    assert(!setenv("DUNLIN_TEST_PRIVATE","secret",1));
    char *child_home=NULL;
    J *home_args=json_tokener_parse("{\"executable\":\"/bin/sh\",\"argv\":[\"-c\",\"printf %s \\\"$HOME\\\"\"]}");
    assert(tools.call(tools.ctx,"exec",home_args,&hooks,1024,1000,&child_home)==AH_DONE);
    assert(child_home && !strcmp(child_home,temp));
    free(child_home); json_object_put(home_args);
    run_tool(&tools,&hooks,"exec","{\"executable\":\"/bin/sh\",\"argv\":[\"-c\",\"test -z \\\"$DUNLIN_TEST_PRIVATE\\\" && printf clean\"]}",100,AH_DONE,"clean");
    assert(!ah_linux_tool_enable(&tools,"shell",1));
    J *listing=json_tokener_parse("{\"command\":\"printf BEGIN; i=0; while [ $i -lt 50 ]; do printf 目录; i=$((i+1)); done; printf END; exit 17\"}");
    char *preview=NULL;
    assert(tools.call(tools.ctx,"shell",listing,&hooks,89,5000,&preview)==AH_EXEC_FAILED);
    assert(strlen(preview)<=89 && ah_utf8_text(preview,strlen(preview)));
    assert(!strncmp(preview,"exit_code=17\nBEGIN",18) && strstr(preview,"[output truncated]"));
    assert(!strcmp(preview+strlen(preview)-3,"END"));
    free(preview); json_object_put(listing);
    run_tool(&tools,&hooks,"shell","{\"command\":\"yes x\"}",80,AH_UNKNOWN,"output limit");
    assert(!ah_linux_tool_enable(&tools,"exec",0));
    run_tool(&tools,&hooks,"exec","{\"executable\":\"/bin/true\",\"argv\":[]}",100,AH_FAILED,"disabled");
    ah_linux_tools_close(&tools); ah_linux_sessions_close(&sessions);
    assert(!chdir("/")); assert(!nftw(temp,rm_item,16,FTW_DEPTH|FTW_PHYS));
    puts("PASS capped file reads, safe replacement, traversal, FIFO, hardlink, symlink swaps, clean child environment and tool revocation");
    return 0;
}
