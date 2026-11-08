#define _POSIX_C_SOURCE 200809L
#include "dunlin/compact/runtime.h"
#include "dunlin/compact/protocol.h"
#include "dunlin/compact/session.h"
#include "../dunlin-compact/runtime/profile_hash.h"
#include <json-c/json.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static dunlin_runtime *current;
static unsigned calls,native_calls,reasoning_events,kernel_reasoning,metrics_events,starts;
static size_t sent_bytes;
static const char *public_session="conversation";
static int require_echo;
#ifdef DUNLIN_PROFILE_FAULTS
static const char *fail_metadata_key;
static ah_session_service *rename_store;
static void *rename_writer;
static struct json_object *rename_replacement;
static int rename_after_open;
int dunlin_profile_test_openat(int directory,const char *name,int flags,...) {
    int fd;
    if(flags&O_CREAT) {
        va_list args;va_start(args,flags);mode_t mode=(mode_t)va_arg(args,int);va_end(args);
        fd=openat(directory,name,flags,mode);
    } else fd=openat(directory,name,flags);
    if(fd>=0 && rename_after_open && !strcmp(name,"rename-race.json")) {
        rename_after_open=0;CHECK(!rename_store->save(rename_writer,rename_replacement));
        struct stat st;CHECK(!fstat(fd,&st)&&st.st_nlink==0);
    }
    return fd;
}
/* 此二进制只对 runtime.o 的 JSON 键插入边界注入故障，不替换会话保存逻辑。 */
#if JSON_C_VERSION_NUM >= 0x000d00
int dunlin_profile_test_add(struct json_object *o,const char *key,struct json_object *value) {
    if(fail_metadata_key&&!strcmp(key,fail_metadata_key)){fail_metadata_key=NULL;return -1;}
    return json_object_object_add(o,key,value);
}
#else
void dunlin_profile_test_add(struct json_object *o,const char *key,struct json_object *value) {
    if(fail_metadata_key&&!strcmp(key,fail_metadata_key)){fail_metadata_key=NULL;return;}
    json_object_object_add(o,key,value);
}
#endif
#endif
static struct json_object *field(struct json_object *o,const char *key) {
    struct json_object *v=NULL;if(o)json_object_object_get_ex(o,key,&v);return v;
}
static const char *str(struct json_object *o,const char *key){return json_object_get_string(field(o,key));}
static void reject_reentry(void) {
    char error[256];const char *id=dunlin_current_model(current),*effort=dunlin_reasoning_effort(current);
    int shown=dunlin_show_reasoning(current);
    CHECK(!dunlin_select_model(current,"beta",error,sizeof error)&&strstr(error,"busy"));
    CHECK(!strcmp(id,dunlin_current_model(current)));
    CHECK(!dunlin_set_reasoning_effort(current,"high",error,sizeof error)&&strstr(error,"busy"));
    CHECK(!strcmp(effort,dunlin_reasoning_effort(current)));
    CHECK(!dunlin_set_show_reasoning(current,!shown)&&shown==dunlin_show_reasoning(current));
    CHECK(!dunlin_require_tool(current,NULL));CHECK(dunlin_close(current)==AK_BUSY);
}
/* 仅替换网络边界；真实配置、协议、Loop、工具和会话参与验证。 */
static dunlin_http_result fake_post(void *unused,const dunlin_http_request *req,long *status) {
    (void)unused;calls++;sent_bytes=strlen(req->json);CHECK(strstr(req->url,"/chat/completions"));
    CHECK(req->bearer&&!strcmp(req->bearer,"fixture-secret-not-real"));reject_reentry();
    struct json_object *body=json_tokener_parse(req->json);CHECK(body);const char *model=str(body,"model");CHECK(model);
    if(!strcmp(model,"alpha-model")) {
        CHECK(!strstr(req->json,"beta-only"));CHECK(json_object_get_int(field(body,"max_tokens"))==64);
        CHECK(json_object_get_double(field(body,"temperature"))==0.25);
        CHECK(json_object_get_boolean(field(body,"enable_thinking")));
        CHECK(!strcmp(str(field(body,"chat_template_kwargs"),"chat_template"),"fixture-template"));
        CHECK(!strcmp(str(body,"reasoning_effort"),"low"));
    } else CHECK(!strcmp(model,"beta-model")&&!strstr(req->json,"alpha-only"));
    const char *wire="{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"fixture answer\",\"reasoning_content\":\"public fixture note\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":2,\"total_tokens\":13}}";
    if(require_echo&&field(body,"tool_choice")) {
        CHECK(!strcmp(str(field(field(body,"tool_choice"),"function"),"name"),"echo"));
        wire="{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"fixture-call\",\"type\":\"function\",\"function\":{\"name\":\"echo\",\"arguments\":\"{\\\"text\\\":\\\"echoed\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":2,\"total_tokens\":13}}";
    }
    json_object_put(body);*status=200;return req->body(req->user,wire,strlen(wire))?DUNLIN_HTTP_OK:DUNLIN_HTTP_FAILED;
}
static dunlin_http_result fake_post_v2(void *unused,const dunlin_http_request_v2 *req,long *status) {
    (void)unused;native_calls++;sent_bytes=strlen(req->request.json);CHECK(strstr(req->request.url,"/responses"));
    reject_reentry();*status=503;return DUNLIN_HTTP_FAILED;
}
const dunlin_http_v1 dunlin_curl_http={fake_post,NULL};
const dunlin_http_v2 dunlin_curl_http_v2={fake_post_v2,NULL};
static void observer(void *unused,const char *type,const char *text,size_t n) {
    (void)unused;
    if(!strcmp(type,"turn.start")) {
        starts++;reject_reentry();ah_result nested=dunlin_turn_for_model(current,NULL,public_session,"nested",NULL,NULL);
        CHECK(nested.status==AH_BUSY&&!nested.text);
        nested=dunlin_turn(dunlin_scope(current),dunlin_limits(current),NULL,"nested","nested");CHECK(nested.status==AH_BUSY&&!nested.text);
    } else if(!strcmp(type,"reasoning.delta")) {
        reasoning_events++;CHECK(n==strlen("public fixture note")&&!memcmp(text,"public fixture note",n));
    } else if(!strcmp(type,"model.metrics")) {
        struct json_object *data=json_tokener_parse(text);CHECK(data&&strlen(text)==n);
        CHECK((size_t)json_object_get_int64(field(data,"request_bytes"))==sent_bytes);
        if(json_object_get_int(field(data,"usage_present"))) {
            CHECK(json_object_get_boolean(field(data,"response_bytes_known")));
            CHECK(json_object_get_int(field(data,"input_tokens"))==11);CHECK(json_object_get_int(field(data,"output_tokens"))==2);
            CHECK(json_object_get_int(field(data,"total_tokens"))==13);
        } else CHECK(!json_object_get_boolean(field(data,"response_bytes_known")));
        json_object_put(data);metrics_events++;
    }
}
static ak_value kernel_event(void *unused,const void *data,ak_next *next) {
    (void)unused;CHECK(!next);const dunlin_event_v1 *event=data;CHECK(!strcmp(event->session_id,public_session));
    if(!strcmp(event->type,"reasoning.delta"))kernel_reasoning++;
    return (ak_value){0};
}
static void write_config(const char *text){FILE *f=fopen("config.json","w");CHECK(f&&fputs(text,f)>=0&&!fclose(f));}
static const char profile_config[]=
    "{\"base_url\":\"http://fixture/v1\",\"model\":\"shared-model\",\"key_env\":\"PROFILE_SECRET_ENV\",\"max_tokens\":64,\"stream\":false,\"active_model\":\"alpha\",\"models\":{"
    "\"alpha\":{\"model\":\"alpha-model\",\"temperature\":0.25,\"enable_thinking\":true,\"omlx_chat_template\":\"fixture-template\",\"chat_tool_metadata\":\"complete\",\"reasoning_capabilities\":[\"none\",\"levels\"]},"
    "\"beta\":{\"model\":\"beta-model\",\"base_url\":\"http://other/v1\"},\"native\":{\"protocol\":\"responses\"},\"unsupported\":{\"protocol\":\"gemini\"},\"bad-native\":{\"protocol\":\"anthropic\",\"temperature\":0.5}}}";
