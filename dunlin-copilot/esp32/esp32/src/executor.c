#include "hal.h"
#if defined(ESP_PLATFORM) || defined(AE_CONSOLE_TEXT_TEST)
#include "agent_host.h"
#include "../../serial/shell.h"
#endif
#ifdef ESP_PLATFORM
#include "agent_host.h"
#include "profile.h"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "python_host.h"
#endif
#include "../../serial/shell.h"
#endif
#include <dunlin/compact/esp32.h>
#include "driver/uart.h"
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define WAITING_JOBS 2u
#define JOB_SLOTS (WAITING_JOBS+1u)
#define OWNER_STACK 8192u
#define RX_STACK 3072u
static struct {
 char line[AE_LINE_MAX];size_t length;uint32_t id;
 ae_esp_completion_fn done;void *user;int used;
} slots[JOB_SLOTS];
static StaticQueue_t queue_control;
static unsigned char queue_storage[WAITING_JOBS*sizeof(unsigned)];
static QueueHandle_t queue;
static StaticSemaphore_t gate_control,output_control;
static SemaphoreHandle_t gate,output_lock;
static StaticTask_t owner_control,rx_control;
_Alignas(16) static StackType_t owner_stack[OWNER_STACK],rx_stack[RX_STACK];
static ae_serial serial;
static ak_scope *scope;
static ae_io_plugin *io;
static uint32_t next_id;
static atomic_int ready;
static atomic_int startup_attempt;
static void wipe(void *data,size_t size) {volatile unsigned char *p=data;while(size--)*p++=0;}
static void output(void *unused,const char *text,size_t size) {
 (void)unused;
 if(xSemaphoreTakeRecursive(output_lock,portMAX_DELAY)==pdTRUE){
/* Line-oriented serial monitors may only consume one frame per callback.
 * Send bounded UTF-8 chunks without coalescing several lines in one USB read. */
while(size){
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
 const size_t chunk=48;
#else
 /* CP2102 UART receive callbacks can be smaller than native USB packets.
  * Keep each paced write below that observed boundary; never split UTF-8. */
 const size_t chunk=16;
#endif
 size_t n=size<chunk?size:chunk;
 const char *lf=memchr(text,'\n',n);if(lf)n=(size_t)(lf-text)+1;
 else if(n<size){while(n && ((unsigned char)text[n]&0xc0)==0x80)n--;if(!n)n=size<chunk?size:chunk;}
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
 int written=usb_serial_jtag_write_bytes(text,n,pdMS_TO_TICKS(100));
 if(written!=(int)n)break;
 usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
#else
 int written=uart_write_bytes(UART_NUM_0,text,n);
 if(written!=(int)n || uart_wait_tx_done(UART_NUM_0,pdMS_TO_TICKS(100))!=0)break;
#endif
 text+=n;size-=n;
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
 vTaskDelay(1);
#else
 vTaskDelay(pdMS_TO_TICKS(20)>0?pdMS_TO_TICKS(20):1);
#endif
}
xSemaphoreGiveRecursive(output_lock);}
}
ae_status ae_esp_submit(const char *line,size_t length,ae_esp_completion_fn done,void *user,uint32_t *id) {
 if(id)*id=0;
 if(!line || !length || length>=AE_LINE_MAX || memchr(line,0,length) || xPortInIsrContext())return AE_INVALID;
 if(!atomic_load(&ready) || xSemaphoreTake(gate,0)!=pdTRUE)return AE_BUSY;
 unsigned index=JOB_SLOTS;
 for(unsigned i=0;i<JOB_SLOTS;i++)if(!slots[i].used){index=i;break;}
 ae_status status=AE_BUSY;
 if(index<JOB_SLOTS) {
  if(++next_id==0)next_id=1;
  slots[index].used=1;slots[index].length=length;slots[index].id=next_id;slots[index].done=done;slots[index].user=user;
  memcpy(slots[index].line,line,length);slots[index].line[length]=0;
  if(xQueueSend(queue,&index,0)==pdTRUE){if(id)*id=next_id;status=AE_OK;}
  else wipe(&slots[index],sizeof slots[index]);
 }
 xSemaphoreGive(gate);return status;
}
#ifdef ESP_PLATFORM
void ae_esp_console_print(const char *text) {
 xSemaphoreTakeRecursive(output_lock,portMAX_DELAY);output(NULL,text,strlen(text));output(NULL,"\r\n",2);xSemaphoreGiveRecursive(output_lock);
}
void ae_esp_console_write(void *unused,const char *text,size_t size){output(unused,text,size);}
#endif
#if defined(ESP_PLATFORM) || defined(AE_CONSOLE_TEXT_TEST)
typedef struct {int locked;} model_output_state;
static void model_output(void *user,const char *text,size_t size){
 model_output_state *state=user;
 while(size){
  const char *lf=memchr(text,'\n',size);size_t n=lf?(size_t)(lf-text)+1:size;
  if(!state->locked){xSemaphoreTakeRecursive(output_lock,portMAX_DELAY);state->locked=1;}
  output(NULL,text,n);text+=n;size-=n;
  if(lf){state->locked=0;xSemaphoreGiveRecursive(output_lock);}
 }
}
void ae_esp_console_model_text(const char *text){
 model_output_state state={0};ae_shell_text(text,model_output,&state);
 if(state.locked)xSemaphoreGiveRecursive(output_lock);
}
#endif
static void complete_serial(void *user,uint32_t id,const ae_reply *reply) {
 (void)user;char buffer[AE_REPLY_MAX+48];
 int n=snprintf(buffer,sizeof buffer,"\r\njob=%lu %s: %s\r\n> \r\n",(unsigned long)id,ae_status_name(reply->status),reply->text);
 if(n>0)output(NULL,buffer,(size_t)n<sizeof buffer?(size_t)n:sizeof buffer-1);
}
static ae_reply submit_serial(void *unused,const char *line,size_t length) {
 (void)unused;
 if(!length)return (ae_reply){AE_OK,""};
 uint32_t id;ae_status status=ae_esp_submit(line,length,complete_serial,NULL,&id);
 ae_reply reply={status,""};
 if(status==AE_OK)snprintf(reply.text,sizeof reply.text,"queued job=%lu; completion follows",(unsigned long)id);
 else snprintf(reply.text,sizeof reply.text,"not queued; %s",ae_status_name(status));
 return reply;
}
static void owner_task(void *arg) {
 (void)arg;
 ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
 static const ap_python_host python_host={ae_esp_console_print,ae_esp_console_model_text,ae_esp_device_info,ae_board_led_apply,ae_esp_wifi_ipv4_ready,ae_esp_model_clock_sync};
 ae_python_start(&python_host);
#endif
#ifdef ESP_PLATFORM
 ae_esp_agent_restore();ae_esp_profile_start_wifi(io);
#endif
 for(;;) {
  unsigned index;
  if(xQueueReceive(queue,&index,portMAX_DELAY)!=pdTRUE)continue;
  ae_reply reply;
#ifdef ESP_PLATFORM
  char normalized[AE_LINE_MAX];
  if(!ae_shell_normalize(slots[index].line,normalized,sizeof normalized))reply=(ae_reply){AE_INVALID,"invalid local command length"};
  else if(!ae_shell_kind(slots[index].line))ae_esp_agent_prompt(normalized,&reply);
  else if(!ae_esp_agent_command(normalized,&reply))reply=ae_execute(scope,io,normalized,strlen(normalized));
  if(reply.status==AE_INVALID && !strcmp(reply.text,"invalid syntax; use help"))
   snprintf(reply.text,sizeof reply.text,"invalid syntax; use /help");
  wipe(normalized,sizeof normalized);

#else
  reply=ae_execute(scope,io,slots[index].line,slots[index].length);
#endif
  ae_esp_completion_fn done=slots[index].done;void *user=slots[index].user;uint32_t id=slots[index].id;
  xSemaphoreTake(gate,portMAX_DELAY);wipe(&slots[index],sizeof slots[index]);xSemaphoreGive(gate);
  if(done)done(user,id,&reply);
 }
}
static void serial_task(void *arg) {
 (void)arg;unsigned char input[32];
 ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
 for(;;) {
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  int n=usb_serial_jtag_read_bytes(input,sizeof input,pdMS_TO_TICKS(20));
#else
  int n=uart_read_bytes(UART_NUM_0,input,sizeof input,pdMS_TO_TICKS(20));
#endif
#if defined(ESP_PLATFORM) && !defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
  if(n>=0){
#else
  if(n>0){
#endif
   xSemaphoreTakeRecursive(output_lock,portMAX_DELAY);
#if defined(ESP_PLATFORM) && !defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
   if(ae_esp_console_uart_fault()){
    wipe(serial.line,sizeof serial.line);serial.length=0;serial.swallow_lf=0;serial.escape=0;serial.dropping=1;
    const char *warning="\r\nlimit: UART RX overflow/frame error; buffered input discarded; send newline then retry\r\n> ";
    output(NULL,warning,strlen(warning));
   }else
#endif
   if(n>0)ae_serial_feed(&serial,input,(size_t)n);
   xSemaphoreGiveRecursive(output_lock);wipe(input,sizeof input);
  }
  vTaskDelay(1);
 }
}
ae_status ae_esp_rtos_start(ak_scope *root,ae_io_plugin *plugin) {
 if(!root || !plugin)return AE_INVALID;
 if(atomic_exchange(&startup_attempt,1))return AE_BUSY;
 scope=root;io=plugin;
 queue=xQueueCreateStatic(WAITING_JOBS,sizeof(unsigned),queue_storage,&queue_control);
 gate=xSemaphoreCreateMutexStatic(&gate_control);output_lock=xSemaphoreCreateRecursiveMutexStatic(&output_control);
 if(!queue || !gate || !output_lock)return AE_IO_ERROR;
 ae_serial_init(&serial,scope,io,output,NULL);ae_serial_set_submit(&serial,submit_serial,NULL);
 TaskHandle_t owner=xTaskCreateStatic(owner_task,"dunlin_owner",OWNER_STACK,NULL,4,owner_stack,&owner_control);
 if(!owner)return AE_LIMIT;
 TaskHandle_t rx=xTaskCreateStatic(serial_task,"dunlin_serial",RX_STACK,NULL,3,rx_stack,&rx_control);
 if(!rx){vTaskDelete(owner);return AE_LIMIT;}
 atomic_store(&ready,1);xTaskNotifyGive(owner);xTaskNotifyGive(rx);return AE_OK;
}
