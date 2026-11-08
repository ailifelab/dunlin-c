#include "agent_host.h"
#include "profile.h"
#include "../../common/http_date.h"
#include <sys/time.h>
#include <time.h>
#include <strings.h>
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "python_host.h"
#include "../config/python_defaults.h"
#include "../config/openai_defaults.h"
#include "esp_psram.h"
#include "../../agent/responses.h"
#include "../../agent/json.h"
#include "esp_crt_bundle.h"
#endif
#include "hal.h"
#include "../../serial/shell.h"
#include "../../agent/agent.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define MODEL_KEY_BYTES 256
#define MODEL_SCAN "%127s %95s %255s %c"
static char effort[8]="low";
#ifdef CONFIG_IDF_TARGET_ESP32S3
static ae_responses responses;
static unsigned api_attempts;
#endif
static char url[128],model[96],key[MODEL_KEY_BYTES],pending[AE_LINE_MAX];
static int openai_url(void){
#ifdef CONFIG_IDF_TARGET_ESP32S3
 return !strcmp(url,"https://api.openai.com/v1/responses");
#else
 return 0;
#endif
}
static ae_agent_memory memory;
static TaskHandle_t task;
static SemaphoreHandle_t guard;
static int busy,hidden;
static uint64_t deadline;
static void wipe(void *p,size_t size){volatile unsigned char *b=p;while(size--)*b++=0;}
/* Prefix and escape model-facing text so it cannot forge serial control frames. */
static void print(void *unused,const char *text) {
 (void)unused;ae_esp_console_model_text(text);
}
static int within_deadline(esp_http_client_handle_t client) {
 uint64_t now=(uint64_t)esp_timer_get_time()/1000;if(now>=deadline)return 0;
 unsigned remaining=(unsigned)(deadline-now);if(remaining>30000)remaining=30000;
 esp_http_client_set_timeout_ms(client,(int)remaining);return 1;
}
static int local_url(const char *value) {
 char ip[16];unsigned port;int end=0;struct in_addr address;
 return sscanf(value,"http://%15[^:]:%u/v1/chat/completions%n",ip,&port,&end)==2 && end>0 && !value[end] && port && port<=65535 && inet_pton(AF_INET,ip,&address)==1 &&
  ((ntohl(address.s_addr)>>24)==10 || (ntohl(address.s_addr)>>20)==0xac1 || (ntohl(address.s_addr)>>16)==0xc0a8);
}
static int model_url(const char *value){
#ifdef CONFIG_IDF_TARGET_ESP32S3
 if(!strcmp(value,"https://api.openai.com/v1/responses"))return 1;
#endif
 return local_url(value);
}
void ae_esp_agent_restore(void) {
 ae_saved_model saved;int found=ae_profile_load_model(&saved);
 if(!guard)guard=xSemaphoreCreateMutex();
 if(found==1 && guard && model_url(saved.url)){
  snprintf(url,sizeof url,"%s",saved.url);snprintf(model,sizeof model,"%s",saved.model);snprintf(key,sizeof key,"%s",saved.key);
  snprintf(effort,sizeof effort,"%s",saved.effort);
  ae_esp_console_print("[启动] 已恢复模型配置；网络就绪后可直接输入自然语言，无需重新/model set。");
 }else if(found<0 || (found==1 && (!guard || !model_url(saved.url))))ae_esp_console_print("[启动] 模型保存配置无效或不可读取，未启用。");
 wipe(&saved,sizeof saved);
}
static esp_err_t clock_header(esp_http_client_event_t *event){
 if(event->event_id==HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key,"date") && strlen(event->header_value)<64)snprintf(event->user_data,64,"%s",event->header_value);
 return ESP_OK;
}
int ae_esp_model_clock_sync(unsigned timeout_ms){
 if(!guard || timeout_ms<100)return 0;
 char address[sizeof url],date[64]={0};xSemaphoreTake(guard,portMAX_DELAY);snprintf(address,sizeof address,"%s",url);xSemaphoreGive(guard);
 if(!strcmp(address,"https://api.openai.com/v1/responses"))snprintf(address,sizeof address,"http://api.openai.com/");
 else if(!local_url(address))return 0;
 esp_http_client_config_t config={.url=address,.method=HTTP_METHOD_HEAD,.timeout_ms=(int)timeout_ms,.disable_auto_redirect=true,.event_handler=clock_header,.user_data=date,.buffer_size=512};
 esp_http_client_handle_t client=esp_http_client_init(&config);if(!client)return 0;
 uint64_t until=(uint64_t)esp_timer_get_time()/1000+timeout_ms;int64_t epoch=0;int ok=0;
 if(esp_http_client_open(client,0)==ESP_OK){
  uint64_t now=(uint64_t)esp_timer_get_time()/1000;
  if(now<until){esp_http_client_set_timeout_ms(client,(int)(until-now));
   if(esp_http_client_fetch_headers(client)>=0 && ae_http_date_epoch(date,&epoch)){struct timeval tv={.tv_sec=(time_t)epoch};ok=settimeofday(&tv,NULL)==0;}
  }
 }
 esp_http_client_close(client);esp_http_client_cleanup(client);return ok;
}
static int current_time(void *unused,char *out,size_t cap){
 (void)unused;
 if(!ae_esp_wifi_ipv4_ready() || !ae_esp_model_clock_sync(3000))return 0;
 char value[160];if(!ae_date_shanghai((int64_t)time(NULL),value,sizeof value))return 0;
 int n=snprintf(out,cap,"当前日期和时间：%s；来源：配置模型服务器的实时 HTTP Date（网络取值，非模型生成）。",value);
 return n>=0 && (size_t)n<cap;
}
static int http_exchange(void *unused,const char *request,char *response,size_t capacity) {
 (void)unused;uint64_t now=(uint64_t)esp_timer_get_time()/1000;
 uint64_t until=now+10000;if(until>deadline)until=deadline;
 if(!ae_esp_wifi_ipv4_ready())ae_esp_console_print("[联网] 等待Wi-Fi取得IP，最多10秒；可用/wifi status检查。");
 while(!ae_esp_wifi_ipv4_ready() && now<until){vTaskDelay(pdMS_TO_TICKS(100));now=(uint64_t)esp_timer_get_time()/1000;}
 if(!ae_esp_wifi_ipv4_ready())return 0;
 if(now>=deadline || heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)<24*1024)return 0;
