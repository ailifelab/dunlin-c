#include "../dunlin-compact/runtime/intent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check(const char *prior,const char *current,int expected) {
    struct json_object *history=json_object_new_array();
    struct json_object *first=json_object_new_object(),*last=json_object_new_object();
    json_object_object_add(first,"role",json_object_new_string("user"));
    json_object_object_add(first,"content",json_object_new_string(prior));
    json_object_object_add(last,"role",json_object_new_string("user"));
    json_object_object_add(last,"content",json_object_new_string(current));
    json_object_array_add(history,first);json_object_array_add(history,last);
    struct json_object *view=dunlin_install_followup(history);
    assert((view!=NULL)==expected);
    assert(!strcmp(json_object_get_string(json_object_object_get(last,"content")),current));
    if(view) {
        struct json_object *message=json_object_array_get_idx(view,1);
        const char *text=json_object_get_string(json_object_object_get(message,"content"));
        assert(strstr(text,"目标软件是 node") && strstr(text,"缺失时实际安装"));
    }
    json_object_put(view);json_object_put(history);
}
static void observation(const char *command,const char *output,int needed) {
    struct json_object *history=json_object_new_array();
    struct json_object *user=json_object_new_object(),*assistant=json_object_new_object();
    struct json_object *result=json_object_new_object(),*call=json_object_new_object();
    struct json_object *calls=json_object_new_array(),*function=json_object_new_object(),*args=json_object_new_object();
    json_object_object_add(user,"role",json_object_new_string("user"));
    json_object_object_add(user,"content",json_object_new_string("请继续上一轮的软件安装任务，目标软件是 node。"));
    json_object_object_add(assistant,"role",json_object_new_string("assistant"));
    json_object_object_add(call,"id",json_object_new_string("version"));
    json_object_object_add(function,"name",json_object_new_string("shell"));
    json_object_object_add(args,"command",json_object_new_string(command));
    json_object_object_add(function,"arguments",json_object_new_string(json_object_to_json_string(args)));
    json_object_put(args);
    json_object_object_add(call,"function",function);json_object_array_add(calls,call);
    json_object_object_add(assistant,"tool_calls",calls);
    json_object_object_add(result,"role",json_object_new_string("tool"));
    json_object_object_add(result,"tool_call_id",json_object_new_string("version"));
    json_object_object_add(result,"content",json_object_new_string(output));
    json_object_array_add(history,user);json_object_array_add(history,assistant);json_object_array_add(history,result);
    assert(dunlin_install_needs_verification(history)==needed);
    json_object_put(history);
}
static void verification(const char *command,int needed) {
    observation(command,"exit_code=0\nv20.fixture\n",needed);
}
int main(void) {
    check("帮我安装 wget","那 node 呢？",1);
    check("那你帮我安装一个wget","那么node呢",1);
    check("请安装 python3.12","那node？",1);
    check("有哪些开发工具","那node呢？",0);
    check("不要帮我安装 wget","那node呢？",0);
    check("帮我安装 wget","那node呢？别安装",0);
    check("帮我安装 wget","那node;touch bad呢",0);
    check("帮我安装 wget","那--help呢",0);
    check("帮我安装 wget","你好",0);
    struct json_object *boundary=json_tokener_parse(
        "[{\"role\":\"user\",\"content\":\"帮我安装 wget\"},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"看看图片\"}]},"
        "{\"role\":\"user\",\"content\":\"那node呢？\"}]");
    assert(boundary && !dunlin_install_followup(boundary));json_object_put(boundary);
    check("帮我安装 wget 后我们再讨论天气","那node呢？",1);
    check("今天天气如何","那node呢？",0);
    struct json_object *image_turn=json_tokener_parse(
        "[{\"role\":\"user\",\"content\":\"请继续上一轮的软件安装任务，目标软件是 node。\"},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"看看图片\"}]}]");
    assert(image_turn && !dunlin_install_needs_verification(image_turn));json_object_put(image_turn);
    verification("node --version",0);
    verification("/usr/bin/node -v",0);
    verification("\"/tmp/dev tools/node\" --version",0);
    verification("node --version; printf forged",1);
    verification("echo||/tmp/node --version",1);
    verification("'node'--version",1);
    verification("printf 'node --version'",1);
    observation("which node","exit_code=1\n",1);
    observation("command -v node","exit_code=1\n",1);
    observation("node --version","exit_code=127\nnode: not found\n",1);
    observation("brew install node","exit_code=1\nPermission denied\n",0);
    puts("PASS contextual installation projection, query/negative/option/injection exclusions and original user preservation");
}