static struct json_object *snapshot(const char *id) {
    char path[96];snprintf(path,sizeof path,"sessions/%s.json",id);struct json_object *s=json_object_from_file(path);CHECK(s);return s;
}
static void run(const char *input) {
    ah_hooks hooks={.event=observer};ah_result result=dunlin_turn_for_model(current,&hooks,public_session,input,NULL,NULL);
    if(result.status!=AH_DONE)fprintf(stderr,"turn failed: %s\n",result.text?result.text:"(no detail)");
    CHECK(result.status==AH_DONE&&result.text&&!strcmp(result.text,"fixture answer"));free(result.text);
}
static void hash_vectors(void) {
    char a[65],b[65];dunlin_profile_sha256("",0,a);CHECK(!strcmp(a,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    dunlin_profile_sha256("abc",3,a);CHECK(!strcmp(a,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *longer="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    dunlin_profile_sha256(longer,strlen(longer),a);CHECK(!strcmp(a,"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    char *million=malloc(1000000);CHECK(million);memset(million,'a',1000000);dunlin_profile_sha256(million,1000000,a);free(million);
    CHECK(!strcmp(a,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    CHECK(dunlin_profile_session_id("ab","c","chat","model","endpoint",a));
    CHECK(dunlin_profile_session_id("a","bc","chat","model","endpoint",b)&&strcmp(a,b));
    CHECK(dunlin_profile_session_id("ab","c","responses","model","endpoint",b)&&strcmp(a,b));
    CHECK(dunlin_profile_session_id("ab","c","chat","model","another",b)&&strcmp(a,b));
    CHECK(dunlin_profile_session_id("ab","c","chat","different","endpoint",b)&&strcmp(a,b));
    CHECK(strlen(a)==64&&strspn(a,"0123456789abcdef")==64);
}
static void legacy(void) {
    char error[256],id[65];write_config("{\"base_url\":\"http://127.0.0.1:1/v1\",\"model\":\"fixture\",\"enabled_tools\":[\"echo\",\"calc\",\"read_file\",\"write_file\",\"exec\",\"http_get\",\"shell\"]}");
    current=dunlin_open("config.json",NULL,error,sizeof error);if(!current)fprintf(stderr,"seven tools: %s\n",error);CHECK(current);
    CHECK(dunlin_get_permission_mode(current)==DUNLIN_PERMISSION_REQUEST);
    const dunlin_permission_v2 *permission=ak_get(dunlin_scope(current),"permission",2);CHECK(permission);
    CHECK(dunlin_set_permission_mode(current,DUNLIN_PERMISSION_AGENT)&&dunlin_get_permission_mode(current)==DUNLIN_PERMISSION_AGENT);
    CHECK(dunlin_permission_authorize(permission,"write_file","{\"path\":\"note\",\"text\":\"x\"}"));
    CHECK(!dunlin_permission_authorize(permission,"shell","{\"command\":\"date\"}"));
    CHECK(dunlin_set_permission_mode(current,DUNLIN_PERMISSION_FULL_ACCESS)&&dunlin_get_permission_mode(current)==DUNLIN_PERMISSION_FULL_ACCESS);
    CHECK(dunlin_permission_authorize(permission,"shell","{\"command\":\"date\"}"));
    CHECK(!dunlin_set_permission_mode(current,(dunlin_permission_mode)99));
    CHECK(dunlin_set_permission_mode(current,DUNLIN_PERMISSION_REQUEST));
    const ah_tool_service *tools=ak_get(dunlin_scope(current),"tools",1);CHECK(tools);struct json_object *s=tools->schema(tools->ctx);
    CHECK(json_object_array_length(s)==7);json_object_put(s);
    CHECK(!strcmp(dunlin_current_model(current),"default")&&dunlin_show_reasoning(current));CHECK(!strcmp(dunlin_reasoning_effort(current),"default"));
    CHECK(!dunlin_set_reasoning_effort(current,"high",error,sizeof error)&&strstr(error,"unsupported"));
    CHECK(!dunlin_set_show_reasoning(current,2)&&dunlin_show_reasoning(current));
    CHECK(dunlin_session_for_model(current,"legacy",id,sizeof id,error,sizeof error)&&!strcmp(id,"legacy"));
    CHECK(!dunlin_session_for_model(current,"../escape",id,sizeof id,error,sizeof error));CHECK(!dunlin_session_for_model(current,"legacy",id,2,error,sizeof error));
    CHECK(!dunlin_close(current));current=NULL;
}
static void locked_snapshot(const char *beta,const char *alpha) {
    ah_session_service raw={0};CHECK(!ah_linux_sessions(&raw,"sessions"));
    void *lease=NULL;struct json_object *uncommitted=NULL,*snapshot=NULL,*list=NULL;
    CHECK(raw.open(raw.ctx,beta,&lease,&uncommitted)==AH_DONE&&uncommitted);
    json_object_object_add(uncommitted,"_dunlin_conversation",json_object_new_string("uncommitted-change"));
    CHECK(ah_session_read_snapshot(&raw,beta,&snapshot)==AH_DONE&&snapshot);
    CHECK(!strcmp(str(snapshot,"_dunlin_conversation"),"conversation"));json_object_put(snapshot);
    list=dunlin_conversations(current);CHECK(list&&json_object_array_length(list)==1);json_object_put(list);
    char error[256];CHECK(dunlin_select_model(current,"beta",error,sizeof error));
    list=dunlin_conversations(current);CHECK(list&&json_object_array_length(list)==1);
    CHECK(!strcmp(json_object_get_string(json_object_array_get_idx(list,0)),"conversation"));json_object_put(list);
    CHECK(dunlin_select_model(current,"alpha",error,sizeof error));
    raw.close(lease);json_object_put(uncommitted);
    CHECK(ah_session_read_snapshot(&raw,"missing-snapshot",&snapshot)==AH_DONE&&!snapshot);
    CHECK(access("sessions/missing-snapshot.lock",F_OK)!=0);
    CHECK(ah_session_read_snapshot(&raw,"../escape",&snapshot)==AH_FAILED);
    char target[80];snprintf(target,sizeof target,"%s.json",alpha);CHECK(!symlink(target,"sessions/unsafe.json"));
    CHECK(ah_session_read_snapshot(&raw,"unsafe",&snapshot)==AH_FAILED);CHECK(!unlink("sessions/unsafe.json"));
    int fd=open("sessions/loose.json",O_CREAT|O_WRONLY|O_EXCL,0644);CHECK(fd>=0&&write(fd,"{}",2)==2&&!close(fd));
    CHECK(!chmod("sessions/loose.json",0644));CHECK(ah_session_read_snapshot(&raw,"loose",&snapshot)==AH_FAILED);
    CHECK(!unlink("sessions/loose.json"));
    fd=open("sessions/oversized.json",O_CREAT|O_WRONLY|O_EXCL,0600);CHECK(fd>=0&&!ftruncate(fd,32*1024*1024+1)&&!close(fd));
    CHECK(ah_session_read_snapshot(&raw,"oversized",&snapshot)==AH_FAILED);CHECK(!unlink("sessions/oversized.json"));
#ifdef DUNLIN_PROFILE_FAULTS
    CHECK(raw.open(raw.ctx,"rename-race",&rename_writer,&snapshot)==AH_DONE&&!snapshot);
    struct json_object *before=json_tokener_parse("{\"generation\":\"before\"}");
    rename_replacement=json_tokener_parse("{\"generation\":\"after\"}");CHECK(before&&rename_replacement);
    CHECK(!raw.save(rename_writer,before));json_object_put(before);rename_store=&raw;rename_after_open=1;
    CHECK(ah_session_read_snapshot(&raw,"rename-race",&snapshot)==AH_DONE&&snapshot&&!rename_after_open);
    CHECK(!strcmp(str(snapshot,"generation"),"before"));json_object_put(snapshot);
    CHECK(ah_session_read_snapshot(&raw,"rename-race",&snapshot)==AH_DONE&&snapshot);
    CHECK(!strcmp(str(snapshot,"generation"),"after"));json_object_put(snapshot);
    raw.close(rename_writer);json_object_put(rename_replacement);rename_store=NULL;rename_writer=NULL;rename_replacement=NULL;
#endif
    ah_linux_sessions_close(&raw);
}
static void profiles(void) {
    char error[256],alpha[65],beta[65],again[65];write_config(profile_config);current=dunlin_open("config.json",NULL,error,sizeof error);CHECK(current);
    const ah_tool_service *tools=ak_get(dunlin_scope(current),"tools",1);const char *workspace=dunlin_workspace(current);
    struct json_object *list=dunlin_models(current);CHECK(json_object_array_length(list)==5);
    const char *encoded=json_object_to_json_string_ext(list,JSON_C_TO_STRING_PLAIN);
    CHECK(!strstr(encoded,"PROFILE_SECRET_ENV")&&!strstr(encoded,"fixture-secret")&&!strstr(encoded,"http://"));
    for(size_t i=0;i<(size_t)json_object_array_length(list);i++) {
        CHECK(json_object_object_length(json_object_array_get_idx(list,i))==4);
    }
    json_object_put(list);
    CHECK(!dunlin_select_model(current,"missing",error,sizeof error)&&!strcmp(dunlin_current_model(current),"alpha"));
    CHECK(dunlin_select_model(current,"unsupported",error,sizeof error)&&!strcmp(dunlin_current_model(current),"unsupported"));
    CHECK(dunlin_select_model(current,"alpha",error,sizeof error));
    CHECK(!dunlin_select_model(current,"bad-native",error,sizeof error)&&strstr(error,"temperature"));CHECK(!strcmp(dunlin_current_model(current),"alpha"));
    CHECK(dunlin_set_reasoning_effort(current,"low",error,sizeof error));
    CHECK(dunlin_session_for_model(current,public_session,alpha,sizeof alpha,error,sizeof error)&&strlen(alpha)==64&&strcmp(alpha,public_session));
    CHECK(ak_on(dunlin_scope(current),"agent/event",kernel_event,NULL,0));run("alpha-only");
    CHECK(reasoning_events==1&&kernel_reasoning==1&&metrics_events==1&&starts==1);
    struct json_object *s=snapshot(alpha);CHECK(!strcmp(str(s,"_dunlin_conversation"),public_session));CHECK(strlen(str(s,"_dunlin_profile"))==64);
    CHECK(json_object_array_length(field(s,"messages"))==2);json_object_put(s);
    struct json_object *projection=dunlin_session_events(current,public_session,error,sizeof error);CHECK(projection);
    CHECK(json_object_array_length(projection)==4);
    CHECK(!strcmp(str(json_object_array_get_idx(projection,0),"type"),"session.started"));
    CHECK(!strcmp(str(json_object_array_get_idx(projection,1),"type"),"message.user"));
    CHECK(!strcmp(str(json_object_array_get_idx(projection,2),"type"),"message.assistant"));
    CHECK(!strcmp(str(json_object_array_get_idx(projection,3),"type"),"session.ended"));
    const char *projection_wire=json_object_to_json_string_ext(projection,JSON_C_TO_STRING_PLAIN);
    CHECK(!strstr(projection_wire,"_dunlin_provider")&&!strstr(projection_wire,"base_url")&&!strstr(projection_wire,"key_env"));
    json_object_put(projection);
    CHECK(!dunlin_session_events(current,"missing",error,sizeof error)&&strstr(error,"unavailable"));
    list=dunlin_conversations(current);CHECK(json_object_array_length(list)==1&&!strcmp(json_object_get_string(json_object_array_get_idx(list,0)),public_session));json_object_put(list);
    CHECK(dunlin_set_show_reasoning(current,0));run("second-alpha");CHECK(reasoning_events==1&&kernel_reasoning==1);
    CHECK(dunlin_select_model(current,"beta",error,sizeof error));CHECK(!dunlin_set_reasoning_effort(current,"high",error,sizeof error)&&strstr(error,"unsupported"));
    CHECK(!strcmp(dunlin_reasoning_effort(current),"default"));CHECK(dunlin_session_for_model(current,public_session,beta,sizeof beta,error,sizeof error)&&strcmp(alpha,beta));
    list=dunlin_conversations(current);CHECK(json_object_array_length(list)==0);json_object_put(list);run("beta-only");
    CHECK(dunlin_select_model(current,"alpha",error,sizeof error)&&!strcmp(dunlin_reasoning_effort(current),"low"));
    locked_snapshot(beta,alpha);
    CHECK(dunlin_session_for_model(current,public_session,again,sizeof again,error,sizeof error)&&!strcmp(alpha,again));
    CHECK(ak_get(dunlin_scope(current),"tools",1)==tools&&dunlin_workspace(current)==workspace);run("resume-alpha");
#ifdef DUNLIN_PROFILE_FAULTS
    const char *keys[]={"_dunlin_conversation","_dunlin_profile"};
    for(unsigned i=0;i<2;i++) {
        public_session=i?"fault-profile":"fault-conversation";fail_metadata_key=keys[i];
        unsigned before=calls;ah_result failed=dunlin_turn_for_model(current,NULL,public_session,"first save",NULL,NULL);
        CHECK(failed.status==AH_FAILED&&!fail_metadata_key&&calls==before);free(failed.text);
        char storage[65],path[96];CHECK(dunlin_session_for_model(current,public_session,storage,sizeof storage,error,sizeof error));
        snprintf(path,sizeof path,"sessions/%s.json",storage);CHECK(access(path,F_OK)!=0);
    }
    public_session="conversation";
#endif
    CHECK(dunlin_require_tool(current,"echo"));require_echo=1;public_session="tool-turn";run("echo something");require_echo=0;CHECK(dunlin_require_tool(current,NULL));
    CHECK(dunlin_select_model(current,"native",error,sizeof error));public_session="native-turn";ah_hooks hooks={.event=observer};
    ah_result result=dunlin_turn_for_model(current,&hooks,public_session,"native",NULL,NULL);CHECK(result.status==AH_FAILED&&native_calls==1);free(result.text);
    CHECK(dunlin_select_model(current,"alpha",error,sizeof error));public_session="conversation";CHECK(!dunlin_close(current));current=NULL;
    current=dunlin_open("config.json",NULL,error,sizeof error);CHECK(current&&!strcmp(dunlin_reasoning_effort(current),"default"));
    CHECK(dunlin_set_reasoning_effort(current,"low",error,sizeof error));list=dunlin_conversations(current);CHECK(json_object_array_length(list)==2);json_object_put(list);run("after-reopen");
    s=snapshot(alpha);CHECK(json_object_array_length(field(s,"messages"))==8);json_object_object_del(s,"_dunlin_conversation");json_object_object_del(s,"_dunlin_profile");
    char path[96];snprintf(path,sizeof path,"sessions/%s.json",alpha);CHECK(!json_object_to_file(path,s));json_object_put(s);
    list=dunlin_conversations(current);CHECK(json_object_array_length(list)==1);json_object_put(list);
    result=dunlin_turn_for_model(current,NULL,public_session,NULL,NULL,NULL);CHECK(result.status==AH_FAILED&&result.text&&strstr(result.text,"metadata"));free(result.text);
    CHECK(!dunlin_close(current));current=NULL;
}
static void invalid_profiles(void) {
    const char *forbidden[]={"workspace","session_dir","max_rounds","timeout_ms","plugins","stream"};char error[256];
    for(size_t i=0;i<sizeof forbidden/sizeof *forbidden;i++) {
        struct json_object *o=json_tokener_parse(profile_config),*a=field(field(o,"models"),"alpha");json_object_object_add(a,forbidden[i],json_object_new_string("forbidden"));
        write_config(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));json_object_put(o);CHECK(!dunlin_open("config.json",NULL,error,sizeof error)&&strstr(error,"profile field"));
    }
    const char *bad[]={"{\"models\":{}}","{\"base_url\":\"http://fixture/v1\",\"model\":\"x\",\"show_reasoning\":1}",
      "{\"models\":{\"x\":{\"base_url\":\"http://fixture/v1\",\"model\":\"x\",\"reasoning_capabilities\":[\"low\"]}}}",
      "{\"models\":{\"x\":{\"base_url\":\"http://fixture/v1\",\"model\":\"x\",\"reasoning_effort\":\"high\"}}}",
      "{\"models\":{\"x\":{\"base_url\":\"http://fixture/v1\",\"model\":\"x\",\"protocol\":\"unknown\"}}}"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++){write_config(bad[i]);CHECK(!dunlin_open("config.json",NULL,error,sizeof error));}
    write_config("{\"base_url\":\"http://fixture/v1\",\"model\":\"sol-fixture\",\"protocol\":\"responses\",\"reasoning_capabilities\":[\"levels\",\"xhigh\"],\"reasoning_effort\":\"xhigh\"}");
    current=dunlin_open("config.json",NULL,error,sizeof error);CHECK(current && !strcmp(dunlin_reasoning_effort(current),"xhigh"));
    CHECK(dunlin_set_reasoning_effort(current,"low",error,sizeof error));CHECK(dunlin_set_reasoning_effort(current,"xhigh",error,sizeof error));CHECK(!dunlin_close(current));current=NULL;
    write_config("{\"base_url\":\"http://fixture/v1\",\"model\":\"sol-fixture\",\"protocol\":\"responses\",\"reasoning_capabilities\":[\"levels\"],\"reasoning_effort\":\"xhigh\"}");
    CHECK(!dunlin_open("config.json",NULL,error,sizeof error));
    write_config("{\"stream\":false,\"models\":{\"only\":{\"base_url\":\"http://fixture/v1\",\"model\":\"without-base\"}}}");
    current=dunlin_open("config.json",NULL,error,sizeof error);CHECK(current&&!strcmp(dunlin_current_model(current),"only"));CHECK(!dunlin_close(current));current=NULL;
}
int main(void) {
    hash_vectors();char original[4096],dir[]="/tmp/dunlin-profiles-XXXXXX",error[256];CHECK(getcwd(original,sizeof original));
    CHECK(mkdtemp(dir)&&!chdir(dir));CHECK(dunlin_init_config("config.json",error,sizeof error));CHECK(!setenv("PROFILE_SECRET_ENV","fixture-secret-not-real",1));
    legacy();profiles();invalid_profiles();DIR *d=opendir("sessions");CHECK(d);struct dirent *entry;
    while((entry=readdir(d)))if(strcmp(entry->d_name,".")&&strcmp(entry->d_name,"..")){char path[512];snprintf(path,sizeof path,"sessions/%s",entry->d_name);CHECK(!unlink(path));}
    closedir(d);CHECK(!rmdir("sessions")&&!rmdir("work")&&!unlink("config.json")&&!chdir(original)&&!rmdir(dir));CHECK(!unsetenv("PROFILE_SECRET_ENV"));
    printf("PASS runtime profiles: seven tools, SHA-256 vectors, rollback, busy, inventory, reasoning/metrics, native dispatch, public session listing/resume, isolated histories (%u mock requests)\n",calls+native_calls);return 0;
}