#ifdef CONFIG_IDF_TARGET_ESP32S3
 if(openai_url() && time(NULL)<1700000000){
  ae_esp_console_print("[OpenAI] Bootstrapping UTC from API host HTTP Date, without credentials.");
  unsigned clock_ms=(unsigned)(deadline-now);if(clock_ms>3000)clock_ms=3000;
  if(!ae_esp_model_clock_sync(clock_ms)){ae_esp_console_print("[OpenAI] UTC clock not ready; use /model time UNIX_SECONDS. TLS checks remain enabled.");return 0;}
  now=(uint64_t)esp_timer_get_time()/1000;if(now>=deadline)return 0;
 }
#endif
 unsigned timeout=(unsigned)(deadline-now);if(timeout>30000)timeout=30000;
 esp_http_client_config_t cfg={.url=url,.timeout_ms=(int)timeout,.disable_auto_redirect=true,.buffer_size=1024,.buffer_size_tx=1024};
#ifdef CONFIG_IDF_TARGET_ESP32S3
 if(openai_url()){cfg.crt_bundle_attach=esp_crt_bundle_attach;ae_esp_console_print("[OpenAI] Connecting to api.openai.com over verified HTTPS.");}
#endif
 esp_http_client_handle_t client=esp_http_client_init(&cfg);if(!client)return 0;
 esp_http_client_set_method(client,HTTP_METHOD_POST);esp_http_client_set_header(client,"Content-Type","application/json");
 char authorization[sizeof key+8];snprintf(authorization,sizeof authorization,"Bearer %s",key);
 if(key[0])esp_http_client_set_header(client,"Authorization",authorization);
 int ok=0;size_t length=strlen(request),sent=0,used=0;
 if(esp_http_client_open(client,(int)length)!=ESP_OK)goto cleanup;
 while(sent<length){if(!within_deadline(client))goto cleanup;int n=esp_http_client_write(client,request+sent,(int)(length-sent));if(n<=0)goto cleanup;sent+=(size_t)n;}
#ifdef CONFIG_IDF_TARGET_ESP32S3
 if(openai_url())ae_esp_console_print("[OpenAI] Request sent; waiting for model response.");
