#include "internal.h"
#include "../platform/clock.h"
#include "history.h"
#include "intent.h"
#include "profile_hash.h"
#include "dunlin/compact/tools.h"
#include "dunlin/compact/session.h"
#include "dunlin/compact/http.h"
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define ah_linux_tools ah_windows_tools
#define ah_linux_tool_enable ah_windows_tool_enable
#define ah_linux_tools_close ah_windows_tools_close
#define ah_linux_sessions ah_windows_sessions
#define ah_linux_sessions_close ah_windows_sessions_close
#define strdup _strdup
#endif

static const char *const tool_names[]={"echo","calc","read_file","write_file","exec","http_get","shell"};
enum { BASE_COUNT=6, TOOL_COUNT=sizeof tool_names/sizeof *tool_names, PLUGIN_COUNT=BASE_COUNT+TOOL_COUNT };
struct tool_slot {
    ah_tool_service *tools;
    const char *name;
};
struct post_policy { dunlin_runtime *runtime; const char *required; };
struct delivery;
static void event(void *,const char *,const char *,size_t);
struct dunlin_runtime {
    ak_context *kernel;
    ak_scope *scopes[PLUGIN_COUNT];
    dunlin_config config;
    dunlin_model_v1 model;
    char first_tool[65];
    dunlin_http_v1 limited_http;
    struct post_policy policy;
    const dunlin_http_v1 *upstream;
    ah_tool_service tools;
    dunlin_sessions_v1 sessions;
    ah_session_service storage;
    dunlin_permission_v1 permission;
    dunlin_permission_v2 permission_v2;
    struct tool_slot slots[TOOL_COUNT];
    const dunlin_http_v2 *upstream_v2;
    struct delivery *delivery;
    size_t wire_request_bytes;
    int busy, model_busy, model_trace;
    char public_session[65], storage_session[65];
    const char *session_issue;
    const dunlin_context_projector_v1 *projector;
    const ah_tool_pipeline_v1 *tool_pipeline;
};
static void *allocate(void *user,size_t size) { (void)user; return malloc(size); }
static void release(void *user,void *memory) { (void)user; free(memory); }

static dunlin_permission_decision workspace_decide(void *unused,const char *name,const char *arguments) {
    (void)unused;(void)arguments;
    return name&&!strcmp(name,"write_file")?DUNLIN_PERMISSION_DECISION_ALLOW:DUNLIN_PERMISSION_DECISION_ASK;
}
static const dunlin_permission_adapter_v1 workspace_adapter={NULL,workspace_decide};

const char *dunlin_permission_mode_name(dunlin_permission_mode mode) {
    switch(mode) {
    case DUNLIN_PERMISSION_REQUEST:return "request";
    case DUNLIN_PERMISSION_AGENT:return "agent";
    case DUNLIN_PERMISSION_FULL_ACCESS:return "full-access";
    default:return NULL;
    }
}
int dunlin_permission_mode_parse(const char *name,dunlin_permission_mode *mode) {
    if(!name||!mode)return 0;
    for(int i=DUNLIN_PERMISSION_REQUEST;i<=DUNLIN_PERMISSION_FULL_ACCESS;i++)
        if(!strcmp(name,dunlin_permission_mode_name((dunlin_permission_mode)i))){*mode=(dunlin_permission_mode)i;return 1;}
    return 0;
}
int dunlin_permission_authorize(const dunlin_permission_v2 *policy,const char *name,const char *arguments) {
    if(!policy||!name||!arguments||!dunlin_permission_mode_name(policy->mode))return 0;
    if(policy->mode==DUNLIN_PERMISSION_FULL_ACCESS)return 1;
    if(policy->mode==DUNLIN_PERMISSION_AGENT&&policy->adapter&&policy->adapter->decide) {
        dunlin_permission_decision decision=policy->adapter->decide(policy->adapter->user,name,arguments);
        if(decision==DUNLIN_PERMISSION_DECISION_ALLOW)return 1;
        if(decision==DUNLIN_PERMISSION_DECISION_DENY)return 0;
        if(decision!=DUNLIN_PERMISSION_DECISION_ASK)return 0;
    }
    return policy->request&&policy->request(policy->request_user,name,arguments);
}

static struct json_object *member(struct json_object *o,const char *name) {
    struct json_object *v=NULL;
    if(o) json_object_object_get_ex(o,name,&v);
    return v;
}
static const char *function_name(struct json_object *item) {
    return json_object_get_string(member(member(item,"function"),"name"));
}
static int only_named_calls(struct json_object *calls,const char *name) {
    if(!json_object_is_type(calls,json_type_array) || !json_object_array_length(calls)) return 0;
    for(size_t i=0;i<(size_t)json_object_array_length(calls);i++) {
        const char *got=function_name(json_object_array_get_idx(calls,i));
        if(!got || strcmp(got,name)) return 0;
    }
    return 1;
}
static int last_role(struct json_object *history,const char *expected) {
    if(!json_object_is_type(history,json_type_array)) return 0;
    size_t n=json_object_array_length(history);
    struct json_object *last=n?json_object_array_get_idx(history,n-1):NULL;
    const char *role=json_object_get_string(member(last,"role"));
    return role && !strcmp(role,expected);
}
/* Apply domain guidance only to results from the configured tool. */
static int result_prompt_applies(const dunlin_config *cfg,struct json_object *history) {
    if(!*cfg->tool_result_prompt || !last_role(history,"tool")) return 0;
    if(!*cfg->tool_result_for) return 1;
    for(size_t i=json_object_array_length(history);i>0;i--) {
        struct json_object *message=json_object_array_get_idx(history,i-1);
        const char *role=json_object_get_string(member(message,"role"));
        if(!role || strcmp(role,"tool"))
            return only_named_calls(member(message,"tool_calls"),cfg->tool_result_for);
    }
    return 0;
}
/* Apply the preset's provider limit to every POST, including follow-up requests
 * made by a replacement loop. The v1 transport and model ABI stay unchanged. */
