#include "../runtime/history.h"
#include "dunlin/compact/agent.h"
#include "dunlin/compact/json.h"
#include "png.h"
#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct json_object J;
typedef struct {
    const ah_agent *agent;
    const ah_tool_pipeline_v1 *pipeline;
    void *lease;
    J *state, *journal, *history, *schemas;
    uint64_t start, model_started, model_progress;
    int *allocation_failed; /* Shared by temporary model-request views of this turn. */
} turn;

static size_t count(J *array) { return (size_t)json_object_array_length(array); }
static J *get(J *o,const char *k) {
    J *value=NULL;
    if(json_object_is_type(o,json_type_object)) json_object_object_get_ex(o,k,&value);
    return value;
}
static const char *str(J *o,const char *k) {
    J *value=get(o,k);
    return json_object_is_type(value,json_type_string)?json_object_get_string(value):NULL;
}
static size_t bytes(J *o) {
    const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
    return s?strlen(s):SIZE_MAX;
}
static ah_result reply(ah_status status,const char *message) {
    ah_result r={status,NULL};
    if (!message) message="operation failed";
    size_t len=strlen(message)+1;
    r.text=malloc(len);
    if (r.text) memcpy(r.text,message,len); else r.status=AH_FAILED;
    return r;
}
const char *ah_status_name(ah_status st) {
    static const char *const labels[]={"completed","failed","cancelled","denied","budget","unknown","busy","timeout","invalid_arguments","execution_failed"};
    return (unsigned)st<sizeof labels/sizeof *labels?labels[st]:"invalid";
}
ah_limits ah_default_limits(void) {
    return (ah_limits){16,256,16,512*1024,64*1024,16*1024,120000,10000};
}
static void emit(const turn *t,const char *type,const char *text,size_t size) {
    const ah_hooks *h=&t->agent->hooks;
    if (h->event) h->event(h->user,type,text,size);
}
static int save(turn *t) { return !*t->allocation_failed && t->agent->sessions->save(t->lease,t->state)==0; }
/* JSON-C does not consume a value when attachment fails. Keep ownership
 * explicit, and never persist or dispatch from a partially allocated state. */
static void assign(turn *t,J *o,const char *key,J *value) {
    if(!o || !value || *t->allocation_failed || json_object_object_add(o,key,value)) {
        json_object_put(value); *t->allocation_failed=1;
    }
}
static void push(turn *t,J *array,J *value) {
    if(!array || !value || *t->allocation_failed || json_object_array_add(array,value)) {
        json_object_put(value); *t->allocation_failed=1;
    }
}
static void set_text(turn *t,J *o,const char *key,const char *s) {
    if(*t->allocation_failed)return;
    assign(t,o,key,s?json_object_new_string(s):NULL);
}
static J *message(turn *t,const char *role,const char *content) {
    J *m=json_object_new_object();
    set_text(t,m,"role",role);
    set_text(t,m,"content",content);
    if(*t->allocation_failed){json_object_put(m);return NULL;}
    return m;
}
static uint64_t elapsed(turn *t) {
    return t->agent->hooks.now_ms(t->agent->hooks.user)-t->start;
}
static ah_status interrupted(turn *t) {
    const ah_agent *a=t->agent;
    if(a->hooks.cancelled && a->hooks.cancelled(a->hooks.user)) return AH_CANCELLED;
    if(elapsed(t)>=a->limits.task_timeout_ms) return AH_TIMEOUT;
    return AH_DONE;
}
static int model_cancel(void *ctx) {
    turn *t=ctx;
    if(interrupted(t)!=AH_DONE) return 1;
    uint64_t now=t->agent->hooks.now_ms(t->agent->hooks.user);
    if(now-t->model_progress>=5000) {
        char progress[80];
        int length=snprintf(progress,sizeof progress,"elapsed_ms=%llu",
            (unsigned long long)(now-t->model_started));
        emit(t,"model.progress",progress,(size_t)length);
        t->model_progress=now;
    }
    return 0;
}
static void model_delta(void *ctx,const char *s,size_t n) { emit(ctx,"delta",s,n); }
static int share_message(J *array,J *msg);
static int within_limits(turn *t,J *addition) {
    const ah_limits *l=&t->agent->limits;
    size_t used=bytes(t->history), schemas=bytes(t->schemas), n=addition?bytes(addition):0;
    return !*t->allocation_failed && count(t->history)+(addition!=NULL)<=l->max_messages &&
        n<=l->max_message_bytes && used<l->max_context_bytes && schemas<l->max_context_bytes-used &&
        n<l->max_context_bytes-used-schemas;
}
/* Evict only closed older turns from the model view, never the durable journal
 * or any message in the current turn. No model call or external effect here. */
