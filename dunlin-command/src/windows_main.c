#define WIN32_LEAN_AND_MEAN
#include "dunlin/compact/runtime.h"
#include "console.h"
#include <json-c/json.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void event(void *unused,const char *type,const char *data,size_t size) {
    (void)unused;
    if(!strcmp(type,"model.reasoning"))fprintf(stderr,"[thinking] %.*s",(int)size,data);
}
static int approve(void *unused,const char *name,const char *arguments) {
    (void)unused;
    fprintf(stderr,"Approve %s %s? [y/N] ",name,arguments);fflush(stderr);
    char answer[8];
    return fgets(answer,sizeof answer,stdin)&&(*answer=='y'||*answer=='Y');
}
static void usage(void) {
    fprintf(stderr,"usage: dunlin CONFIG [--permissions request|agent|full-access] "
        "validate|models|run SESSION TEXT|resume SESSION|chat SESSION\n");
}
static int utf8_main(int argc,char **argv) {
    SetConsoleOutputCP(CP_UTF8);SetConsoleCP(CP_UTF8);
    if(argc<3){usage();return 2;}
    int command=2;
    dunlin_permission_mode mode=DUNLIN_PERMISSION_REQUEST;
    if(command<argc&&!strcmp(argv[command],"--permissions")) {
        if(command+1>=argc||!dunlin_permission_mode_parse(argv[command+1],&mode)){usage();return 2;}
        command+=2;
    }
    if(command>=argc){usage();return 2;}
    dunlin_console_reset();
    if(!dunlin_console_install()){fprintf(stderr,"cannot install console handler\n");return 1;}
    char error[256];
    ah_hooks hooks={.cancelled=dunlin_console_cancelled,.approve=approve,.event=event};
    dunlin_runtime *runtime=dunlin_open(argv[1],&hooks,error,sizeof error);
    if(!runtime){fprintf(stderr,"%s\n",error);return 1;}
    if(!dunlin_set_permission_mode(runtime,mode)){fprintf(stderr,"cannot set permission mode\n");dunlin_close(runtime);return 1;}
    const char *action=argv[command];
    int remaining=argc-command,code=0;
    if(!strcmp(action,"validate")&&remaining==1) {
        puts("configuration valid");
    } else if(!strcmp(action,"models")&&remaining==1) {
        struct json_object *models=dunlin_models(runtime);
        puts(json_object_to_json_string_ext(models,JSON_C_TO_STRING_PRETTY));json_object_put(models);
    } else if(!strcmp(action,"run")&&remaining>=3) {
        size_t total=0;for(int i=command+2;i<argc;i++)total+=strlen(argv[i])+1;
        char *input=malloc(total+1);
        if(!input)code=1;
        else {
            char *cursor=input;
            for(int i=command+2;i<argc;i++) {
                if(i>command+2)*cursor++=' ';
                size_t length=strlen(argv[i]);memcpy(cursor,argv[i],length);cursor+=length;
            }
            *cursor=0;
            ah_result result=dunlin_turn_for_model(runtime,&hooks,argv[command+1],input,NULL,NULL);
            if(result.text)puts(result.text);code=result.status==AH_DONE?0:1;
            free(result.text);free(input);
        }
    } else if(!strcmp(action,"resume")&&remaining==2) {
        ah_result result=dunlin_turn_for_model(runtime,&hooks,argv[command+1],NULL,NULL,NULL);
        if(result.text)puts(result.text);code=result.status==AH_DONE?0:1;free(result.text);
    } else if(!strcmp(action,"chat")&&remaining==2) {
        char line[4096];
        while(!dunlin_console_cancelled(NULL)) {
            fputs("> ",stderr);fflush(stderr);
            if(!fgets(line,sizeof line,stdin))break;
            line[strcspn(line,"\r\n")]=0;if(!*line)continue;
            ah_result result=dunlin_turn_for_model(runtime,&hooks,argv[command+1],line,NULL,NULL);
            if(result.text)puts(result.text);free(result.text);
            if(result.status!=AH_DONE){code=1;break;}
        }
    } else {usage();code=2;}
    if(dunlin_close(runtime))code=1;
    return code;
}

int wmain(int argc,wchar_t **wide_argv) {
    char **argv=calloc((size_t)argc+1,sizeof *argv);if(!argv)return 1;
    for(int i=0;i<argc;i++) {
        int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide_argv[i],-1,NULL,0,NULL,NULL);
        if(n<=0||(argv[i]=malloc((size_t)n))==NULL||
           !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide_argv[i],-1,argv[i],n,NULL,NULL)) {
            for(int j=0;j<=i;j++)free(argv[j]);free(argv);
            fprintf(stderr,"invalid Windows command line\n");return 2;
        }
    }
    int code=utf8_main(argc,argv);
    for(int i=0;i<argc;i++)free(argv[i]);free(argv);return code;
}
