#define _POSIX_C_SOURCE 200809L
#include "../desktop/file_history.h"
#include "../desktop/project_files.h"
#include "../desktop/grants.h"
#include "dunlin/compact/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct {dc_file_journal_v1 real;} wrapper;
static int prepared(void *user,const char *operation,const char *path,struct json_object *args,int existed,const char *before,const char *after,const char *device,const char *inode,char id[65]){
 wrapper *w=user;int ok=w->real.prepare(w->real.user,operation,path,args,existed,before,after,device,inode,id);if(!ok)return 0;printf("PREPARED %s\n",id);fflush(stdout);for(;;)pause();
}
static int settled(void *user,const char *id,const char *state,const char *detail){wrapper *w=user;return w->real.settle(w->real.user,id,state,detail);}
int main(int argc,char **argv){
 if(argc!=3)return 2;dc_store *store=dc_open(argv[1]);if(!store)return 3;
 const char *raw="{\"path\":\"crash.txt\",\"text\":\"partial 中文 body\",\"expected_sha256\":\"new\"}";
 /* Explicit authorization of one owned deterministic fixture, no model/service. */
 long long grant=dc_grant_begin(store,"crash","write_file",raw);if(!grant||!dc_grant_decide(store,grant,1,0))return 4;
 dc_file_history_run run={.store=store,.chat="crash",.run="owned-crash-run",.proposal=0,.grant=grant,.authorized_arguments=raw};snprintf(run.call,sizeof run.call,"owned-crash-call");wrapper w={dc_file_history_journal(&run)};dc_file_journal_v1 journal={&w,prepared,settled};
 ah_tool_service tools={0};if(!dc_project_tools_open(&tools,argv[2],"ask"))return 5;dc_project_tools_journal(&tools,&journal);struct json_object *args=dunlin_json_parse(raw,strlen(raw));char *out=NULL;tools.call(tools.ctx,"write_file",args,NULL,65536,1000,&out);free(out);json_object_put(args);dc_project_tools_close(&tools);dc_close(store);return 6;
}
