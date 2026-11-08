#ifndef __APPLE__
#define _XOPEN_SOURCE 700
#endif
#include "internal.h"
#include "dunlin/compact/json.h"
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <fcntl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static const char sample[] =
    "{\n  \"base_url\": \"http://127.0.0.1:8080/v1\",\n  \"model\": \"local-model\",\n"
    "  \"stream\": true,\n  \"timeout_ms\": 30000,\n  \"workspace\": \"work\",\n"
    "  \"session_dir\": \"sessions\",\n"
    "  \"plugins\": [\"loop\", \"model\", \"tools\", \"sessions\", \"permission\", \"clock\"]\n}\n";

static int fail(char *error,size_t size,const char *message) {
    if (error && size) snprintf(error,size,"%s",message);
    return 0;
}

static int beside(const char *file,const char *path,char out[PATH_MAX]) {
    const char *slash = strrchr(file,'/');
#ifdef _WIN32
    const char *back=strrchr(file,'\\');if(!slash || (back&&back>slash))slash=back;
    int absolute=(path[0]&&path[1]==':')||(path[0]=='\\'&&path[1]=='\\')||path[0]=='/';
    int n=absolute||!slash?snprintf(out,PATH_MAX,"%s",path):
        snprintf(out,PATH_MAX,"%.*s\\%s",(int)(slash-file),file,path);
#else
    int n = path[0]=='/' || !slash ? snprintf(out,PATH_MAX,"%s",path) :
        snprintf(out,PATH_MAX,"%.*s/%s",(int)(slash-file),file,path);
#endif
    return n >= 0 && n < PATH_MAX;
}
int dunlin_init_config(const char *file,char *error,size_t size) {
    char work[PATH_MAX];
    if (!file || !beside(file,"work",work)) return fail(error,size,"path too long");
#ifdef _WIN32
    HANDLE fd=CreateFileA(file,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(fd==INVALID_HANDLE_VALUE)return fail(error,size,"cannot create new config (existing file is protected)");
    if(!CreateDirectoryA(work,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS) {
        CloseHandle(fd);DeleteFileA(file);return fail(error,size,"cannot initialize workspace");
    }
    DWORD attributes=GetFileAttributesA(work),written=0;
    int ok=attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&
        !(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&WriteFile(fd,sample,(DWORD)(sizeof(sample)-1),&written,NULL)&&
        written==sizeof(sample)-1&&FlushFileBuffers(fd);
    if(!CloseHandle(fd))ok=0;
    if(!ok){DeleteFileA(file);return fail(error,size,"cannot save config");}
#else
    int fd=open(file,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd<0) return fail(error,size,"cannot create new config (existing file is protected)");
    struct stat st;
    if ((mkdir(work,0700)<0 && errno!=EEXIST) || lstat(work,&st)<0 || !S_ISDIR(st.st_mode)) {
        close(fd); unlink(file); return fail(error,size,"cannot initialize workspace");
    }
    ssize_t n=write(fd,sample,sizeof(sample)-1);
    int ok=n==(ssize_t)(sizeof(sample)-1) && fsync(fd)==0;
    if(close(fd)!=0 || !ok) {
        unlink(file); return fail(error,size,"cannot save config");
    }
#endif
    return 1;
}

static const char *string_field(struct json_object *o,const char *name,const char *fallback) {
    struct json_object *v;
    if (!json_object_object_get_ex(o,name,&v)) return fallback;
    if(!json_object_is_type(v,json_type_string)) return NULL;
    const char *s=json_object_get_string(v);
    if(!*s || strlen(s)!=(size_t)json_object_get_string_len(v)) return NULL;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++) if(*p<32 || *p==127) return NULL;
    return s;
}
static const char *prompt_field(struct json_object *o,const char *name) {
    struct json_object *v=NULL;
    if(!json_object_object_get_ex(o,name,&v))return "";
    if(!json_object_is_type(v,json_type_string))return NULL;
    const char *p=json_object_get_string(v);size_t n=(size_t)json_object_get_string_len(v);
    if(!ah_utf8_text(p,n)||n>65536)return NULL;
    for(size_t i=0;i<n;i++)if(((unsigned char)p[i]<32&&p[i]!='\n'&&p[i]!='\r'&&p[i]!='\t')||(unsigned char)p[i]==127)return NULL;
    return p;
}
static int inside(const char *dir,const char *path) {
    size_t len=strlen(dir);
#ifdef _WIN32
    return !_strnicmp(dir,path,len)&&(dir[len-1]=='/'||dir[len-1]=='\\'||!path[len]||path[len]=='/'||path[len]=='\\');
#else
    return !strncmp(dir,path,len) && (dir[len-1]=='/' || !path[len] || path[len]=='/');
#endif
}
/* Resolve an as-yet-uncreated final session directory using its real parent. */
static int storage_path(const char *file,const char *name,char out[PATH_MAX]) {
#ifdef _WIN32
    char full[PATH_MAX],parent[PATH_MAX];
    if(!beside(file,name,full))return 0;
    DWORD n=GetFullPathNameA(full,PATH_MAX,out,NULL);if(!n||n>=PATH_MAX)return 0;
    DWORD attr=GetFileAttributesA(out);
    if(attr!=INVALID_FILE_ATTRIBUTES)return (attr&FILE_ATTRIBUTE_DIRECTORY)&&!(attr&FILE_ATTRIBUTE_REPARSE_POINT);
    if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)return 0;
    snprintf(parent,sizeof parent,"%s",out);char *slash=strrchr(parent,'\\');
    if(!slash||slash==parent||!slash[1]||!strcmp(slash+1,".")||!strcmp(slash+1,".."))return 0;
    *slash=0;attr=GetFileAttributesA(parent);
    return attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY)&&!(attr&FILE_ATTRIBUTE_REPARSE_POINT);
#else
    char full[PATH_MAX],parent[PATH_MAX],base[PATH_MAX];
    if(!beside(file,name,full)) return 0;
    struct stat st;
    if(lstat(full,&st)==0) return S_ISDIR(st.st_mode) && realpath(full,out);
    if(errno!=ENOENT) return 0;
    char *slash=strrchr(full,'/');
    snprintf(base,sizeof(base),"%s",slash?slash+1:full);
    if(!*base || !strcmp(base,".") || !strcmp(base,"..")) return 0;
    if(slash) *slash=0;
    if(!realpath(!slash?".":*full?full:"/",parent)) return 0;
    return snprintf(out,PATH_MAX,"%s/%s",parent,base)<PATH_MAX;
#endif
}

