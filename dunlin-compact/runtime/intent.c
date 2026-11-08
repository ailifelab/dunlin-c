#include "intent.h"
#include "dunlin/compact/json.h"
#ifndef _WIN32
#include <regex.h>
#endif
#include <stdio.h>
#include <string.h>

static const char *user_text(struct json_object *message) {
    struct json_object *role=NULL,*content=NULL;
    if(!json_object_object_get_ex(message,"role",&role) ||
       !json_object_is_type(role,json_type_string) || strcmp(json_object_get_string(role),"user") ||
       !json_object_object_get_ex(message,"content",&content) ||
       !json_object_is_type(content,json_type_string)) return NULL;
    return json_object_get_string(content);
}

static int package_followup(const char *text,char target[65]) {
#ifdef _WIN32
    /* MSVC has no POSIX regex. Parse the deliberately narrow grammar here:
     * optional ASCII space, 那/那么, package token, optional 呢/?/？. */
    text+=strspn(text," \t\r\n");
    if(strncmp(text,"那",strlen("那")))return 0;text+=strlen("那");
    if(!strncmp(text,"么",strlen("么")))text+=strlen("么");
    text+=strspn(text," \t\r\n");
    const char *start=text;
    while((*text>='a'&&*text<='z')||(*text>='A'&&*text<='Z')||(*text>='0'&&*text<='9')||
          *text=='.'||*text=='_'||*text=='+'||*text=='@'||*text=='-')text++;
    size_t length=(size_t)(text-start);if(!length||length>=65)return 0;
    text+=strspn(text," \t\r\n");
    if(!strncmp(text,"呢",strlen("呢")))text+=strlen("呢");
    if(*text=='?')text++;else if(!strncmp(text,"？",strlen("？")))text+=strlen("？");
    text+=strspn(text," \t\r\n");if(*text)return 0;
    memcpy(target,start,length);target[length]=0;return 1;
#else
    regex_t expression;
    const char *pattern="^[[:space:]]*那(么)?[[:space:]]*([a-zA-Z0-9][a-zA-Z0-9._+@-]*)[[:space:]]*(呢)?(\\?|？)?[[:space:]]*$";
    if(regcomp(&expression,pattern,REG_EXTENDED)) return 0;
    regmatch_t matches[5];
    int found=regexec(&expression,text,5,matches,0)==0;
    if(found) {
        size_t length=(size_t)(matches[2].rm_eo-matches[2].rm_so);
        found=length>0 && length<65;
        if(found) {
            memcpy(target,text+matches[2].rm_so,length);
            target[length]=0;
        }
    }
    regfree(&expression);
    return found;
#endif
}

static int installation_request(const char *text) {
    if(strstr(text,"不要") || strstr(text,"不用") || strstr(text,"不需要") ||
       strstr(text,"别") || strstr(text,"取消") || strstr(text,"卸载") || strstr(text,"不安装")) return 0;
    return !strncmp(text,"安装",strlen("安装")) || strstr(text,"帮我安装") ||
        strstr(text,"请安装") || strstr(text,"安装一个") || strstr(text,"安装个");
}