static int fits(turn *t,J *addition) {
    if(*t->allocation_failed)return 0;
    if(addition && bytes(addition)>t->agent->limits.max_message_bytes) return 0;
    while(!within_limits(t,addition)) {
        if(!dunlin_history_evict(&t->history)) return 0;
        const char *note="old completed turn evicted from model context";
        emit(t,"context.trimmed",note,strlen(note));
    }
    return 1;
}
static J *initial(turn *t) {
    J *o=json_object_new_object();
    assign(t,o,"version",json_object_new_int(1));
    assign(t,o,"messages",json_object_new_array());
    assign(t,o,"active",json_object_new_boolean(0));
    assign(t,o,"rounds",json_object_new_int(0));
    if(*t->allocation_failed){json_object_put(o);return NULL;}
    return o;
}
static int valid_state(J *o) {
    const char *keys[]={"repair","required_retry"};
    for(size_t i=0;i<2;i++) {
        J *repair=get(o,keys[i]);
        if(!repair) continue;
        const char *phase=str(repair,"phase");
        if(!str(repair,"name") || (!i && !str(repair,"id")) || !phase ||
           (strcmp(phase,"ready") && strcmp(phase,"sent") && strcmp(phase,"used") && (i || strcmp(phase,"preparing")))) return 0;
    }
    J *proposal=get(o,"proposal_retry");
    if(proposal) {
        const char *phase=str(proposal,"phase");
        if(!phase || (strcmp(phase,"ready") && strcmp(phase,"sent") && strcmp(phase,"used"))) return 0;
    }
    return json_object_is_type(o,json_type_object) && json_object_is_type(get(o,"version"),json_type_int) &&
        (json_object_get_int(get(o,"version"))==1 || json_object_get_int(get(o,"version"))==2) &&
        (json_object_get_int(get(o,"version"))!=1 || !proposal) && json_object_is_type(get(o,"messages"),json_type_array) &&
        json_object_is_type(get(o,"active"),json_type_boolean) && json_object_is_type(get(o,"rounds"),json_type_int) &&
        json_object_get_int64(get(o,"rounds"))>=0 && json_object_get_int64(get(o,"rounds"))<=INT_MAX;
}
/* v1 checkpoints store one user message per turn. Only successful turns
 * commit a final text assistant message. Keep all other records in the journal
 * for inspection, including tool results from turns that later hit a limit. */
static int share_message(J *array,J *msg) {
    if(json_object_array_add(array,json_object_get(msg))==0) return 1;
    json_object_put(msg);
    return 0;
}
static int load_history(turn *t,int active) {
    t->journal=get(t->state,"messages");
    t->history=json_object_new_array();
    if(!t->history) return 0;
    size_t n=count(t->journal);
    if(!n) return !active;
    for(size_t i=0;i<n;i++) {
        const char *role=str(json_object_array_get_idx(t->journal,i),"role");
        if(!role || (!i && strcmp(role,"user"))) return 0;
        /* Closed failures carry intent and observations needed by follow-ups.
         * Pending effects are rejected before reaching this reconstruction. */
        if(!share_message(t->history,json_object_array_get_idx(t->journal,i))) return 0;
    }
    return 1;
}
/* Consumes msg. Both arrays belong to this lease; an append failure aborts
 * without saving a partially updated checkpoint. */
