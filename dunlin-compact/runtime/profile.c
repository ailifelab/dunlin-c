#include "internal.h"
#include <stdio.h>
#include <string.h>

static int fail(char *error,size_t size,const char *message) {
    if(error && size) snprintf(error,size,"%s",message);
    return 0;
}
static const char *const protocols[]={"chat","responses","anthropic","ollama","gemini"};
static const char *const efforts[]={"default","none","low","medium","high","xhigh"};
const char *dunlin_profile_protocol_name(dunlin_protocol value) {
    return (unsigned)value<sizeof protocols/sizeof *protocols?protocols[value]:NULL;
}
const char *dunlin_profile_effort_name(dunlin_reasoning_level value) {
    return (unsigned)value<sizeof efforts/sizeof *efforts?efforts[value]:NULL;
}
static int text(struct json_object *o,const char *field,const char **value,int empty) {
    struct json_object *v;
    if(!json_object_object_get_ex(o,field,&v)) return 1;
    if(!json_object_is_type(v,json_type_string)) return 0;
    const char *s=json_object_get_string(v);
    size_t n=(size_t)json_object_get_string_len(v);
    if((!empty && !n) || !ah_utf8_text(s,n)) return 0;
    for(size_t i=0;i<n;i++) if((unsigned char)s[i]<32 || (unsigned char)s[i]==127) return 0;
    *value=s;
    return 1;
}
static int choice(const char *name,const char *const *values,size_t count) {
    for(size_t i=0;i<count;i++) if(!strcmp(name,values[i])) return (int)i;
    return -1;
}
static int effort_supported(const dunlin_profile *p) {
    return p->effort==DUNLIN_REASONING_DEFAULT ||
        (p->effort==DUNLIN_REASONING_NONE && (p->reasoning_supported&DUNLIN_REASONING_CAN_DISABLE)) ||
        (p->effort>=DUNLIN_REASONING_LOW && p->effort<=DUNLIN_REASONING_HIGH &&
         (p->reasoning_supported&DUNLIN_REASONING_CAN_EFFORT)) ||
        (p->effort==DUNLIN_REASONING_XHIGH && (p->reasoning_supported&DUNLIN_REASONING_CAN_XHIGH) &&
         (p->protocol==DUNLIN_PROTOCOL_RESPONSES || p->protocol==DUNLIN_PROTOCOL_CHAT));
}
int dunlin_profile_effort(dunlin_profile *p,const char *name,char *error,size_t size) {
    int value=name?choice(name,efforts,sizeof efforts/sizeof *efforts):-1;
    if(value<0) return fail(error,size,"unknown reasoning effort; use default/none/low/medium/high/xhigh");
    dunlin_profile next=*p;
    next.effort=(dunlin_reasoning_level)value;
    if(!effort_supported(&next)) return fail(error,size,"unsupported reasoning effort for this model's declared capabilities");
    *p=next;
    return 1;
}
int dunlin_profile_usable(const dunlin_profile *p,char *error,size_t size) {
    if(!effort_supported(p)) return fail(error,size,"unsupported reasoning effort for this model's declared capabilities");
    if(p->protocol==DUNLIN_PROTOCOL_ANTHROPIC && !p->max_tokens)
        return fail(error,size,"anthropic requires an explicit max_tokens limit");
    if(p->protocol==DUNLIN_PROTOCOL_ANTHROPIC && p->temperature)
        return fail(error,size,"temperature is unsupported by the pinned anthropic protocol");
    if(p->protocol!=DUNLIN_PROTOCOL_CHAT && (p->omlx_chat_template || p->enable_thinking))
        return fail(error,size,"enable_thinking and omlx_chat_template are supported only for chat");
    return 1;
}
static int overlay(dunlin_profile *p,struct json_object *o,int profile,char *error,size_t size) {
    static const char *const allowed[]={"base_url","model","key_env","protocol","max_tokens",
        "temperature","reasoning_effort","reasoning_capabilities","enable_thinking","omlx_chat_template","chat_tool_metadata",NULL};
    if(!json_object_is_type(o,json_type_object)) return fail(error,size,"model profile must be an object");
    if(profile) { json_object_object_foreach(o,key,value) {
        (void)value;
        size_t i=0; while(allowed[i] && strcmp(allowed[i],key)) i++;
        if(!allowed[i]) return fail(error,size,"unrecognized model profile field (workspace/session/limits cannot be overridden)");
    } }
    if(!text(o,"base_url",&p->model.base_url,0) || !text(o,"model",&p->model.model,0) ||
       !text(o,"key_env",&p->model.key_env,1)) return fail(error,size,"invalid model profile text field");
    if(p->model.base_url && !dunlin_model_url_valid(p->model.base_url))
        return fail(error,size,"base_url must be a valid http(s) API root without credentials or query");
    if(p->model.key_env && *p->model.key_env) {
        const char *name=p->model.key_env;
        if(strspn(name,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789")!=strlen(name) ||
           (*name>='0' && *name<='9')) return fail(error,size,"invalid key_env name");
    } else p->model.key_env=NULL;
    const char *protocol=dunlin_profile_protocol_name(p->protocol), *effort=dunlin_profile_effort_name(p->effort);
    if(!text(o,"protocol",&protocol,0) || !text(o,"reasoning_effort",&effort,0))
        return fail(error,size,"invalid protocol or reasoning_effort");
    int protocol_id=choice(protocol,protocols,sizeof protocols/sizeof *protocols);
    int effort_id=choice(effort,efforts,sizeof efforts/sizeof *efforts);
    if(protocol_id<0 || effort_id<0) return fail(error,size,"unknown protocol or reasoning_effort");
    p->protocol=(dunlin_protocol)protocol_id; p->effort=(dunlin_reasoning_level)effort_id;
    const char *metadata=p->chat_metadata_complete?"complete":"delta";
    if(!text(o,"chat_tool_metadata",&metadata,0) || (strcmp(metadata,"delta") && strcmp(metadata,"complete")))
        return fail(error,size,"chat_tool_metadata must be delta or complete");
    p->chat_metadata_complete=!strcmp(metadata,"complete");
    struct json_object *v;
    if(json_object_object_get_ex(o,"max_tokens",&v)) {
        int64_t value=json_object_get_int64(v);
        if(!json_object_is_type(v,json_type_int) || value<1 || value>65536) return fail(error,size,"invalid max_tokens");
        p->max_tokens=(unsigned)value;
    }
    if(json_object_object_get_ex(o,"temperature",&v)) {
        double value=json_object_get_double(v);
        if((!json_object_is_type(v,json_type_int) && !json_object_is_type(v,json_type_double)) ||
           !(value>=0 && value<=2)) return fail(error,size,"temperature must be a number between 0 and 2");
        p->temperature=v;
    }
    if(json_object_object_get_ex(o,"enable_thinking",&v)) {
        if(!json_object_is_type(v,json_type_boolean)) return fail(error,size,"enable_thinking must be boolean");
        p->enable_thinking=v;
    }
    if(json_object_object_get_ex(o,"omlx_chat_template",&v)) {
        int length=json_object_get_string_len(v);
        if(!json_object_is_type(v,json_type_string) || length<1 || length>8192 ||
           !ah_utf8_text(json_object_get_string(v),(size_t)length))
            return fail(error,size,"omlx_chat_template must be UTF-8 text of 1..8192 bytes");
        p->omlx_chat_template=v;
    }
    if(json_object_object_get_ex(o,"reasoning_capabilities",&v)) {
        if(!json_object_is_type(v,json_type_array) || json_object_array_length(v)>3)
            return fail(error,size,"reasoning_capabilities accepts none, levels, xhigh");
        p->reasoning_supported=0;
        for(size_t i=0;i<(size_t)json_object_array_length(v);i++) {
            struct json_object *item=json_object_array_get_idx(v,i);
            if(!json_object_is_type(item,json_type_string)) return fail(error,size,"invalid reasoning capability");
            const char *name=json_object_get_string(item);
            unsigned bit=!strcmp(name,"none")?DUNLIN_REASONING_CAN_DISABLE:!strcmp(name,"levels")?DUNLIN_REASONING_CAN_EFFORT:!strcmp(name,"xhigh")?DUNLIN_REASONING_CAN_XHIGH:0;
            if(!bit || (p->reasoning_supported&bit)) return fail(error,size,"unknown or duplicate reasoning capability");
            p->reasoning_supported|=bit;
        }
    }
    return 1;
}
static int complete(const dunlin_profile *p,char *error,size_t size) {
    if(!p->model.base_url || !p->model.model) return fail(error,size,"each model requires base_url and model, inherited or explicit");
    if(!effort_supported(p)) return fail(error,size,"unsupported reasoning effort for this model's declared capabilities");
    return 1;
}
int dunlin_profile_select(dunlin_config *c,const char *id,char *error,size_t size) {
    dunlin_profile next=c->base;
    const char *selected="default";
    if(!id || !*id) return fail(error,size,"missing model id");
    if(c->models) {
        struct json_object *profile=NULL;
        if(!json_object_object_get_ex(c->models,id,&profile)) return fail(error,size,"unknown model id");
        if(!overlay(&next,profile,1,error,size)) return 0;
        /* 借用对象自身的 key，调用者的 id 缓冲区可立即释放。 */
        json_object_object_foreach(c->models,key,value) {
            (void)value;
            if(!strcmp(key,id)) {selected=key; break;}
        }
    } else if(strcmp(id,"default")) return fail(error,size,"unknown model id");
    if(!complete(&next,error,size) || !dunlin_profile_usable(&next,error,size)) return 0;
    c->active=next; c->active_id=selected;
    return 1;
}
int dunlin_profiles_read(dunlin_config *c,char *error,size_t size) {
    if(!overlay(&c->base,c->document,0,error,size)) return 0;
    const char *active=NULL;
    if(!text(c->document,"active_model",&active,0)) return fail(error,size,"invalid active_model");
    if(json_object_object_get_ex(c->document,"models",&c->models)) {
        if(!json_object_is_type(c->models,json_type_object) || !json_object_object_length(c->models))
            return fail(error,size,"models must be a nonempty object");
        json_object_object_foreach(c->models,key,value) {
            size_t length=strlen(key);
            if(!length || length>64 || strspn(key,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=length)
                return fail(error,size,"model ids must contain 1..64 letters, digits, '_' or '-'");
            dunlin_profile candidate=c->base;
            if(!overlay(&candidate,value,1,error,size) || !complete(&candidate,error,size)) return 0;
            if(!active) active=key;
        }
    }
    return dunlin_profile_select(c,active?active:"default",error,size);
}
