#define WIN32_LEAN_AND_MEAN
#include "dunlin/compact/runtime.h"
#include "process.h"
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void remove_files(const char *dir) {
    char pattern[MAX_PATH],path[MAX_PATH];snprintf(pattern,sizeof pattern,"%s\\*",dir);
    WIN32_FIND_DATAA item;HANDLE find=FindFirstFileA(pattern,&item);
    if(find!=INVALID_HANDLE_VALUE){do{if(strcmp(item.cFileName,".")&&strcmp(item.cFileName,"..")){
        snprintf(path,sizeof path,"%s\\%s",dir,item.cFileName);
        assert(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&DeleteFileA(path));
    }}while(FindNextFileA(find,&item));FindClose(find);}assert(RemoveDirectoryA(dir));
}
static void expect_valid(const char *exe,const char *directory,const char *config,const char *mode) {
    const char *plain[]={config,"validate"};
    const char *selected[]={config,"--permissions",mode,"validate"};
    const char *const *args=mode?selected:plain;size_t count=mode?4:2;
    dwp_result out={0};int status=dwp_run(exe,args,count,directory,5000,4096,&out);
    if(status!=DWP_OK||out.exit_code||!out.output||!strstr(out.output,"configuration valid"))
        fprintf(stderr,"mode=%s status=%d exit=%lu output=%s\n",mode?mode:"default",status,out.exit_code,out.output?out.output:"(null)");
    assert(status==DWP_OK&&!out.exit_code&&out.output&&strstr(out.output,"configuration valid"));dwp_result_free(&out);
}
int main(int argc,char **argv) {
    assert(argc==2);char temp[MAX_PATH],root[MAX_PATH],config[MAX_PATH],error[192],exe[MAX_PATH],exe_dir[MAX_PATH];
    assert(GetTempPathA(MAX_PATH,temp));snprintf(root,sizeof root,"%sdunlin-cli-%lu",temp,(unsigned long)GetCurrentProcessId());assert(CreateDirectoryA(root,NULL));
    snprintf(config,sizeof config,"%s\\config.json",root);assert(dunlin_init_config(config,error,sizeof error));
    snprintf(exe,sizeof exe,"%s",argv[1]);for(char *p=exe;*p;p++)if(*p=='/')*p='\\';
    snprintf(exe_dir,sizeof exe_dir,"%s",exe);char *slash=strrchr(exe_dir,'\\');assert(slash);*slash=0;
    expect_valid(exe,exe_dir,config,NULL);expect_valid(exe,exe_dir,config,"request");
    expect_valid(exe,exe_dir,config,"agent");expect_valid(exe,exe_dir,config,"full-access");
    const char *utf8[]={config,"run","utf8-check","你好，验证中文参数"};dwp_result out={0};
    int status=dwp_run(exe,utf8,4,exe_dir,5000,4096,&out);
    assert(status==DWP_EXITED&&out.exit_code==1&&out.output&&!strstr(out.output,"invalid UTF-8 input"));dwp_result_free(&out);
    assert(DeleteFileA(config));char path[MAX_PATH];snprintf(path,sizeof path,"%s\\work",root);assert(RemoveDirectoryA(path));
    snprintf(path,sizeof path,"%s\\sessions",root);remove_files(path);assert(RemoveDirectoryA(root));
    puts("Windows CLI permission modes, validate and UTF-8 argv flows passed");return 0;
}
