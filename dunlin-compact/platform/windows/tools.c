#define WIN32_LEAN_AND_MEAN
#include "dunlin/compact/tools.h"
#include "../random.h"
#include "../http_get.h"
#include "process.h"
#include <windows.h>
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ECHO,CALC,READ_FILE,WRITE_FILE,EXEC,HTTP_GET,SHELL,TOOL_COUNT } tool_id;
typedef struct { char workspace[MAX_PATH];HANDLE directory;FILE_ID_INFO identity;unsigned enabled; } win_tools;
static const char *const names[TOOL_COUNT]={"echo","calc","read_file","write_file","exec","http_get","shell"};
static const unsigned field_counts[TOOL_COUNT]={1,3,1,2,2,1,1};
static int find(const char *name){for(int i=0;i<TOOL_COUNT;i++)if(name&&!strcmp(name,names[i]))return i;return -1;}
static struct json_object *schemas(void *opaque) {
    win_tools *state=opaque;struct json_object *out=json_object_new_array();
    static const char *const params[TOOL_COUNT]={
      "{\"text\":{\"type\":\"string\"}}","{\"op\":{\"type\":\"string\"},\"a\":{\"type\":\"number\"},\"b\":{\"type\":\"number\"}}",
      "{\"path\":{\"type\":\"string\"}}","{\"path\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}}",
      "{\"executable\":{\"type\":\"string\"},\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}",
      "{\"url\":{\"type\":\"string\"}}","{\"command\":{\"type\":\"string\"}}"};
    static const char *const descriptions[TOOL_COUNT]={
      "Return UTF-8 text unchanged.","Perform add, sub, mul, or div on two numbers.",
      "Read one UTF-8 file by basename from the configured workspace.",
      "Atomically write one UTF-8 file by basename in the configured workspace.",
      "Run an approved Windows executable with an exact argv array; executable must be an absolute Windows path.",
      "Read bounded UTF-8 text from a public HTTPS URL. GET only; no credentials, headers, body or redirects; no approval. Use this instead of shell/curl for public web reads.",
      "Run an approved Windows cmd.exe command. This host is Windows, not POSIX: never use /dev/null, jq, grep, sed, awk, sh, or bash. Use NUL for discarded output, curl.exe for curl, and powershell.exe -NoProfile -Command with ConvertFrom-Json for HTTP JSON processing."};
    for(int i=0;out&&i<TOOL_COUNT;i++)if(state->enabled&(1u<<i)) {
        struct json_object *entry=json_object_new_object(),*fn=json_object_new_object(),*par=json_object_new_object();
        struct json_object *properties=json_tokener_parse(params[i]);
        json_object_object_add(par,"type",json_object_new_string("object"));json_object_object_add(par,"properties",properties);
        json_object_object_add(par,"additionalProperties",json_object_new_boolean(0));
        struct json_object *required=json_object_new_array();
        json_object_object_foreach(properties,key,value){(void)value;json_object_array_add(required,json_object_new_string(key));}
        json_object_object_add(par,"required",required);
        json_object_object_add(fn,"name",json_object_new_string(names[i]));json_object_object_add(fn,"description",json_object_new_string(descriptions[i]));
        json_object_object_add(fn,"parameters",par);json_object_object_add(entry,"type",json_object_new_string("function"));json_object_object_add(entry,"function",fn);
        if(json_object_array_add(out,entry)){json_object_put(entry);json_object_put(out);out=NULL;}
    }
    return out;
}
static struct json_object *field(struct json_object *o,const char *name){struct json_object *v=NULL;if(json_object_is_type(o,json_type_object))json_object_object_get_ex(o,name,&v);return v;}
static const char *text(struct json_object *o,const char *name){struct json_object *v=field(o,name);return json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL;}
static int basename_ok(const char *s){return s&&*s&&strlen(s)<=255&&!strchr(s,'/')&&!strchr(s,'\\')&&strcmp(s,".")&&strcmp(s,"..");}
static ah_status check(void *opaque,const char *name,struct json_object *args,int *approval,int *effect,char *error,size_t size) {
    win_tools *state=opaque;int id=find(name);const char *why=NULL;
    if(id<0||!(state->enabled&(1u<<id)))why="unknown or disabled tool";
    else if(!json_object_is_type(args,json_type_object))why="arguments must be an object";
    else if(id==ECHO&&!text(args,"text"))why="echo.text must be a string";
    else if((id==READ_FILE||id==WRITE_FILE)&&!basename_ok(text(args,"path")))why="path must be a basename";
    else if(id==WRITE_FILE&&!text(args,"text"))why="write_file.text must be a string";
    else if(id==SHELL&&(!text(args,"command")||!*text(args,"command")))why="shell.command must be nonempty";
    else if(id==HTTP_GET&&!text(args,"url"))why="http_get.url must be a string";
    else if(id==CALC&&(!text(args,"op")||!field(args,"a")||!field(args,"b")))why="calc requires op, a and b";
    else if(id==EXEC) {struct json_object *a=field(args,"argv");const char *exe=text(args,"executable");
        if(!exe||!json_object_is_type(a,json_type_array)||json_object_array_length(a)>64)why="exec requires executable and up to 64 argv strings";
        else for(size_t i=0;i<(size_t)json_object_array_length(a);i++)if(!json_object_is_type(json_object_array_get_idx(a,i),json_type_string)){why="exec argv must contain strings";break;}}
    if(!why&&(unsigned)json_object_object_length(args)!=field_counts[id])why="remove fields not listed in the tool schema";
    if(why){snprintf(error,size,"%s",why);return AH_INVALID_ARGUMENTS;}
    if(id==HTTP_GET&&!ah_https_url_check(text(args,"url"),error,size))return AH_INVALID_ARGUMENTS;
    *approval=*effect=id==WRITE_FILE||id==EXEC||id==SHELL;return AH_DONE;
}
static ah_status result(char **out,ah_status status,const char *value){size_t n=strlen(value);*out=malloc(n+1);if(!*out)return AH_FAILED;memcpy(*out,value,n+1);return status;}
static int current_workspace(win_tools *s) {
    HANDLE h=CreateFileA(s->workspace,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);FILE_ID_INFO id={0};
    int ok=h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandleEx(h,FileIdInfo,&id,sizeof id)&&id.VolumeSerialNumber==s->identity.VolumeSerialNumber&&!memcmp(&id.FileId,&s->identity.FileId,sizeof id.FileId);
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return ok;
}
static int full(win_tools *s,const char *name,char out[MAX_PATH]){return current_workspace(s)&&snprintf(out,MAX_PATH,"%s\\%s",s->workspace,name)>0&&strlen(out)<MAX_PATH;}
static ah_status run_process(win_tools *s,const char *exe,const char *const *argv,size_t argc,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out) {
    dwp_result run={0};size_t capture=cap>64?cap-64:1;
    int code=dwp_run_ex(exe,argv,argc,s->workspace,timeout,capture,hooks?hooks->cancelled:NULL,hooks?hooks->user:NULL,&run);
    char header[64];int n=snprintf(header,sizeof header,"exit_code=%lu\n",run.exit_code);size_t total=(size_t)n+run.output_size;
    if(total<=cap){*out=malloc(total+1);if(*out){memcpy(*out,header,(size_t)n);memcpy(*out+(size_t)n,run.output,run.output_size);(*out)[total]=0;}}
    dwp_result_free(&run);if(!*out)return AH_FAILED;
    if(code==DWP_OK)return AH_DONE;if(code==DWP_EXITED)return AH_EXEC_FAILED;
    if(code==DWP_TIMEOUT||code==DWP_OUTPUT_LIMIT||code==DWP_CANCELLED)return AH_UNKNOWN;return AH_FAILED;
}
static ah_status run_shell(win_tools *s,const char *exe,const char *command,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out) {
    dwp_result run={0};size_t capture=cap>64?cap-64:1;
    int code=dwp_run_shell(exe,command,s->workspace,timeout,capture,hooks?hooks->cancelled:NULL,hooks?hooks->user:NULL,&run);
    char header[64];int n=snprintf(header,sizeof header,"exit_code=%lu\n",run.exit_code);size_t total=(size_t)n+run.output_size;
    if(total<=cap){*out=malloc(total+1);if(*out){memcpy(*out,header,(size_t)n);memcpy(*out+(size_t)n,run.output,run.output_size);(*out)[total]=0;}}
    dwp_result_free(&run);if(!*out)return AH_FAILED;if(code==DWP_OK)return AH_DONE;if(code==DWP_EXITED)return AH_EXEC_FAILED;
    if(code==DWP_TIMEOUT||code==DWP_OUTPUT_LIMIT||code==DWP_CANCELLED)return AH_UNKNOWN;return AH_FAILED;
}
static ah_status call(void *opaque,const char *name,struct json_object *args,const ah_hooks *hooks,size_t cap,unsigned timeout,char **out) {
    *out=NULL;win_tools *s=opaque;int id=find(name);if(id<0)return AH_FAILED;
    if(id==ECHO)return result(out,AH_DONE,text(args,"text"));
    if(id==CALC){double a=json_object_get_double(field(args,"a")),b=json_object_get_double(field(args,"b")),v=0;const char *op=text(args,"op");
        if(!strcmp(op,"add"))v=a+b;else if(!strcmp(op,"sub"))v=a-b;else if(!strcmp(op,"mul"))v=a*b;else if(!strcmp(op,"div")&&b!=0)v=a/b;else return result(out,AH_INVALID_ARGUMENTS,"invalid calculation");
        char buf[64];snprintf(buf,sizeof buf,"%.17g",v);return isfinite(v)?result(out,AH_DONE,buf):result(out,AH_FAILED,"non-finite result");}
    if(id==READ_FILE){char path[MAX_PATH];if(!full(s,text(args,"path"),path))return result(out,AH_FAILED,"workspace changed");
        HANDLE h=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);BY_HANDLE_FILE_INFORMATION info={0};
        if(h==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||info.nNumberOfLinks!=1){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return result(out,AH_FAILED,"unsafe file");}
        LARGE_INTEGER len;if(!GetFileSizeEx(h,&len)||len.QuadPart<0||(uint64_t)len.QuadPart>cap){CloseHandle(h);return result(out,AH_BUDGET,"file exceeds output limit");}
        char *data=malloc((size_t)len.QuadPart+1);DWORD got=0;int ok=data&&ReadFile(h,data,(DWORD)len.QuadPart,&got,NULL)&&got==(DWORD)len.QuadPart&&ah_utf8_text(data,got);CloseHandle(h);
        if(!ok){free(data);return result(out,AH_FAILED,"cannot read UTF-8 file");}data[got]=0;*out=data;return AH_DONE;}
    if(id==WRITE_FILE){char path[MAX_PATH],temp[MAX_PATH];if(!full(s,text(args,"path"),path))return result(out,AH_FAILED,"workspace changed");
        unsigned char nonce[8];if(!ah_os_random(nonce,sizeof nonce))return result(out,AH_FAILED,"entropy unavailable");char hex[17];for(int i=0;i<8;i++)snprintf(hex+2*i,3,"%02x",nonce[i]);
        snprintf(temp,sizeof temp,"%s\\.dunlin-%s.tmp",s->workspace,hex);HANDLE h=CreateFileA(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,NULL);const char *value=text(args,"text");DWORD wrote=0;size_t n=strlen(value);
        int ok=h!=INVALID_HANDLE_VALUE&&n<=MAXDWORD&&WriteFile(h,value,(DWORD)n,&wrote,NULL)&&wrote==n&&FlushFileBuffers(h);if(h!=INVALID_HANDLE_VALUE&& !CloseHandle(h))ok=0;
        if(ok)ok=MoveFileExA(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);if(!ok){DeleteFileA(temp);return result(out,AH_FAILED,"cannot replace file");}return result(out,AH_DONE,"written");}
    if(id==HTTP_GET)return ah_https_get(text(args,"url"),hooks,cap,timeout,out);
    if(id==EXEC){struct json_object *array=field(args,"argv");size_t count=json_object_array_length(array);const char **argv=calloc(count,sizeof *argv);if(!argv)return AH_FAILED;for(size_t i=0;i<count;i++)argv[i]=json_object_get_string(json_object_array_get_idx(array,i));
        ah_status status=run_process(s,text(args,"executable"),argv,count,hooks,cap,timeout,out);free(argv);return status;}
    char system[MAX_PATH],cmd[MAX_PATH];UINT n=GetSystemDirectoryA(system,MAX_PATH);if(!n||n>=MAX_PATH||snprintf(cmd,sizeof cmd,"%s\\cmd.exe",system)<=0)return AH_FAILED;
    return run_shell(s,cmd,text(args,"command"),hooks,cap,timeout,out);
}
int ah_windows_tools(ah_tool_service *out,const char *workspace) {
    if(out)*out=(ah_tool_service){0};if(!out||!workspace)return -1;win_tools *s=calloc(1,sizeof *s);if(!s)return -1;
    DWORD n=GetFullPathNameA(workspace,MAX_PATH,s->workspace,NULL);if(!n||n>=MAX_PATH){free(s);return -1;}
    s->directory=CreateFileA(s->workspace,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    FILE_ATTRIBUTE_TAG_INFO info={0};if(s->directory==INVALID_HANDLE_VALUE||!GetFileInformationByHandleEx(s->directory,FileAttributeTagInfo,&info,sizeof info)||(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!(info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)||!GetFileInformationByHandleEx(s->directory,FileIdInfo,&s->identity,sizeof s->identity)){if(s->directory!=INVALID_HANDLE_VALUE)CloseHandle(s->directory);free(s);return -1;}
    s->enabled=(1u<<ECHO)|(1u<<CALC)|(1u<<READ_FILE)|(1u<<WRITE_FILE)|(1u<<EXEC)|(1u<<HTTP_GET);*out=(ah_tool_service){s,schemas,check,call};return 0;
}
int ah_windows_tool_enable(ah_tool_service *tools,const char *name,int enable){if(!tools||!tools->ctx)return -1;int id=find(name);if(id<0)return -1;win_tools *s=tools->ctx;if(enable)s->enabled|=1u<<id;else s->enabled&=~(1u<<id);return 0;}
void ah_windows_tools_close(ah_tool_service *tools){if(tools&&tools->ctx){win_tools *s=tools->ctx;CloseHandle(s->directory);free(s);*tools=(ah_tool_service){0};}}