static dunlin_http_result limited_post(void *data,const dunlin_http_request *req,long *status) {
    const struct post_policy *policy=data;
    dunlin_runtime *r=policy->runtime;
    struct json_object *body=json_tokener_parse(req->json);
    if(!json_object_is_type(body,json_type_object)) {
        json_object_put(body);
        return DUNLIN_HTTP_FAILED;
    }
    const dunlin_profile *profile=&r->config.active;
    if(profile->omlx_chat_template) {
        struct json_object *kwargs=json_object_new_object();
        json_object_object_add(kwargs,"chat_template",json_object_get(profile->omlx_chat_template));
        json_object_object_add(body,"chat_template_kwargs",kwargs);
    }
    if(profile->enable_thinking)
        json_object_object_add(body,"enable_thinking",json_object_get(profile->enable_thinking));
    dunlin_http_request bounded=*req;
    bounded.json=json_object_to_json_string_ext(body,JSON_C_TO_STRING_PLAIN);
    r->wire_request_bytes+=strlen(bounded.json);
    dunlin_http_result rc=r->upstream->post(r->upstream->user,&bounded,status);
    json_object_put(body);
    return rc;
}
static void public_reasoning(void *data,const char *type,const char *text,size_t length) {
    dunlin_runtime *r=data;
    if(r->delivery && r->config.show_reasoning) event(r->delivery,type,text,length);
}
static void public_metrics(dunlin_runtime *r,const dunlin_model_metrics *metrics,uint64_t elapsed) {
    if(!r->delivery) return;
    /* Bionic json-c 不能保真构造 uint64；固定字段直接编码，不截断厂商计量。 */
    char data[768];
    int length=snprintf(data,sizeof data,"{\"request_bytes\":%zu,\"response_bytes\":%zu,\"response_bytes_known\":%s,"
        "\"elapsed_ms\":%" PRIu64 ",\"usage_present\":%u",metrics->request_bytes,metrics->response_bytes,
        metrics->response_bytes_known?"true":"false",elapsed,metrics->present);
    if(length<0 || (size_t)length>=sizeof data) return;
#define METRIC(bit,field) do { if(metrics->present&(bit)) { \
    int n=snprintf(data+length,sizeof data-(size_t)length,",\"" #field "\":%" PRIu64,metrics->field); \
    if(n<0 || (size_t)n>=sizeof data-(size_t)length) return; \
    length+=n; } } while(0)
    METRIC(DUNLIN_USAGE_INPUT,input_tokens); METRIC(DUNLIN_USAGE_OUTPUT,output_tokens);
    METRIC(DUNLIN_USAGE_TOTAL,total_tokens); METRIC(DUNLIN_USAGE_CACHED_INPUT,cached_input_tokens);
    METRIC(DUNLIN_USAGE_CACHE_CREATION,cache_creation_tokens); METRIC(DUNLIN_USAGE_REASONING,reasoning_tokens);
#undef METRIC
    if((size_t)length+1>=sizeof data) return;
    data[length++]='}';data[length]=0;
    event(r->delivery,"model.metrics",data,(size_t)length);
}
static struct json_object *portable_message(struct json_object *source) {
    struct json_object *out=json_object_new_object();
    const char *fields[]={"role","content","tool_calls","tool_call_id","name"};
    for(size_t i=0;i<sizeof fields/sizeof *fields;i++) {
        struct json_object *v=member(source,fields[i]);
        /* Rich media payloads are deliberately omitted from this text ledger. */
        if(i==1 && v && !json_object_is_type(v,json_type_string)) {
            json_object_object_add(out,"content_omitted",json_object_new_boolean(1));continue;
        }
        if(v)json_object_object_add(out,fields[i],json_object_get(v));
    }
    return out;
}
static void trace_model(dunlin_runtime *r,const char *name,struct json_object *data) {
    if(r->model_trace && r->delivery && data) {
        const char *text=json_object_to_json_string_ext(data,JSON_C_TO_STRING_PLAIN);
        event(r->delivery,name,text,strlen(text));
    }
    json_object_put(data);
}
static dunlin_model_status checked_proposal(const dunlin_model_config *cfg,struct json_object *history,
        struct json_object *tools,const dunlin_model_events *events,const char *required,
        struct json_object **answer,char *error,size_t size) {
    if(!required && dunlin_install_needs_verification(history)) {
        int found=0;
        for(size_t i=0;i<json_object_array_length(tools);i++) {
            const char *name=function_name(json_object_array_get_idx(tools,i));
            if(name && !strcmp(name,"shell")) found=1;
        }
        if(!found) {
            if(answer) *answer=NULL;
            if(error && size) snprintf(error,size,"installation verification requires an enabled shell tool");
            return DUNLIN_MODEL_INVALID;
        }
        required="shell";
    }
    dunlin_model_events observer=events?*events:(dunlin_model_events){0};
    if(required) observer.delta=NULL;
    dunlin_runtime *r=((const struct post_policy *)cfg->http->user)->runtime;
    const dunlin_profile *profile=&r->config.active;
    dunlin_model_metrics metrics={0};
    dunlin_protocol_options options={.protocol=profile->protocol,.max_tokens=profile->max_tokens,
        .chat_metadata_complete=profile->chat_metadata_complete,
        .has_temperature=profile->temperature!=NULL,.temperature=json_object_get_double(profile->temperature),
        .required_tool=required,.reasoning_effort=profile->effort,.reasoning_supported=profile->reasoning_supported,
        .show_reasoning=r->config.show_reasoning,.user=r,.reasoning=public_reasoning,.metrics=&metrics,.http=r->upstream_v2};
    dunlin_model_config request=*cfg;
    if(profile->protocol!=DUNLIN_PROTOCOL_CHAT) request.http=NULL;
    struct json_object *projected=NULL;
    if(r->projector){
        dunlin_model_config auxiliary=request;auxiliary.http=r->upstream;
        dunlin_model_status status=r->projector->project(r->projector->user,&auxiliary,&options,history,tools,&observer,&projected,error,size);
        if(status!=DUNLIN_MODEL_OK||!json_object_is_type(projected,json_type_array)){
            json_object_put(projected);if(answer)*answer=NULL;return status==DUNLIN_MODEL_OK?DUNLIN_MODEL_INVALID:status;
        }
    }
    r->wire_request_bytes=0;
    if(r->model_trace) {
        struct json_object *detail=json_object_new_object(),*messages=json_object_new_array();
        struct json_object *source=projected?projected:history;
        for(size_t i=0;i<json_object_array_length(source);i++)json_object_array_add(messages,portable_message(json_object_array_get_idx(source,i)));
        json_object_object_add(detail,"messages",messages);
        json_object_object_add(detail,"tools",json_object_get(tools));
        json_object_object_add(detail,"model",json_object_new_string(cfg->model));
        json_object_object_add(detail,"protocol",json_object_new_string(dunlin_profile_protocol_name(profile->protocol)));
        json_object_object_add(detail,"max_tokens",json_object_new_int(profile->max_tokens));
        json_object_object_add(detail,"stream",json_object_new_boolean(cfg->stream));
        if(profile->temperature)json_object_object_add(detail,"temperature",json_object_get(profile->temperature));
        if(profile->enable_thinking)json_object_object_add(detail,"enable_thinking",json_object_get(profile->enable_thinking));
        if(profile->omlx_chat_template)json_object_object_add(detail,"chat_template",json_object_get(profile->omlx_chat_template));
        if(required)json_object_object_add(detail,"required_tool",json_object_new_string(required));
        trace_model(r,"model.request",detail);
    }
    uint64_t started=ah_os_now_ms(NULL);
    dunlin_model_status rc=dunlin_protocol_proposal(&request,&options,projected?projected:history,tools,&observer,answer,error,size);
    json_object_put(projected);
    if(r->model_trace && answer && *answer)trace_model(r,"model.response",portable_message(*answer));
    if(profile->protocol==DUNLIN_PROTOCOL_CHAT && r->wire_request_bytes) metrics.request_bytes=r->wire_request_bytes;
    public_metrics(r,&metrics,ah_os_now_ms(NULL)-started);
    if(rc==DUNLIN_MODEL_OK && dunlin_install_repeat(history,*answer)) {
        json_object_put(*answer);*answer=NULL;
        if(error && size) snprintf(error,size,"installation step already completed; use its result and continue to the next step");
        rc=DUNLIN_MODEL_TOOL_FORMAT;
    }
    if(required && rc==DUNLIN_MODEL_OK && !only_named_calls(member(*answer,"tool_calls"),required)) {
        json_object_put(*answer); *answer=NULL;
        if(error && size) snprintf(error,size,"%s",required);
        rc=DUNLIN_MODEL_REQUIRED_TOOL_MISSING;
    }
    if(r->model_trace) {
        struct json_object *detail=json_object_new_object();
        json_object_object_add(detail,"status",json_object_new_int(rc));
        json_object_object_add(detail,"accepted",json_object_new_boolean(rc==DUNLIN_MODEL_OK));
        if(rc!=DUNLIN_MODEL_OK && error && *error)json_object_object_add(detail,"error",json_object_new_string(error));
        trace_model(r,"model.request.end",detail);
    }
    return rc;
}
/* Apply profile instructions to ordinary requests; correction requests already
 * carry their own system message. Neither changes the user's stored history. */