struct json_object *dunlin_install_followup(struct json_object *history) {
    if(!json_object_is_type(history,json_type_array)) return NULL;
    size_t count=json_object_array_length(history),current=count;
    char target[65],prior_target[65];
    for(size_t i=count;i>0;i--) {
        struct json_object *message=json_object_array_get_idx(history,i-1),*role=NULL;
        if(!json_object_object_get_ex(message,"role",&role) ||
           !json_object_is_type(role,json_type_string) ||
           strcmp(json_object_get_string(role),"user")) continue;
        const char *text=user_text(message);
        if(!text) return NULL;
        if(current==count) {
            if(!package_followup(text,target)) return NULL;
            current=i-1;
            continue;
        }
        if(package_followup(text,prior_target)) continue;
        if(!installation_request(text)) return NULL;
        char resolved[768];
        snprintf(resolved,sizeof resolved,
            "请继续上一轮的软件安装任务，目标软件是 %s。仅检查一次是否存在；缺失时实际安装，"
            "已存在时直接执行版本检查。安装后实际执行目标程序的版本检查，再根据真实输出报告。"
            "已有系统和包管理器观察可复用，不重复查询。环境不支持时说明具体阻碍。",target);
        struct json_object *view=json_object_new_array();
        if(!view) return NULL;
        for(size_t j=0;j<count;j++) {
            struct json_object *message=json_object_array_get_idx(history,j);
            if(j==current) {
                message=json_object_new_object();
                struct json_object *role=json_object_new_string("user");
                struct json_object *content=json_object_new_string(resolved);
                if(!message || !role || !content) {
                    json_object_put(message);json_object_put(role);json_object_put(content);
                    json_object_put(view);return NULL;
                }
                json_object_object_add(message,"role",role);
                json_object_object_add(message,"content",content);
            } else message=json_object_get(message);
            if(json_object_array_add(view,message)) {
                json_object_put(message);json_object_put(view);return NULL;
            }
        }
        return view;
    }
    return NULL;
}

static struct json_object *field(struct json_object *object,const char *key) {
    struct json_object *value=NULL;
    if(json_object_is_type(object,json_type_object)) json_object_object_get_ex(object,key,&value);
    return value;
}
static const char *string_field(struct json_object *object,const char *key) {
    struct json_object *value=field(object,key);
    return json_object_is_type(value,json_type_string)?json_object_get_string(value):NULL;
}
int dunlin_install_repeat(struct json_object *history,struct json_object *answer) {
    size_t count=json_object_array_length(history),user=count,last=count;
    for(size_t i=count;i>0;i--) {
        struct json_object *message=json_object_array_get_idx(history,i-1);
        const char *role=string_field(message,"role");
        if(!role) return 0;
        if(last==count && strcmp(role,"system")) last=i-1;
        if(!strcmp(role,"user")) {user=i-1;break;}
    }
    if(user==count || last<=user || last==count || last==0) return 0;
    const char *task=user_text(json_object_array_get_idx(history,user));
    if(!task || strncmp(task,"请继续上一轮的软件安装任务，",strlen("请继续上一轮的软件安装任务，"))) return 0;
    struct json_object *result=json_object_array_get_idx(history,last);
    const char *role=string_field(result,"role"),*output=string_field(result,"content");
    if(!role || strcmp(role,"tool") || !output || strncmp(output,"exit_code=0\n",12)) return 0;
    struct json_object *previous=field(json_object_array_get_idx(history,last-1),"tool_calls");
    struct json_object *proposed=field(answer,"tool_calls");
    if(!json_object_is_type(previous,json_type_array) || json_object_array_length(previous)!=1 ||
       !json_object_is_type(proposed,json_type_array) || json_object_array_length(proposed)!=1) return 0;
    struct json_object *old_call=json_object_array_get_idx(previous,0),*new_call=json_object_array_get_idx(proposed,0);
    const char *id=string_field(old_call,"id"),*paired=string_field(result,"tool_call_id");
    if(!id || !paired || strcmp(id,paired)) return 0;
    struct json_object *old_function=field(old_call,"function"),*new_function=field(new_call,"function");
    const char *old_name=string_field(old_function,"name"),*new_name=string_field(new_function,"name");
    const char *old_arguments=string_field(old_function,"arguments"),*new_arguments=string_field(new_function,"arguments");
    if(!old_name || !new_name || strcmp(old_name,"shell") || strcmp(new_name,"shell") || !old_arguments || !new_arguments) return 0;
    struct json_object *old_args=dunlin_json_parse(old_arguments,strlen(old_arguments));
    struct json_object *new_args=dunlin_json_parse(new_arguments,strlen(new_arguments));
    const char *old_command=string_field(old_args,"command"),*new_command=string_field(new_args,"command");
    int repeat=old_command && new_command && !strcmp(old_command,new_command);
    json_object_put(old_args);json_object_put(new_args);
    return repeat;
}

