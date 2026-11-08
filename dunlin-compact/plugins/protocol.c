#include "protocol_internal.h"
#include "dunlin/compact/json.h"
#include "../platform/random.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

int ahp_fail(ahp_context *c,dunlin_model_status status,const char *reason) {
    c->status=status;snprintf(c->reason,sizeof c->reason,"%s",reason);return 0;
}
int ahp_append(ahp_context *c,ahp_buffer *b,const char *s,size_t n) {
    if(n>c->cfg->max_response_bytes || b->n>c->cfg->max_response_bytes-n)
        return ahp_fail(c,DUNLIN_MODEL_LIMIT,"model response exceeds byte limit");
    size_t need=b->n+n+1;
    if(need>b->capacity) {
        size_t cap=b->capacity?b->capacity*2:64;
        if(cap<need)cap=need;
        if(cap>c->cfg->max_response_bytes+1)cap=c->cfg->max_response_bytes+1;
        char *p=realloc(b->s,cap);
        if(!p)return ahp_fail(c,DUNLIN_MODEL_LIMIT,"model response allocation failed");
        b->s=p;b->capacity=cap;
    }
    if(n)memcpy(b->s+b->n,s,n);
    b->n+=n;b->s[b->n]=0;return 1;
}
void ahp_buffer_free(ahp_buffer *b) {free(b->s);memset(b,0,sizeof *b);}
ahp_json *ahp_field(ahp_json *o,const char *name) {ahp_json *v=NULL;json_object_object_get_ex(o,name,&v);return v;}
const char *ahp_str(ahp_json *o,const char *name) {
    ahp_json *v=ahp_field(o,name);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;
}
int ahp_is(const char *a,const char *b) {return a && !strcmp(a,b);}
ahp_json *ahp_clone(ahp_json *o) {
    if(!o)return NULL;
    const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
    return dunlin_json_parse(s,strlen(s));
}
ahp_json *ahp_native(ahp_context *c,ahp_json *msg,const char *protocol) {
    ahp_json *meta=ahp_field(msg,"_dunlin_provider");
    if(!meta)return NULL;
    if(!json_object_is_type(meta,json_type_object) || !ahp_is(ahp_str(meta,"protocol"),protocol) ||
       !ahp_field(meta,"output")) {
        ahp_fail(c,DUNLIN_MODEL_INVALID,"provider continuation state does not match protocol");return NULL;
    }
    return ahp_field(meta,"output");
}
int ahp_metadata(ahp_context *c,ahp_json *msg,const char *protocol,ahp_json *native) {
    ahp_json *meta=json_object_new_object();
    ahp_json *name=json_object_new_string(protocol);
    if(!meta || !name) {
        if(meta)json_object_put(meta);
        if(name)json_object_put(name);
        return ahp_fail(c,DUNLIN_MODEL_LIMIT,"model continuation allocation failed");
    }
    json_object_object_add(meta,"protocol",name);
    if(ahp_field(meta,"protocol")!=name) {
        json_object_put(name);json_object_put(meta);
        return ahp_fail(c,DUNLIN_MODEL_LIMIT,"model continuation allocation failed");
    }
    json_object_object_add(meta,"output",json_object_get(native));
    if(ahp_field(meta,"output")!=native) {
        json_object_put(native);json_object_put(meta);
        return ahp_fail(c,DUNLIN_MODEL_LIMIT,"model continuation allocation failed");
    }
    json_object_object_add(msg,"_dunlin_provider",meta);
    if(ahp_field(msg,"_dunlin_provider")!=meta) {
        json_object_put(meta);
        return ahp_fail(c,DUNLIN_MODEL_LIMIT,"model continuation allocation failed");
    }
    return 1;
}
int ahp_metric(ahp_context *c,ahp_json *usage,const char *name,unsigned flag,uint64_t *value) {
    ahp_json *v=ahp_field(usage,name);
    if(!v)return 1;
    if(!json_object_is_type(v,json_type_int) || json_object_get_int64(v)<0)
        return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid provider token statistic");
    if(value)*value=(uint64_t)json_object_get_int64(v);
    if(c->options->metrics)c->options->metrics->present|=flag;
    return 1;
}
void ahp_reasoning(ahp_context *c,const char *event,const char *text) {
    if(text && *text && c->options->show_reasoning && c->options->reasoning)
        c->options->reasoning(c->options->user,event,text,strlen(text));
}
int ahp_text(ahp_context *c,ahp_buffer *buffer,const char *text) {
    if(!text)return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid model text fragment");
    size_t n=strlen(text);
    if(!ahp_append(c,buffer,text,n))return 0;
    if(n && c->events && c->events->delta)c->events->delta(c->events->user,text,n);
    return 1;
}
int ahp_call_id(ahp_context *c,char output[49]) {
    unsigned char bytes[16];
    if(!ah_os_random(bytes,sizeof bytes))
        return ahp_fail(c,DUNLIN_MODEL_INVALID,"cannot create local tool call identity");
    memcpy(output,"dunlin_",5);
    const char hex[]="0123456789abcdef";
    for(size_t i=0;i<sizeof bytes;i++){output[5+i*2]=hex[bytes[i]>>4];output[6+i*2]=hex[bytes[i]&15];}
    output[37]=0;return 1;
}
const char *ahp_effort(dunlin_reasoning_level e) {
    static const char *const names[]={"default","none","low","medium","high","xhigh"};
    return e>=DUNLIN_REASONING_DEFAULT && e<=DUNLIN_REASONING_XHIGH?names[e]:NULL;
}
typedef struct {
    ahp_context *c;ahp_framing framing;ahp_consume consume;void *state;
    ahp_buffer body,line,data,event;size_t received;int cr;
} exchange;
static int cancel(void *ptr) {
    exchange *x=ptr;return x->c->events && x->c->events->cancelled && x->c->events->cancelled(x->c->events->user);
}
static int dispatch(exchange *x,ahp_buffer *b,const char *event) {
    ahp_json *o=dunlin_json_parse(b->s,b->n);
    if(!json_object_is_type(o,json_type_object)) {
        if(o)json_object_put(o);
        return ahp_fail(x->c,DUNLIN_MODEL_PROTOCOL,"malformed JSON in model stream");
    }
    int ok=x->consume && x->consume(x->state,o,event);
    json_object_put(o);return ok;
}
static int end_line(exchange *x) {
    int ok=1;
    if(x->framing==AHP_NDJSON) {
        if(x->line.n)ok=dispatch(x,&x->line,NULL);
    } else if(!x->line.n) {
        if(x->data.n) {
            x->data.s[--x->data.n]=0;
            ok=dispatch(x,&x->data,x->event.s);
            x->data.n=0;
        }
        x->event.n=0;if(x->event.s)x->event.s[0]=0;
    } else {
        size_t start=0;ahp_buffer *target=NULL;
        if(x->line.n>=4 && !memcmp(x->line.s,"data",4) && (x->line.n==4 || x->line.s[4]==':')) {
            start=x->line.n==4?4:5;target=&x->data;
        } else if(x->line.n>=5 && !memcmp(x->line.s,"event",5) && (x->line.n==5 || x->line.s[5]==':')) {
            start=x->line.n==5?5:6;target=&x->event;target->n=0;
        }
        if(target) {
            if(start<x->line.n && x->line.s[start]==' ')start++;
            ok=ahp_append(x->c,target,x->line.s+start,x->line.n-start);
            if(ok && target==&x->data)ok=ahp_append(x->c,target,"\n",1);
        }
    }
    x->line.n=0;if(x->line.s)x->line.s[0]=0;return ok;
}
static int receive(void *ptr,const char *s,size_t n) {
    exchange *x=ptr;
    if(cancel(x))return ahp_fail(x->c,DUNLIN_MODEL_CANCELLED,"model request cancelled");
    if(n>SIZE_MAX-x->received)return ahp_fail(x->c,DUNLIN_MODEL_LIMIT,"model response exceeds byte limit");
    x->received+=n;if(x->c->options->metrics)x->c->options->metrics->response_bytes=x->received;
    if(x->received>x->c->cfg->max_response_bytes)return ahp_fail(x->c,DUNLIN_MODEL_LIMIT,"model response exceeds byte limit");
    if(!x->c->cfg->stream)return ahp_append(x->c,&x->body,s,n);
    while(n) {
        char ch=*s;
        if(ch=='\r' || ch=='\n') {
            if(!(x->cr && ch=='\n') && !end_line(x))return 0;
            x->cr=ch=='\r';s++;n--;
        } else {
            x->cr=0;size_t i=0;while(i<n && s[i]!='\r' && s[i]!='\n')i++;
            if(!ahp_append(x->c,&x->line,s,i))return 0;
            s+=i;n-=i;
        }
    }
    return 1;
}
int ahp_exchange(ahp_context *c,const char *suffix,ahp_json *request,ahp_framing framing,
    const dunlin_http_header *headers,size_t header_count,ahp_consume consume,void *state,ahp_json **plain) {
    exchange x={.c=c,.framing=framing,.consume=consume,.state=state};
    if(plain)*plain=NULL;
    if(cancel(&x))return ahp_fail(c,DUNLIN_MODEL_CANCELLED,"model request cancelled");
    if(c->cfg->http && c->cfg->http!=&dunlin_curl_http && !c->options->http)
        return ahp_fail(c,DUNLIN_MODEL_INVALID,"native protocol requires a v2 custom transport");
    const char *key=NULL;
    if(c->cfg->key_env && *c->cfg->key_env) {
        key=getenv(c->cfg->key_env);
        if(!key || !*key || strlen(key)>16384 || strpbrk(key,"\r\n"))
            return ahp_fail(c,DUNLIN_MODEL_INVALID,"model credential unavailable or invalid");
    }
    char url[2080];size_t n=strlen(c->cfg->base_url);
    while(n && c->cfg->base_url[n-1]=='/')n--;
    if(snprintf(url,sizeof url,"%.*s%s",(int)n,c->cfg->base_url,suffix)>=(int)sizeof url)
        return ahp_fail(c,DUNLIN_MODEL_INVALID,"model endpoint exceeds limit");
    const char *wire=json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN);
    if(c->options->metrics)c->options->metrics->request_bytes=strlen(wire);
    int key_is_header=0;
    for(size_t i=0;i<header_count;i++)if(headers[i].name && !strcasecmp(headers[i].name,"x-goog-api-key"))key_is_header=1;
    dunlin_http_request_v2 req={
        .request={url,wire,key_is_header?NULL:key,c->cfg->stream,c->cfg->timeout_ms,&x,receive,cancel},
        .accept=c->cfg->stream?(framing==AHP_NDJSON?"application/x-ndjson":"text/event-stream"):"application/json",
        .headers=headers,.header_count=header_count};
    const dunlin_http_v2 *transport=c->options->http?c->options->http:&dunlin_curl_http_v2;
    if(!transport->post)return ahp_fail(c,DUNLIN_MODEL_INVALID,"missing v2 HTTP transport");
    long code=0;dunlin_http_result result=transport->post(transport->user,&req,&code);
    if(c->options->metrics)c->options->metrics->response_bytes_known=code>=200 && code<300;
    if(code==429)ahp_fail(c,DUNLIN_MODEL_RATE_LIMIT,"model HTTP 429 (rate limited)");
    else if(code && (code<200 || code>=300)) {
        char reason[128];snprintf(reason,sizeof reason,"model HTTP %ld (check protocol, credentials and model capabilities)",code);
        ahp_fail(c,DUNLIN_MODEL_HTTP,reason);
    } else if(c->status==DUNLIN_MODEL_OK) {
        if(result==DUNLIN_HTTP_ABORTED || cancel(&x))ahp_fail(c,DUNLIN_MODEL_CANCELLED,"model request cancelled");
        else if(result==DUNLIN_HTTP_EXPIRED)ahp_fail(c,DUNLIN_MODEL_TIMEOUT,"model request timed out");
        else if(result!=DUNLIN_HTTP_OK || !code)ahp_fail(c,DUNLIN_MODEL_TRANSPORT,"model connection or TLS transfer failed");
        else if(!c->cfg->stream) {
            ahp_json *o=dunlin_json_parse(x.body.s,x.body.n);
            if(!json_object_is_type(o,json_type_object)) {if(o)json_object_put(o);ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"malformed model response JSON");}
            else if(plain)*plain=o;else json_object_put(o);
        } else if(framing==AHP_NDJSON) {if(x.line.n)end_line(&x);}
        else if(x.line.n || x.data.n)ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"incomplete SSE event");
    }
    ahp_buffer_free(&x.body);ahp_buffer_free(&x.line);ahp_buffer_free(&x.data);ahp_buffer_free(&x.event);
    return c->status==DUNLIN_MODEL_OK;
}
int ahp_valid_assistant(ahp_context *c,ahp_json *msg,int expect_tools) {
    if(!ahp_is(ahp_str(msg,"role"),"assistant"))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid assistant role");
    ahp_json *calls=ahp_field(msg,"tool_calls");
    if(calls && !json_object_is_type(calls,json_type_array))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid model tool calls");
    size_t count=calls?(size_t)json_object_array_length(calls):0;
    if(count>64)return ahp_fail(c,DUNLIN_MODEL_LIMIT,"too many model tool calls");
    if((count>0)!=!!expect_tools || (!count && !ahp_str(msg,"content")) ||
       (ahp_field(msg,"content") && !ahp_str(msg,"content")))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"model terminal state disagrees with content");
    for(size_t i=0;i<count;i++) {
        ahp_json *call=json_object_array_get_idx(calls,i),*fn=ahp_field(call,"function");
        const char *id=ahp_str(call,"id"),*name=ahp_str(fn,"name");
        if(!ahp_is(ahp_str(call,"type"),"function") || !id || !*id || !name || !*name || !ahp_str(fn,"arguments"))
            return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"malformed model function call");
        for(size_t j=0;j<i;j++)if(ahp_is(ahp_str(json_object_array_get_idx(calls,j),"id"),id))
            return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"duplicate model tool call id");
    }
    return 1;
}
dunlin_model_status ahp_result(ahp_context *c,char *error,size_t size) {
    if(error && size)snprintf(error,size,"%s",c->reason);
    return c->status;
}
dunlin_model_status dunlin_protocol_proposal(const dunlin_model_config *cfg,const dunlin_protocol_options *options,
    ahp_json *messages,ahp_json *tools,const dunlin_model_events *events,ahp_json **assistant,char *error,size_t size) {
    dunlin_protocol_options defaults={.protocol=DUNLIN_PROTOCOL_CHAT,.show_reasoning=1};
    if(!options)options=&defaults;
    if(options->metrics)memset(options->metrics,0,sizeof *options->metrics);
    ahp_context c={cfg,options,events,DUNLIN_MODEL_OK,{0}};
    if(assistant)*assistant=NULL;
    if(!assistant || !cfg || !dunlin_model_url_valid(cfg->base_url) || !cfg->model || !*cfg->model ||
       cfg->timeout_ms<1 || !cfg->max_response_bytes || cfg->max_response_bytes>=INT_MAX ||
       !json_object_is_type(messages,json_type_array) || (tools && !json_object_is_type(tools,json_type_array)) ||
       options->protocol<DUNLIN_PROTOCOL_CHAT || options->protocol>DUNLIN_PROTOCOL_GEMINI ||
       !ahp_effort(options->reasoning_effort) || (options->reasoning_supported&~7u) ||
       (options->chat_metadata_complete!=0 && options->chat_metadata_complete!=1) ||
       (options->has_temperature && (!isfinite(options->temperature) || options->temperature<0 || options->temperature>2))) {
        ahp_fail(&c,DUNLIN_MODEL_INVALID,"invalid model protocol configuration");return ahp_result(&c,error,size);
    }
    if((options->reasoning_effort==DUNLIN_REASONING_NONE && !(options->reasoning_supported&DUNLIN_REASONING_CAN_DISABLE)) ||
       (options->reasoning_effort>=DUNLIN_REASONING_LOW && options->reasoning_effort<=DUNLIN_REASONING_HIGH && !(options->reasoning_supported&DUNLIN_REASONING_CAN_EFFORT)) ||
       (options->reasoning_effort==DUNLIN_REASONING_XHIGH &&
        (!(options->reasoning_supported&DUNLIN_REASONING_CAN_XHIGH) ||
         (options->protocol!=DUNLIN_PROTOCOL_RESPONSES && options->protocol!=DUNLIN_PROTOCOL_CHAT)))) {
        ahp_fail(&c,DUNLIN_MODEL_INVALID,"reasoning effort unsupported by configured model capabilities");return ahp_result(&c,error,size);
    }
    dunlin_model_events hidden=events?*events:(dunlin_model_events){0};
    if(options->required_tool && *options->required_tool)hidden.delta=NULL;
    ahp_json *selected=NULL;
    if(options->required_tool && *options->required_tool) {
        selected=json_object_new_array();
        if(!selected){ahp_fail(&c,DUNLIN_MODEL_LIMIT,"model request allocation failed");return ahp_result(&c,error,size);}
        for(size_t i=0;tools && i<(size_t)json_object_array_length(tools);i++) {
            ahp_json *tool=json_object_array_get_idx(tools,i);
            if(ahp_is(ahp_str(ahp_field(tool,"function"),"name"),options->required_tool))
                json_object_array_add(selected,json_object_get(tool));
        }
        if(json_object_array_length(selected)!=1) {
            json_object_put(selected);ahp_fail(&c,DUNLIN_MODEL_INVALID,"required tool is not a unique registered function");
            return ahp_result(&c,error,size);
        }
        tools=selected;
    }
    dunlin_model_status status;
    switch(options->protocol) {
    case DUNLIN_PROTOCOL_CHAT: status=ahp_chat(cfg,options,messages,tools,&hidden,assistant,error,size);break;
    case DUNLIN_PROTOCOL_RESPONSES: status=ahp_responses(cfg,options,messages,tools,&hidden,assistant,error,size);break;
    case DUNLIN_PROTOCOL_ANTHROPIC: status=ahp_anthropic(cfg,options,messages,tools,&hidden,assistant,error,size);break;
    case DUNLIN_PROTOCOL_OLLAMA: status=ahp_ollama(cfg,options,messages,tools,&hidden,assistant,error,size);break;
    case DUNLIN_PROTOCOL_GEMINI: status=ahp_gemini(cfg,options,messages,tools,&hidden,assistant,error,size);break;
    default: ahp_fail(&c,DUNLIN_MODEL_INVALID,"unsupported model protocol");status=ahp_result(&c,error,size);break;
    }
    if(selected)json_object_put(selected);
    if(status==DUNLIN_MODEL_OK && options->required_tool && *options->required_tool) {
        ahp_json *calls=ahp_field(*assistant,"tool_calls");
        int matched=json_object_is_type(calls,json_type_array) && json_object_array_length(calls)>0;
        for(size_t i=0;matched && i<(size_t)json_object_array_length(calls);i++)
            matched=ahp_is(ahp_str(ahp_field(json_object_array_get_idx(calls,i),"function"),"name"),options->required_tool);
        if(!matched) {
            json_object_put(*assistant);*assistant=NULL;
            if(error && size)snprintf(error,size,"%s",options->required_tool);
            return DUNLIN_MODEL_REQUIRED_TOOL_MISSING;
        }
    }
    return status;
}