static int append_message(turn *t,J *msg) {
    if(!msg || *t->allocation_failed){json_object_put(msg);return 0;}
    if(share_message(t->history,msg) && json_object_array_add(t->journal,msg)==0) return 1;
    json_object_put(msg);
    return 0;
}
/* Derive next call only from the assistant batch and its durable, ordered tool results. */
static int outstanding(turn *t,J **calls,size_t *done) {
    size_t n=count(t->history), i=n;
    *calls=NULL; *done=0;
    while (i) {
        const char *role=str(json_object_array_get_idx(t->history,i-1),"role");
        if (!role) return -1;
        if (strcmp(role,"tool")) break;
        --i;
    }
    if (!i) return n?-1:0;
    J *last=json_object_array_get_idx(t->history,i-1);
    J *batch=get(last,"tool_calls");
    if (!batch) return i==n?0:-1;
    if (!json_object_is_type(batch,json_type_array) || n-i>count(batch)) return -1;
    for(size_t k=0;k<n-i;k++) {
        const char *id=str(json_object_array_get_idx(batch,k),"id");
        const char *pair=str(json_object_array_get_idx(t->history,i+k),"tool_call_id");
        if(!id || !pair || strcmp(id,pair)) return -1;
    }
    *calls=batch; *done=n-i;
    return 0;
}
static ah_result terminal(turn *t,ah_status status,const char *detail) {
    if(*t->allocation_failed)return reply(AH_FAILED,"cannot allocate session state");
    if (!ah_utf8_text(detail,strlen(detail))) detail="invalid UTF-8 from plugin";
    /* Complete unused IDs with explicit non-execution records for the next user turn. */
    J *calls; size_t done;
    if (outstanding(t,&calls,&done)) return reply(AH_FAILED,"invalid checkpoint pairing");
    while(calls && done<count(calls)) {
        const char *id=str(json_object_array_get_idx(calls,done++),"id");
        if (!id) return reply(AH_FAILED,"missing call ID in checkpoint");
        J *msg=message(t,"tool",detail);
        set_text(t,msg,"tool_call_id",id);
        if (!fits(t,msg)) {
            json_object_put(msg);
            /* Keep coherent history and block this exhausted session. */
            set_text(t,t->state,"sealed","insufficient budget to close tool batch");
            status=AH_BUDGET;
            break;
        }
        if(!append_message(t,msg)) return reply(AH_FAILED,"cannot append skipped tool result");
    }
    assign(t,t->state,"active",json_object_new_boolean(0));
    assign(t,t->state,"status",json_object_new_int(status));
    set_text(t,t->state,"detail",detail);
    if (!save(t)) return reply(AH_FAILED,"cannot persist session outcome");
    return reply(status,detail);
}
static int valid_batch(J *calls) {
    for(size_t i=0;i<count(calls);i++) {
        J *call=json_object_array_get_idx(calls,i);
        const char *id=str(call,"id"),*type=str(call,"type");
        J *f=get(call,"function");
        if(!id || !*id || strlen(id)>256 || !type || strcmp(type,"function") ||
           !str(f,"name") || !str(f,"arguments")) return 0;
        for(size_t j=0;j<i;j++)
            if(!strcmp(id,str(json_object_array_get_idx(calls,j),"id"))) return 0;
    }
    return 1;
}
static ah_status model_status(dunlin_model_status s) {
    switch(s) {
    case DUNLIN_MODEL_CANCELLED: return AH_CANCELLED;
    case DUNLIN_MODEL_LIMIT: return AH_BUDGET;
    case DUNLIN_MODEL_TIMEOUT: return AH_TIMEOUT;
    default: return AH_FAILED;
    }
}
static J *tool_schema(turn *t,const char *name) {
    for(size_t i=0;i<count(t->schemas);i++) {
        J *entry=json_object_array_get_idx(t->schemas,i);
        const char *known=str(get(entry,"function"),"name");
        if(known && !strcmp(known,name)) return entry;
    }
    return NULL;
}
static dunlin_model_status request_model(turn *t,const dunlin_model_config *cfg,
        const dunlin_model_events *events,const char *required,int omitted,J **answer,char *error,size_t size) {
    J *repair=get(t->state,"repair");
    int finalizing=!required && str(repair,"kind") && !strcmp(str(repair,"phase"),"used");
    int recovery=required && str(repair,"kind");
    int prepared=json_object_get_boolean(get(repair,"prepared"));
    int regenerating=str(get(t->state,"proposal_retry"),"phase") && !strcmp(str(get(t->state,"proposal_retry"),"phase"),"sent");
    if(!required && !finalizing && !regenerating) return t->agent->model(cfg,t->history,t->schemas,events,answer,error,size);
    J *schema=required?tool_schema(t,required):NULL;
    if(required && !schema) {
        snprintf(error,size,"rejected tool is no longer available");
        return DUNLIN_MODEL_INVALID;
    }
    /* Model-visible instructions apply only to this request, including providers
     * that accept a named tool_choice but do not enforce tool generation. */
    int executed=str(get(t->state,"repair"),"kind")!=NULL;
    J *instruction=message(t,"system",finalizing?
        "纠正执行已成功。继续完成当前用户尚未完成的事项；不要重复已完成的调用。"
        "逐项对照用户要求与真实输出，尚未运行的必须继续调用工具，不能推算结果。全部完成后才简答。":
        executed?
        "命令已经执行并返回非零退出码 (executed; partial side effects possible)。"
        "读取最后工具结果里的具体错误，修正 command 或 argv。"
        "如果结果明确是只读文件系统、权限不足、所需版本没有可用来源等环境阻碍，"
        "允许直接用文字解释实际阻碍和下一步，该回合仍标记失败；不要调用无关命令冒充修复。"
        "若在盘点开发环境，缺失程序是有效观察；逐项用 if command -v 检查并输出路径或缺失标记，"
        "只查询已找到程序的版本，不用 && 串联独立检查，不原样重复空结果查询。"
        "若错误来自文件，可先调用一次必要的修复工具批次，再调用原执行工具重试。"
        "或在同一批次先修复文件，最后仅调用一次原执行工具。修复已完成时只调用原执行工具。"
        "使用新 ID，修复及执行均需审批，不要重复原错误参数，不要文字代替调用。":
        omitted?
        "Your previous response omitted the required tool. Use the conversation to resolve the user's references. "
        "Call the provided function now. Return the tool call alone, with valid arguments, no prose.":
        "The last tool call failed validation and produced no result. "
        "Your next message must call the provided function with corrected arguments. "
        "Keep the other arguments unchanged. Do not calculate or answer in text.");
    if(!instruction)return DUNLIN_MODEL_LIMIT;
    if(regenerating) {
        const char *guide="The previous tool proposal was rejected before execution. "
            "Continue the CURRENT user task from the paired results below. Never repeat completed writes or executions. "
            "Use the supplied tool JSON schema, with concise arguments and short calls. A successful command must not be repeated immediately; use its result to continue installation or version verification. "
            "Complete the remaining requested observations; final text alone is appropriate only if all are already observed. ";
        const char *prior=(required || finalizing)?str(instruction,"content"):"";
        size_t size=strlen(guide)+strlen(prior)+1;
        char *combined=malloc(size);
        if(!combined) {json_object_put(instruction);return DUNLIN_MODEL_LIMIT;}
        snprintf(combined,size,"%s%s",guide,prior);set_text(t,instruction,"content",combined);free(combined);
    }
    turn request=*t;
    request.history=json_object_new_array();
    request.schemas=(!required || (recovery && !prepared))?json_object_get(t->schemas):json_object_new_array();
    if(schema && !(recovery && !prepared)) push(t,request.schemas,json_object_get(schema));
    push(t,request.history,instruction);
    /* Concrete-call repair is local to the current turn. The failed call
     * and paired observations are retained, without examples from unrelated
     * completed tool tasks competing with this narrowed tool schema. */
    size_t first=0;
    if(repair && !omitted && (!regenerating || required)) {
        for(size_t i=count(t->history);i>0;i--) {
            const char *role=str(json_object_array_get_idx(t->history,i-1),"role");
            if(role && !strcmp(role,"user")) {
                first=i-1;
                break;
            }
        }
    }
    /* Retain the preceding task's intent and summary for referential execution
     * follow-ups, without bringing its old tool proposals into correction. */
    if(executed && first) {
        size_t previous=first;
        while(previous>0) {
            --previous;
            if(!strcmp(str(json_object_array_get_idx(t->history,previous),"role"),"user")) break;
        }
        for(size_t i=previous;i<first;i++) {
            J *m=json_object_array_get_idx(t->history,i);
            const char *role=str(m,"role");
            if(!strcmp(role,"user") || (!strcmp(role,"assistant") && !get(m,"tool_calls")))
                push(t,request.history,json_object_get(m));
        }
    }
    for(size_t i=first;i<count(t->history);i++)
        push(t,request.history,json_object_get(json_object_array_get_idx(t->history,i)));
    dunlin_model_status status=DUNLIN_MODEL_LIMIT;
    if(!*t->allocation_failed && bytes(instruction)<=t->agent->limits.max_message_bytes && fits(&request,NULL))
        status=t->agent->model(cfg,request.history,request.schemas,events,answer,error,size);
    else snprintf(error,size,"tool correction request exceeds context budget");
    json_object_put(request.history);
    json_object_put(request.schemas);
    return status;
}
/* A nonzero exit is a known observation, not proof that no effects occurred.
 * Close the rest of this batch before requesting one new approved proposal. */
