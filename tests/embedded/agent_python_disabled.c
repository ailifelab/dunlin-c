#include "../../dunlin-copilot/esp32/agent/agent.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls;
static int exchange(void *u,const char *request,char *out,size_t cap){
 (void)u;calls++;cJSON *j=cJSON_Parse(request);assert(j);assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j,"tools"))==2);assert(cJSON_GetObjectItemCaseSensitive(j,"max_tokens")->valueint==1024);cJSON *messages=cJSON_GetObjectItemCaseSensitive(j,"messages"); cJSON *m=NULL; cJSON_ArrayForEach(m,messages){cJSON *role=cJSON_GetObjectItemCaseSensitive(m,"role"); if(cJSON_IsString(role) && !strcmp(role->valuestring,"system")){cJSON *content=cJSON_GetObjectItemCaseSensitive(m,"content"); assert(cJSON_IsString(content) && !strstr(content->valuestring,"python_exec"));}}cJSON_Delete(j);
 snprintf(out,cap,"{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"blocked\",\"function\":{\"name\":\"python_exec\",\"arguments\":\"{\\\"code\\\":\\\"print(1)\\\"}\"}}]}}]}");return 1;
}
static int python(void *u,const char *code,char *out,size_t cap){(void)u;(void)code;(void)out;(void)cap;assert(0);return 0;}
static int led(void *u,const ae_led_action *a,char *out,size_t cap){(void)u;(void)a;(void)out;(void)cap;assert(0);return 0;}
static void print(void *u,const char *s){(void)u;(void)s;}
int main(void){ae_agent a={NULL,"{\"device\":\"fixture\"}",1,exchange,led,print,python,NULL,NULL,NULL};assert(ae_agent_turn(&a,"fixture","compute")!=0 && calls==2);puts("PASS Python-disabled agent keeps two tools, 1024 tokens, no Python prompt/schema/callback execution even with a nonnull optional callback");}
