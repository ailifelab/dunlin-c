#include "../../dunlin-copilot/esp32/agent/agent.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned requests,effects;static int scenario;
static int exchange(void *u,const char *request,char *response,size_t cap){
 (void)u;requests++;cJSON *j=cJSON_Parse(request);assert(j);cJSON *tools=cJSON_GetObjectItemCaseSensitive(j,"tools");assert(cJSON_GetArraySize(tools)==3);cJSON_Delete(j);
 if(requests>1 && scenario!=3 && !(scenario==4 && requests==2)){snprintf(response,cap,"{\"choices\":[{\"message\":{\"content\":\"%s42\"}}]}",scenario==2?"fake completed ":"");return 1;}
 const char *args=scenario==1?"{\"code\":\"print(6*7)\",\"extra\":1}":scenario==3?"{\"code\":\"print(1)\",\"code\":\"print(2)\"}":"{\"code\":\"print(6*7)\"}";
 cJSON *root=cJSON_CreateObject(),*choices=cJSON_AddArrayToObject(root,"choices"),*choice=cJSON_CreateObject();cJSON_AddItemToArray(choices,choice);cJSON *m=cJSON_AddObjectToObject(choice,"message"),*calls=cJSON_AddArrayToObject(m,"tool_calls"),*call=cJSON_CreateObject();cJSON_AddItemToArray(calls,call);cJSON_AddStringToObject(call,"id","python-id");cJSON *fn=cJSON_AddObjectToObject(call,"function");cJSON_AddStringToObject(fn,"name","python_exec");cJSON_AddStringToObject(fn,"arguments",args);char *s=cJSON_PrintUnformatted(root);snprintf(response,cap,"%s",s);cJSON_free(s);cJSON_Delete(root);return 1;
}
static int execute(void *u,const char *code,char *out,size_t cap){(void)u;assert(!strcmp(code,"print(6*7)"));effects++;snprintf(out,cap,"%s",scenario==2 || (scenario==4 && requests==1)?"RuntimeError: expected":"42");return scenario!=2 && !(scenario==4 && requests==1);}
static int led(void *u,const ae_led_action *a,char *out,size_t cap){(void)u;(void)a;(void)out;(void)cap;assert(0);return 0;}
static void print(void *u,const char *s){(void)u;(void)s;}
int main(void){
 for(scenario=0;scenario<5;scenario++){
  requests=effects=0;ae_agent a={NULL,"{\"active_model\":\"actual-fixture\",\"python\":true}",1,exchange,led,print,execute,NULL,NULL,NULL};int status=ae_agent_turn(&a,"fixture","compute using Python");
  if(scenario==0 || scenario==4)assert(!status && effects==(scenario==4?2u:1u));else assert(status);
  if(scenario==1 || scenario==3)assert(!effects);
  assert(requests<=5);
 }
 puts("PASS Python tool schema/callback gating, exact code, invalid/duplicate arguments denied, failed execution cannot be completed by prose, bounded corrected execution");
}