int dunlin_config_read(dunlin_config *c,const char *path,char *error,size_t size) {
    char buf[16385],file[PATH_MAX],work[PATH_MAX];
#ifdef _WIN32
    if(!path)return fail(error,size,"config unavailable");
    DWORD full=GetFullPathNameA(path,PATH_MAX,file,NULL);if(!full||full>=PATH_MAX)return fail(error,size,"config unavailable");
    HANDLE fd=CreateFileA(file,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(fd==INVALID_HANDLE_VALUE)return fail(error,size,"cannot read config");
    FILE_ATTRIBUTE_TAG_INFO info={0};LARGE_INTEGER length={0};
    int valid=GetFileInformationByHandleEx(fd,FileAttributeTagInfo,&info,sizeof info)&&
        !(info.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
        GetFileSizeEx(fd,&length)&&length.QuadPart>=0&&length.QuadPart<(LONGLONG)sizeof buf;
    DWORD got=0;int ok=valid&&ReadFile(fd,buf,(DWORD)length.QuadPart,&got,NULL)&&got==(DWORD)length.QuadPart;
    CloseHandle(fd);if(!ok)return fail(error,size,"config must be a bounded regular file");
    size_t n=got;
#else
    if(!path || !realpath(path,file)) return fail(error,size,"config unavailable");
    int fd=open(file,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    struct stat st;
    if(fd<0) return fail(error,size,"cannot read config");
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>= (off_t)sizeof(buf)) {
        close(fd); return fail(error,size,"config must be a bounded regular file");
    }
    FILE *f=fdopen(fd,"rb");
    if(!f) {close(fd); return fail(error,size,"cannot read config");}
    size_t n=fread(buf,1,sizeof(buf),f);
    int bad=ferror(f); fclose(f);
    if(bad || n==sizeof(buf)) return fail(error,size,"unreadable or oversized config");
#endif
    struct json_object *o=c->document=dunlin_json_parse(buf,n);
    if(!o || !json_object_is_type(o,json_type_object)) return fail(error,size,"invalid config JSON");
    static const char *const fields[]={"base_url","model","omlx_chat_template","max_tokens","temperature","enable_thinking","chat_tool_metadata","protocol","reasoning_effort","reasoning_capabilities","show_reasoning","models","active_model","system_prompt","tool_result_prompt","tool_result_for","key_env","workspace","session_dir","stream","timeout_ms","plugins","enabled_tools",
        "max_rounds","max_messages","max_calls","max_context_bytes","max_message_bytes","max_output_bytes",
        "max_response_bytes","tool_timeout_ms","task_timeout_ms",NULL};
    json_object_object_foreach(o,key,unused) {
        (void)unused;
        size_t i=0; while(fields[i] && strcmp(fields[i],key)) ++i;
        if(!fields[i]) return fail(error,size,"unrecognized config field");
    }
    c->limits=ah_default_limits();
    c->system_prompt=prompt_field(o,"system_prompt");
    c->tool_result_prompt=string_field(o,"tool_result_prompt","");
    c->tool_result_for=string_field(o,"tool_result_for","");
    if(!c->tool_result_prompt || !c->tool_result_for || (*c->tool_result_for && !*c->tool_result_prompt))
        return fail(error,size,"invalid tool result prompt or filter");
    if(!c->system_prompt) return fail(error,size,"invalid system_prompt");
    const char *w=string_field(o,"workspace","work"), *s=string_field(o,"session_dir","sessions");
    if(!w || !s) return fail(error,size,"invalid path setting");
#ifdef _WIN32
    if(!beside(file,w,work))return fail(error,size,"workspace or session directory parent unavailable");
    DWORD work_n=GetFullPathNameA(work,PATH_MAX,c->workspace,NULL),work_attr=work_n&&work_n<PATH_MAX?GetFileAttributesA(c->workspace):INVALID_FILE_ATTRIBUTES;
    if(!work_n||work_n>=PATH_MAX||work_attr==INVALID_FILE_ATTRIBUTES||!(work_attr&FILE_ATTRIBUTE_DIRECTORY)||
       (work_attr&FILE_ATTRIBUTE_REPARSE_POINT)||!storage_path(file,s,c->sessions))
        return fail(error,size,"workspace or session directory parent unavailable");
#else
    if(!beside(file,w,work) || !realpath(work,c->workspace) || !storage_path(file,s,c->sessions))
        return fail(error,size,"workspace or session directory parent unavailable");
#endif
    if(inside(c->workspace,file) || inside(c->sessions,file) || inside(c->workspace,c->sessions) || inside(c->sessions,c->workspace))
        return fail(error,size,"workspace must be separate from config and sessions");
    c->base.model.timeout_ms=30000; c->base.model.max_response_bytes=1024*1024;
    struct json_object *v;
    c->base.model.stream=1;
    if(json_object_object_get_ex(o,"stream",&v)) {
        if(!json_object_is_type(v,json_type_boolean)) return fail(error,size,"stream must be boolean");
        c->base.model.stream=json_object_get_boolean(v);
    }
#define NUMBER(field,name,cap) do { if(json_object_object_get_ex(o,#name,&v)) { \
    int64_t k=json_object_get_int64(v); if(!json_object_is_type(v,json_type_int) || k<1 || k>(cap)) { \
    return fail(error,size,"invalid " #name); } field.name=k; } } while(0)
    NUMBER(c->base.model,timeout_ms,3600000); NUMBER(c->base.model,max_response_bytes,16*1024*1024);
    NUMBER(c->limits,max_rounds,256); NUMBER(c->limits,max_messages,4096);
    NUMBER(c->limits,max_calls,256); NUMBER(c->limits,max_context_bytes,4*1024*1024);
    NUMBER(c->limits,max_message_bytes,1024*1024); NUMBER(c->limits,max_output_bytes,1024*1024);
    NUMBER(c->limits,task_timeout_ms,3600000); NUMBER(c->limits,tool_timeout_ms,3600000);
#undef NUMBER
    if(json_object_object_get_ex(o,"plugins",&c->plugins) &&
       (!json_object_is_type(c->plugins,json_type_array) || json_object_array_length(c->plugins)>6))
        return fail(error,size,"invalid plugin list");
    if(json_object_object_get_ex(o,"enabled_tools",&c->enabled_tools) &&
       (!json_object_is_type(c->enabled_tools,json_type_array) || json_object_array_length(c->enabled_tools)>7))
        return fail(error,size,"invalid enabled_tools list");
    c->show_reasoning=1;
    if(json_object_object_get_ex(o,"show_reasoning",&v)) {
        if(!json_object_is_type(v,json_type_boolean)) return fail(error,size,"show_reasoning must be boolean");
        c->show_reasoning=json_object_get_boolean(v);
    }
    return dunlin_profiles_read(c,error,size);
}
