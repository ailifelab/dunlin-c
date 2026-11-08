#define WIN32_LEAN_AND_MEAN
#include "../dunlin-compact/platform/windows/process.h"
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int cancel_now(void *unused){(void)unused;return 1;}

int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"--echo")){for(int i=2;i<argc;i++)printf("[%s]\n",argv[i]);return 0;}
    if(argc>1&&!strcmp(argv[1],"--exit")){fputs("nonzero",stderr);return 23;}
    if(argc>1&&!strcmp(argv[1],"--sleep")){Sleep(5000);return 0;}
    if(argc>1&&!strcmp(argv[1],"--output")){for(int i=0;i<1024;i++)putchar('x');return 0;}
    char exe[MAX_PATH],workspace[MAX_PATH];assert(GetModuleFileNameA(NULL,exe,MAX_PATH));
    assert(GetCurrentDirectoryA(MAX_PATH,workspace));dwp_result result;
    const char *literal[]={"--echo","plain","space value","quote\"slash\\",""};
    assert(dwp_run(exe,literal,5,workspace,5000,4096,&result)==DWP_OK);
    assert(!strcmp(result.output,"[plain]\r\n[space value]\r\n[quote\"slash\\]\r\n[]\r\n"));dwp_result_free(&result);
    const char *nonzero[]={"--exit"};assert(dwp_run(exe,nonzero,1,workspace,5000,64,&result)==DWP_EXITED);
    assert(result.exit_code==23&&!strcmp(result.output,"nonzero"));dwp_result_free(&result);
    const char *sleeping[]={"--sleep"};assert(dwp_run(exe,sleeping,1,workspace,50,64,&result)==DWP_TIMEOUT);dwp_result_free(&result);
    assert(dwp_run_ex(exe,sleeping,1,workspace,5000,64,cancel_now,NULL,&result)==DWP_CANCELLED);dwp_result_free(&result);
    const char *large[]={"--output"};assert(dwp_run(exe,large,1,workspace,5000,32,&result)==DWP_OUTPUT_LIMIT);dwp_result_free(&result);
    assert(dwp_run("relative.exe",NULL,0,workspace,100,32,&result)==DWP_FAILED);
    char alias[MAX_PATH];snprintf(alias,sizeof alias,"%s\\process-alias.exe",workspace);
    assert(CreateSymbolicLinkA(alias,exe,0));
    assert(dwp_run(alias,NULL,0,workspace,100,32,&result)==DWP_FAILED);assert(DeleteFileA(alias));
    puts("Windows process tests passed");return 0;
}
