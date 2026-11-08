#define WIN32_LEAN_AND_MEAN
#include "process.h"
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DWP_MAX_ARGS 64
#define DWP_MAX_OUTPUT (1024u * 1024u)

static wchar_t *wide(const char *text) {
    if(!text)return NULL;
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0);
    wchar_t *out=count>0?malloc((size_t)count*sizeof *out):NULL;
    if(!out||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,out,count)){free(out);return NULL;}
    return out;
}
static int absolute_safe(const wchar_t *path) {
    if(!path||!path[0]||!path[1])return 0;
    if(!wcsncmp(path,L"\\\\.\\",4)||!wcsncmp(path,L"\\\\?\\",4))return 0;
    return (path[1]==L':'&&path[2]==L'\\')||(path[0]==L'\\'&&path[1]==L'\\');
}
static size_t quoted_length(const wchar_t *arg) {
    size_t n=2;
    for(size_t i=0;arg[i];) {
        size_t slashes=0;while(arg[i+slashes]==L'\\')slashes++;
        i+=slashes;
        if(!arg[i]){n+=slashes*2;break;}
        n+=slashes+(arg[i]==L'"'?2:1);i++;
    }
    return n;
}
static wchar_t *append_quoted(wchar_t *out,const wchar_t *arg) {
    *out++=L'"';
    for(size_t i=0;arg[i];) {
        size_t slashes=0;while(arg[i+slashes]==L'\\')slashes++;
        i+=slashes;
        if(!arg[i]){for(size_t j=0;j<slashes*2;j++)*out++=L'\\';break;}
        for(size_t j=0;j<slashes+(arg[i]==L'"'?slashes+1:0);j++)*out++=L'\\';
        *out++=arg[i++];
    }
    *out++=L'"';return out;
}
static wchar_t *command_line(const wchar_t *exe,const char *const *argv,size_t argc) {
    wchar_t **items=calloc(argc+1,sizeof *items);if(!items)return NULL;
    items[0]=(wchar_t *)exe;size_t total=quoted_length(exe)+1;int ok=1;
    for(size_t i=0;i<argc;i++) {
        items[i+1]=wide(argv[i]);if(!items[i+1]){ok=0;break;}
        size_t add=quoted_length(items[i+1])+1;if(total>SIZE_MAX-add){ok=0;break;}total+=add;
    }
    wchar_t *line=ok&&total<=32767?malloc(total*sizeof *line):NULL,*at=line;
    if(line)for(size_t i=0;i<=argc;i++){if(i)*at++=L' ';at=append_quoted(at,items[i]);} 
    if(line)*at=0;
    for(size_t i=1;i<=argc;i++)free(items[i]);free(items);return line;
}
static wchar_t *shell_line(const wchar_t *exe,const char *command) {
    wchar_t *raw=wide(command);if(!raw||!*raw){free(raw);return NULL;}
    static const wchar_t prefix[]=L" /d /q /c ";
    size_t a=quoted_length(exe),b=wcslen(raw),p=wcslen(prefix),total=a+p+b+1;
    wchar_t *line=total<=32767?malloc(total*sizeof *line):NULL,*at=line;
    if(line){at=append_quoted(at,exe);memcpy(at,prefix,p*sizeof *at);at+=p;memcpy(at,raw,(b+1)*sizeof *at);}
    free(raw);return line;
}
/* cmd.exe writes redirected built-in output in the process OEM code page even
 * when the parent is a UTF-8 console. Normalize only the shell path; exact-argv
 * exec keeps the child program's byte contract unchanged. */
