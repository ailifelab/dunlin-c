#define _POSIX_C_SOURCE 200809L
#include <dunlin/compact/runtime.h>
#include <assert.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Third-party loop mounted through precisely the API used by the stock host. */
static ah_result alternative(const ah_agent *agent,const char *id,const char *input) {
    assert(agent->tools && agent->sessions && agent->model && !strcmp(id,"sample"));
    assert(!strcmp(input,"hello"));
    return (ah_result){AH_DONE,strdup("external loop")};
}
static int mount(ak_scope *s,void *cfg) {
    return !ak_provide(s,"loop",1,cfg);
}
static ak_value observe(void *user,const void *data,ak_next *next) {
    int *seen=user;
    const dunlin_event_v1 *event=data;
    assert(!next && !strcmp(event->session_id,"sample"));
    assert(!strcmp(event->type,*seen?"turn.end":"turn.start"));
    (*seen)++;
    return (ak_value){0};
}
int main(void) {
    char cfg_dir[]="/tmp/dunlin-embed-XXXXXX",error[128];
    assert(mkdtemp(cfg_dir) && !chdir(cfg_dir));
    assert(dunlin_init_config("profile.json",error,sizeof(error)));
    FILE *f=fopen("profile.json","w");
    assert(f && fputs("{\"base_url\":\"http://127.0.0.1/v1\",\"model\":\"offline\","
        "\"plugins\":[\"sessions\",\"clock\",\"model\",\"permission\",\"tools\"]}",f)>=0 && !fclose(f));
    dunlin_runtime *r=dunlin_open("profile.json",NULL,error,sizeof(error));
    assert(r);
    assert(dunlin_workspace(r) && strstr(dunlin_workspace(r),"/work"));
    const ah_tool_service *tools=ak_get(dunlin_scope(r),"tools",1);
    struct json_object *args=json_tokener_parse("{\"executable\":\"/bin/echo\",\"argv\":[\"hello\"]}");
    int approval=0,effect=0;
    assert(tools && tools->check(tools->ctx,"exec",args,&approval,&effect,error,sizeof error)==AH_DONE);
    assert(approval && effect); /* CLI policy must not leak into a C host. */
    json_object_put(args);
    int seen=0;
    ak_scope *root=dunlin_scope(r),*installed=NULL;
    ak_context *ctx=dunlin_kernel(r);
    assert(!ak_get(root,"loop",1));
    const ak_key keys[]={{"loop",1},{0}};
    ak_plugin plugin={.abi=AK_ABI,.name="external-loop",.provides=keys,.start=mount};
    dunlin_loop_v1 table={alternative};
    assert(!ak_install(ctx,&plugin,&table,&installed));
    ak_handle listener=ak_on(installed,"agent/event",observe,&seen,0);
    assert(listener);
    ah_result result=dunlin_turn(root,dunlin_limits(r),NULL,"sample","hello");
    assert(result.status==AH_DONE && !strcmp(result.text,"external loop") && seen==2);
    free(result.text);
    assert(!ak_remove(installed) && !ak_get(root,"loop",1) && !ak_revoke(ctx,listener));
    dunlin_close(r);
    assert(!unlink("profile.json") && !rmdir("work") && !rmdir("sessions") && !chdir("/") && !rmdir(cfg_dir));
    puts("External C host: installed replacement loop, observed scoped events, revoked plugin");
    return 0;
}
