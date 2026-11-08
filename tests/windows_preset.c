#define WIN32_LEAN_AND_MEAN
#include "dunlin/compact/runtime.h"
#include <json-c/json.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
int main(void){char temp[MAX_PATH],root[MAX_PATH],cfg[MAX_PATH],err[256];assert(GetTempPathA(MAX_PATH,temp));snprintf(root,sizeof root,"%sdunlin-preset-%lu",temp,(unsigned long)GetCurrentProcessId());assert(CreateDirectoryA(root,NULL));snprintf(cfg,sizeof cfg,"%s\\config.json",root);assert(dunlin_init_config(cfg,err,sizeof err));dunlin_runtime*r=dunlin_open(cfg,NULL,err,sizeof err);if(!r)fprintf(stderr,"%s\n",err);assert(r);assert(dunlin_workspace(r));struct json_object*p=dunlin_plugins(r);assert(p&&json_object_array_length(p)>=6);json_object_put(p);assert(!dunlin_close(r));assert(DeleteFileA(cfg));char path[MAX_PATH];snprintf(path,sizeof path,"%s\\work",root);assert(RemoveDirectoryA(path));snprintf(path,sizeof path,"%s\\sessions",root);RemoveDirectoryA(path);assert(RemoveDirectoryA(root));puts("Windows preset assembly passed");return 0;}
