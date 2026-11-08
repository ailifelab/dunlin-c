#define WIN32_LEAN_AND_MEAN
#include "../dunlin-compact/runtime/internal.h"
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char temp[MAX_PATH],root[MAX_PATH],config[MAX_PATH],alias[MAX_PATH],work[MAX_PATH];
    assert(GetTempPathA(MAX_PATH,temp));snprintf(root,sizeof root,"%sdunlin-config-%lu",temp,(unsigned long)GetCurrentProcessId());
    assert(CreateDirectoryA(root,NULL));snprintf(config,sizeof config,"%s\\config.json",root);
    char error[192];assert(dunlin_init_config(config,error,sizeof error));
    dunlin_config parsed={0};assert(dunlin_config_read(&parsed,config,error,sizeof error));
    assert(!strcmp(parsed.active.model.model,"local-model")&&parsed.active.protocol==DUNLIN_PROTOCOL_CHAT);
    assert(strstr(parsed.workspace,"\\work")&&strstr(parsed.sessions,"\\sessions"));json_object_put(parsed.document);
    snprintf(alias,sizeof alias,"%s\\alias.json",root);assert(CreateSymbolicLinkA(alias,config,0));
    memset(&parsed,0,sizeof parsed);assert(!dunlin_config_read(&parsed,alias,error,sizeof error));assert(DeleteFileA(alias));
    assert(!dunlin_init_config(config,error,sizeof error));
    assert(DeleteFileA(config));snprintf(work,sizeof work,"%s\\work",root);assert(RemoveDirectoryA(work));assert(RemoveDirectoryA(root));
    puts("Windows file-backed config contracts passed");return 0;
}
