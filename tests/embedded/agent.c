#include "../../dunlin-copilot/esp32/agent/agent.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct {unsigned requests,effects,lines;int mode,hidden;char last[1024];} state;
static const char device[]="{\"device\":\"ESP32-S3\",\"os\":\"FreeRTOS\",\"shell\":false,\"led\":true}";
static int exchange(void *unused,const char *request,char *out,size_t cap) {
 (void)unused;state.requests++;
 cJSON *j=cJSON_Parse(request);assert(j);
 const cJSON *messages=cJSON_GetObjectItemCaseSensitive(j,"messages");
 if(state.requests==1)assert(cJSON_GetArraySize(messages)==(state.hidden?2:3));
 cJSON_Delete(j);
 if(state.mode==4){memset(out,'{',cap-1);out[cap-1]=0;return 1;}
 if(state.mode==5)return 0;
 const char *name=state.mode==1?"shell":state.mode==2?"led_control":"device_info";
 const char *args=state.mode==1?"{\"command\":\"apt install nodejs\"}":state.mode==2?"{\"mode\":\"solid\",\"red\":256,\"green\":0,\"blue\":0,\"period_ms\":200}":"{}";
 if(state.mode==7 || state.mode==8 || state.mode==9){name="led_control";args=(state.mode==8 || (state.mode==9 && state.requests==1))?"{\"mode\":\"solid\",\"red\":256}":"{\"mode\":\"off\"}";}
 if(state.mode==3 || state.mode==6){name="led_control";args=state.mode==6?"{\"mode\":\"off\",\"mode\":\"solid\",\"red\":1,\"green\":0,\"blue\":0,\"period_ms\":200}":"{\"mode\":\"solid\",\"red\":255,\"green\":0,\"blue\":0,\"period_ms\":500}";}
 if(((state.mode==0 || state.mode==3 || state.mode==7 || state.mode==8) && state.requests>=2) || (state.mode==9 && state.requests==3)){snprintf(out,cap,"{\"choices\":[{\"message\":{\"content\":\"observed\"}}]}");return 1;}
 cJSON *root=cJSON_CreateObject(),*choices=cJSON_AddArrayToObject(root,"choices"),*choice=cJSON_CreateObject();cJSON_AddItemToArray(choices,choice);
 cJSON *m=cJSON_AddObjectToObject(choice,"message"),*calls=cJSON_AddArrayToObject(m,"tool_calls"),*call=cJSON_CreateObject();cJSON_AddItemToArray(calls,call);
 cJSON_AddStringToObject(call,"id","id-test");cJSON_AddStringToObject(call,"type","function");cJSON *fn=cJSON_AddObjectToObject(call,"function");
 cJSON_AddStringToObject(fn,"name",name);cJSON_AddStringToObject(fn,"arguments",args);
 char *raw=cJSON_PrintUnformatted(root);assert(strlen(raw)<cap);snprintf(out,cap,"%s",raw);cJSON_free(raw);cJSON_Delete(root);return 1;
}
static int led(void *unused,const ae_led_action *action,char *out,size_t cap) {
 (void)unused;assert((!strcmp(action->mode,"solid") && action->red==255 && action->period_ms==500) || !strcmp(action->mode,"off"));
 state.effects++;snprintf(out,cap,"led applied red=255 green=0 blue=0 mode=solid");return 1;
}
static void print(void *unused,const char *s){(void)unused;state.lines++;snprintf(state.last,sizeof state.last,"%s",s);}
int main(void) {
 for(int hidden=0;hidden<2;hidden++)for(int mode=0;mode<10;mode++) {
  memset(&state,0,sizeof state);state.mode=mode;state.hidden=hidden;
  ae_agent a={NULL,device,!hidden,exchange,led,print,NULL,NULL,NULL,NULL};int status=ae_agent_turn(&a,"fixture","安装Linux软件或设置灯色");
  assert(state.requests<=5 && state.lines<20);
  if(mode==0 || mode==3 || mode==7 || mode==9)assert(status==0 && state.requests==(mode==9?3u:2u) && !strcmp(state.last,"observed"));
  else assert(status!=0);
  assert(state.effects==((mode==3 || mode==7 || mode==9)?1u:0u));
  if(mode==1 || mode==2 || mode==6)assert(state.requests==2 && strstr(state.last,"ESP32-S3") && strstr(state.last,"FreeRTOS"));
 }
 puts("PASS embedded agent identity/hidden identity, actual LED dispatch, unknown Linux tools never execute, invalid/duplicate arguments, minimal off arguments, failed LED then prose cannot complete, corrected LED required, bounded retries, malformed/deep/failed network responses");
}
