#include "dunlin/compact/runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {int requests,decisions,answer;const char *tool,*arguments;dunlin_permission_decision decision;} fixture;
static int request(void *opaque,const char *tool,const char *arguments) {
    fixture *f=opaque;f->requests++;f->tool=tool;f->arguments=arguments;return f->answer;
}
static dunlin_permission_decision decide(void *opaque,const char *tool,const char *arguments) {
    fixture *f=opaque;f->decisions++;f->tool=tool;f->arguments=arguments;return f->decision;
}
int main(void) {
    dunlin_permission_mode mode;
    assert(dunlin_permission_mode_parse("request",&mode)&&mode==DUNLIN_PERMISSION_REQUEST);
    assert(dunlin_permission_mode_parse("agent",&mode)&&mode==DUNLIN_PERMISSION_AGENT);
    assert(dunlin_permission_mode_parse("full-access",&mode)&&mode==DUNLIN_PERMISSION_FULL_ACCESS);
    assert(!dunlin_permission_mode_parse("unsafe",&mode));
    fixture f={.answer=1,.decision=DUNLIN_PERMISSION_DECISION_ASK};
    dunlin_permission_adapter_v1 adapter={&f,decide};
    dunlin_permission_v2 policy={&f,request,DUNLIN_PERMISSION_REQUEST,&adapter};
    assert(dunlin_permission_authorize(&policy,"shell","{\"command\":\"date\"}"));
    assert(f.requests==1&&f.decisions==0&&!strcmp(f.tool,"shell"));
    policy.mode=DUNLIN_PERMISSION_AGENT;f.decision=DUNLIN_PERMISSION_DECISION_ALLOW;
    assert(dunlin_permission_authorize(&policy,"write_file","{\"path\":\"a\"}"));
    assert(f.requests==1&&f.decisions==1);
    f.decision=DUNLIN_PERMISSION_DECISION_DENY;
    assert(!dunlin_permission_authorize(&policy,"exec","{}")&&f.requests==1&&f.decisions==2);
    f.decision=DUNLIN_PERMISSION_DECISION_ASK;f.answer=0;
    assert(!dunlin_permission_authorize(&policy,"shell","{}")&&f.requests==2&&f.decisions==3);
    policy.mode=DUNLIN_PERMISSION_FULL_ACCESS;f.decision=DUNLIN_PERMISSION_DECISION_DENY;
    assert(dunlin_permission_authorize(&policy,"shell","{}"));
    assert(f.requests==2&&f.decisions==3);
    policy.mode=(dunlin_permission_mode)99;
    assert(!dunlin_permission_authorize(&policy,"shell","{}"));
    puts("PASS native request, agent adapter and full-access permission modes");
    return 0;
}