#endif
 if(!within_deadline(client) || esp_http_client_fetch_headers(client)<0)goto cleanup;
 if(esp_http_client_get_status_code(client)!=200){char msg[64];snprintf(msg,sizeof msg,"[Model HTTP] status=%d; credentials hidden",esp_http_client_get_status_code(client));ae_esp_console_print(msg);goto cleanup;}
 for(;;){
  if((uint64_t)esp_timer_get_time()/1000>=deadline || used+1>=capacity)goto cleanup;
  if(!within_deadline(client))goto cleanup;
  int n=esp_http_client_read(client,response+used,(int)(capacity-used-1));
  if(n<0)goto cleanup;
  if(!n){ok=esp_http_client_is_complete_data_received(client);break;}
  used+=(size_t)n;vTaskDelay(1);
 }
 response[used]=0;
cleanup:
 wipe(authorization,sizeof authorization);esp_http_client_close(client);esp_http_client_cleanup(client);return ok;
}
static int exchange(void *unused,const char *request,char *response,size_t capacity){
#ifdef CONFIG_IDF_TARGET_ESP32S3
 if(openai_url()){
  char *wire=ae_responses_request(&responses,request,effort,AE_OPENAI_MAX_OUTPUT_TOKENS);
  char *raw=heap_caps_malloc(AE_OPENAI_RESPONSE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!wire || !raw){cJSON_free(wire);heap_caps_free(raw);ae_esp_console_print("[OpenAI] Protocol/buffer allocation failed; no request sent.");return 0;}
  uint64_t start=(uint64_t)esp_timer_get_time()/1000;api_attempts++;
  int ok=http_exchange(unused,wire,raw,AE_OPENAI_RESPONSE_BYTES);responses.network_ms+=(uint64_t)esp_timer_get_time()/1000-start;cJSON_free(wire);
  if(ok){ok=ae_responses_reply(&responses,raw,response,capacity);if(!ok)ae_esp_console_print(response);}
  if(!ok)responses.usage_known=0;
  heap_caps_free(raw);return ok;
 }
#endif
 return http_exchange(unused,request,response,capacity);
}
static int led(void *unused,const ae_led_action *a,char *out,size_t size){(void)unused;return ae_board_led_apply(a->mode,a->red,a->green,a->blue,a->period_ms,out,size);}
static void json_model(char *out,size_t cap){
 size_t used=0;for(const unsigned char *p=(const unsigned char *)model;*p && used+7<cap;p++){
  if(*p==34 || *p==92){out[used++]=92;out[used++]=(char)*p;}
  else if(*p<32)used+=(size_t)snprintf(out+used,cap-used,"%cu%04x",92,*p);
  else out[used++]=(char)*p;
 }out[used]=0;
}
void ae_esp_device_info(char *info,size_t cap){
 char led_state[160],active_model[600];esp_chip_info_t chip;esp_chip_info(&chip);json_model(active_model,sizeof active_model);
 ae_board_led_status(led_state,sizeof led_state);
#ifdef CONFIG_IDF_TARGET_ESP32S3
 char allowed[800];ae_python_hosts(allowed,sizeof allowed);
 snprintf(info,cap,"{\"device\":\"%s\",\"os\":\"FreeRTOS / ESP-IDF %s\",\"active_model\":\"%s\",\"cores\":%u,\"chip_revision\":%u,\"linux\":false,\"wifi_transport\":true,\"shell\":false,\"filesystem\":false,\"package_manager\":false,\"tools\":[\"current_time\",\"device_info\",\"led_control\"%s],\"python\":{\"available\":%s,\"runtime\":\"MicroPython 1.26.1 embed\",\"modules\":[\"json\",\"dunlin\"],\"network_policy\":\"%s\",\"tls_verify\":true,\"redirects\":false,\"allowed_hosts\":\"%s\",\"psram_bytes\":%u,\"code_bytes\":%u,\"script_ms\":%u,\"job_ms\":%u},\"led_pin\":%d,\"led_state\":\"%s\",\"led_restore\":\"Use mode on only to restore the saved mode RGB and period; never guess another effect.\",\"heap_free\":%u}",CONFIG_IDF_TARGET,esp_get_idf_version(),active_model,(unsigned)chip.cores,(unsigned)chip.revision,ae_python_available()?",\"python_exec\"":"",ae_python_available()?"true":"false",ae_python_network_policy(),allowed,(unsigned)esp_psram_get_size(),AP_CODE_BYTES,AP_SCRIPT_MS,AP_JOB_MS,ae_board_led_pin(),led_state,(unsigned)esp_get_free_heap_size());
#else
 snprintf(info,cap,"{\"device\":\"%s\",\"active_model\":\"%s\",\"tools\":[\"current_time\",\"device_info\",\"led_control\"],\"shell\":false,\"led_state\":\"%s\"}",CONFIG_IDF_TARGET,active_model,led_state);
#endif
}
#ifdef CONFIG_IDF_TARGET_ESP32S3
static int python_exec(void *unused,const char *code,char *out,size_t cap){(void)unused;return ae_python_exec(code,out,cap);}

