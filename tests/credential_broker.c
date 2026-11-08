#include "dunlin/compact/credential.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {int authorize,execute,revoke;dunlin_credential_status authorization;} fixture;
static dunlin_credential_status authorize(void *opaque,const dunlin_credential_request *request,const dunlin_credential_policy *policy) {
    fixture *f=opaque;f->authorize++;
    assert(!strcmp(request->purpose,"install-package")&&!strcmp(request->executable,"/usr/bin/id"));
    assert(policy->mode==DUNLIN_CREDENTIAL_PROMPT_EACH_TIME||policy->mode==DUNLIN_CREDENTIAL_CACHE);
    return f->authorization;
}
static ah_status execute(void *opaque,const dunlin_credential_request *request,const ah_hooks *hooks,
        size_t cap,unsigned timeout,char **result) {
    (void)request;(void)hooks;(void)cap;(void)timeout;fixture *f=opaque;f->execute++;
    *result=malloc(3);assert(*result);memcpy(*result,"ok",3);return AH_DONE;
}
static void revoke(void *opaque,const char *purpose){fixture *f=opaque;f->revoke++;assert(!strcmp(purpose,"install-package"));}
int main(void) {
    const char *argv[]={"-u"};
    dunlin_credential_request request={"install-package","/usr/bin/id",argv,1};
    fixture f={.authorization=DUNLIN_CREDENTIAL_OK};
    dunlin_credential_broker_v1 broker={&f,authorize,execute,revoke};
    dunlin_credential_policy policy={DUNLIN_CREDENTIAL_PROMPT_EACH_TIME,0};
    ah_status execution;char *result=NULL;
    assert(dunlin_credential_run(&broker,&policy,&request,NULL,1024,1000,&execution,&result)==DUNLIN_CREDENTIAL_OK);
    assert(execution==AH_DONE&&result&&!strcmp(result,"ok")&&f.authorize==1&&f.execute==1&&f.revoke==1);free(result);
    policy=(dunlin_credential_policy){DUNLIN_CREDENTIAL_CACHE,300};
    assert(dunlin_credential_run(&broker,&policy,&request,NULL,1024,1000,&execution,&result)==DUNLIN_CREDENTIAL_OK);
    assert(f.authorize==2&&f.execute==2&&f.revoke==1);free(result);
    policy=(dunlin_credential_policy){DUNLIN_CREDENTIAL_MANUAL,0};
    assert(dunlin_credential_run(&broker,&policy,&request,NULL,1024,1000,&execution,&result)==DUNLIN_CREDENTIAL_MANUAL_REQUIRED);
    assert(!result&&f.authorize==2&&f.execute==2);
    assert(dunlin_credential_run(NULL,&policy,&request,NULL,1024,1000,&execution,&result)==DUNLIN_CREDENTIAL_MANUAL_REQUIRED);
    policy=(dunlin_credential_policy){DUNLIN_CREDENTIAL_CACHE,0};
    assert(dunlin_credential_run(&broker,&policy,&request,NULL,1024,1000,&execution,&result)==DUNLIN_CREDENTIAL_INVALID);
    policy=(dunlin_credential_policy){DUNLIN_CREDENTIAL_PROMPT_EACH_TIME,0};f.authorization=DUNLIN_CREDENTIAL_DENIED;
    assert(dunlin_credential_run(&broker,&policy,&request,NULL,1024,1000,&execution,&result)==DUNLIN_CREDENTIAL_DENIED);
    assert(f.execute==2&&f.revoke==1);
    puts("PASS secretless credential broker modes, binding, denial and revocation");return 0;
}
