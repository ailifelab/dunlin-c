#include "../../dunlin-copilot/esp32/agent/agent.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
typedef union {max_align_t align;size_t bytes;} header;
static size_t attempts,fail_at,live;
static unsigned calls,effects;
static void *allocate(size_t n){if(++attempts==fail_at)return NULL;header *p=malloc(sizeof *p+n);if(!p)return NULL;live++;return p+1;}
static void release(void *p){if(!p)return;assert(live);live--;free((header *)p-1);}
static int exchange(void *unused,const char *request,char *response,size_t capacity){
 (void)unused;assert(strstr(request,"device_info") && strstr(request,"\"model\":\"fixture\""));calls++;
 const char *s=calls==1?"{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"once\",\"function\":{\"name\":\"led_control\",\"arguments\":\"{\\\"mode\\\":\\\"off\\\"}\"}}]}}]}":"{\"choices\":[{\"message\":{\"content\":\"observed\"}}]}";
 assert(strlen(s)<capacity);snprintf(response,capacity,"%s",s);return 1;
}
static int led(void *unused,const ae_led_action *action,char *out,size_t cap){(void)unused;assert(!strcmp(action->mode,"off"));effects++;snprintf(out,cap,"applied off");return 1;}
static void print(void *unused,const char *s){(void)unused;(void)s;}
int main(void){
 cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);
 ae_agent agent={NULL,"{\"device\":\"ESP32-S3\"}",1,exchange,led,print,NULL,NULL,NULL,NULL};
 assert(!ae_agent_turn(&agent,"fixture","关闭灯"));assert(!live && calls==2 && effects==1);
 size_t allocation_points=attempts;
 for(size_t index=1;index<=allocation_points;index++){
  attempts=0;fail_at=index;live=0;calls=effects=0;
  if(getenv("AE_FAULT_TRACE"))fprintf(stderr,"allocation_index=%zu\n",index);
  ae_agent a={NULL,"{\"device\":\"ESP32-S3\"}",1,exchange,led,print,NULL,NULL,NULL,NULL};
  (void)ae_agent_turn(&a,"fixture","关闭灯");
  if(live){fprintf(stderr,"allocation failure index=%zu left %zu allocations\n",index,live);return 1;}
  assert(calls<=5 && effects<=1);
 }
 cJSON_InitHooks(NULL);printf("PASS %zu observed successful-path cJSON allocation positions injected, custom allocator ownership, bounded dispatch and complete cleanup\n",allocation_points);
}