#endif
#ifdef CONFIG_IDF_TARGET_ESP32S3
#define AGENT_STACK_BYTES 16384
#define DEVICE_INFO_BYTES 2048
#else
#define AGENT_STACK_BYTES 12288
#define DEVICE_INFO_BYTES 768
#endif
static void work(void *unused) {
 (void)unused;
 for(;;){
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
#ifdef CONFIG_IDF_TARGET_ESP32S3
  ae_responses_reset(&responses);api_attempts=0;
#endif
  char info[DEVICE_INFO_BYTES];ae_esp_device_info(info,sizeof info);
  ae_agent agent={NULL,info,!hidden,exchange,led,print,
#ifdef CONFIG_IDF_TARGET_ESP32S3
  ae_python_available()?python_exec:NULL,
#else
  NULL,
#endif
  NULL,&memory,current_time
  };deadline=(uint64_t)esp_timer_get_time()/1000+120000;
  int status=ae_agent_turn(&agent,model,pending);
#ifdef CONFIG_IDF_TARGET_ESP32S3
  if(openai_url()){
   char stats[224];
   if(responses.usage_known==7 && responses.calls)snprintf(stats,sizeof stats,"[OpenAI metrics] effort=%s; attempts=%u; completed=%u; request_wall_ms=%llu; input=%llu; output=%llu; reasoning=%llu",effort,api_attempts,responses.calls,(unsigned long long)responses.network_ms,(unsigned long long)responses.input_tokens,(unsigned long long)responses.output_tokens,(unsigned long long)responses.reasoning_tokens);
   else snprintf(stats,sizeof stats,"[OpenAI metrics] effort=%s; attempts=%u; completed=%u; request_wall_ms=%llu; token_usage=unavailable_or_partial",effort,api_attempts,responses.calls,(unsigned long long)responses.network_ms);
   ae_esp_console_print(stats);
  }
#endif
  char done[224];snprintf(done,sizeof done,"[任务结束] status=%s; model=%s; heap_free=%u; 串口仍可接收命令。",status==AE_AGENT_REPLIED?"replied":status==AE_AGENT_OBSERVED?"observed":status==AE_AGENT_TRUNCATED?"truncated":"stopped",model,(unsigned)esp_get_free_heap_size());
  /* Publish completion and release the task atomically to serial dispatch.
   * A fast next prompt must not overtake the footer or see a stale busy bit. */
  xSemaphoreTake(guard,portMAX_DELAY);ae_esp_console_print(done);wipe(pending,sizeof pending);busy=0;xSemaphoreGive(guard);
 }
}
static void queue_prompt(const char *prompt,int blind,ae_reply *reply){
 if(!url[0]){*reply=(ae_reply){AE_INVALID,"model is not configured; /help model shows configuration commands"};return;}
 if(!*prompt || strlen(prompt)>=sizeof pending){*reply=(ae_reply){AE_INVALID,"prompt is empty or exceeds byte limit"};return;}
 if(!task && xTaskCreate(work,"dunlin_agent",AGENT_STACK_BYTES,NULL,2,&task)!=pdPASS){*reply=(ae_reply){AE_LIMIT,"model task allocation failed"};return;}
 hidden=blind;snprintf(pending,sizeof pending,"%s",prompt);busy=1;xTaskNotifyGive(task);
 *reply=(ae_reply){AE_OK,"model task queued; actual tool results and bounded completion follow"};
}
void ae_esp_agent_prompt(const char *prompt,ae_reply *reply){
 if(!guard)guard=xSemaphoreCreateMutex();
 if(!guard){*reply=(ae_reply){AE_LIMIT,"agent lock unavailable"};return;}
 xSemaphoreTake(guard,portMAX_DELAY);
 if(busy
#ifdef CONFIG_IDF_TARGET_ESP32S3
 || ae_python_active()
#endif
 )*reply=(ae_reply){AE_BUSY,"model task active; local help and status remain available"};
 else queue_prompt(prompt,0,reply);
 xSemaphoreGive(guard);
}
/* Config accepts operator input only. Never register these commands as model tools. */
int ae_esp_agent_command(const char *line,ae_reply *reply) {
#ifdef CONFIG_IDF_TARGET_ESP32S3
 if(!strcmp(line,"python status")){*reply=(ae_reply){AE_OK,""};ae_python_status(reply->text,sizeof reply->text);return 1;}
 if(!strcmp(line,"python network")){*reply=(ae_reply){AE_OK,""};ae_python_network(NULL,reply->text,sizeof reply->text);return 1;}
 if(!strcmp(line,"python hosts")){*reply=(ae_reply){AE_OK,""};ae_python_hosts(reply->text,sizeof reply->text);return 1;}
#else
 if(!strncmp(line,"python",6) && (!line[6] || line[6]==' ')){*reply=(ae_reply){AE_UNSUPPORTED,"MicroPython unavailable on this board"};return 1;}
#endif
 int help=ae_shell_help(line,ae_esp_console_write,NULL);
 if(help){*reply=(ae_reply){help>0?AE_OK:AE_INVALID,"local help shown; no model request"};return 1;}
 if(strncmp(line,"agent",5) && strncmp(line,"ask ",4) && strncmp(line,"ask-blind ",10) && strncmp(line,"led pin ",8)
 && strncmp(line,"led ",4) && strncmp(line,"python ",7)
 )return 0;
 if(!guard)guard=xSemaphoreCreateMutex();
 if(!guard){*reply=(ae_reply){AE_LIMIT,"agent lock unavailable"};return 1;}
 xSemaphoreTake(guard,portMAX_DELAY);
 *reply=(ae_reply){AE_INVALID,"use /help, /help model or /help led; commands must start with /"};
 if(!strcmp(line,"agent memory") && busy){*reply=(ae_reply){AE_BUSY,"conversation update active; retry after task completion"};goto done;}
 if(!strcmp(line,"agent memory")){snprintf(reply->text,sizeof reply->text,"conversation=%u/%u turns; RAM only; assistant excerpts <=1023 bytes; /model clear clears conversation; reboot clears conversation",memory.count,AE_MEMORY_TURNS);reply->status=AE_OK;goto done;}
 if(!strcmp(line,"agent effort")){snprintf(reply->text,sizeof reply->text,"Reasoning effort: %s%s",openai_url()?effort:"default",openai_url()?"":" (provider effort not configured)");reply->status=AE_OK;goto done;}
 if(!strcmp(line,"agent status")) {
  char value[256];snprintf(value,sizeof value,"model=%s",model[0]?model:"[not configured]");ae_esp_console_print(value);
  snprintf(value,sizeof value,"endpoint=%s",url[0]?url:"[not configured]");ae_esp_console_print(value);
#ifdef CONFIG_IDF_TARGET_ESP32S3
  if(openai_url()){snprintf(value,sizeof value,"protocol=responses; effort=%s; UTC_ready=%s; server_continuation=stored-per-task",effort,time(NULL)>=1700000000?"yes":"no");ae_esp_console_print(value);}
#endif
  snprintf(reply->text,sizeof reply->text,"configured=%s; authentication=%s (key hidden); task=%s; storage=NVS",url[0]?"yes":"no",key[0]?"on":"off",busy?"running":"idle");reply->status=AE_OK;goto done;
 }
 if(!strcmp(line,"led status")){reply->status=ae_board_led_status(reply->text,sizeof reply->text)?AE_OK:AE_UNSUPPORTED;goto done;}
 if(busy
#ifdef CONFIG_IDF_TARGET_ESP32S3
 || ae_python_active()
#endif
 ){*reply=(ae_reply){AE_BUSY,"model task active; status/wifi/other serial commands remain available"};goto done;}
 if(!strcmp(line,"agent clear")){ae_agent_memory_clear(&memory);*reply=(ae_reply){AE_OK,"conversation cleared; Wi-Fi/model profile retained"};goto done;}
#ifdef CONFIG_IDF_TARGET_ESP32S3
 if(!strncmp(line,"agent time ",11)){
  char *end=NULL;unsigned long long stamp=strtoull(line+11,&end,10);
  if(end!=line+11 && !*end && stamp>=1700000000ULL && stamp<=4102444800ULL && (sizeof(time_t)>4 || stamp<=2147483647ULL)){
   struct timeval value={.tv_sec=(time_t)stamp};*reply=(ae_reply){settimeofday(&value,NULL)==0?AE_OK:AE_IO_ERROR,"UTC clock set by operator; TLS certificate checks remain enabled"};
  }else *reply=(ae_reply){AE_INVALID,"use /model time UNIX_SECONDS; valid UTC epoch required"};
  goto done;
 }
 if(!strncmp(line,"agent effort ",13)){
  const char *value=line+13;
  if(!openai_url() || (strcmp(value,"low") && strcmp(value,"xhigh")))*reply=(ae_reply){AE_INVALID,"Use /effort low|xhigh (OpenAI Responses only)"};
  else if(ae_profile_save_model_options(url,model,key,value)!=ESP_OK)*reply=(ae_reply){AE_IO_ERROR,"effort save failed; previous configuration retained"};
  else {snprintf(effort,sizeof effort,"%s",value);*reply=(ae_reply){AE_OK,"reasoning effort saved"};}
  goto done;
 }
 if(!strncmp(line,"python run ",11)){reply->status=ae_python_queue(line+11,reply->text,sizeof reply->text)?AE_OK:AE_INVALID;goto done;}
 if(!strncmp(line,"python network ",15)){reply->status=ae_python_network(line+15,reply->text,sizeof reply->text)?AE_OK:AE_INVALID;goto done;}
 if(!strncmp(line,"python allow ",13)){reply->status=ae_python_allow(line+13,reply->text,sizeof reply->text)?AE_OK:AE_INVALID;goto done;}
#endif
 ae_led_action action;int led_request=strncmp(line,"led pin ",8)?ae_shell_led(line,&action):0;
 if(led_request){
  if(led_request<0)*reply=(ae_reply){AE_INVALID,"invalid LED command; /help led shows modes/ranges"};
  else reply->status=ae_board_led_apply(action.mode,action.red,action.green,action.blue,action.period_ms,reply->text,sizeof reply->text)?AE_OK:AE_IO_ERROR;
  goto done;
 }
 if(!strncmp(line,"led pin ",8)) {
  unsigned selected;char extra;
  if(sscanf(line+8,"%u %c",&selected,&extra)==1 && ae_board_led_configure(selected))*reply=(ae_reply){AE_OK,"confirmed onboard RGB LED configured; model cannot select pins"};
  else *reply=(ae_reply){AE_UNSUPPORTED,"LED pin unsupported/unconfirmed or RMT setup failed; no model-selected GPIO"};
 } else if(!strcmp(line,"agent forget")) {
  if(ae_profile_forget("model")!=ESP_OK){*reply=(ae_reply){AE_IO_ERROR,"saved model profile deletion failed; RAM unchanged"};goto done;}
  wipe(url,sizeof url);wipe(model,sizeof model);wipe(key,sizeof key);ae_agent_memory_clear(&memory);*reply=(ae_reply){AE_OK,"model configuration forgotten"};
 } else if(!strncmp(line,"agent set ",10)) {
  char u[128],m[96],k[MODEL_KEY_BYTES],extra;
  if(sscanf(line+10,MODEL_SCAN,u,m,k,&extra)==3 && model_url(u)) {
   esp_err_t saved=ae_profile_save_model_options(u,m,strcmp(k,"-")?k:"","low");
   if(saved!=ESP_OK){*reply=(ae_reply){AE_IO_ERROR,"NVS model save failed; previous RAM model retained"};wipe(k,sizeof k);goto done;}
   ae_agent_memory_clear(&memory);snprintf(url,sizeof url,"%s",u);snprintf(model,sizeof model,"%s",m);wipe(key,sizeof key);if(strcmp(k,"-"))snprintf(key,sizeof key,"%s",k);
   snprintf(effort,sizeof effort,"low");
   *reply=(ae_reply){AE_OK,"model config saved to NVS; restored on reboot; enter natural language directly"};
  }
  wipe(k,sizeof k);
 } else if(!strncmp(line,"ask ",4) || !strncmp(line,"ask-blind ",10)) {
  int blind=!strncmp(line,"ask-blind ",10);queue_prompt(line+(blind?10:4),blind,reply);
 }
done:
 xSemaphoreGive(guard);return 1;
}
