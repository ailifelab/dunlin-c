#include "agent.h"
#include "json.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifndef AE_AGENT_ENABLE_PYTHON
#define AE_AGENT_ENABLE_PYTHON 1
#endif
#include "system_prompt.h"
#if AE_AGENT_ENABLE_PYTHON
#include "../python/system_prompt.h"
#endif
#define RESPONSE_MAX 8192u
#define REQUEST_MAX 16384u
#define ROUNDS 5u
/* Before cJSON recursion/allocation, bound nesting and reject unbalanced framing. */
static int framing(const char *s) {
 unsigned depth=0;int quoted=0,escaped=0;
 for(;*s;s++) {
  unsigned char c=(unsigned char)*s;
  if(quoted){if(escaped)escaped=0;else if(c=='\\')escaped=1;else if(c=='"')quoted=0;else if(c<32)return 0;continue;}
  if(c=='"')quoted=1;
  else if(c=='{' || c=='['){if(++depth>12)return 0;}
  else if(c=='}' || c==']'){if(!depth)return 0;depth--;}
 }
 return !depth && !quoted;
}
static int unique(const cJSON *j) {
 if(cJSON_IsObject(j))for(const cJSON *a=j->child;a;a=a->next)for(const cJSON *b=a->next;b;b=b->next)
  if(!strcmp(a->string,b->string))return 0;
 for(const cJSON *a=j->child;a;a=a->next)if(!unique(a))return 0;
 return 1;
}
cJSON *ae_json_parse_bounded(const char *s,size_t limit) {
 if(!s || strlen(s)>=limit || (!framing(s) || strstr(s,"\\u0000")))return NULL;
 cJSON *j=cJSON_ParseWithOpts(s,NULL,1);
 if(j && !unique(j)){cJSON_Delete(j);return NULL;}return j;
}
static const char *string(const cJSON *o,const char *name) {
 const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,name);return cJSON_IsString(v)?v->valuestring:NULL;
}
static cJSON *message(cJSON *messages,const char *role,const char *content) {
 if(!messages)return NULL;
 cJSON *m=cJSON_CreateObject();
 if(!m || !cJSON_AddStringToObject(m,"role",role) || !cJSON_AddStringToObject(m,"content",content) || !cJSON_AddItemToArray(messages,m)){cJSON_Delete(m);return NULL;}
 return m;
}
static int integer(cJSON *j,const char *key,unsigned min,unsigned max,unsigned *out) {
 cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
 if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<min || v->valuedouble>max || floor(v->valuedouble)!=v->valuedouble)return 0;
 *out=(unsigned)v->valuedouble;return 1;
}
static int arguments(cJSON *j,ae_led_action *a) {
 if(!cJSON_IsObject(j))return 0;
 for(cJSON *field=j->child;field;field=field->next)
  if(strcmp(field->string,"mode") && strcmp(field->string,"red") && strcmp(field->string,"green") && strcmp(field->string,"blue") && strcmp(field->string,"period_ms"))return 0;
 a->mode=string(j,"mode");
 if(!a->mode || (strcmp(a->mode,"off") && strcmp(a->mode,"on") && strcmp(a->mode,"solid") && strcmp(a->mode,"blink") && strcmp(a->mode,"breathe") && strcmp(a->mode,"rainbow")))return 0;
 a->red=a->green=a->blue=32;a->period_ms=1000;
 int toggle=!strcmp(a->mode,"off") || !strcmp(a->mode,"on");
 const char *keys[]={"red","green","blue","period_ms"};unsigned *values[]={&a->red,&a->green,&a->blue,&a->period_ms};
 for(unsigned i=0;i<4;i++) {
  cJSON *v=cJSON_GetObjectItemCaseSensitive(j,keys[i]);
  if(!v){if(!toggle && i<3 && strcmp(a->mode,"rainbow"))return 0;continue;}
  if(!integer(j,keys[i],i==3?200:0,i==3?5000:255,values[i]))return 0;
 }
 return 1;
}
static const char schema[]="[{\"type\":\"function\",\"function\":{\"name\":\"device_info\",\"description\":\"Read actual device capabilities and limits.\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}},{\"type\":\"function\",\"function\":{\"name\":\"led_control\",\"description\":\"Control onboard RGB. off/on need only mode; on restores saved state. Effects need RGB 0..255, optional period_ms defaults to 1000. May be unavailable.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"off\",\"on\",\"solid\",\"blink\",\"breathe\",\"rainbow\"]},\"red\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"green\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"blue\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"period_ms\":{\"type\":\"integer\",\"minimum\":200,\"maximum\":5000}},\"required\":[\"mode\"],\"additionalProperties\":false}}}]";
void ae_agent_memory_clear(ae_agent_memory *m){if(m)memset(m,0,sizeof *m);}
static void remember(const ae_agent *a,const char *prompt,const char *answer){
 ae_agent_memory *m=a->memory;if(!m)return;
 if(m->count>=AE_MEMORY_TURNS){memmove(m->turns,m->turns+1,(AE_MEMORY_TURNS-1)*sizeof m->turns[0]);m->count=AE_MEMORY_TURNS-1;}
 ae_memory_turn *t=&m->turns[m->count++];snprintf(t->user,sizeof t->user,"%s",prompt);
 size_t n=strlen(answer);if(n>=sizeof t->assistant){
  size_t head=480,tail=n-480;while(head && ((unsigned char)answer[head]&0xc0)==0x80)head--;
  while(tail<n && ((unsigned char)answer[tail]&0xc0)==0x80)tail++;
  memcpy(t->assistant,answer,head);strcpy(t->assistant+head," [history excerpt truncated] ");
  strcat(t->assistant,answer+tail);
 }
 else memcpy(t->assistant,answer,n+1);
}
static int date_question(const char *p){
 const char *questions[]={"今天几号","今天是几号","今天日期是什么，是星期几","今天日期是什么","今天的日期是什么","今天的日期","今天日期","今天是星期几","今天星期几","当前日期","现在日期","能查到日期吗","what day is today","today's date"};
 p+=strspn(p," \t\r\n");
 for(size_t i=0;i<sizeof questions/sizeof *questions;i++){
  size_t n=strlen(questions[i]);if(strncmp(p,questions[i],n))continue;
  const char *end=p+n;
  for(;;){if(*end && strchr(" ?!.\t\r\n",*end)){end++;continue;}
   if(!strncmp(end,"？",3) || !strncmp(end,"。",3) || !strncmp(end,"！",3)){end+=3;continue;}break;}
  if(!*end)return 1;
 }
 return 0;
}
int ae_agent_turn(const ae_agent *a,const char *model,const char *prompt) {
 if(!a || !a->exchange || !a->led || !a->print || !a->device_json || !model || strlen(model)>95 || !prompt || strlen(prompt)>383 || (a->previous_prompt && strlen(a->previous_prompt)>383))return 1;
 if(a->memory && a->memory->count>AE_MEMORY_TURNS)return AE_AGENT_STOPPED;
 if(a->current_time && date_question(prompt)){
  char value[512]={0};int ok=a->current_time(a->user,value,sizeof value);value[sizeof value-1]=0;
  if(!ok)snprintf(value,sizeof value,"无法获取当前日期：网络时间源不可用；不会使用模型猜测。请检查 /wifi status 后重试。");
  a->print(a->user,value);remember(a,prompt,value);return ok?AE_AGENT_OBSERVED:AE_AGENT_STOPPED;
 }
 char *response=calloc(1,RESPONSE_MAX);cJSON *root=cJSON_CreateObject();int result=1;
 if(!response || !root)goto oom;
 if(!cJSON_AddStringToObject(root,"model",model) || !cJSON_AddBoolToObject(root,"stream",0) || !cJSON_AddBoolToObject(root,"enable_thinking",0) ||
    !cJSON_AddNumberToObject(root,"temperature",0) || !cJSON_AddNumberToObject(root,"max_tokens",1024))goto oom;
 cJSON *messages=cJSON_AddArrayToObject(root,"messages");if(!messages)goto oom;
#if AE_AGENT_ENABLE_PYTHON
 if(a->python_exec && !message(messages,"system",python_system_prompt))goto oom;
#endif
 if(a->disclose_identity && !message(messages,"system",a->device_json))goto oom;
 if(!message(messages,"system",device_system_prompt))goto oom;
 if(a->memory){
  for(unsigned i=0;i<a->memory->count;i++)if(!message(messages,"user",a->memory->turns[i].user) || !message(messages,"assistant",a->memory->turns[i].assistant))goto oom;
 }else if(a->previous_prompt && *a->previous_prompt){
  if(!message(messages,"user",a->previous_prompt) || !message(messages,"assistant","Earlier turn ended; use current device state, never replay it."))goto oom;
 }
 if(!message(messages,"user",prompt))goto oom;
 cJSON *tools=ae_json_parse_bounded(schema,sizeof schema+1);if(!tools)goto oom;
#if AE_AGENT_ENABLE_PYTHON
 if(a->python_exec){
  const char *definition="{\"type\":\"function\",\"function\":{\"name\":\"python_exec\",\"description\":\"Execute bounded MicroPython code in a fresh VM per call; variables do not persist between calls. Modules json and dunlin; print results. dunlin.http_get(https_url) uses allowed_hosts; dunlin.info() is device JSON; dunlin.led(mode[,r,g,b,period_ms]) uses registered RGB driver. No shell, files, pip or arbitrary GPIO. 2047 code bytes, 2s Python time excluding bounded HTTPS, 30s job, bounded output.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\",\"maxLength\":2047}},\"required\":[\"code\"],\"additionalProperties\":false}}}";
  cJSON *extension=ae_json_parse_bounded(definition,1024);if(!extension || !cJSON_AddItemToArray(tools,extension)){cJSON_Delete(extension);cJSON_Delete(tools);goto oom;}
 }
#endif
 if(a->current_time){
  const char *definition="{\"type\":\"function\",\"function\":{\"name\":\"current_time\",\"description\":\"Fetch real current date/time and weekday in Asia/Shanghai from network time source. Never guess.\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}}";
  cJSON *extension=ae_json_parse_bounded(definition,512);if(!extension || !cJSON_AddItemToArray(tools,extension)){cJSON_Delete(extension);cJSON_Delete(tools);goto oom;}
 }
 if(!cJSON_AddItemToObject(root,"tools",tools)){cJSON_Delete(tools);goto oom;}
 unsigned rejected=0,observed=0;int last_ok=0,led_requested=0,led_applied=0,python_requested=0,python_applied=0;
 for(unsigned round=0;round<ROUNDS;round++) {
  char *request=cJSON_PrintUnformatted(root);
  if(!request || strlen(request)>=REQUEST_MAX){cJSON_free(request);a->print(a->user,"任务停止：模型上下文超过设备上限。");goto cleanup;}
  a->print(a->user,"[模型] 正在请求模型服务。");
  memset(response,0,RESPONSE_MAX);int exchanged=a->exchange(a->user,request,response,RESPONSE_MAX);cJSON_free(request);
  response[RESPONSE_MAX-1]=0;
  if(!exchanged){a->print(a->user,"任务停止：模型服务不可达、超时或响应超限；未自动重试，也未重放已执行操作。");goto cleanup;}
  cJSON *reply=ae_json_parse_bounded(response,RESPONSE_MAX);cJSON *choices=cJSON_GetObjectItemCaseSensitive(reply,"choices");
  cJSON *m=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(choices,0),"message");
  if(!cJSON_IsObject(m)){cJSON_Delete(reply);a->print(a->user,"任务停止：模型响应格式无效。");goto cleanup;}
  cJSON *calls=cJSON_GetObjectItemCaseSensitive(m,"tool_calls");
  if(!calls || (cJSON_IsArray(calls) && !cJSON_GetArraySize(calls))) {
   const char *answer=string(m,"content");
   const char *finish=string(cJSON_GetArrayItem(choices,0),"finish_reason");
   int truncated=finish && !strcmp(finish,"length");
   if(!observed && answer){
    const char *text=answer;while(*text==' ' || *text=='\t' || *text=='\r' || *text=='\n')text++;
    if(*text){a->print(a->user,answer);remember(a,prompt,answer);result=truncated?AE_AGENT_TRUNCATED:AE_AGENT_REPLIED;if(truncated)a->print(a->user,"[回复未完] 已达到单次生成上限，可输入“继续”；这不是完整答复。");cJSON_Delete(reply);goto cleanup;}
   }
   if(observed && last_ok && (!led_requested || led_applied) && (!python_requested || python_applied) && answer && *answer){a->print(a->user,answer);remember(a,prompt,answer);result=truncated?AE_AGENT_TRUNCATED:AE_AGENT_OBSERVED;if(truncated)a->print(a->user,"[回复未完] 已达到单次生成上限，可输入“继续”；这不是完整答复。");cJSON_Delete(reply);goto cleanup;}
   cJSON_Delete(reply);
   if(++rejected>=2)break;
   if(!message(messages,"system","No successful tool result. Conversation may be answered directly. Quote configured model ID exactly. Actions/live data need tools. Failed LED/Python need corrected successful calls. Off/on need only mode; explain unsupported limits."))goto oom;
   continue;
  }
  if(!cJSON_IsArray(calls) || cJSON_GetArraySize(calls)!=1){cJSON_Delete(reply);a->print(a->user,"任务停止：设备只允许每轮一个工具调用，未执行该批次。");goto cleanup;}
  cJSON *call=cJSON_GetArrayItem(calls,0),*fn=cJSON_GetObjectItemCaseSensitive(call,"function");
  const char *id=string(call,"id"),*name=string(fn,"name"),*raw=string(fn,"arguments");
  if(!id || !*id || strlen(id)>80 || !name || !raw || strlen(raw)>(AE_AGENT_ENABLE_PYTHON?3072u:512u)){cJSON_Delete(reply);a->print(a->user,"任务停止：工具帧无效，未执行。");goto cleanup;}
  cJSON *args=ae_json_parse_bounded(raw,AE_AGENT_ENABLE_PYTHON?3073:513);char output[AE_AGENT_ENABLE_PYTHON?2048:768]={0};ae_led_action action;
  int ok=0;
  if(!strcmp(name,"device_info") && cJSON_IsObject(args) && !cJSON_GetArraySize(args)) {snprintf(output,sizeof output,"%s",a->device_json);ok=1;}
  else if(!strcmp(name,"current_time") && a->current_time && cJSON_IsObject(args) && !cJSON_GetArraySize(args)){
   ok=a->current_time(a->user,output,sizeof output);
   if(!ok){
    a->print(a->user,"无法获取当前日期：网络时间源不可用；不能猜测日期或星期。");
    cJSON_Delete(args);cJSON_Delete(reply);goto cleanup;
   }
   /* Record this observation like other tools. It must not finish a compound
    * request or erase an earlier failed action. Pure date queries bypass model. */
  }
  else if(!strcmp(name,"led_control")) {
   led_requested=1;
   if(arguments(args,&action)){ok=a->led(a->user,&action,output,sizeof output);if(ok)led_applied=1;}
   else snprintf(output,sizeof output,"invalid_arguments: mode=off|on|solid|blink|breathe|rainbow; off/on need only mode. Effects need integer RGB 0..255; optional period_ms 200..5000. No action executed; correct before claiming success.");
  }
#if AE_AGENT_ENABLE_PYTHON
  else if(!strcmp(name,"python_exec") && a->python_exec){
   python_requested=1;const char *code=string(args,"code");
   if(cJSON_IsObject(args) && cJSON_GetArraySize(args)==1 && code && *code && strlen(code)<2048){a->print(a->user,"[工具] python_exec：模型生成的MicroPython代码");a->print(a->user,code);ok=a->python_exec(a->user,code,output,sizeof output);if(ok)python_applied=1;}
   else snprintf(output,sizeof output,"invalid_arguments: python_exec requires only a nonempty code string under 2048 bytes; no script executed");
  }
#endif
  else snprintf(output,sizeof output,"unsupported_or_invalid: tool is not registered or arguments are invalid; no command was executed. Actual capabilities: %.530s",a->device_json);
  output[sizeof output-1]=0;a->print(a->user,output);
  cJSON *assistant=message(messages,"assistant","");
  cJSON *copied=assistant?cJSON_Duplicate(calls,1):NULL;
  int linked=copied && cJSON_AddItemToObject(assistant,"tool_calls",copied);
  if(!linked)cJSON_Delete(copied);
  cJSON *tool=linked?message(messages,"tool",output):NULL;
  int recorded=tool && cJSON_AddStringToObject(tool,"tool_call_id",id);
  cJSON_Delete(args);cJSON_Delete(reply);
  if(!recorded)goto oom;
  observed++;last_ok=ok;if(!ok)rejected++;
  if(rejected>=2)break;
 }
 a->print(a->user,"任务停止：设备能力不支持、参数错误或模型尝试达到上限；没有执行任意系统命令。实际设备能力如下：");a->print(a->user,a->device_json);
 goto cleanup;
oom:
 a->print(a->user,"任务停止：协议内存分配失败；已显示的实际工具结果仍有效，不自动重放操作。");
cleanup:
 free(response);cJSON_Delete(root);return result;
}
