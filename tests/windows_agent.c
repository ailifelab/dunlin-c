#include "dunlin/compact/agent.h"
#include "dunlin/compact/http.h"
#include "dunlin/compact/session.h"
#include "../dunlin-compact/platform/clock.h"
#include <json-c/json.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int requests,calls; } fixture;
static struct json_object *schemas(void *opaque) {
    (void)opaque;
    return json_tokener_parse("[{\"type\":\"function\",\"function\":{\"name\":\"echo\",\"description\":\"echo\",\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"],\"additionalProperties\":false}}}]");
}
static ah_status check(void *opaque,const char *name,struct json_object *args,int *approval,int *effect,char *error,size_t size) {
    (void)opaque;(void)error;(void)size;struct json_object *text=NULL;
    if(strcmp(name,"echo")||!json_object_object_get_ex(args,"text",&text)||!json_object_is_type(text,json_type_string))return AH_INVALID_ARGUMENTS;
    *approval=*effect=0;return AH_DONE;
}
static ah_status call(void *opaque,const char *name,struct json_object *args,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out) {
    (void)hooks;(void)timeout;fixture *f=opaque;struct json_object *text=NULL;
    assert(!strcmp(name,"echo")&&json_object_object_get_ex(args,"text",&text));const char *value=json_object_get_string(text);
    size_t n=strlen(value);if(n>cap)return AH_BUDGET;*out=malloc(n+1);assert(*out);memcpy(*out,value,n+1);f->calls++;return AH_DONE;
}
static dunlin_model_status model(const dunlin_model_config *cfg,struct json_object *messages,struct json_object *tools,
        const dunlin_model_events *events,struct json_object **answer,char *error,size_t size) {
    (void)tools;(void)events;(void)error;(void)size;fixture *f=cfg->http->user;f->requests++;
    struct json_object *last=json_object_array_get_idx(messages,json_object_array_length(messages)-1),*role=NULL;
    assert(json_object_object_get_ex(last,"role",&role));
    if(!strcmp(json_object_get_string(role),"tool"))
        *answer=json_tokener_parse("{\"role\":\"assistant\",\"content\":\"Windows 工具闭环完成\"}");
    else
        *answer=json_tokener_parse("{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"echo-1\",\"type\":\"function\",\"function\":{\"name\":\"echo\",\"arguments\":\"{\\\"text\\\":\\\"你好\\\"}\"}}]}");
    return *answer?DUNLIN_MODEL_OK:DUNLIN_MODEL_LIMIT;
}
int main(void) {
    char temp[MAX_PATH],dir[MAX_PATH];assert(GetTempPathA(MAX_PATH,temp));snprintf(dir,sizeof dir,"%sdunlin-agent-%lu",temp,(unsigned long)GetCurrentProcessId());
    ah_session_service sessions={0};assert(!ah_windows_sessions(&sessions,dir));fixture f={0};
    dunlin_http_v1 marker={.user=&f};dunlin_model_config config={.http=&marker,.timeout_ms=5000};
    ah_tool_service tools={&f,schemas,check,call};ah_agent agent={model,&config,&tools,&sessions,{.now_ms=ah_os_now_ms},ah_default_limits()};
    ah_result result=ah_agent_run(&agent,"conversation","请回显你好");
    assert(result.status==AH_DONE&&result.text&&!strcmp(result.text,"Windows 工具闭环完成"));free(result.text);
    assert(f.requests==2&&f.calls==1);
    result=ah_agent_run(&agent,"conversation",NULL);assert(result.status==AH_DONE&&!strcmp(result.text,"Windows 工具闭环完成"));free(result.text);
    assert(f.requests==2&&f.calls==1);
    void *lease=NULL;struct json_object *state=NULL;assert(sessions.open(sessions.ctx,"conversation",&lease,&state)==AH_DONE&&state);json_object_put(state);
    result=ah_agent_run(&agent,"conversation","busy");assert(result.status==AH_BUSY);free(result.text);sessions.close(lease);
    ah_windows_sessions_close(&sessions);char path[MAX_PATH];
    snprintf(path,sizeof path,"%s\\conversation.json",dir);assert(DeleteFileA(path));snprintf(path,sizeof path,"%s\\conversation.lock",dir);assert(DeleteFileA(path));assert(RemoveDirectoryA(dir));
    puts("Windows deterministic Agent tool loop passed");return 0;
}
