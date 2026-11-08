#include "dunlin/compact/credential.h"
#include <string.h>

const char *dunlin_credential_mode_name(dunlin_credential_mode mode) {
    switch(mode) {
    case DUNLIN_CREDENTIAL_PROMPT_EACH_TIME:return "each-time";
    case DUNLIN_CREDENTIAL_CACHE:return "cache";
    case DUNLIN_CREDENTIAL_MANUAL:return "manual";
    default:return NULL;
    }
}
int dunlin_credential_mode_parse(const char *name,dunlin_credential_mode *mode) {
    if(!name||!mode)return 0;
    for(int i=DUNLIN_CREDENTIAL_PROMPT_EACH_TIME;i<=DUNLIN_CREDENTIAL_MANUAL;i++)
        if(!strcmp(name,dunlin_credential_mode_name((dunlin_credential_mode)i))){*mode=(dunlin_credential_mode)i;return 1;}
    return 0;
}
static int valid_text(const char *text,size_t cap) {
    return text&&*text&&strlen(text)<=cap&&ah_utf8_text(text,strlen(text));
}
static int valid(const dunlin_credential_broker_v1 *broker,const dunlin_credential_policy *policy,
        const dunlin_credential_request *request,size_t max_output,unsigned timeout_ms) {
    if(!policy||!request||!valid_text(request->purpose,64)||
       !valid_text(request->executable,4096)||request->argc>64||!max_output||!timeout_ms||
       !dunlin_credential_mode_name(policy->mode))return 0;
    if((policy->mode==DUNLIN_CREDENTIAL_CACHE&&(policy->cache_seconds<1||policy->cache_seconds>86400))||
       (policy->mode!=DUNLIN_CREDENTIAL_CACHE&&policy->cache_seconds))return 0;
    for(size_t i=0;i<request->argc;i++)if(!valid_text(request->argv[i],4096))return 0;
    if(policy->mode==DUNLIN_CREDENTIAL_MANUAL)return 1;
    return broker&&broker->authorize&&broker->execute;
}
dunlin_credential_status dunlin_credential_run(const dunlin_credential_broker_v1 *broker,
        const dunlin_credential_policy *policy,const dunlin_credential_request *request,
        const ah_hooks *hooks,size_t max_output,unsigned timeout_ms,
        ah_status *execution_status,char **result) {
    if(result)*result=NULL;if(execution_status)*execution_status=AH_FAILED;
    if(!result||!execution_status||!valid(broker,policy,request,max_output,timeout_ms))return DUNLIN_CREDENTIAL_INVALID;
    if(policy->mode==DUNLIN_CREDENTIAL_MANUAL)return DUNLIN_CREDENTIAL_MANUAL_REQUIRED;
    dunlin_credential_status authorized=broker->authorize(broker->user,request,policy);
    if(authorized!=DUNLIN_CREDENTIAL_OK)return authorized;
    *execution_status=broker->execute(broker->user,request,hooks,max_output,timeout_ms,result);
    if(policy->mode==DUNLIN_CREDENTIAL_PROMPT_EACH_TIME&&broker->revoke)
        broker->revoke(broker->user,request->purpose);
    return DUNLIN_CREDENTIAL_OK;
}
