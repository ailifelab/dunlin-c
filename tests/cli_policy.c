#define _POSIX_C_SOURCE 200809L
#include "../dunlin-command/src/policy.h"
#include <dunlin/compact/session.h>
#include <assert.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void classify(const char *command,int expected) {
    struct json_object *args=json_object_new_object();
    json_object_object_add(args,"command",json_object_new_string(command));
    struct json_object *result=cli_readonly("shell",args);
    if((result!=NULL)!=expected) fprintf(stderr,"classification mismatch: %s; expected=%d\n",command,expected);
    assert((result!=NULL)==expected);
    if(result) {
        struct json_object *value=NULL;
        assert(json_object_object_get_ex(result,"command",&value));
        const char *normalized=json_object_get_string(value);
        assert(normalized[0]=='\'');
        if(strstr(command,"curl")) assert(strstr(normalized,"--disable --globoff --proto"));
    }
    json_object_put(result);json_object_put(args);
}
int main(void) {
    const char *safe[]={"date","date -u '+%Y-%m-%d'","pwd","uname -a","ls -la",
        "echo 'literal;$(not-run)'","printf '%s' 'literal value'",
        "curl -s 'https://example.com/path?a=1&b=2'","curl -X GET https://example.com/"};
    const char *unknown[]={"date; touch marker","date > marker","echo $(touch marker)",
        "echo `touch marker`","echo \"$HOME\"","echo *","echo ~","ls /etc","ls -LR",
        "curl -o marker https://example.com/","curl -d payload https://example.com/",
        "curl -K config https://example.com/","curl -L https://example.com/","curl -k https://example.com/",
        "curl file:///etc/passwd","curl https://user:pass@example.com/","curl -X POST https://example.com/",
        "curl https://example.com/ -H 'Authorization: private'","curl http://one/ http://two/",
        "printf x | sh","date\ntouch marker","python3 -c 'print(12)'","/tmp/date","PATH=/tmp date",
        "env date","sh -c date","printf 'unclosed","echo foo # ignored","echo foo &",
        "curl -s https://example.com/?a=1&b=2","ls -- -R"};
    for(size_t i=0;i<sizeof safe/sizeof *safe;i++) classify(safe[i],1);
    for(size_t i=0;i<sizeof unknown/sizeof *unknown;i++) classify(unknown[i],0);
    char temp[]="/tmp/dunlin-cli-policy-XXXXXX";
    assert(mkdtemp(temp));
    ah_tool_service base;cli_tools ui;
    assert(!ah_linux_tools(&base,temp));assert(!ah_linux_tool_enable(&base,"shell",1));
    assert(cli_tools_open(&ui,&base,temp,1));
    struct json_object *args=json_tokener_parse("{\"command\":\"pwd\"}");
    int approval=1,effect=0;char error[128];char *out=NULL;
    assert(ui.service.check(&ui,"shell",args,&approval,&effect,error,sizeof error)==AH_DONE);
    assert(!approval && effect);
    ah_hooks hooks={.now_ms=ah_linux_now_ms};
    assert(ui.service.call(&ui,"shell",args,&hooks,1024,5000,&out)==AH_DONE);
    assert(strstr(out,temp) && !strncmp(out,"exit_code=0\n",12));
    free(out);out=NULL;
    assert(ui.service.call(&ui,"shell",args,&hooks,1024,5000,&out)==AH_FAILED);
    free(out);json_object_put(args);
    cli_tools_begin_turn(&ui,"请安装nodejs");
    args=json_tokener_parse("{\"command\":\"command -v sh\"}");
    assert(ui.service.check(&ui,"shell",args,&approval,&effect,error,sizeof error)==AH_DONE);
    out=NULL;assert(ui.service.call(&ui,"shell",args,&hooks,1024,5000,&out)==AH_DONE);free(out);
    assert(ui.service.check(&ui,"shell",args,&approval,&effect,error,sizeof error)==AH_INVALID_ARGUMENTS);
    assert(strstr(error,"already succeeded"));
    cli_tools_begin_turn(&ui,"请重复查询两次");
    assert(ui.service.check(&ui,"shell",args,&approval,&effect,error,sizeof error)==AH_DONE);
    out=NULL;assert(ui.service.call(&ui,"shell",args,&hooks,1024,5000,&out)==AH_DONE);free(out);
    assert(ui.service.check(&ui,"shell",args,&approval,&effect,error,sizeof error)==AH_DONE);
    cli_tools_begin_turn(&ui,"下一轮");
    assert(ui.service.check(&ui,"shell",args,&approval,&effect,error,sizeof error)==AH_DONE);
    json_object_put(args);
    args=json_tokener_parse("{\"command\":\"printf changed > marker\"}");
    assert(ui.service.check(&ui,"shell",args,&approval,&effect,error,sizeof error)==AH_DONE && approval);
    json_object_put(args);
    struct json_object *wrapped=json_tokener_parse("{\"command\":\"\\\"printf observed\\\"\"}");
    assert(ui.service.check(&ui,"shell",wrapped,&approval,&effect,error,sizeof error)==AH_INVALID_ARGUMENTS);
    assert(strstr(error,"ONE executable") && strstr(error,"outer quotes"));
    json_object_put(wrapped);
    char script[512];snprintf(script,sizeof script,"%s/runner3",temp);
    FILE *fixture=fopen(script,"w");assert(fixture);
    assert(fputs("#!/bin/sh\nprintf candidate",fixture)>=0 && !fclose(fixture) && !chmod(script,0700));
    const char *old_path=getenv("PATH");char *saved_path=old_path?strdup(old_path):NULL;
    assert(!setenv("PATH",temp,1));
    wrapped=json_tokener_parse("{\"command\":\"\\\"runner script.file\\\"\"}");
    assert(ui.service.check(&ui,"shell",wrapped,&approval,&effect,error,sizeof error)==AH_INVALID_ARGUMENTS);
    assert(strstr(error,script));json_object_put(wrapped);
    struct json_object *missing=json_object_new_object();
    char command[512];snprintf(command,sizeof command,"%s/runner script.file",temp);
    json_object_object_add(missing,"command",json_object_new_string(command));
    assert(ui.service.check(&ui,"shell",missing,&approval,&effect,error,sizeof error)==AH_DONE && approval);
    out=NULL;
    assert(ui.service.call(&ui,"shell",missing,&hooks,1024,5000,&out)==AH_EXEC_FAILED);
    assert(strstr(out,"exit_code=127\n") && strstr(out,script) && strstr(out,"not executed"));
    assert(strlen(out)<=1024);free(out);json_object_put(missing);
    if(saved_path) {assert(!setenv("PATH",saved_path,1));free(saved_path);} else assert(!unsetenv("PATH"));
    assert(!unlink(script));
    snprintf(script,sizeof script,"%s/space tool",temp);
    fixture=fopen(script,"w");assert(fixture);
    assert(fputs("#!/bin/sh\nprintf legitimate",fixture)>=0 && !fclose(fixture) && !chmod(script,0700));
    wrapped=json_tokener_parse("{\"command\":\"\\\"./space tool\\\"\"}");
    assert(ui.service.check(&ui,"shell",wrapped,&approval,&effect,error,sizeof error)==AH_DONE && approval);
    out=NULL;
    assert(ui.service.call(&ui,"shell",wrapped,&hooks,1024,5000,&out)==AH_DONE && strstr(out,"legitimate"));
    free(out);json_object_put(wrapped);assert(!unlink(script));

    struct json_object *quoted=json_tokener_parse("{\"path\":\"\\\"confusing.py\\\"\",\"text\":\"print(12)\"}");
    assert(!ah_linux_tool_enable(&base,"write_file",1));
    assert(ui.service.check(&ui,"write_file",quoted,&approval,&effect,error,sizeof error)==AH_INVALID_ARGUMENTS);
    assert(strstr(error,"quote"));
    ui.automatic=0;
    assert(ui.service.check(&ui,"write_file",quoted,&approval,&effect,error,sizeof error)==AH_DONE && approval);
    json_object_put(quoted);
    args=json_tokener_parse("{\"command\":\"date\"}");
    assert(ui.service.check(&ui,"shell",args,&approval,&effect,error,sizeof error)==AH_DONE && approval);
    json_object_put(args);
    cli_tools_close(&ui);ah_linux_tools_close(&base);
    assert(!rmdir(temp));
    puts("PASS CLI-only literal read policy, URL/flag/operator negatives, fixed argv, checked dispatch and strict mode");
}