static ah_result execution_failed(turn *t,const char *name,const char *id,const char *output) {
    J *previous=get(t->state,"repair");
    if(previous) {
        const char *prior_id=str(previous,"id");
        for(size_t i=0;prior_id && i<count(t->history);i++) {
            J *m=json_object_array_get_idx(t->history,i);
            const char *paired=str(m,"tool_call_id"),*content=str(m,"content");
            if(paired && content && !strcmp(paired,prior_id)) {
                size_t size=strlen(content)+strlen(output)+80;
                char *detail=malloc(size);
                if(!detail) return terminal(t,AH_FAILED,output);
                snprintf(detail,size,"Initial command failure:\n%s\nCorrection command failure:\n%s",content,output);
                ah_result result=terminal(t,AH_FAILED,detail);
                free(detail);
                return result;
            }
        }
        return terminal(t,AH_FAILED,output);
    }
    J *calls; size_t done;
    if(outstanding(t,&calls,&done)) return reply(AH_FAILED,"invalid failed command pairing");
    while(calls && done<count(calls)) {
        J *m=message(t,"tool","not executed: preceding command failed; awaiting correction");
        set_text(t,m,"tool_call_id",str(json_object_array_get_idx(calls,done++),"id"));
        if(!fits(t,m)) { json_object_put(m); return terminal(t,AH_BUDGET,"no budget to close failed batch"); }
        if(!append_message(t,m)) return reply(AH_FAILED,"cannot append skipped call");
    }
    J *repair=json_object_new_object();
    set_text(t,repair,"name",name);
    set_text(t,repair,"id",id);
    set_text(t,repair,"kind","execution");
    set_text(t,repair,"phase","ready");
    assign(t,t->state,"repair",repair);
    if(!save(t)) return reply(AH_UNKNOWN,"cannot checkpoint failed command observation");
    emit(t,"tool.execution_failed",id,strlen(id));
    return (ah_result){AH_DONE,NULL};
}
static ah_result ask_model(turn *t) {
    const ah_agent *a=t->agent;
    J *proposal=get(t->state,"proposal_retry");
    const char *proposal_phase=str(proposal,"phase");
    if(proposal_phase && !strcmp(proposal_phase,"sent"))
        return terminal(t,AH_FAILED,"model proposal retry interrupted; retry already consumed");
    int regenerating=proposal_phase && !strcmp(proposal_phase,"ready");
    int rounds=json_object_get_int(get(t->state,"rounds"));
    J *repair=get(t->state,"repair");
    J *required=get(t->state,"required_retry");
    int omitted=required && strcmp(str(required,"phase"),"used")!=0;
    if(omitted) repair=required;
    const char *phase=str(repair,"phase");
    if(phase && !strcmp(phase,"preparing")) {set_text(t,repair,"phase","ready");phase=str(repair,"phase");}
    if(phase && !strcmp(phase,"sent"))
        return terminal(t,AH_FAILED,"tool correction interrupted; retry already consumed");
    int correcting=phase && !strcmp(phase,"ready");
    if ((unsigned)rounds>=a->limits.max_rounds) return terminal(t,AH_BUDGET,"model round limit reached");
    if(correcting) set_text(t,repair,"phase","sent");
    if(regenerating) set_text(t,proposal,"phase","sent");
    assign(t,t->state,"rounds",json_object_new_int(rounds+1));
    if (!save(t)) return reply(AH_FAILED,"cannot checkpoint model round");
    dunlin_model_config cfg=*a->model_config;
    dunlin_model_events events={t,model_cancel,(correcting || regenerating)?NULL:model_delta};
    uint64_t used=elapsed(t);
    if (used>=a->limits.task_timeout_ms) return terminal(t,AH_TIMEOUT,"task timed out");
    long remaining=(long)(a->limits.task_timeout_ms-used);
    if (cfg.timeout_ms>remaining) cfg.timeout_ms=remaining;
    J *assistant=NULL;
    char error[256]="model request failed";
    t->model_started=t->model_progress=a->hooks.now_ms(a->hooks.user);
    dunlin_model_status rc=request_model(t,&cfg,&events,correcting?str(repair,"name"):NULL,omitted,
                                      &assistant,error,sizeof error);
    ah_status interruption=interrupted(t);
    if (rc!=DUNLIN_MODEL_OK || interruption!=AH_DONE) {
        json_object_put(assistant);
        if (interruption!=AH_DONE) return terminal(t,interruption,ah_status_name(interruption));
        if(rc==DUNLIN_MODEL_TOOL_FORMAT && !proposal) {
            if(correcting) set_text(t,repair,"phase","ready");
            J *retry=json_object_new_object();set_text(t,retry,"phase","ready");
            assign(t,t->state,"proposal_retry",retry);
            assign(t,t->state,"version",json_object_new_int(2));
            if(!save(t)) return reply(AH_FAILED,"cannot checkpoint proposal retry");
            emit(t,"model.proposal_retry","tool proposal rejected before execution",strlen("tool proposal rejected before execution"));
            return (ah_result){AH_DONE,NULL};
        }
        if(rc==DUNLIN_MODEL_REQUIRED_TOOL_MISSING) {
            if(required || correcting || !tool_schema(t,error))
                return terminal(t,AH_FAILED,"required tool missing; nothing executed");
            J *retry=json_object_new_object();
            set_text(t,retry,"name",error);
            set_text(t,retry,"phase","ready");
            assign(t,t->state,"required_retry",retry);
            if(!save(t)) return reply(AH_FAILED,"cannot checkpoint required tool retry");
            emit(t,"tool.required_retry",error,strlen(error));
            return (ah_result){AH_DONE,NULL};
        }
        return terminal(t,model_status(rc),error);
    }
    if(regenerating) set_text(t,proposal,"phase","used");
    const char *encoded=assistant?json_object_to_json_string_ext(assistant,JSON_C_TO_STRING_PLAIN):NULL;
    const char *role=str(assistant,"role");
    J *batch=get(assistant,"tool_calls");
    const char *content=str(assistant,"content");
    ah_status failure=AH_DONE;
    int preparation=0;
    const char *why=NULL;
    char reason[128];
    if (!role || strcmp(role,"assistant") || (batch && !json_object_is_type(batch,json_type_array))) {
        failure=AH_FAILED; why="invalid assistant response";
    } else if (!encoded || !ah_utf8_text(encoded,strlen(encoded))) {
        failure=AH_FAILED; why="invalid UTF-8 in assistant response";
    } else if (batch && count(batch)>a->limits.max_calls) {
        snprintf(reason,sizeof reason,"assistant proposed %zu calls; max_calls is %u",count(batch),a->limits.max_calls);
        failure=AH_BUDGET; why=reason;
    } else if (!fits(t,assistant)) {
        failure=AH_BUDGET;
        why=bytes(assistant)>a->limits.max_message_bytes?"assistant exceeds max_message_bytes":
            count(t->history)>=a->limits.max_messages?"assistant exceeds max_messages":"assistant exceeds max_context_bytes";
    } else if ((batch && (!count(batch) || !valid_batch(batch))) || (!batch && !content)) {
        failure=AH_FAILED; why="incomplete or duplicate model call";
    } else if(correcting && str(repair,"kind") && !omitted && !batch && content && *content) {
        set_text(t,repair,"phase","used");
        failure=AH_FAILED; why=content;
    } else if(correcting && str(repair,"kind") && !omitted) {
        size_t original=0;int valid=batch && count(batch);
        for(size_t i=0;valid && i<count(batch);i++) {
            J *call=json_object_array_get_idx(batch,i);
            const char *name=str(get(call,"function"),"name");
            if(!strcmp(str(call,"id"),str(repair,"id")) || !tool_schema(t,name)) valid=0;
            if(!strcmp(name,str(repair,"name"))) original++;
        }
        if(valid && original==0 && !json_object_get_boolean(get(repair,"prepared"))) preparation=1;
        else if(!valid || original!=1 || strcmp(str(get(json_object_array_get_idx(batch,count(batch)-1),"function"),"name"),str(repair,"name")) ||
                (json_object_get_boolean(get(repair,"prepared")) && count(batch)!=1)) {
            failure=AH_FAILED;why="correction requires one retry of the original tool, optionally after one repair batch";
        }
    } else if(correcting) {
        J *call=batch && count(batch)==1?json_object_array_get_idx(batch,0):NULL;
        if (!call || strcmp(str(get(call,"function"),"name"),str(repair,"name")) ||
            (!omitted && !strcmp(str(call,"id"),str(repair,"id")))) {
            failure=AH_FAILED; why=omitted?"required tool still missing or ambiguous after one correction; nothing executed":
                                   "correction requires one new call to the rejected tool";
        }
    }
    if (why) {
        ah_result stopped=terminal(t,failure,why);
        json_object_put(assistant);
        return stopped;
    }
    if(correcting) {
        set_text(t,repair,"phase",preparation?"preparing":"used");
        if(preparation) {assign(t,repair,"prepared",json_object_new_boolean(1));assign(t,t->state,"version",json_object_new_int(2));}
    }
    if(!append_message(t,assistant)) return reply(AH_FAILED,"cannot append assistant message");
    if (!batch) return terminal(t,AH_DONE,content);
    if(!save(t)) return reply(AH_FAILED,"cannot checkpoint assistant tool batch");
    return (ah_result){AH_DONE,NULL}; /* continue */
}
/* Only a known, unexecuted call is eligible. Save the paired error and the
 * retry allowance together so a restarted process cannot grant a second try. */
