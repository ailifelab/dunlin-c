#include "../../dunlin-copilot/esp32/agent/agent.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int mode,calls,clocks,effects;static ae_agent_memory memory;
static void reply(char *out,size_t cap,const char *text){snprintf(out,cap,"{\"choices\":[{\"message\":{\"content\":\"%s\"}}]}",text);}
static void tool(char *out,size_t cap,const char *name,const char *args){snprintf(out,cap,"{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call%d\",\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}]}}]}",calls,name,args);}
static int exchange(void *u,const char *req,char *out,size_t cap){
 (void)u;calls++;
 if(mode==0){if(calls==1){char text[1800];memset(text,'a',sizeof text);memcpy(text,"START",5);strcpy(text+1700,"TAIL-47");reply(out,cap,text);}else{assert(strstr(req,"START") && strstr(req,"TAIL-47"));reply(out,cap,"continued");}}
 else if(mode==1){assert(strstr(req,"再计算"));reply(out,cap,"5");}
 else if(mode==2){if(calls==1)tool(out,cap,"led_control","{\\\"mode\\\":\\\"off\\\"}");else if(calls==2)tool(out,cap,"current_time","{}");else reply(out,cap,"all done");}
 else {if(calls==1)tool(out,cap,"current_time","{}");else {assert(strstr(req,"2026-11-08") && strstr(req,"tool_call_id"));reply(out,cap,"2026-11-08; 2+3=5");}}
 return 1;
}
static int clock_read(void *u,char *out,size_t cap){(void)u;clocks++;snprintf(out,cap,"2026-11-08 星期日");return 1;}
static int led(void *u,const ae_led_action *a,char *out,size_t cap){(void)u;(void)a;effects++;snprintf(out,cap,"LED unavailable");return 0;}
static void print(void *u,const char *s){(void)u;(void)s;}
int main(int argc,char **argv){assert(argc==2);mode=atoi(argv[1]);ae_agent a={NULL,"{}",1,exchange,led,print,NULL,NULL,&memory,clock_read};
 if(mode==0){assert(ae_agent_turn(&a,"fixture","long reply")==AE_AGENT_REPLIED);assert(ae_agent_turn(&a,"fixture","continue")==AE_AGENT_REPLIED);assert(strlen(memory.turns[0].assistant)<1024);}
 else if(mode==1){assert(ae_agent_turn(&a,"fixture","今天日期是什么？再计算2+3。")==AE_AGENT_REPLIED);assert(calls==1);}
 else if(mode==2){assert(ae_agent_turn(&a,"fixture","turn off LED and read clock")==AE_AGENT_STOPPED);assert(effects==1 && clocks==1);}
 else {assert(ae_agent_turn(&a,"fixture","get current time and calculate 2+3")==AE_AGENT_OBSERVED);assert(calls==2 && clocks==1);}
 printf("PASS audit case %d\n",mode);
}
