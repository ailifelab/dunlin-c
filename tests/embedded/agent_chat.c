#include "../../dunlin-copilot/esp32/agent/agent.h"
#include <assert.h>
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
static unsigned requests,effects;static int scenario;static char last[1024];
static int exchange(void *u,const char *req,char *out,size_t cap){
 (void)u;requests++;
 if(scenario==6){cJSON *j=cJSON_Parse(req);assert(j);cJSON *m=cJSON_GetObjectItemCaseSensitive(j,"messages");int n=cJSON_GetArraySize(m);assert(n==5);assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(m,n-3),"content")->valuestring,"previous question"));assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(m,n-1),"content")->valuestring,"你好"));cJSON_Delete(j);}
 const char *s=scenario==1?"{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":[]}}]}":scenario==3?"{\"choices\":[{\"message\":{\"content\":\" \\n\"}}]}":scenario==4?"{\"choices\":[{\"message\":{\"content\":\"hello\"}}]}":scenario==2?"{\"choices\":[{\"message\":{\"content\":\"你好，有什么可以帮你？\",\"tool_calls\":[]}}]}":"{\"choices\":[{\"message\":{\"content\":\"你好，有什么可以帮你？\"}}]}";
 if(scenario==5 && requests==1)s="{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"bad\",\"function\":{\"name\":\"led_control\",\"arguments\":\"{\\\"mode\\\":\\\"solid\\\",\\\"red\\\":256}\"}}]}}]}";
 snprintf(out,cap,"%s",s);return 1;
}
static int led(void *u,const ae_led_action *a,char *o,size_t c){(void)u;(void)a;(void)o;(void)c;effects++;assert(0);return 0;}
static void print(void *u,const char *s){(void)u;snprintf(last,sizeof last,"%s",s);}
int main(void){
 for(scenario=0;scenario<7;scenario++){
  requests=effects=0;last[0]=0;ae_agent a={NULL,"{\"device\":\"fixture\"}",1,exchange,led,print,NULL,scenario==6?"previous question":NULL,NULL,NULL};
  int result=ae_agent_turn(&a,"fixture","你好");
  if(scenario==1 || scenario==3 || scenario==5)assert(result==AE_AGENT_STOPPED && requests==2);
  else assert(result==AE_AGENT_REPLIED && requests==1 && (strstr(last,"你好") || !strcmp(last,"hello")));
  assert(!effects);
 }
 char large[385];memset(large,'x',sizeof large-1);large[sizeof large-1]=0;requests=0;ae_agent invalid={NULL,"{}",1,exchange,led,print,NULL,large,NULL,NULL};assert(ae_agent_turn(&invalid,"fixture","你好")==AE_AGENT_STOPPED && !requests);
 invalid.previous_prompt=NULL;assert(ae_agent_turn(&invalid,large,"你好")==AE_AGENT_STOPPED && !requests);
 puts("PASS bounded model/prior-user context; plain/English/empty-tool-array replies once without effects; empty/whitespace answers bounded; failed LED cannot escape with prose; replied is distinct from observed");
}
