#include "python_host.h"
#include "../../common/https_policy.h"
#include "py/runtime.h"
#include "py/objstr.h"
#include "py/objlist.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "../config/python_defaults.h"
#define VM_HEAP AP_VM_HEAP_BYTES
static struct {
 ap_python_host host;SemaphoreHandle_t gate,done;TaskHandle_t task;
 void *heap;int busy,sync,abandoned,complete,ok,truncated;
 char input[AP_CODE_BYTES+1],result[AP_STDOUT_BYTES+1];size_t used;unsigned lines;
 uint64_t deadline,cpu_deadline;unsigned polls;
} vm;
static const char *const default_hosts[]=AP_DEFAULT_HOSTS;
static char *added_hosts[AP_MAX_ALLOWED_HOSTS];
static const char *host_at(unsigned i){return i<AP_DEFAULT_HOST_COUNT?default_hosts[i]:added_hosts[i-AP_DEFAULT_HOST_COUNT];}
static unsigned host_count=AP_DEFAULT_HOST_COUNT;
static int restrict_hosts=AP_RESTRICT_HOSTS;
static int ready_for_tls(void);
static uint64_t now_ms(void){return (uint64_t)esp_timer_get_time()/1000;}
void dunlin_mp_write(const char *s,size_t n);
void dunlin_mp_poll(void);
int dunlin_mp_http(const char *,char *,size_t);
void dunlin_mp_write(const char *s,size_t n){
 size_t room=sizeof vm.result-1-vm.used;if(n>room){n=room;vm.truncated=1;}
 for(size_t i=0;i<n;i++)if(s[i]=='\n' && ++vm.lines>AP_STDOUT_LINES){n=i;vm.truncated=1;break;}
 memcpy(vm.result+vm.used,s,n);vm.used+=n;vm.result[vm.used]=0;
}
void dunlin_mp_poll(void){
 if((++vm.polls&255)!=0)return;
 if(now_ms()>vm.deadline || now_ms()>vm.cpu_deadline)mp_raise_msg(&mp_type_RuntimeError,MP_ERROR_TEXT("Python execution timeout"));
 vTaskDelay(1);
}
int dunlin_mp_http(const char *url,char *out,size_t cap){
 out[0]=0;
 const char *hosts[AP_MAX_ALLOWED_HOSTS];for(unsigned i=0;i<host_count;i++)hosts[i]=host_at(i);
 char target[96];
 if(!ae_https_permitted(url,!restrict_hosts,hosts,host_count,target,sizeof target)){
  snprintf(out,cap,restrict_hosts?"HTTPS URL invalid or host denied by allowlist; operator /python network trusted or /python allow HOST":"HTTPS URL invalid or over limit; valid HTTPS authority required");return 0;
 }
 uint64_t began=now_ms();if(began>=vm.deadline || !ready_for_tls()){snprintf(out,cap,"Wi-Fi or TLS clock not ready within deadline");return 0;}
 if(now_ms()>=vm.deadline)return 0;
 unsigned left=(unsigned)(vm.deadline-now_ms());if(left>AP_HTTPS_TIMEOUT_MS)left=AP_HTTPS_TIMEOUT_MS;
 esp_http_client_config_t config={.url=url,.timeout_ms=(int)left,.disable_auto_redirect=true,.crt_bundle_attach=esp_crt_bundle_attach,.buffer_size=1024,.buffer_size_tx=1024};
 char stage[160];snprintf(stage,sizeof stage,"[Python/HTTPS] 正在连接已授权主机：%s",target);vm.host.log(stage);
 esp_http_client_handle_t client=esp_http_client_init(&config);if(!client){snprintf(out,cap,"HTTPS client allocation failed");return 0;}
 int ok=0,detailed_error=0;size_t used=0;
 if(esp_http_client_open(client,0)!=ESP_OK)goto done;
 if(esp_http_client_fetch_headers(client)<0)goto done;
 if(esp_http_client_get_status_code(client)!=200){
  char reason[257]={0};int bytes=esp_http_client_read(client,reason,256);
  snprintf(out,cap,"HTTP=%d: %.*s",esp_http_client_get_status_code(client),bytes>0?bytes:0,reason);detailed_error=1;goto done;
 }
 vm.host.log("[Python/HTTPS] 已收到HTTP 200，正在读取有界响应。");
 for(;;){
  uint64_t now=now_ms();if(now>=vm.deadline || used+1>=cap){snprintf(out,cap,"HTTPS deadline or response-size limit exceeded");detailed_error=1;goto done;}
  unsigned wait=(unsigned)(vm.deadline-now);if(wait>AP_HTTPS_TIMEOUT_MS)wait=AP_HTTPS_TIMEOUT_MS;esp_http_client_set_timeout_ms(client,(int)wait);
  int n=esp_http_client_read(client,out+used,(int)(cap-used-1));if(n<0)goto done;
  if(!n){ok=esp_http_client_is_complete_data_received(client);break;}used+=(size_t)n;vTaskDelay(1);
 }
 out[used]=0;
done:
 if(!ok && !detailed_error)snprintf(out,cap,"HTTPS failed: TLS/network/HTTP status/response limit; HTTP=%d",esp_http_client_get_status_code(client));
 esp_http_client_close(client);esp_http_client_cleanup(client);
 vm.cpu_deadline+=now_ms()-began;return ok;
}
static int ready_for_tls(void){
 uint64_t until=now_ms()+AP_NTP_WAIT_MS;if(until>vm.deadline)until=vm.deadline;
 while(!vm.host.ip_ready() && now_ms()<until)vTaskDelay(pdMS_TO_TICKS(100));
 if(!vm.host.ip_ready())return 0;
 if(time(NULL)<1700000000)vm.host.log("[Python/联网] 正在通过配置的NTP时间源校时，用于TLS证书校验。");
 if(!esp_sntp_enabled()){esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,AP_NTP_SERVER);esp_sntp_init();}
 while(time(NULL)<1700000000 && now_ms()<until)vTaskDelay(pdMS_TO_TICKS(100));
 if(time(NULL)<1700000000 && vm.host.sync_clock && now_ms()<vm.deadline){
  unsigned remaining=(unsigned)(vm.deadline-now_ms());if(remaining>3000)remaining=3000;
  if(vm.host.sync_clock(remaining))vm.host.log("[Python/联网] 已由宿主时间源校时，继续验证TLS证书。");
 }
 return time(NULL)>=1700000000;
}
static mp_obj_t execute(const char *s,size_t length){
 mp_lexer_t *lex=mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,s,length,0);
 qstr name=lex->source_name;mp_parse_tree_t tree=mp_parse(lex,MP_PARSE_FILE_INPUT);
 return mp_call_function_0(mp_compile(&tree,name,true));
}
static void key_text(mp_obj_t key){
 size_t n;const unsigned char *s=(const unsigned char *)mp_obj_str_get_data(key,&n);if(n>48)n=48;
 for(size_t i=0;i<n;i++){
  if(s[i]<32 || s[i]==127){char escaped[5];snprintf(escaped,sizeof escaped,"%cx%02x",92,s[i]);dunlin_mp_write(escaped,4);}
  else dunlin_mp_write((const char *)s+i,1);
 }
}
static void exception_shapes(void){
 mp_map_t *globals=&mp_globals_get()->map;size_t begin=vm.used;unsigned objects=0;
 for(size_t i=0;i<globals->alloc && objects<8 && vm.used-begin<512;i++){
  if(!mp_map_slot_is_filled(globals,i))continue;
  mp_map_elem_t *slot=&globals->table[i];
  if(!mp_obj_is_str(slot->key) || !mp_obj_is_type(slot->value,&mp_type_dict))continue;
  dunlin_mp_write("[Python变量结构] ",strlen("[Python变量结构] "));key_text(slot->key);dunlin_mp_write(" keys: ",7);
  mp_map_t *map=&((mp_obj_dict_t *)MP_OBJ_TO_PTR(slot->value))->map;unsigned keys=0;
  for(size_t j=0;j<map->alloc && keys<12 && vm.used-begin<512;j++){
   if(mp_map_slot_is_filled(map,j) && mp_obj_is_str(map->table[j].key)){if(keys)dunlin_mp_write(", ",2);key_text(map->table[j].key);keys++;}
  }
  dunlin_mp_write("\n",1);objects++;
  /* Show one level of child keys too, including the first object in a JSON
   * array. No values, allocations, recursive walks or unbounded cyclic data. */
  for(size_t j=0;j<map->alloc && objects<8 && vm.used-begin<512;j++){
   if(!mp_map_slot_is_filled(map,j) || !mp_obj_is_str(map->table[j].key))continue;
   mp_obj_t child=map->table[j].value;int first=0;
   if(mp_obj_is_type(child,&mp_type_list)){
    mp_obj_list_t *list=MP_OBJ_TO_PTR(child);if(!list->len)continue;
    child=list->items[0];first=1;
   }
   if(!mp_obj_is_type(child,&mp_type_dict))continue;
   dunlin_mp_write("[Python变量结构] ",strlen("[Python变量结构] "));
   key_text(slot->key);dunlin_mp_write(".",1);key_text(map->table[j].key);
   if(first)dunlin_mp_write("[0]",3);
   dunlin_mp_write(" keys: ",7);
   mp_map_t *nested=&((mp_obj_dict_t *)MP_OBJ_TO_PTR(child))->map;unsigned shown=0;
   for(size_t k=0;k<nested->alloc && shown<12 && vm.used-begin<512;k++){
    if(mp_map_slot_is_filled(nested,k) && mp_obj_is_str(nested->table[k].key)){
     if(shown)dunlin_mp_write(", ",2);
     key_text(nested->table[k].key);shown++;
    }
   }
   dunlin_mp_write("\n",1);objects++;
  }
 }
}
static void worker(void *unused){
 (void)unused;int stack_top;
 for(;;){
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);vm.result[0]=0;vm.used=0;vm.polls=vm.lines=0;vm.ok=vm.truncated=0;vm.deadline=now_ms()+AP_JOB_MS;
  vm.host.log("[Python] 正在执行MicroPython代码。");
  {
   mp_stack_set_top(&stack_top);mp_stack_set_limit(20*1024);gc_init(vm.heap,(char *)vm.heap+VM_HEAP);mp_init();vm.cpu_deadline=now_ms()+AP_SCRIPT_MS;
   nlr_buf_t nlr;
   if(nlr_push(&nlr)==0){
    mp_store_global(qstr_from_str("dunlin"),mp_import_name(qstr_from_str("dunlin"),mp_const_none,MP_OBJ_NEW_SMALL_INT(0)));
    mp_store_global(qstr_from_str("json"),mp_import_name(qstr_from_str("json"),mp_const_none,MP_OBJ_NEW_SMALL_INT(0)));
    execute(vm.input,strlen(vm.input));
    if(vm.truncated)mp_raise_msg(&mp_type_RuntimeError,MP_ERROR_TEXT("Python output limit exceeded"));
    vm.ok=1;nlr_pop();
   }else {mp_obj_print_exception(&mp_plat_print,MP_OBJ_FROM_PTR(nlr.ret_val));exception_shapes();}
   mp_deinit();
   if(vm.truncated){size_t at=vm.used;if(at+80>sizeof vm.result)at=sizeof vm.result-80;snprintf(vm.result+at,sizeof vm.result-at,"...[Python output/line limit exceeded]");}
  }
  xSemaphoreTake(vm.gate,portMAX_DELAY);vm.complete=1;
  if(vm.sync && !vm.abandoned){xSemaphoreGive(vm.done);xSemaphoreGive(vm.gate);}
  else {
   int publish=!vm.abandoned;xSemaphoreGive(vm.gate);
   if(publish)vm.host.text(vm.result);
   xSemaphoreTake(vm.gate,portMAX_DELAY);
   if(publish)vm.host.log(vm.ok?"[Python结束] status=ok":"[Python结束] status=failed");
   vm.busy=0;xSemaphoreGive(vm.gate);
  }
 }
}
void ae_python_start(const ap_python_host *host){
 if(vm.gate || !host || !host->log || !host->text || !host->info || !host->led || !host->ip_ready)return;
 vm.host=*host;
 vm.gate=xSemaphoreCreateMutex();vm.done=xSemaphoreCreateBinary();
 if(vm.gate && vm.done && esp_psram_is_initialized())vm.heap=heap_caps_malloc(VM_HEAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(vm.heap && xTaskCreate(worker,"dunlin_python",24576,NULL,tskIDLE_PRIORITY,&vm.task)!=pdPASS){heap_caps_free(vm.heap);vm.heap=NULL;}
 if(!vm.task){if(vm.gate)vSemaphoreDelete(vm.gate);if(vm.done)vSemaphoreDelete(vm.done);vm.gate=vm.done=NULL;}
}
void ae_python_status(char *out,size_t cap){
 snprintf(out,cap,"MicroPython=1.26.1 embed; available=%s; PSRAM=%u; VM_heap=%u; task=%s; script_ms=%u; job_ms=%u; network=%s; TLS_verify=yes; redirects=no; filesystem=no",vm.task?"yes":"no",(unsigned)esp_psram_get_size(),vm.heap?VM_HEAP:0,ae_python_active()?"busy":"idle",AP_SCRIPT_MS,AP_JOB_MS,ae_python_network_policy());
}
static int submit(const char *input,int sync,char *out,size_t cap){
 if(!vm.task){snprintf(out,cap,"Python unavailable: PSRAM VM heap/task allocation failed; console and LED remain usable");return 0;}
 size_t n=strlen(input);if(!n || n>=sizeof vm.input){snprintf(out,cap,"invalid Python input length");return 0;}
 xSemaphoreTake(vm.gate,portMAX_DELAY);
 if(vm.busy){xSemaphoreGive(vm.gate);snprintf(out,cap,"Python task busy; no request queued");return 0;}
 xSemaphoreTake(vm.done,0);snprintf(vm.input,sizeof vm.input,"%s",input);vm.sync=sync;vm.abandoned=vm.complete=0;vm.busy=1;
 xTaskNotifyGive(vm.task);xSemaphoreGive(vm.gate);
 if(!sync){snprintf(out,cap,"Python task queued; completion follows; /python status remains available");return 1;}
 if(xSemaphoreTake(vm.done,pdMS_TO_TICKS(AP_JOB_MS+5000))!=pdTRUE){
  xSemaphoreTake(vm.gate,portMAX_DELAY);vm.abandoned=1;if(vm.complete)vm.busy=0;xSemaphoreGive(vm.gate);
  snprintf(out,cap,"Python result timeout; do not replay until /python status is idle");return 0;
 }
 xSemaphoreTake(vm.gate,portMAX_DELAY);int ok=vm.ok;snprintf(out,cap,"%s",vm.result);if(ok && !vm.result[0]){snprintf(out,cap,"Python finished with no observable result; use print(...), and pass executable code rather than a quoted program string");ok=0;}vm.busy=0;xSemaphoreGive(vm.gate);return ok;
}
int ae_python_exec(const char *code,char *out,size_t cap){return submit(code,1,out,cap);}
int ae_python_queue(const char *input,char *out,size_t cap){return submit(input,0,out,cap);}
int ae_python_active(void){if(!vm.gate)return 0;xSemaphoreTake(vm.gate,portMAX_DELAY);int active=vm.busy;xSemaphoreGive(vm.gate);return active;}
const char *ae_python_network_policy(void){
 if(!vm.gate)return restrict_hosts?"allowlist":"trusted_https";
 xSemaphoreTake(vm.gate,portMAX_DELAY);int restricted=restrict_hosts;xSemaphoreGive(vm.gate);
 return restricted?"allowlist":"trusted_https";
}
int ae_python_network(const char *value,char *out,size_t cap){
 if(!value || !*value){snprintf(out,cap,"network=%s; TLS CA+hostname verification required; redirects=no; limits unchanged",ae_python_network_policy());return 1;}
 if(strcmp(value,"trusted") && strcmp(value,"allowlist")){snprintf(out,cap,"use /python network trusted|allowlist");return 0;}
 if(!vm.gate){snprintf(out,cap,"Python unavailable; policy unchanged");return 0;}
 xSemaphoreTake(vm.gate,portMAX_DELAY);
 if(vm.busy){xSemaphoreGive(vm.gate);snprintf(out,cap,"Python busy; network policy unchanged");return 0;}
 restrict_hosts=!strcmp(value,"allowlist");xSemaphoreGive(vm.gate);
 snprintf(out,cap,"network=%s; saved in RAM; reset restores build default; TLS verification always enabled",ae_python_network_policy());return 1;
}
int ae_python_allow(const char *host,char *out,size_t cap){
 if(!vm.task){snprintf(out,cap,"Python unavailable; host policy unchanged");return 0;}
 size_t n=strlen(host);if(n<3 || n>=96 || !strchr(host,'.')){snprintf(out,cap,"invalid HTTPS host");return 0;}
 for(size_t i=0;i<n;i++)if(!((host[i]>='a'&&host[i]<='z') || (host[i]>='0'&&host[i]<='9') || host[i]=='-' || host[i]=='.')){snprintf(out,cap,"use lowercase DNS host without scheme/path/port");return 0;}
 xSemaphoreTake(vm.gate,portMAX_DELAY);
 if(vm.busy){xSemaphoreGive(vm.gate);snprintf(out,cap,"Python busy; host policy unchanged");return 0;}
 for(unsigned i=0;i<host_count;i++)if(!strcmp(host,host_at(i))){xSemaphoreGive(vm.gate);snprintf(out,cap,"host already allowed");return 1;}
 if(host_count==AP_MAX_ALLOWED_HOSTS){xSemaphoreGive(vm.gate);snprintf(out,cap,"host allowlist full");return 0;}
 char *copy=heap_caps_malloc(n+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!copy){xSemaphoreGive(vm.gate);snprintf(out,cap,"host-policy allocation failed; unchanged");return 0;}
 memcpy(copy,host,n+1);added_hosts[host_count++-AP_DEFAULT_HOST_COUNT]=copy;xSemaphoreGive(vm.gate);snprintf(out,cap,"HTTPS host allowed in RAM; reset restores defaults");return 1;
}
void ae_python_hosts(char *out,size_t cap){
 size_t used=0;out[0]=0;if(!vm.gate){snprintf(out,cap,"unavailable");return;}xSemaphoreTake(vm.gate,portMAX_DELAY);
 for(unsigned i=0;i<host_count;i++){int n=snprintf(out+used,cap-used,"%s%s",i?",":"",host_at(i));if(n<0 || (size_t)n>=cap-used)break;used+=(size_t)n;}
 xSemaphoreGive(vm.gate);
}

int dunlin_mp_led(const char *mode,unsigned r,unsigned g,unsigned b,unsigned ms,char *out,size_t cap);
void dunlin_mp_info(char *,size_t);
int dunlin_mp_led(const char *mode,unsigned r,unsigned g,unsigned b,unsigned ms,char *out,size_t cap){
 if(strcmp(mode,"off") && strcmp(mode,"on") && strcmp(mode,"solid") && strcmp(mode,"blink") && strcmp(mode,"breathe") && strcmp(mode,"rainbow"))return 0;
 int ok=vm.host.led(mode,r,g,b,ms,out,cap);if(ok){dunlin_mp_write("[LED] ",6);dunlin_mp_write(out,strlen(out));dunlin_mp_write("\n",1);}return ok;
}
void dunlin_mp_info(char *out,size_t cap){vm.host.info(out,cap);}

int ae_python_available(void){return vm.task!=NULL;}
