#define WIN32_LEAN_AND_MEAN
#include "dunlin/compact/tools.h"
#include <windows.h>
#include <json-c/json.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cancelled(void *unused){(void)unused;return 1;}
static struct json_object *parse(const char *s){struct json_object *o=json_tokener_parse(s);assert(o);return o;}
static ah_status invoke(ah_tool_service *tools,const char *name,const char *json,const ah_hooks *hooks,char **out){
    struct json_object *args=parse(json);int approval=0,effect=0;char error[128];
    assert(tools->check(tools->ctx,name,args,&approval,&effect,error,sizeof error)==AH_DONE);
    ah_status status=tools->call(tools->ctx,name,args,hooks,4096,3000,out);json_object_put(args);return status;
}
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--child")){fputs("child-ok",stdout);return 0;}
    char temp[MAX_PATH],workspace[MAX_PATH],exe[MAX_PATH];assert(GetTempPathA(MAX_PATH,temp));snprintf(workspace,sizeof workspace,"%sdunlin-tools-%lu",temp,(unsigned long)GetCurrentProcessId());assert(CreateDirectoryA(workspace,NULL));
    ah_tool_service tools={0};assert(!ah_windows_tools(&tools,workspace));char *out=NULL;
    assert(invoke(&tools,"echo","{\"text\":\"你好\"}",NULL,&out)==AH_DONE&&!strcmp(out,"你好"));free(out);
    assert(invoke(&tools,"write_file","{\"path\":\"note.txt\",\"text\":\"内容\"}",NULL,&out)==AH_DONE);free(out);
    assert(invoke(&tools,"read_file","{\"path\":\"note.txt\"}",NULL,&out)==AH_DONE&&!strcmp(out,"内容"));free(out);
    assert(GetModuleFileNameA(NULL,exe,MAX_PATH));char request[2*MAX_PATH];snprintf(request,sizeof request,"{\"executable\":\"%s\",\"argv\":[\"--child\"]}",exe);
    for(char *p=request;*p;p++)if(*p=='\\'){memmove(p+1,p,strlen(p)+1);*p++='\\';}
    assert(invoke(&tools,"exec",request,NULL,&out)==AH_DONE&&strstr(out,"exit_code=0")&&strstr(out,"child-ok"));free(out);
    struct json_object *web=parse("{\"url\":\"https://api.open-meteo.com/v1/forecast?latitude=39.9042&longitude=116.3985&current_weather=true\"}");
    int approval=1,effect=1;char check_error[160]={0};
    assert(tools.check(tools.ctx,"http_get",web,&approval,&effect,check_error,sizeof check_error)==AH_DONE&&!approval&&!effect);
    json_object_put(web);
    const char *blocked[]={"{\"url\":\"http://example.com/\"}","{\"url\":\"https://127.0.0.1/\"}",
        "{\"url\":\"https://localhost/\"}","{\"url\":\"https://user:secret@example.com/\"}",
        "{\"url\":\"https://example.com/\",\"headers\":{\"Authorization\":\"secret\"}}"};
    for(size_t i=0;i<sizeof blocked/sizeof *blocked;i++){
        web=parse(blocked[i]);approval=effect=1;
        assert(tools.check(tools.ctx,"http_get",web,&approval,&effect,check_error,sizeof check_error)==AH_INVALID_ARGUMENTS);
        json_object_put(web);
    }
    assert(!ah_windows_tool_enable(&tools,"shell",1));
    struct json_object *tool_schemas=tools.schema(tools.ctx);assert(tool_schemas);
    int found_shell=0,found_http=0;for(size_t i=0;i<(size_t)json_object_array_length(tool_schemas);i++){
        struct json_object *entry=json_object_array_get_idx(tool_schemas,i),*fn=NULL,*name=NULL,*description=NULL;
        assert(json_object_object_get_ex(entry,"function",&fn));
        assert(json_object_object_get_ex(fn,"name",&name)&&json_object_object_get_ex(fn,"description",&description));
        if(!strcmp(json_object_get_string(name),"shell")){const char *d=json_object_get_string(description);found_shell=1;assert(strstr(d,"Windows cmd.exe")&&strstr(d,"/dev/null")&&strstr(d,"powershell.exe")&&strstr(d,"ConvertFrom-Json"));}
        if(!strcmp(json_object_get_string(name),"http_get")){const char *d=json_object_get_string(description);found_http=1;assert(strstr(d,"public HTTPS")&&strstr(d,"no credentials")&&strstr(d,"no approval"));}
    }assert(found_shell&&found_http);json_object_put(tool_schemas);
    ah_status shell_status=invoke(&tools,"shell","{\"command\":\"echo shell-ok\"}",NULL,&out);
    if(shell_status!=AH_DONE||!out||!strstr(out,"shell-ok"))fprintf(stderr,"shell status=%d output=%s\n",shell_status,out?out:"NULL");
    assert(shell_status==AH_DONE&&strstr(out,"shell-ok"));free(out);
    shell_status=invoke(&tools,"shell","{\"command\":\"echo 中文输出\"}",NULL,&out);
    if(shell_status!=AH_DONE||!out||!ah_utf8_text(out,strlen(out))||!strstr(out,"中文输出")){
        fprintf(stderr,"utf8 shell status=%d size=%zu text=%s bytes=",shell_status,out?strlen(out):0,out?out:"NULL");
        if(out)for(size_t i=0;i<strlen(out);i++)fprintf(stderr,"%02x",(unsigned char)out[i]);fputc('\n',stderr);
    }
    assert(shell_status==AH_DONE&&out&&ah_utf8_text(out,strlen(out))&&strstr(out,"中文输出"));free(out);
    ah_hooks hooks={.cancelled=cancelled};assert(invoke(&tools,"shell","{\"command\":\"ping -n 6 127.0.0.1 >nul\"}",&hooks,&out)==AH_UNKNOWN);free(out);
    ah_windows_tools_close(&tools);char path[MAX_PATH];snprintf(path,sizeof path,"%s\\note.txt",workspace);assert(DeleteFileA(path));assert(RemoveDirectoryA(workspace));
    puts("Windows JSON tool service tests passed");return 0;
}
