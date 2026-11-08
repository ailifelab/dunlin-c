#define _POSIX_C_SOURCE 200809L
#include <dunlin/compact/session.h>
#include <json-c/json.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
int dunlin_session_test_push(struct json_object *array,struct json_object *value){(void)array;(void)value;return -1;}
int main(int argc,char **argv){
 char dir[]="/tmp/dunlin-list-oom-XXXXXX",file[128];assert(mkdtemp(dir));
 ah_session_service service;assert(!ah_linux_sessions(&service,dir));snprintf(file,sizeof file,"%s/listed.json",dir);FILE *f=fopen(file,"w");assert(f && fputs("{}",f)>=0 && !fclose(f));
 struct json_object *list=ah_session_list(&service);int probe=argc==2 && !strcmp(argv[1],"--probe");
 if(!probe)assert(!list);json_object_put(list);ah_linux_sessions_close(&service);assert(!unlink(file) && !rmdir(dir));puts("Session list attachment failure completed; LSan must find no lost item");return 0;
}
