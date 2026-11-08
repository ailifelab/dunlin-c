#include "protocol_internal.h"
#include "dunlin/compact/json.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GEMINI_MAX_CALLS = 64 };
typedef struct {
    ahp_context *c; ahp_buffer text; ahp_json *parts; ahp_json *native;
    size_t calls; int done, saw_candidate;
} gemini_response;

static int put(ahp_context *c,ahp_json *o,const char *key,ahp_json *v) {
    if(!v)return ahp_fail(c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");
    json_object_object_add(o,key,v);return 1;
}
static int text(ahp_context *c,ahp_json *o,const char *key,const char *s) {
    return put(c,o,key,json_object_new_string(s));
}
static int push(ahp_context *c,ahp_json *a,ahp_json *v) {
    if(v && json_object_array_add(a,v)==0)return 1;
    if(v)json_object_put(v);return ahp_fail(c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");
}
static int present(ahp_json *o,const char *key) {ahp_json *v=NULL;return json_object_object_get_ex(o,key,&v);}
static int input_error(ahp_context *c) {return ahp_fail(c,DUNLIN_MODEL_INVALID,"invalid Gemini message or tool result pairing");}

static ahp_json *part_text(ahp_context *c,const char *s) {
    ahp_json *p=json_object_new_object();if(!p)return NULL;
    if(!text(c,p,"text",s)){json_object_put(p);return NULL;}return p;
}
static ahp_json *content(ahp_context *c,const char *role,ahp_json *parts) {
    ahp_json *o=json_object_new_object();
    if(!o || !text(c,o,"role",role)){if(o)json_object_put(o);json_object_put(parts);return NULL;}
    json_object_object_add(o,"parts",parts);return o;
}
static int call_matches(ahp_json *part,ahp_json *call) {
    ahp_json *fc=ahp_field(part,"functionCall"),*fn=ahp_field(call,"function"),*args=NULL;
    const char *encoded=ahp_str(fn,"arguments"),*name=ahp_str(fn,"name");
    if(!json_object_is_type(fc,json_type_object)||!encoded||!name||!ahp_is(name,ahp_str(fc,"name")))return 0;
    args=dunlin_json_parse(encoded,strlen(encoded));
    int ok=json_object_is_type(args,json_type_object) &&
        !strcmp(json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN),
                json_object_to_json_string_ext(ahp_field(fc,"args"),JSON_C_TO_STRING_PLAIN));
    if(args)json_object_put(args);return ok;
}
static ahp_json *history(ahp_context *c,ahp_json *messages,ahp_json **system) {
    ahp_json *out=json_object_new_array(),*pending=NULL;size_t paired=0;
    if(!out){ahp_fail(c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");return NULL;}
    for(size_t i=0;i<(size_t)json_object_array_length(messages);i++) {
        ahp_json *m=json_object_array_get_idx(messages,i),*parts=NULL,*row=NULL;
        const char *role=ahp_str(m,"role"),*body=ahp_str(m,"content");
        if(pending && paired<(size_t)json_object_array_length(pending) && !ahp_is(role,"tool"))goto invalid;
        ahp_json *native=ahp_native(c,m,"gemini");if(c->status!=DUNLIN_MODEL_OK)goto failed;
        if(ahp_is(role,"system")) {
            if(!body || native)goto invalid;
            if(*system)parts=ahp_field(*system,"parts");
            else {
                parts=json_object_new_array();if(!parts)goto failed;
                *system=content(c,"user",parts);if(!*system)goto failed;
            }
            if(!push(c,parts,part_text(c,body)))goto failed;continue;
        }
        if(ahp_is(role,"tool")) {
            if(!pending||paired>=(size_t)json_object_array_length(pending)||!body)goto invalid;
            ahp_json *call=json_object_array_get_idx(pending,paired++),*fn=ahp_field(call,"function");
            if(!ahp_is(ahp_str(m,"tool_call_id"),ahp_str(call,"id")))goto invalid;
            ahp_json *response=json_object_new_object(),*fr=json_object_new_object(),*part=json_object_new_object();
            parts=json_object_new_array();
            if(!response||!fr||!part||!parts){if(response)json_object_put(response);if(fr)json_object_put(fr);if(part)json_object_put(part);if(parts)json_object_put(parts);goto allocation;}
            if(!text(c,response,"output",body)||!text(c,fr,"name",ahp_str(fn,"name"))){json_object_put(response);json_object_put(fr);json_object_put(part);json_object_put(parts);goto failed;}
            json_object_object_add(fr,"response",response);json_object_object_add(part,"functionResponse",fr);
            if(!push(c,parts,part)){json_object_put(parts);goto failed;}row=content(c,"user",parts);
        } else if(ahp_is(role,"assistant")) {
            ahp_json *calls=ahp_field(m,"tool_calls");size_t n=calls?(size_t)json_object_array_length(calls):0;
            if((calls&&!json_object_is_type(calls,json_type_array))||n>GEMINI_MAX_CALLS||(!body&&!n))goto invalid;
            if(native) {
                row=ahp_clone(native);if(!row)goto allocation;
                ahp_json *np=ahp_field(row,"parts");if(!json_object_is_type(np,json_type_array))goto invalid_row;
                size_t found=0;
                for(size_t j=0;j<(size_t)json_object_array_length(np);j++)if(ahp_field(json_object_array_get_idx(np,j),"functionCall")) {
                    if(found>=n||!call_matches(json_object_array_get_idx(np,j),json_object_array_get_idx(calls,found++)))goto invalid_row;
                }
                if(found!=n)goto invalid_row;
            } else {
                parts=json_object_new_array();if(!parts)goto allocation;
                if(body&&*body&&!push(c,parts,part_text(c,body)))goto failed;
                for(size_t j=0;j<n;j++) {
                    ahp_json *call=json_object_array_get_idx(calls,j),*fn=ahp_field(call,"function"),*fc=json_object_new_object(),*part=json_object_new_object();
                    const char *encoded=ahp_str(fn,"arguments"),*name=ahp_str(fn,"name");ahp_json *args=NULL;
                    if(!encoded||!name||!fc||!part||(args=dunlin_json_parse(encoded,strlen(encoded)))==NULL||!json_object_is_type(args,json_type_object)) {if(args)json_object_put(args);if(fc)json_object_put(fc);if(part)json_object_put(part);goto invalid;}
                    if(!text(c,fc,"name",name)){json_object_put(args);json_object_put(fc);json_object_put(part);goto failed;}
                    json_object_object_add(fc,"args",args);json_object_object_add(part,"functionCall",fc);if(!push(c,parts,part))goto failed;
                }
                row=content(c,"model",parts);
            }
            pending=calls;paired=0;
        } else if(ahp_is(role,"user")) {
            if(!body||native||ahp_field(m,"tool_calls"))goto invalid;
            parts=json_object_new_array();if(!parts||!push(c,parts,part_text(c,body)))goto failed;row=content(c,"user",parts);pending=NULL;paired=0;
        } else goto invalid;
        if(!row||!push(c,out,row))goto failed;continue;
invalid_row: json_object_put(row);goto invalid;
    }
    if(pending&&paired<(size_t)json_object_array_length(pending))goto invalid;
    return out;
allocation: ahp_fail(c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");goto failed;
invalid: input_error(c);
failed: json_object_put(out);return NULL;
}

static ahp_json *tool_config(ahp_context *c,ahp_json *tools) {
    if(!tools||!json_object_array_length(tools))return NULL;
    ahp_json *decl=json_object_new_array(),*wrapper=json_object_new_object(),*all=json_object_new_array();
    if(!decl||!wrapper||!all)goto failed;
    for(size_t i=0;i<(size_t)json_object_array_length(tools);i++) {
        ahp_json *fn=ahp_field(json_object_array_get_idx(tools,i),"function"),*copy=ahp_clone(fn);
        if(!json_object_is_type(copy,json_type_object)||!ahp_str(copy,"name")||!push(c,decl,copy))goto failed;
    }
    json_object_object_add(wrapper,"functionDeclarations",decl);decl=NULL;
    if(!push(c,all,wrapper))goto failed;return all;
failed: if(decl)json_object_put(decl);if(wrapper)json_object_put(wrapper);if(all)json_object_put(all);ahp_fail(c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");return NULL;
}

static int usage(ahp_context *c,ahp_json *o) {
    ahp_json *u=ahp_field(o,"usageMetadata");if(!u)return 1;
    if(!json_object_is_type(u,json_type_object))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid Gemini usage metadata");
    dunlin_model_metrics scratch={0},*m=c->options->metrics?c->options->metrics:&scratch;
    return ahp_metric(c,u,"promptTokenCount",DUNLIN_USAGE_INPUT,&m->input_tokens)&&
        ahp_metric(c,u,"candidatesTokenCount",DUNLIN_USAGE_OUTPUT,&m->output_tokens)&&
        ahp_metric(c,u,"totalTokenCount",DUNLIN_USAGE_TOTAL,&m->total_tokens)&&
        ahp_metric(c,u,"cachedContentTokenCount",DUNLIN_USAGE_CACHED_INPUT,&m->cached_input_tokens)&&
        ahp_metric(c,u,"thoughtsTokenCount",DUNLIN_USAGE_REASONING,&m->reasoning_tokens);
}
static int consume(void *user,ahp_json *o,const char *event) {
    (void)event;gemini_response *r=user;ahp_context *c=r->c;
    if(r->done)return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"Gemini data after completed response");
    if(!json_object_is_type(o,json_type_object))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid Gemini response object");
    if(ahp_field(o,"promptFeedback")&&!ahp_field(o,"candidates"))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"Gemini prompt blocked before generation");
    ahp_json *candidates=ahp_field(o,"candidates");
    if(!json_object_is_type(candidates,json_type_array)||json_object_array_length(candidates)!=1)return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"Gemini requires exactly one candidate");
    ahp_json *candidate=json_object_array_get_idx(candidates,0),*body=ahp_field(candidate,"content"),*parts=ahp_field(body,"parts");
    if(!json_object_is_type(body,json_type_object)||!ahp_is(ahp_str(body,"role"),"model")||!json_object_is_type(parts,json_type_array))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid Gemini candidate content");
    r->saw_candidate=1;
    for(size_t i=0;i<(size_t)json_object_array_length(parts);i++) {
        ahp_json *part=json_object_array_get_idx(parts,i);const char *s=ahp_str(part,"text");ahp_json *fc=ahp_field(part,"functionCall");
        ahp_json *thought=ahp_field(part,"thought");
        if(thought&&!json_object_is_type(thought,json_type_boolean))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid Gemini thought marker");
        if(s&&thought&&json_object_get_boolean(thought)) {ahp_reasoning(c,"reasoning.delta",s);}
        else if(s) {if(!ahp_text(c,&r->text,s))return 0;}
        else if(fc) {
            if(r->calls>=GEMINI_MAX_CALLS||!json_object_is_type(fc,json_type_object)||!ahp_str(fc,"name")||!json_object_is_type(ahp_field(fc,"args"),json_type_object))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"invalid Gemini function call");
            r->calls++;
        } else if(!present(part,"thoughtSignature"))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"unsupported Gemini content part");
        if(!push(c,r->parts,ahp_clone(part)))return 0;
    }
    if(!usage(c,o))return 0;
    const char *finish=ahp_str(candidate,"finishReason");
    if(finish) {
        if(ahp_is(finish,"MAX_TOKENS"))return ahp_fail(c,DUNLIN_MODEL_LIMIT,"Gemini stopped at its token limit");
        if(ahp_is(finish,"SAFETY")||ahp_is(finish,"RECITATION")||ahp_is(finish,"BLOCKLIST")||ahp_is(finish,"PROHIBITED_CONTENT"))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"Gemini stopped for safety policy");
        if(ahp_is(finish,"MALFORMED_FUNCTION_CALL"))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"Gemini returned a malformed function call");
        if(!ahp_is(finish,"STOP"))return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"unsupported Gemini finish reason");
        r->done=1;
    }
    return 1;
}
static ahp_json *finish(gemini_response *r) {
    ahp_context *c=r->c;if(!r->done||!r->saw_candidate)return ahp_fail(c,DUNLIN_MODEL_PROTOCOL,"incomplete Gemini response"),NULL;
    ahp_json *msg=json_object_new_object(),*calls=r->calls?json_object_new_array():NULL;
    if(!msg||(r->calls&&!calls))goto allocation;
    if(!text(c,msg,"role","assistant")||!text(c,msg,"content",r->text.s?r->text.s:""))goto failed;
    if(calls)json_object_object_add(msg,"tool_calls",calls);
    size_t call_index=0;
    for(size_t i=0;i<(size_t)json_object_array_length(r->parts);i++) {
        ahp_json *fc=ahp_field(json_object_array_get_idx(r->parts,i),"functionCall");if(!fc)continue;
        char id[49];if(!ahp_call_id(c,id))goto failed;
        ahp_json *call=json_object_new_object(),*fn=json_object_new_object();if(!call||!fn){if(call)json_object_put(call);if(fn)json_object_put(fn);goto allocation;}
        json_object_object_add(call,"function",fn);
        if(!text(c,call,"type","function")||!text(c,call,"id",id)||!text(c,fn,"name",ahp_str(fc,"name"))||!text(c,fn,"arguments",json_object_to_json_string_ext(ahp_field(fc,"args"),JSON_C_TO_STRING_PLAIN))||!push(c,calls,call))goto failed;
        call_index++;
    }
    (void)call_index;
    if(!ahp_valid_assistant(c,msg,r->calls!=0))goto failed;
    if(!ahp_metadata(c,msg,"gemini",r->native))goto failed;return msg;
allocation: ahp_fail(c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");
failed: if(msg)json_object_put(msg);return NULL;
}

dunlin_model_status ahp_gemini(const dunlin_model_config *cfg,const dunlin_protocol_options *options,
    ahp_json *messages,ahp_json *tools,const dunlin_model_events *events,
    ahp_json **assistant,char *error,size_t error_size) {
    ahp_context c={.cfg=cfg,.options=options,.events=events,.status=DUNLIN_MODEL_OK};
    gemini_response r={.c=&c};ahp_json *request=NULL,*contents=NULL,*system=NULL,*defs=NULL,*plain=NULL,*generation=NULL;
    *assistant=NULL;r.parts=json_object_new_array();r.native=json_object_new_object();
    if(!r.parts||!r.native){ahp_fail(&c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");goto done;}
    contents=history(&c,messages,&system);if(!contents)goto done;
    request=json_object_new_object();if(!request){ahp_fail(&c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");goto done;}
    json_object_object_add(request,"contents",contents);contents=NULL;
    if(system)json_object_object_add(request,"systemInstruction",system),system=NULL;
    defs=tool_config(&c,tools);if(c.status!=DUNLIN_MODEL_OK)goto done;if(defs)json_object_object_add(request,"tools",defs),defs=NULL;
    if(options->max_tokens||options->has_temperature) {
        generation=json_object_new_object();if(!generation){ahp_fail(&c,DUNLIN_MODEL_LIMIT,"model JSON allocation failed");goto done;}
        json_object_object_add(request,"generationConfig",generation);
        if(options->max_tokens&&!put(&c,generation,"maxOutputTokens",json_object_new_int64(options->max_tokens)))goto done;
        if(options->has_temperature&&!put(&c,generation,"temperature",json_object_new_double(options->temperature)))goto done;
    }
    if(strspn(cfg->model,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")!=strlen(cfg->model)) {ahp_fail(&c,DUNLIN_MODEL_INVALID,"Gemini model name contains unsupported URL characters");goto done;}
    char suffix[512];if(snprintf(suffix,sizeof suffix,"/models/%s:%s",cfg->model,cfg->stream?"streamGenerateContent?alt=sse":"generateContent")>=(int)sizeof suffix){ahp_fail(&c,DUNLIN_MODEL_INVALID,"Gemini model endpoint exceeds limit");goto done;}
    const char *key=cfg->key_env?getenv(cfg->key_env):NULL;dunlin_http_header header={"x-goog-api-key",key};
    if(!ahp_exchange(&c,suffix,request,AHP_SSE,key?&header:NULL,key?1:0,consume,&r,&plain))goto done;
    if(!cfg->stream&&!consume(&r,plain,NULL))goto done;
    if(!text(&c,r.native,"role","model")||!put(&c,r.native,"parts",json_object_get(r.parts)))goto done;
    *assistant=finish(&r);
done:
    if(request)json_object_put(request);if(contents)json_object_put(contents);if(system)json_object_put(system);if(defs)json_object_put(defs);if(plain)json_object_put(plain);
    if(r.parts)json_object_put(r.parts);if(r.native)json_object_put(r.native);ahp_buffer_free(&r.text);
    return ahp_result(&c,error,error_size);
}