static ah_result invalid_arguments(turn *t,const char *name,const char *id,const char *reason) {
    ah_status stop=interrupted(t);
    if(stop!=AH_DONE) return terminal(t,stop,ah_status_name(stop));
    char feedback[384];
    if(get(t->state,"repair")) {
        snprintf(feedback,sizeof feedback,"Argument correction unavailable for another invalid call: %s",reason);
        return terminal(t,AH_FAILED,feedback);
    }
    snprintf(feedback,sizeof feedback,"Invalid arguments. %s. Correct the arguments and call the same tool once.",reason);
    J *m=message(t,"tool",feedback);
    set_text(t,m,"tool_call_id",id);
    if(strlen(feedback)>t->agent->limits.max_output_bytes || !fits(t,m)) {
        json_object_put(m);
        return terminal(t,AH_BUDGET,"tool validation feedback exceeds budget");
    }
    J *repair=json_object_new_object();
    set_text(t,repair,"name",name);
    set_text(t,repair,"id",id);
    set_text(t,repair,"phase","ready");
    assign(t,t->state,"repair",repair);
    if(!append_message(t,m)) return reply(AH_FAILED,"cannot append validation feedback");
    if(!save(t)) return reply(AH_FAILED,"cannot checkpoint validation feedback");
    emit(t,"tool.invalid_arguments",id,strlen(id));
    return (ah_result){AH_DONE,NULL};
}
static ah_result invoke(turn *t,J *call) {
    const ah_agent *a=t->agent;
    const char *id=str(call,"id");
    J *f=get(call,"function");
    const char *name=str(f,"name"), *arguments=str(f,"arguments");
    J *args=arguments?dunlin_json_parse(arguments,strlen(arguments)):NULL;
    if (!json_object_is_type(args,json_type_object)) {
        const char *why=args?"arguments must be a JSON object":"arguments must be valid JSON with unique keys";
        json_object_put(args);
        if(tool_schema(t,name)) return invalid_arguments(t,name,id,why);
        return terminal(t,AH_FAILED,"unknown or disabled tool");
    }
    char error[256]="invalid tool";
    int approval=0,effect=0;
    ah_status rc=a->tools->check(a->tools->ctx,name,args,&approval,&effect,error,sizeof error);
    if(rc==AH_INVALID_ARGUMENTS) {
        json_object_put(args);
        return invalid_arguments(t,name,id,error);
    }
    if(rc==AH_DONE && t->pipeline && t->pipeline->pre_execute) {
        J *replacement=NULL;
        rc=t->pipeline->pre_execute(t->pipeline->ctx,id,name,args,&replacement,error,sizeof error);
        if(rc==AH_DONE && replacement) {
            if(!json_object_is_type(replacement,json_type_object)) {
                json_object_put(replacement);json_object_put(args);
                return terminal(t,AH_INVALID_ARGUMENTS,"pipeline replacement must be an object");
            }
            int next_approval=0,next_effect=0;
            ah_status checked=a->tools->check(a->tools->ctx,name,replacement,&next_approval,&next_effect,error,sizeof error);
            if(checked!=AH_DONE) {
                json_object_put(replacement);json_object_put(args);
                return terminal(t,checked,error);
            }
            approval=approval||next_approval;effect=effect||next_effect;
            json_object_put(args);args=replacement;
            arguments=json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN);
        } else json_object_put(replacement);
        if(rc!=AH_DONE) {json_object_put(args);return terminal(t,rc,*error?error:"tool pipeline rejected call");}
    }
    const char *detail=error;
    J *envelope=message(t,"tool","");
    set_text(t,envelope,"tool_call_id",id);
    if (rc==AH_DONE && !fits(t,envelope)) {
        rc=AH_BUDGET; detail="no budget for paired tool result";
    }
    json_object_put(envelope);
    if(rc==AH_DONE && approval &&
       (!a->hooks.approve || !a->hooks.approve(a->hooks.user,name,arguments))) {
        rc=AH_DENIED; detail="permission denied";
    }
    ah_status stop=interrupted(t);
    if(stop!=AH_DONE) { rc=stop; detail=ah_status_name(stop); }
    if(rc!=AH_DONE) { json_object_put(args); return terminal(t,rc,detail); }

    if(effect) {
        set_text(t,t->state,"pending",id);
        set_text(t,t->state,"pending_arguments",arguments);
        if(!save(t)) { json_object_put(args); return reply(AH_FAILED,"cannot checkpoint pending effect"); }
    }
    /* This event means validation, approval and (for effects) the durable
     * pending guard all succeeded. It never means the tool was called. */
    emit(t,"tool.pre_execute",id,strlen(id));
    emit(t,"tool.start",id,strlen(id));
    unsigned timeout=a->limits.tool_timeout_ms;
    uint64_t used=elapsed(t);
    unsigned remaining=used>=a->limits.task_timeout_ms?0:(unsigned)(a->limits.task_timeout_ms-used);
    if(timeout>remaining) timeout=remaining;
    char *output=NULL;
    if (!remaining) {
        rc=AH_TIMEOUT;
    } else rc=a->tools->call(a->tools->ctx,name,args,&a->hooks,a->limits.max_output_bytes,timeout,&output);
    json_object_put(args);
    if(rc==AH_UNKNOWN) {
        /* pending remains durable; never dispatch this ID again. */
        emit(t,"tool.outcome_unknown",id,strlen(id));
        ah_result r=reply(AH_UNKNOWN,output?output:"external outcome unknown");
        free(output);
        return r;
    }
    /* The call returned a known status. Projection and durable finalization
     * still follow, so observers must not treat this as a committed result. */
    emit(t,"tool.post_execute",id,strlen(id));
    char *context=NULL;
    if(t->pipeline && t->pipeline->post_execute) {
        ah_status hook=t->pipeline->post_execute(t->pipeline->ctx,id,name,rc,output,&context);
        if(hook!=AH_DONE) {free(context);context=NULL;emit(t,"tool.pipeline_error",id,strlen(id));}
    }
    const char *value=output?output:(rc==AH_TIMEOUT?"task timed out":"tool returned no result");
    char *combined=NULL;
    if(context) {
        size_t x=strlen(value),y=strlen(context);
        if(x<a->limits.max_output_bytes && y<a->limits.max_output_bytes-x) {
            combined=malloc(x+y+2);
            if(combined){memcpy(combined,value,x);combined[x]='\n';memcpy(combined+x+1,context,y+1);value=combined;}
        }
        free(context);
    }
    if (rc==AH_DONE && !output) rc=AH_FAILED;
    if(strlen(value)>a->limits.max_output_bytes) { rc=AH_BUDGET; value="tool output exceeds budget"; }
    if (!ah_utf8_text(value,strlen(value))) { rc=AH_FAILED; value="invalid UTF-8 in tool result"; }
    J *m=message(t,"tool",value);
    set_text(t,m,"tool_call_id",id);
    if(!fits(t,m)) {
        json_object_put(m); free(combined); free(output);
        if(effect) return reply(AH_UNKNOWN,"effect executed but result cannot fit checkpoint");
        return terminal(t,AH_BUDGET,"tool result exceeds context budget");
    }
    if(!append_message(t,m)) {
        free(combined); free(output);
        return reply(effect?AH_UNKNOWN:AH_FAILED,"cannot append tool result");
    }
    json_object_object_del(t->state,"pending");
    json_object_object_del(t->state,"pending_arguments");
    /* Persist failure and all skipped IDs atomically; a crash cannot retry or advance past a known failure. */
    ah_result r={AH_DONE,NULL};
    if(rc==AH_EXEC_FAILED) r=execution_failed(t,name,id,value);
    else if(rc!=AH_DONE) r=terminal(t,rc,value);
    else if(!save(t)) r=reply(effect?AH_UNKNOWN:AH_FAILED,"cannot checkpoint tool result");
    if(effect && rc!=AH_DONE && r.status==AH_FAILED && r.text && !strcmp(r.text,"cannot persist session outcome"))
        r.status=AH_UNKNOWN;
    if (!r.text || r.status==rc) {
        if(t->pipeline && t->pipeline->finalized)t->pipeline->finalized(t->pipeline->ctx,id,name,rc);
        emit(t,"tool.finalized",id,strlen(id));
        emit(t,"tool.result",id,strlen(id)); /* compatibility event */
    }
    free(combined);free(output);
    return r;
}
static ah_result run(turn *t,const char *input,const ah_png *image) {
    if(!valid_state(t->state)) return reply(AH_FAILED,"unsupported or corrupt session");
    if(get(t->state,"pending")) return reply(AH_UNKNOWN,"unresolved external effect; inspect checkpoint before proceeding");
    if(get(t->state,"sealed")) return reply(AH_BUDGET,"session budget exhausted");
    int active=json_object_get_boolean(get(t->state,"active"));
    if(input && active) return reply(AH_BUSY,"unfinished session; resume without input");
    if(!input && !active) {
        if(!get(t->state,"status")) return reply(AH_FAILED,"no saved turn");
        return reply(json_object_get_int(get(t->state,"status")),str(t->state,"detail"));
    }
    if(!load_history(t,active)) return reply(AH_FAILED,"cannot reconstruct session history");
    t->schemas=t->agent->tools->schema(t->agent->tools->ctx);
    if(!json_object_is_type(t->schemas,json_type_array)) return reply(AH_FAILED,"invalid tool schema");
    if(input) {
        if(!*input || strlen(input)>t->agent->limits.max_message_bytes) return reply(AH_BUDGET,"invalid input size");
        if (!ah_utf8_text(input,strlen(input))) return reply(AH_FAILED,"invalid UTF-8 input");
        J *m=message(t,"user",input);
        if(image) {
            if(image->size > t->agent->limits.max_message_bytes) {
                json_object_put(m); return reply(AH_BUDGET,"image exceeds message budget");
            }
            J *content=ah_png_content(input,image);
            if(!content) { json_object_put(m); return reply(AH_FAILED,"invalid or oversized PNG"); }
            assign(t,m,"content",content);
        }
        if(!fits(t,m)) { json_object_put(m); return reply(AH_BUDGET,"context limit reached"); }
        if(!append_message(t,m)) return reply(AH_FAILED,"cannot append user message");
        json_object_object_del(t->state,"repair");
        json_object_object_del(t->state,"required_retry");
        json_object_object_del(t->state,"proposal_retry");
        assign(t,t->state,"rounds",json_object_new_int(0));
        assign(t,t->state,"active",json_object_new_boolean(1));
        if(!save(t)) return reply(AH_FAILED,"cannot checkpoint input");
    }
    for (;;) {
        ah_status stop=interrupted(t);
        if(stop!=AH_DONE) return terminal(t,stop,ah_status_name(stop));
        if(!fits(t,NULL)) return terminal(t,AH_BUDGET,"context limit reached");
        J *calls=NULL; size_t done=0;
        if(outstanding(t,&calls,&done)) return reply(AH_FAILED,"invalid session pairing");
        ah_result r;
        if(calls && done<count(calls)) {
            if(count(calls)>t->agent->limits.max_calls || !valid_batch(calls))
                return reply(AH_FAILED,"invalid saved tool batch");
            r=invoke(t,json_object_array_get_idx(calls,done));
        } else r=ask_model(t);
        if(r.status!=AH_DONE || r.text) return r;
    }
}
static ah_result run_agent(const ah_agent *a,const ah_tool_pipeline_v1 *pipeline,const char *id,const char *input,const ah_png *image) {
    if ((image && !input) || !a || !a->hooks.now_ms || !a->sessions || !a->sessions->open || !a->sessions->save ||
        !a->sessions->close || !a->tools || !a->tools->schema || !a->tools->check || !a->tools->call ||
        !a->model || !a->model_config || a->limits.max_rounds>INT_MAX || a->limits.task_timeout_ms>INT_MAX)
        return reply(AH_FAILED,"invalid agent configuration");
    turn t={0};int allocation_failed=0;t.allocation_failed=&allocation_failed;
    t.agent=a;t.pipeline=pipeline;t.start=a->hooks.now_ms(a->hooks.user);
    ah_status rc=a->sessions->open(a->sessions->ctx,id,&t.lease,&t.state);
    if (rc!=AH_DONE) return reply(rc,rc==AH_BUSY?"session locked":"cannot open session");
    if (!t.state) t.state=initial(&t);
    ah_result r=t.state?run(&t,input,image):reply(AH_FAILED,"cannot allocate initial session");
    if(allocation_failed && r.status!=AH_UNKNOWN) {free(r.text);r=reply(AH_FAILED,"cannot allocate session state");}
    json_object_put(t.schemas); json_object_put(t.history); json_object_put(t.state);
    a->sessions->close(t.lease);
    return r;
}
ah_result ah_agent_run(const ah_agent *a,const char *id,const char *input){return run_agent(a,NULL,id,input,NULL);}
ah_result ah_agent_run_png(const ah_agent *a,const char *id,const char *input,const ah_png *image){return run_agent(a,NULL,id,input,image);}
ah_result ah_agent_run_v2(const ah_agent_v2 *a,const char *id,const char *input){return a?run_agent(&a->base,a->pipeline,id,input,NULL):reply(AH_FAILED,"invalid agent configuration");}
ah_result ah_agent_run_png_v2(const ah_agent_v2 *a,const char *id,const char *input,const ah_png *image){return a?run_agent(&a->base,a->pipeline,id,input,image):reply(AH_FAILED,"invalid agent configuration");}