static int shell_utf8(char **output,size_t *used,size_t limit) {
    if(!*used)return DWP_OK;
    int wide_count=MultiByteToWideChar(CP_OEMCP,0,*output,(int)*used,NULL,0);
    wchar_t *text=wide_count>0?malloc((size_t)wide_count*sizeof *text):NULL;
    if(!text||!MultiByteToWideChar(CP_OEMCP,0,*output,(int)*used,text,wide_count)){free(text);return DWP_FAILED;}
    int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,wide_count,NULL,0,NULL,NULL);
    if(bytes<=0){free(text);return DWP_FAILED;}
    if((size_t)bytes>limit){free(text);return DWP_OUTPUT_LIMIT;}
    char *normalized=malloc((size_t)bytes+1);
    if(!normalized||!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,wide_count,normalized,bytes,NULL,NULL)){
        free(normalized);free(text);return DWP_FAILED;
    }
    normalized[bytes]=0;free(text);free(*output);*output=normalized;*used=(size_t)bytes;return DWP_OK;
}
static int regular_directory(const wchar_t *path) {
    HANDLE h=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    FILE_ATTRIBUTE_TAG_INFO info={0};
    int ok=h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandleEx(h,FileAttributeTagInfo,&info,sizeof info)&&
        (info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return ok;
}
static int regular_file(const wchar_t *path) {
    HANDLE h=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    FILE_ATTRIBUTE_TAG_INFO info={0};
    int ok=h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandleEx(h,FileAttributeTagInfo,&info,sizeof info)&&
        !(info.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return ok;
}
void dwp_result_free(dwp_result *r){if(r){free(r->output);memset(r,0,sizeof *r);}}
static int run_internal(const char *executable,const char *const *argv,size_t argc,const char *raw_command,const char *workspace,
        unsigned timeout_ms,size_t limit,int (*cancelled)(void *),void *cancel_user,dwp_result *result) {
    if(result)memset(result,0,sizeof *result);
    if(!result||!executable||(!argv&&argc)||argc>DWP_MAX_ARGS||!timeout_ms||!limit||limit>DWP_MAX_OUTPUT)
        return DWP_FAILED;
    wchar_t *exe=wide(executable),*dir=wide(workspace),*line=NULL;
    if(!absolute_safe(exe)||!regular_file(exe)||!regular_directory(dir)||
       (line=raw_command?shell_line(exe,raw_command):command_line(exe,argv,argc))==NULL)goto failed;
    SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};HANDLE read_pipe=NULL,write_pipe=NULL,input=NULL,job=NULL,process=NULL,thread=NULL;
    if(!CreatePipe(&read_pipe,&write_pipe,&sa,0)||!SetHandleInformation(read_pipe,HANDLE_FLAG_INHERIT,0))goto handles;
    input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(input==INVALID_HANDLE_VALUE){input=NULL;goto handles;}
    job=CreateJobObjectW(NULL,NULL);if(!job)goto handles;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits))goto handles;
    STARTUPINFOW startup={0};startup.cb=sizeof startup;startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=input;startup.hStdOutput=write_pipe;startup.hStdError=write_pipe;
    PROCESS_INFORMATION child={0};
    if(!CreateProcessW(exe,line,NULL,NULL,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW,NULL,dir,&startup,&child))goto handles;
    process=child.hProcess;thread=child.hThread;
    if(!AssignProcessToJobObject(job,process)||ResumeThread(thread)==(DWORD)-1)goto handles;
    CloseHandle(thread);thread=NULL;CloseHandle(write_pipe);write_pipe=NULL;CloseHandle(input);input=NULL;
    char *output=malloc(limit+1);if(!output)goto handles;
    size_t used=0;ULONGLONG deadline=GetTickCount64()+timeout_ms;int status=DWP_OK,finished=0;
    while(!finished) {
        DWORD available=0;
        if(!PeekNamedPipe(read_pipe,NULL,0,NULL,&available,NULL)) {
            if(GetLastError()==ERROR_BROKEN_PIPE&&WaitForSingleObject(process,0)==WAIT_OBJECT_0) {
                finished=1;continue;
            }
            status=DWP_FAILED;break;
        }
        while(available) {
            char buffer[4096];DWORD want=available>sizeof buffer?sizeof buffer:available,got=0;
            if(!ReadFile(read_pipe,buffer,want,&got,NULL)||!got){status=DWP_FAILED;break;}
            if(used+got>limit){status=DWP_OUTPUT_LIMIT;break;}
            memcpy(output+used,buffer,got);used+=got;available-=got;
        }
        if(status!=DWP_OK)break;
        DWORD wait=WaitForSingleObject(process,0);
        if(wait==WAIT_OBJECT_0){finished=1;continue;}
        if(wait!=WAIT_TIMEOUT){status=DWP_FAILED;break;}
        if(cancelled&&cancelled(cancel_user)){status=DWP_CANCELLED;break;}
        if(GetTickCount64()>=deadline){status=DWP_TIMEOUT;break;}
        Sleep(5);
    }
    if(status!=DWP_OK){TerminateJobObject(job,1);WaitForSingleObject(process,5000);}
    else {
        for(;;){DWORD available=0;if(!PeekNamedPipe(read_pipe,NULL,0,NULL,&available,NULL)||!available)break;
            char buffer[4096];DWORD want=available>sizeof buffer?sizeof buffer:available,got=0;
            if(!ReadFile(read_pipe,buffer,want,&got,NULL)||!got){status=DWP_FAILED;break;}
            if(used+got>limit){status=DWP_OUTPUT_LIMIT;break;}memcpy(output+used,buffer,got);used+=got;
        }
    }
    if(status==DWP_OK&&raw_command)status=shell_utf8(&output,&used,limit);
    DWORD exit_code=0;if(!GetExitCodeProcess(process,&exit_code)&&status==DWP_OK)status=DWP_FAILED;
    output[used]=0;result->status=status==DWP_OK&&exit_code?DWP_EXITED:status;
    result->exit_code=exit_code;result->output=output;result->output_size=used;
    CloseHandle(read_pipe);CloseHandle(process);CloseHandle(job);free(line);free(dir);free(exe);return result->status;
handles:
    if(job)TerminateJobObject(job,1);if(process)TerminateProcess(process,1);if(thread)CloseHandle(thread);if(process){WaitForSingleObject(process,5000);CloseHandle(process);}
    if(job)CloseHandle(job);if(read_pipe)CloseHandle(read_pipe);if(write_pipe)CloseHandle(write_pipe);if(input)CloseHandle(input);
failed:
    free(line);free(dir);free(exe);result->status=DWP_FAILED;return DWP_FAILED;
}
int dwp_run_ex(const char *executable,const char *const *argv,size_t argc,const char *workspace,
        unsigned timeout_ms,size_t limit,int (*cancelled)(void *),void *cancel_user,dwp_result *result) {
    return run_internal(executable,argv,argc,NULL,workspace,timeout_ms,limit,cancelled,cancel_user,result);
}
int dwp_run_shell(const char *cmd_executable,const char *command,const char *workspace,
        unsigned timeout_ms,size_t limit,int (*cancelled)(void *),void *cancel_user,dwp_result *result) {
    return run_internal(cmd_executable,NULL,0,command,workspace,timeout_ms,limit,cancelled,cancel_user,result);
}
int dwp_run(const char *executable,const char *const *argv,size_t argc,const char *workspace,
        unsigned timeout_ms,size_t limit,dwp_result *result) {
    return dwp_run_ex(executable,argv,argc,workspace,timeout_ms,limit,NULL,NULL,result);
}