static dunlin_model_status prompted_completion_body(const dunlin_model_config *cfg,struct json_object *history,
        struct json_object *tools,const dunlin_model_events *events,struct json_object **answer,char *error,size_t size) {
    if(!answer || !json_object_is_type(history,json_type_array) || !cfg || !cfg->http ||
       cfg->http->post!=limited_post || !cfg->http->user) {
        if(answer) *answer=NULL;
        if(error && size) snprintf(error,size,"missing preset model configuration or messages");
        return DUNLIN_MODEL_INVALID;
    }
    dunlin_runtime *r=((const struct post_policy *)cfg->http->user)->runtime;
    int require=*r->first_tool && last_role(history,"user");
    int after_tools=result_prompt_applies(&r->config,history);
    if(!require && !*r->config.system_prompt && !after_tools)
        return checked_proposal(cfg,history,tools,events,NULL,answer,error,size);
    struct json_object *first=json_object_array_length(history)?json_object_array_get_idx(history,0):NULL,*role=NULL;
    if(first && json_object_object_get_ex(first,"role",&role) &&
       json_object_is_type(role,json_type_string) && !strcmp(json_object_get_string(role),"system"))
        return checked_proposal(cfg,history,tools,events,require?r->first_tool:NULL,answer,error,size);
    struct json_object *request=json_object_new_array(),*system=json_object_new_object();
    json_object_object_add(system,"role",json_object_new_string("system"));
    const char *inventory_guide=
        "\n环境盘点规则：用户询问本地有哪些开发软件或工具时，必须逐项检查并输出存在路径或未找到。"
        "使用 for t in python3 python node npm pip pip3 go rustc cargo java javac gcc g++ make cmake git docker kubectl jq curl wget; "
        "do if p=$(command -v \"$t\"); then printf '%s = %s\\n' \"$t\" \"$p\"; "
        "else printf '%s = missing from PATH\\n' \"$t\"; fi; done。"
        "不要使用 which，不要用 && 串联独立检查。缺失项是正常盘点结果。"
        "得到结果后直接汇总已观察的软件与缺失项，不重复执行相同盘点。容器中只报告容器环境。"
        "用户随后要求安装软件时，这是新的安装任务；‘那node呢’或‘那wget呢’继承最近安装意图。"
        "安装前检查 uname、id、包管理器、文件系统写权限及目标版本来源。"
        "不要假定精简容器支持系统安装，或旧发行版仓库提供 python3.12。"
        "只读或权限不足时明确报告阻碍和所需环境，不用无关版本查询冒充安装或安装纠正。"
        "安装完成后必须执行目标程序 --version 核验；实际输出前不得声称安装成功。"
        "长安装使用任务预算内的工具执行；超时结果未知时不得重复安装，应先由宿主核验外部状态。";
    int has_shell=0;
    for(size_t i=0;tools&&i<json_object_array_length(tools);i++){
        const char *name=function_name(json_object_array_get_idx(tools,i));if(name&&!strcmp(name,"shell"))has_shell=1;
    }
    const char *guide=has_shell?inventory_guide:"";
    size_t profile_size=strlen(r->config.system_prompt)+strlen(guide)+1;
    char *profile_buffer=malloc(profile_size);
    if(!profile_buffer) {json_object_put(request);json_object_put(system);return DUNLIN_MODEL_LIMIT;}
    snprintf(profile_buffer,profile_size,"%s%s",r->config.system_prompt,guide);
    const char *profile=profile_buffer;
    if(require) {
        const char *format="%s Local task. First call %s to do the requested work. A text-only answer cannot complete this task.";
        size_t size_prompt=strlen(profile)+strlen(format)+strlen(r->first_tool)+1;
        char *combined=malloc(size_prompt);
        if(!combined) {
            free(profile_buffer);
            json_object_put(system); json_object_put(request);
            *answer=NULL;
            if(error && size) snprintf(error,size,"cannot prepare tool instructions");
            return DUNLIN_MODEL_INVALID;
        }
        snprintf(combined,size_prompt,format,profile,r->first_tool);
        json_object_object_add(system,"content",json_object_new_string(combined));
        free(combined);
    } else json_object_object_add(system,"content",json_object_new_string(profile));
    free(profile_buffer);
    json_object_array_add(request,system);
    size_t n=json_object_array_length(history);
    for(size_t i=0;i<n;i++) json_object_array_add(request,json_object_get(json_object_array_get_idx(history,i)));
    struct json_object *followup=NULL;
    if(after_tools) {
        /* Guidance is policy, never an additional user task. Keep the actual
         * latest user message authoritative while retaining reference history. */
        const char *format="仅处理历史中最后一条 user 消息的请求。更早回合只用于解析必要引用，"
            "不要复述无关历史事实或继续旧任务。以下检查仅针对这条用户请求：\n%s"
            "\n若当前任务是查询开发环境，且工具输出已有各程序路径和 missing from PATH 标记，"
            "查询已经完成，现在直接用中文汇总这些结果。缺失项无需修复，不再调用盘点命令。"
            "\n若当前任务是安装或版本验证：command -v 只观察路径，不包含版本。"
            "若已经查明目标程序缺失，下一步应安装，不能重复查询缺失状态。"
            "仅当程序路径已找到或安装命令成功后，没有目标程序 --version 的真实输出时，"
            "下一步才调用 shell 执行该程序 --version。不能猜测版本。"
            "安装失败且环境不支持时，只报告实际阻碍。";
        size_t guidance_size=strlen(format)+strlen(r->config.tool_result_prompt)+1;
        char *guidance=malloc(guidance_size);
        if(!guidance) {
            json_object_put(request);
            *answer=NULL;
            if(error && size) snprintf(error,size,"cannot prepare tool-result guidance");
            return DUNLIN_MODEL_LIMIT;
        }
        snprintf(guidance,guidance_size,format,r->config.tool_result_prompt);
        followup=json_object_new_object();
        json_object_object_add(followup,"role",json_object_new_string("system"));
        json_object_object_add(followup,"content",json_object_new_string(guidance));
        free(guidance);
        json_object_array_add(request,followup);
    }
    const ah_limits *limits=&r->config.limits;
    size_t bytes=strlen(json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN));
    size_t schemas=tools?strlen(json_object_to_json_string_ext(tools,JSON_C_TO_STRING_PLAIN)):0;
    /* Account policy overhead too: only the cloned request view is trimmed. */
    while(!r->projector&&((size_t)json_object_array_length(request)>limits->max_messages || bytes>=limits->max_context_bytes || schemas>=limits->max_context_bytes-bytes) &&
          dunlin_history_evict(&request))
        bytes=strlen(json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN));
    dunlin_model_status result=DUNLIN_MODEL_LIMIT;
    if((size_t)json_object_array_length(request)<=limits->max_messages &&
       (!followup || strlen(json_object_to_json_string_ext(followup,JSON_C_TO_STRING_PLAIN))<=limits->max_message_bytes) && (r->projector||(bytes<limits->max_context_bytes && schemas<limits->max_context_bytes-bytes)) &&
       strlen(json_object_to_json_string_ext(system,JSON_C_TO_STRING_PLAIN))<=limits->max_message_bytes)
        result=checked_proposal(cfg,request,tools,events,require?r->first_tool:NULL,answer,error,size);
    else {
        *answer=NULL;
        if(error && size) snprintf(error,size,"system prompt and history exceed message or context budget");
    }
    json_object_put(request);
    return result;
}
static dunlin_model_status prompted_completion(const dunlin_model_config *cfg,struct json_object *history,
        struct json_object *tools,const dunlin_model_events *events,struct json_object **answer,char *error,size_t size) {
    if(!cfg || !cfg->http || cfg->http->post!=limited_post || !cfg->http->user)
        return prompted_completion_body(cfg,history,tools,events,answer,error,size);
    dunlin_runtime *r=((const struct post_policy *)cfg->http->user)->runtime;
    if(r->model_busy) {
        if(answer) *answer=NULL;
        if(error && size) snprintf(error,size,"runtime is busy with a model request");
        return DUNLIN_MODEL_INVALID;
    }
    int prior=r->busy;
    r->busy=1; r->model_busy=1;
    struct json_object *resolved=dunlin_install_followup(history);
    int projected_fits=1;
    if(resolved) {
        const ah_limits *limits=&r->config.limits;
        size_t schemas=tools?strlen(json_object_to_json_string_ext(tools,JSON_C_TO_STRING_PLAIN)):0;
        size_t bytes=strlen(json_object_to_json_string_ext(resolved,JSON_C_TO_STRING_PLAIN));
        for(size_t i=0;i<json_object_array_length(resolved);i++) {
            struct json_object *message=json_object_array_get_idx(resolved,i);
            if(message!=json_object_array_get_idx(history,i) &&
               strlen(json_object_to_json_string_ext(message,JSON_C_TO_STRING_PLAIN))>limits->max_message_bytes)
                projected_fits=0;
        }
        while((json_object_array_length(resolved)>limits->max_messages ||
               bytes>=limits->max_context_bytes || schemas>=limits->max_context_bytes-bytes) &&
              dunlin_history_evict(&resolved))
            bytes=strlen(json_object_to_json_string_ext(resolved,JSON_C_TO_STRING_PLAIN));
        if(json_object_array_length(resolved)>limits->max_messages ||
           bytes>=limits->max_context_bytes || schemas>=limits->max_context_bytes-bytes)
            projected_fits=0;
    }
    dunlin_model_status result=DUNLIN_MODEL_LIMIT;
    if(projected_fits)
        result=prompted_completion_body(cfg,resolved?resolved:history,tools,events,answer,error,size);
    else {
        if(answer) *answer=NULL;
        if(error && size) snprintf(error,size,"resolved installation request exceeds message or context budget");
    }
    json_object_put(resolved);
    r->model_busy=0; r->busy=prior;
    return result;
}
static int model_start(ak_scope *s,void *p) {
    dunlin_runtime *r=p;
    r->model.complete=prompted_completion;
    r->model.config=r->config.active.model;
    r->upstream=ak_get(s,"transport",1);
    r->policy=(struct post_policy){r,NULL};
    r->limited_http=(dunlin_http_v1){limited_post,&r->policy};
    r->upstream_v2=ak_get(s,"transport.v2",1);
    r->model.config.http=&r->limited_http;
    return !ak_provide(s,"model",1,&r->model);
}
static int tools_start(ak_scope *s,void *p) {
    dunlin_runtime *r=p;
    if(ah_linux_tools(&r->tools,r->config.workspace)) return 1;
    for(unsigned i=0;i<TOOL_COUNT;i++) ah_linux_tool_enable(&r->tools,tool_names[i],0);
    if(ak_provide(s,"tools",1,&r->tools)) return 0;
    return 1;
}
static void tools_stop(ak_scope *s,void *p) {
    (void)s;
    ah_linux_tools_close(&((dunlin_runtime *)p)->tools);
}
static void tool_off(void *data) {
    struct tool_slot *slot=data;
    ah_linux_tool_enable(slot->tools,slot->name,0);
}
static int tool_start(ak_scope *s,void *data) {
    struct tool_slot *slot=data;
    if(!ak_effect(s,tool_off,slot)) return 1;
    return ah_linux_tool_enable(slot->tools,slot->name,1);
}
struct profile_lease {
    dunlin_runtime *runtime;
    void *inner;
    char conversation[65], identity[65];
};
static void profile_identity(const dunlin_runtime *r,char out[65]) {
    dunlin_profile_session_id("",r->config.active_id,dunlin_profile_protocol_name(r->config.active.protocol),
        r->config.active.model.model,r->config.active.model.base_url,out);
}
static ah_status profile_session_open(void *data,const char *id,void **out,struct json_object **state) {
    dunlin_runtime *r=data;
    *out=NULL;*state=NULL;
    if(r->config.models && (!*r->public_session || !id || strcmp(id,r->storage_session))) return AH_FAILED;
    struct profile_lease *lease=calloc(1,sizeof *lease);
    if(!lease) return AH_FAILED;
    lease->runtime=r;
    ah_status result=r->storage.open(r->storage.ctx,id,&lease->inner,state);
    if(result!=AH_DONE) {free(lease);return result;}
    if(r->config.models) {
        strcpy(lease->conversation,r->public_session);profile_identity(r,lease->identity);
        struct json_object *conversation=member(*state,"_dunlin_conversation"),*identity=member(*state,"_dunlin_profile");
        if((*state && (!conversation || !identity)) ||
           (conversation && (!json_object_is_type(conversation,json_type_string) ||
             strcmp(json_object_get_string(conversation),lease->conversation))) ||
           (identity && (!json_object_is_type(identity,json_type_string) || strcmp(json_object_get_string(identity),lease->identity)))) {
            r->session_issue="model session conversation metadata is absent or mismatched; automatic migration is unsupported";
            r->storage.close(lease->inner);free(lease);json_object_put(*state);*state=NULL;
            return AH_FAILED;
        }
    }
    *out=lease;return AH_DONE;
}
static int profile_session_save(void *data,struct json_object *state) {
    struct profile_lease *lease=data;
    if(*lease->conversation) {
        if(!json_object_is_type(state,json_type_object)) return -1;
        struct json_object *conversation=json_object_new_string(lease->conversation),*identity=json_object_new_string(lease->identity);
        if(!conversation || !identity) {json_object_put(conversation);json_object_put(identity);return -1;}
        /* 元数据与原检查点同一次加锁/原子保存，不另建可能失配的索引。 */
        json_object_object_add(state,"_dunlin_conversation",conversation);
        if(member(state,"_dunlin_conversation")!=conversation) {
            json_object_put(conversation);json_object_put(identity);return -1;
        }
        json_object_object_add(state,"_dunlin_profile",identity);
        if(member(state,"_dunlin_profile")!=identity) {json_object_put(identity);return -1;}
    }
    return lease->runtime->storage.save(lease->inner,state);
}
static void profile_session_close(void *data) {
    struct profile_lease *lease=data;
    lease->runtime->storage.close(lease->inner);free(lease);
}
static struct json_object *profile_session_list(const ah_session_service *service) {
    return service?dunlin_conversations(service->ctx):NULL;
}
static int sessions_start(ak_scope *s,void *p) {
    dunlin_runtime *r=p;
    if(ah_linux_sessions(&r->storage,r->config.sessions)) return 1;
    r->sessions.service=(ah_session_service){r,profile_session_open,profile_session_save,profile_session_close};
    r->sessions.list=profile_session_list;
    if(ak_provide(s,"sessions",1,&r->sessions)) return 0;
    return 1;
}
static void sessions_stop(ak_scope *s,void *p) {
    (void)s;
    ah_linux_sessions_close(&((dunlin_runtime *)p)->storage);
}
static int clock_start(ak_scope *s,void *p) {
    (void)p;
    static const dunlin_clock_v1 clock={ah_os_now_ms,NULL};
    return !ak_provide(s,"clock",1,&clock);
}
static int permission_start(ak_scope *s,void *p) {
    dunlin_runtime *r=p;
    return !ak_provide(s,"permission",1,&r->permission) || !ak_provide(s,"permission",2,&r->permission_v2);
}
static int loop_start(ak_scope *s,void *p) {
    (void)p;
    static const dunlin_loop_v1 loop={ah_agent_run};
    static const dunlin_png_loop_v1 vision={ah_agent_run_png};
    static const dunlin_loop_v2 loop_v2={ah_agent_run_v2};
    static const dunlin_png_loop_v2 vision_v2={ah_agent_run_png_v2};
    return !ak_provide(s,"loop",1,&loop) || !ak_provide(s,"loop.png",1,&vision) ||
        !ak_provide(s,"loop",2,&loop_v2) || !ak_provide(s,"loop.png",2,&vision_v2);
}
static const ak_dep loop_deps[]={{"model",1,0},{"tools",1,0},{"sessions",1,0},{"clock",1,0},{"permission",1,0},{0}};
static const ak_dep http_dep[]={{"transport",1,0},{0}};
static const ak_dep tools_dep[]={{"tools",1,0},{0}};
#define PLUGIN(key,deps,start,stop) {AK_ABI,key,(const ak_key[]){{key,1},{0}},deps,NULL,start,stop,NULL}
#define TOOL(name) {AK_ABI,"tool." name,NULL,tools_dep,NULL,tool_start,NULL,NULL}
static const ak_plugin plugins[PLUGIN_COUNT]={
    {AK_ABI,"loop",(const ak_key[]){{"loop",1},{"loop.png",1},{"loop",2},{"loop.png",2},{0}},loop_deps,NULL,loop_start,NULL,NULL},PLUGIN("model",http_dep,model_start,NULL),
    PLUGIN("tools",NULL,tools_start,tools_stop),PLUGIN("sessions",NULL,sessions_start,sessions_stop),
    {AK_ABI,"permission",(const ak_key[]){{"permission",1},{"permission",2},{0}},NULL,NULL,permission_start,NULL,NULL},PLUGIN("clock",NULL,clock_start,NULL),
    TOOL("echo"),TOOL("calc"),TOOL("read_file"),TOOL("write_file"),TOOL("exec"),TOOL("http_get"),TOOL("shell")
};
static int install(dunlin_runtime *r,unsigned index) {
    if(r->scopes[index]) return 0;
    void *cfg=r;
    if(index>=BASE_COUNT) {
        struct tool_slot *slot=&r->slots[index-BASE_COUNT];
        slot->tools=&r->tools;
        slot->name=tool_names[index-BASE_COUNT];
        cfg=slot;
    }
    return ak_install(r->kernel,&plugins[index],cfg,&r->scopes[index])==AK_OK;
}
static int install_list(dunlin_runtime *r,struct json_object *list,unsigned first,unsigned end) {
    if(!list) {
        for(unsigned i=first;i<end;i++) if(!install(r,i)) return 0;
        return 1;
    }
    for(size_t j=0;j<(size_t)json_object_array_length(list);j++) {
        struct json_object *v=json_object_array_get_idx(list,j);
        if(!json_object_is_type(v,json_type_string)) return 0;
        unsigned i=first;
        for(;i<end;i++) {
            const char *name=i<BASE_COUNT?plugins[i].name:tool_names[i-BASE_COUNT];
            if(!strcmp(name,json_object_get_string(v))) break;
        }
        if(i==end || !install(r,i)) return 0;
    }
    return 1;
}
dunlin_runtime *dunlin_open(const char *file,const ah_hooks *hooks,char *error,size_t size) {
    return dunlin_open_with_transports(file,hooks,NULL,NULL,error,size);
}
dunlin_runtime *dunlin_open_with_transports(const char *file,const ah_hooks *hooks,
    const dunlin_http_v1 *transport,const dunlin_http_v2 *transport_v2,char *error,size_t size) {
    const char *issue="out of memory";
    dunlin_runtime *r=calloc(1,sizeof(*r));
    if(!r) goto bad;
    if(!dunlin_config_read(&r->config,file,error,size)) {dunlin_close(r); return NULL;}
    r->permission_v2.mode=DUNLIN_PERMISSION_REQUEST;r->permission_v2.adapter=&workspace_adapter;
    if(hooks) {
        r->permission.user=hooks->user;r->permission.approve=hooks->approve;
        r->permission_v2.request_user=hooks->user;r->permission_v2.request=hooks->approve;
    }
    ak_platform host={.alloc=allocate,.free=release,.now_ms=ah_os_now_ms};
    r->kernel=ak_create(&host);
    if(!r->kernel) goto bad;
    issue="plugin activation failed (check plugin names, dependencies and directory permissions)";
    if(!ak_provide(ak_root(r->kernel),"transport",1,(void*)(transport?transport:&dunlin_curl_http))) goto bad;
    if(!ak_provide(ak_root(r->kernel),"transport.v2",1,(void*)(transport_v2?transport_v2:&dunlin_curl_http_v2)) ||
       !ak_provide(ak_root(r->kernel),"dunlin.runtime",1,r)) goto bad;
    if(!install_list(r,r->config.plugins,0,BASE_COUNT)) goto bad;
    if(r->scopes[2] || r->config.enabled_tools) {
        /* The command-string shell is opt-in, including on older profiles. */
        if(!install_list(r,r->config.enabled_tools,BASE_COUNT,r->config.enabled_tools?PLUGIN_COUNT:PLUGIN_COUNT-1)) goto bad;
    }
    for(unsigned i=0;i<PLUGIN_COUNT;i++)
        if(r->scopes[i] && ak_status(r->scopes[i])!=AK_ACTIVE) goto bad;
    return r;
bad:
    if(error && size) snprintf(error,size,"%s",issue);
    dunlin_close(r);
    return NULL;
}
int dunlin_require_tool(dunlin_runtime *r,const char *name) {
    if(!r || r->busy || (name && (!*name || strlen(name)>=sizeof r->first_tool)) ||
       ak_get(ak_root(r->kernel),"model",1)!=&r->model) return 0;
    if(!name) {
        r->first_tool[0]=0;
        r->model.complete=prompted_completion;
        return 1;
    }
    const ah_tool_service *tools=ak_get(ak_root(r->kernel),"tools",1);
    if(!tools) return 0;
    struct json_object *schemas=tools->schema(tools->ctx);
    int found=0;
    if(json_object_is_type(schemas,json_type_array))
        for(size_t i=0;i<(size_t)json_object_array_length(schemas);i++) {
            const char *tool=function_name(json_object_array_get_idx(schemas,i));
            if(tool && !strcmp(tool,name)) {found=1; break;}
        }
    json_object_put(schemas);
    if(!found) return 0;
    strcpy(r->first_tool,name);
    r->model.complete=prompted_completion;
    r->model.config.http=&r->limited_http;
    return 1;
}
int dunlin_set_context_projector(dunlin_runtime *r,const dunlin_context_projector_v1 *projector){
    if(!r||r->busy||r->model_busy||(projector&&!projector->project))return 0;
    r->projector=projector;return 1;
}
int dunlin_set_model_trace(dunlin_runtime *r,int enabled) {
    if(!r || r->busy)return 0;
    r->model_trace=enabled!=0;return 1;
}
int dunlin_set_tool_pipeline(dunlin_runtime *r,const ah_tool_pipeline_v1 *pipeline) {
    if(!r||r->busy)return 0;r->tool_pipeline=pipeline;return 1;
}
int dunlin_set_permission_mode(dunlin_runtime *r,dunlin_permission_mode mode) {
    if(!r||r->busy||!dunlin_permission_mode_name(mode))return 0;
    r->permission_v2.mode=mode;return 1;
}
dunlin_permission_mode dunlin_get_permission_mode(const dunlin_runtime *r) {
    return r?r->permission_v2.mode:DUNLIN_PERMISSION_REQUEST;
}
int dunlin_set_permission_adapter(dunlin_runtime *r,const dunlin_permission_adapter_v1 *adapter) {
    if(!r||r->busy||(adapter&&!adapter->decide))return 0;
    r->permission_v2.adapter=adapter?adapter:&workspace_adapter;return 1;
}
int dunlin_close(dunlin_runtime *r) {
    if(!r) return AK_OK;
    if(r->busy) return AK_BUSY;
    if(r->kernel && ak_destroy(r->kernel)!=AK_OK) return AK_BUSY;
    if(r->config.document) json_object_put(r->config.document);
    free(r);
    return AK_OK;
}
ak_context *dunlin_kernel(dunlin_runtime *r) {return r?r->kernel:NULL;}
ak_scope *dunlin_scope(dunlin_runtime *r) {return ak_root(r->kernel);}
const ah_limits *dunlin_limits(dunlin_runtime *r) {return &r->config.limits;}
const char *dunlin_workspace(const dunlin_runtime *r) {return r?r->config.workspace:NULL;}
static int runtime_error(char *error,size_t size,const char *message) {
    if(error && size) snprintf(error,size,"%s",message);
    return 0;
}
static int configurable(dunlin_runtime *r,char *error,size_t size) {
    if(!r) return runtime_error(error,size,"missing runtime");
    if(r->busy) return runtime_error(error,size,"runtime is busy; model settings require idle state");
    if(ak_get(ak_root(r->kernel),"model",1)!=&r->model)
        return runtime_error(error,size,"model settings require the preset model service");
    return 1;
}
const char *dunlin_current_model(const dunlin_runtime *r) {return r?r->config.active_id:NULL;}
int dunlin_show_reasoning(const dunlin_runtime *r) {return r?r->config.show_reasoning:0;}
int dunlin_set_show_reasoning(dunlin_runtime *r,int enabled) {
    if(!r || r->busy || (enabled!=0 && enabled!=1)) return 0;
    r->config.show_reasoning=enabled;
    return 1;
}
const char *dunlin_reasoning_effort(const dunlin_runtime *r) {
    return r?dunlin_profile_effort_name(r->config.active.effort):NULL;
}
int dunlin_select_model(dunlin_runtime *r,const char *id,char *error,size_t size) {
    if(!configurable(r,error,size) || !dunlin_profile_select(&r->config,id,error,size)) return 0;
    r->model.config=r->config.active.model;
    r->model.config.http=&r->limited_http;
    if(error && size) error[0]=0;
    return 1;
}
int dunlin_set_reasoning_effort(dunlin_runtime *r,const char *effort,char *error,size_t size) {
    if(!configurable(r,error,size)) return 0;
    dunlin_profile next=r->config.active;
    if(!dunlin_profile_effort(&next,effort,error,size) || !dunlin_profile_usable(&next,error,size)) return 0;
    struct json_object *target=r->config.models?member(r->config.models,r->config.active_id):r->config.document;
    struct json_object *value=json_object_new_string(dunlin_profile_effort_name(next.effort));
    if(!value) return runtime_error(error,size,"cannot retain reasoning setting");
    json_object_object_add(target,"reasoning_effort",value);
    if(member(target,"reasoning_effort")!=value) {
        json_object_put(value);
        return runtime_error(error,size,"cannot retain reasoning setting");
    }
    /* 只保存运行内的单模型偏好，切回恢复；不改磁盘配置或历史。 */
    r->config.active=next;
    if(!r->config.models) r->config.base.effort=next.effort;
    if(error && size) error[0]=0;
    return 1;
}
struct json_object *dunlin_models(dunlin_runtime *r) {
    if(!r) return NULL;
    struct json_object *result=json_object_new_array();
    if(!result) return NULL;
    if(r->config.models) {
        json_object_object_foreach(r->config.models,key,profile) {
            const char *model=json_object_get_string(member(profile,"model"));
            const char *protocol=json_object_get_string(member(profile,"protocol"));
            struct json_object *row=json_object_new_object();
            if(!row) {json_object_put(result);return NULL;}
            json_object_object_add(row,"id",json_object_new_string(key));
            json_object_object_add(row,"model",json_object_new_string(model?model:r->config.base.model.model));
            json_object_object_add(row,"protocol",json_object_new_string(protocol?protocol:dunlin_profile_protocol_name(r->config.base.protocol)));
            json_object_object_add(row,"active",json_object_new_boolean(!strcmp(key,r->config.active_id)));
            json_object_array_add(result,row);
        }
    } else {
        struct json_object *row=json_object_new_object();
        if(!row) {json_object_put(result);return NULL;}
        json_object_object_add(row,"id",json_object_new_string("default"));
        json_object_object_add(row,"model",json_object_new_string(r->config.active.model.model));
        json_object_object_add(row,"protocol",json_object_new_string(dunlin_profile_protocol_name(r->config.active.protocol)));
        json_object_object_add(row,"active",json_object_new_boolean(1));
        json_object_array_add(result,row);
    }
    return result;
}
int dunlin_session_for_model(const dunlin_runtime *r,const char *conversation,char *out,size_t out_size,
        char *error,size_t error_size) {
    if(!r || !conversation || !*conversation || strlen(conversation)>64 ||
       strspn(conversation,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=strlen(conversation))
        return runtime_error(error,error_size,"conversation id must contain 1..64 letters, digits, '_' or '-'");
    size_t needed=r->config.models?65:strlen(conversation)+1;
    if(!out || out_size<needed) return runtime_error(error,error_size,"session output buffer is too small");
    if(!r->config.models) memmove(out,conversation,needed);
    else {
        char internal[65];
        dunlin_profile_session_id(conversation,r->config.active_id,dunlin_profile_protocol_name(r->config.active.protocol),
            r->config.active.model.model,r->config.active.model.base_url,internal);
        memcpy(out,internal,sizeof internal);
    }
    if(error && error_size) error[0]=0;
    return 1;
}
struct json_object *dunlin_conversations(dunlin_runtime *r) {
    if(!r || r->busy || !r->storage.ctx) return NULL;
    struct json_object *stored=ah_session_list(&r->storage);
    if(!stored || !r->config.models) return stored;
    struct json_object *visible=json_object_new_array();
    if(!visible) {json_object_put(stored);return NULL;}
    char identity[65];profile_identity(r,identity);
    for(size_t i=0;i<(size_t)json_object_array_length(stored);i++) {
        const char *id=json_object_get_string(json_object_array_get_idx(stored,i));
        struct json_object *state=NULL;
        ah_status result=ah_session_read_snapshot(&r->storage,id,&state);
        if(result!=AH_DONE) {json_object_put(stored);json_object_put(visible);return NULL;}
        struct json_object *conversation=member(state,"_dunlin_conversation"),*profile=member(state,"_dunlin_profile");
        const char *public_id=json_object_is_type(conversation,json_type_string)?json_object_get_string(conversation):NULL;
        char expected[65];
        if(public_id && json_object_is_type(profile,json_type_string) &&
           !strcmp(json_object_get_string(profile),identity) &&
           dunlin_session_for_model(r,public_id,expected,sizeof expected,NULL,0) && !strcmp(expected,id))
            json_object_array_add(visible,json_object_new_string(public_id));
        json_object_put(state);
    }
    json_object_put(stored);return visible;
}
static int projected_event(struct json_object *events,size_t sequence,const char *type,
        struct json_object *source,const char *const *fields) {
    struct json_object *row=json_object_new_object(),*data=json_object_new_object();
    if(!row||!data) {json_object_put(row);json_object_put(data);return 0;}
    json_object_object_add(row,"sequence",json_object_new_int64((int64_t)sequence));
    json_object_object_add(row,"type",json_object_new_string(type));
    for(size_t i=0;fields&&fields[i];i++) {
        struct json_object *value=member(source,fields[i]);
        if(value) json_object_object_add(data,fields[i],json_object_get(value));
    }
    json_object_object_add(row,"data",data);
    if(json_object_array_add(events,row)) {json_object_put(row);return 0;}
    return 1;
}
struct json_object *dunlin_session_events(dunlin_runtime *r,const char *conversation,
        char *error,size_t error_size) {
    if(!r||r->busy||!r->storage.ctx) {runtime_error(error,error_size,"runtime is busy or session storage unavailable");return NULL;}
    char id[65];
    if(!dunlin_session_for_model(r,conversation,id,sizeof id,error,error_size))return NULL;
    struct json_object *state=NULL;
    if(ah_session_read_snapshot(&r->storage,id,&state)!=AH_DONE||!state) {
        json_object_put(state);runtime_error(error,error_size,"committed session snapshot unavailable");return NULL;
    }
    struct json_object *messages=member(state,"messages");
    if(!json_object_is_type(messages,json_type_array)) {
        json_object_put(state);runtime_error(error,error_size,"invalid committed session snapshot");return NULL;
    }
    struct json_object *events=json_object_new_array();size_t sequence=0;
    static const char *const message_fields[]={"role","content","tool_call_id","tool_calls",NULL};
    static const char *const pending_fields[]={"pending",NULL};
    static const char *const end_fields[]={"status","detail",NULL};
    if(!events||!projected_event(events,sequence++,"session.started",NULL,NULL))goto allocation;
    for(size_t i=0;i<(size_t)json_object_array_length(messages);i++) {
        struct json_object *message=json_object_array_get_idx(messages,i);
        const char *role=json_object_get_string(member(message,"role"));
        const char *type=role&&!strcmp(role,"user")?"message.user":
            role&&!strcmp(role,"assistant")?(member(message,"tool_calls")?"model.tool_calls":"message.assistant"):
            role&&!strcmp(role,"tool")?"tool.result":NULL;
        if(!type||!projected_event(events,sequence++,type,message,message_fields))goto invalid;
    }
    if(member(state,"pending")&&!projected_event(events,sequence++,"tool.pending",state,pending_fields))goto allocation;
    if(member(state,"active")&&!json_object_get_boolean(member(state,"active")) &&
       !projected_event(events,sequence++,"session.ended",state,end_fields))goto allocation;
    json_object_put(state);if(error&&error_size)error[0]=0;return events;
invalid: runtime_error(error,error_size,"invalid committed session message");goto failed;
allocation: runtime_error(error,error_size,"cannot allocate session event projection");
failed: json_object_put(events);json_object_put(state);return NULL;
}
ah_result dunlin_turn_for_model(dunlin_runtime *r,const ah_hooks *observer,const char *conversation,
        const char *input,const ah_png *image,const ah_tool_service *tools) {
    if(!r) return (ah_result){AH_FAILED,NULL};
    if(r->busy || *r->public_session) return (ah_result){AH_BUSY,NULL};
    char error[160];
    if(!dunlin_session_for_model(r,conversation,r->storage_session,sizeof r->storage_session,error,sizeof error))
        return (ah_result){AH_FAILED,strdup(error)};
    strcpy(r->public_session,conversation);
    r->session_issue=NULL;
    ah_result result=dunlin_turn_with_tools(dunlin_scope(r),dunlin_limits(r),observer,r->storage_session,input,image,tools);
    if(result.status==AH_FAILED && r->session_issue) {free(result.text);result.text=strdup(r->session_issue);}
    r->public_session[0]=0;r->storage_session[0]=0;
    return result;
}
struct json_object *dunlin_plugins(dunlin_runtime *r) {
    struct json_object *items=json_object_new_array();
    for(unsigned i=0;i<PLUGIN_COUNT;i++) if(r->scopes[i] && ak_status(r->scopes[i])==AK_ACTIVE)
        json_object_array_add(items,json_object_new_string(plugins[i].name));
    return items;
}

struct delivery {ak_context *context; const char *session; const ah_hooks *hooks; const dunlin_permission_v1 *permission; const dunlin_permission_v2 *permission_v2; const dunlin_clock_v1 *clock;};
static int cancelled(void *p) {
    const ah_hooks *h=((struct delivery *)p)->hooks;
    return h && h->cancelled && h->cancelled(h->user);
}
static void event(void *p,const char *type,const char *text,size_t n) {
    struct delivery *d=p;
    dunlin_event_v1 payload={d->session,type,text,n};
    ak_dispatch(d->context,AK_EMIT,"agent/event",&payload,NULL,NULL);
    const ah_hooks *h=d->hooks;
    if(h && h->event) h->event(h->user,type,text,n);
}
static uint64_t now(void *p) {
    const dunlin_clock_v1 *clock=((struct delivery *)p)->clock;
    return clock->now_ms(clock->user);
}
static int approve(void *p,const char *name,const char *args) {
    const struct delivery *d=p;
    if(d->permission_v2)return dunlin_permission_authorize(d->permission_v2,name,args);
    return d->permission&&d->permission->approve&&d->permission->approve(d->permission->user,name,args);
}
ah_result dunlin_turn(ak_scope *scope,const ah_limits *limits,const ah_hooks *observer,
                    const char *session,const char *input) {
    return dunlin_turn_png(scope,limits,observer,session,input,NULL);
}
ah_result dunlin_turn_png(ak_scope *scope,const ah_limits *limits,const ah_hooks *observer,
                        const char *session,const char *input,const ah_png *image) {
    return dunlin_turn_with_tools(scope,limits,observer,session,input,image,NULL);
}
static ah_result dunlin_turn_with_session_adapter(ak_scope *,const ah_limits *,const ah_hooks *,const char *,const char *,const ah_png *,const ah_tool_service *,const ah_session_service *,const dunlin_clock_v1 *);
ah_result dunlin_turn_with_tools(ak_scope *scope,const ah_limits *limits,const ah_hooks *observer,
                        const char *session,const char *input,const ah_png *image,const ah_tool_service *adapter) {
    return dunlin_turn_with_session_adapter(scope,limits,observer,session,input,image,adapter,NULL,NULL);
}
ah_result dunlin_turn_with_sessions(ak_scope *scope,const ah_limits *limits,const ah_hooks *observer,
                        const char *session,const char *input,const ah_session_service *storage) {
    return dunlin_turn_with_session_adapter(scope,limits,observer,session,input,NULL,NULL,storage,NULL);
}
ah_result dunlin_turn_with_session_clock(ak_scope *scope,const ah_limits *limits,const ah_hooks *observer,
    const char *session,const char *input,const ah_session_service *storage,const dunlin_clock_v1 *clock) {
    return dunlin_turn_with_session_adapter(scope,limits,observer,session,input,NULL,NULL,storage,clock);
}
ah_result dunlin_turn_with_adapters(ak_scope *scope,const ah_limits *limits,const ah_hooks *observer,
    const char *session,const char *input,const ah_png *image,const ah_tool_service *tools,
    const ah_session_service *storage,const dunlin_clock_v1 *clock) {
    return dunlin_turn_with_session_adapter(scope,limits,observer,session,input,image,tools,storage,clock);
}
static ah_result dunlin_turn_with_session_adapter(ak_scope *scope,const ah_limits *limits,const ah_hooks *observer,
                        const char *session,const char *input,const ah_png *image,const ah_tool_service *adapter,
                        const ah_session_service *storage,const dunlin_clock_v1 *clock) {
    dunlin_runtime *runtime=(dunlin_runtime *)ak_get(scope,"dunlin.runtime",1);
    if(runtime && runtime->busy) return (ah_result){AH_BUSY,NULL};
    const dunlin_loop_v1 *loop=ak_get(scope,"loop",1);
    const dunlin_png_loop_v1 *vision=ak_get(scope,"loop.png",1);
    const dunlin_loop_v2 *loop_v2=ak_get(scope,"loop",2);
    const dunlin_png_loop_v2 *vision_v2=ak_get(scope,"loop.png",2);
    const ah_tool_pipeline_v1 *pipeline=runtime&&runtime->tool_pipeline?runtime->tool_pipeline:ak_get(scope,"tool.pipeline",1);
    if(image && (!input || !vision)) return (ah_result){AH_FAILED,NULL};
    const dunlin_model_v1 *model=ak_get(scope,"model",1);
    const dunlin_sessions_v1 *store=ak_get(scope,"sessions",1);
    const ah_tool_service *tools=adapter?adapter:ak_get(scope,"tools",1);
    struct delivery d={ak_context_of(scope),runtime && *runtime->public_session?runtime->public_session:session,
        observer,ak_get(scope,"permission",1),ak_get(scope,"permission",2),ak_get(scope,"clock",1)};
    if(clock && !clock->now_ms) return (ah_result){AH_FAILED,NULL};
    if(clock) d.clock=clock;
    if(!loop || !model || !store || !tools || (!d.permission&&!d.permission_v2) || !d.clock || !limits)
        return (ah_result){AH_FAILED,NULL};
    ah_agent agent={.model=model->complete,.model_config=&model->config,.tools=tools,.sessions=storage?storage:&store->service,
        .hooks={.user=&d,.cancelled=cancelled,.event=event,.now_ms=now,.approve=approve},.limits=*limits};
    if(runtime) {runtime->busy=1;runtime->delivery=&d;}
    event(&d,"turn.start",input?"input":"resume",input?5:6);
    ah_agent_v2 agent_v2={agent,pipeline};
    ah_result result=pipeline&&loop_v2&&(!image||vision_v2)?(image?vision_v2->run(&agent_v2,session,input,image):loop_v2->run(&agent_v2,session,input)):
        (image?vision->run(&agent,session,input,image):loop->run(&agent,session,input));
    const char *status=ah_status_name(result.status);
    event(&d,"turn.end",status,strlen(status));
    if(runtime) {runtime->delivery=NULL;runtime->busy=0;}
    return result;
}