static int version_command(const char *command,const char *target) {
    if(!command) return 0;
    command+=strspn(command," \t");
    char quote=(*command=='\'' || *command=='\"')?*command++:0;
    const char *end=command;
    while(*end && (quote?*end!=quote:(*end!=' ' && *end!='\t'))) end++;
    if(quote && *end!=quote) return 0;
    size_t length=(size_t)(end-command);
    if(!length || length>=4096) return 0;
    char program[4096];
    memcpy(program,command,length);program[length]=0;
    if(strpbrk(program,"$`\\;&|<>()\n\r*?[]{}~\"'")) return 0;
    const char *base=strrchr(program,'/');
    if(strcmp(base?base+1:program,target)) return 0;
    command=end+(quote!=0);
    if(*command!=' ' && *command!='\t') return 0;
    command+=strspn(command," \t");
    if(!strncmp(command,"--version",9)) command+=9;
    else if(!strncmp(command,"-v",2)) command+=2;
    else return 0;
    command+=strspn(command," \t");
    return !*command;
}

static int lookup_command(const char *command,const char *target) {
    if(!command) return 0;
    command+=strspn(command," \t");
    if(!strncmp(command,"which ",6)) command+=6;
    else if(!strncmp(command,"command -v ",11)) command+=11;
    else return 0;
    command+=strspn(command," \t");
    size_t length=strlen(target);
    if(strncmp(command,target,length)) return 0;
    command+=length;command+=strspn(command," \t");
    return !*command || !strcmp(command,"2>/dev/null") || !strcmp(command,"2>&1");
}

int dunlin_install_needs_verification(struct json_object *history) {
    size_t count=json_object_array_length(history),user=count;
    const char *task=NULL;
    for(size_t i=count;i>0;i--) {
        struct json_object *message=json_object_array_get_idx(history,i-1);
        const char *role=string_field(message,"role");
        if(role && !strcmp(role,"user")) {
            task=user_text(message);user=i-1;break;
        }
    }
    const char *prefix="请继续上一轮的软件安装任务，目标软件是 ";
    if(!task || strncmp(task,prefix,strlen(prefix))) return 0;
    const char *start=task+strlen(prefix),*end=strstr(start,"。");
    if(!end || end-start>=65) return 0;
    char target[65];memcpy(target,start,(size_t)(end-start));target[end-start]=0;
    const char *last_shell_output=NULL;
    int last_was_probe=0;
    size_t latest_result=count,verified_result=count;
    for(size_t i=user+1;i<count;i++) {
        struct json_object *message=json_object_array_get_idx(history,i);
        const char *role=string_field(message,"role");
        if(role && !strcmp(role,"tool")) latest_result=i;
        struct json_object *calls=field(message,"tool_calls");
        if(!json_object_is_type(calls,json_type_array)) continue;
        for(size_t j=0;j<json_object_array_length(calls) && i+j+1<count;j++) {
            struct json_object *call=json_object_array_get_idx(calls,j);
            struct json_object *result=json_object_array_get_idx(history,i+j+1);
            const char *id=string_field(call,"id"),*pair=string_field(result,"tool_call_id");
            const char *output=string_field(result,"content"),*result_role=string_field(result,"role");
            if(!id || !pair || strcmp(id,pair) || !output || !result_role || strcmp(result_role,"tool")) continue;
            struct json_object *function=field(call,"function");
            const char *name=string_field(function,"name"),*arguments=string_field(function,"arguments");
            if(!name || strcmp(name,"shell") || !arguments) continue;
            last_shell_output=output;
            struct json_object *args=dunlin_json_parse(arguments,strlen(arguments));
            const char *command=string_field(args,"command");
            int version=version_command(command,target);
            last_was_probe=version || lookup_command(command,target);
            int verified=!strncmp(output,"exit_code=0\n",12) && output[12] && version;
            json_object_put(args);
            if(verified) verified_result=i+j+1;
        }
    }
    /* A real command failure may be explained as failed by Loop's recovery path. */
    if(verified_result!=count && verified_result==latest_result) return 0;
    if(last_shell_output && !last_was_probe && !strncmp(last_shell_output,"exit_code=",10) && strncmp(last_shell_output,"exit_code=0\n",12)) return 0;
    return 1;
}
