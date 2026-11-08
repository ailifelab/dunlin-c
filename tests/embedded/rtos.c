/* Exercise production executor.c with a deterministic task/queue test double.
 * This is not a FreeRTOS scheduler/ESP hardware validation. */
#include "../../dunlin-copilot/esp32/esp32/src/hal.h"
#include <dunlin/compact/esp32.h>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
static unsigned creates,deletes,fail_create,io_calls,completed;static int isr,fail_queue;
static TaskHandle_t owner;static jmp_buf drained;static int draining;
static ae_status statuses[16];static uint32_t identifiers[16];
int xPortInIsrContext(void){return isr;}
TaskHandle_t xTaskCreateStatic(void (*run)(void *),const char *name,uint32_t bytes,void *arg,UBaseType_t priority,StackType_t *stack,StaticTask_t *task) {
 (void)priority;assert(stack && bytes>=3072 && (uintptr_t)stack%16==0);creates++;if(creates==fail_create)return NULL;
 task->run=run;task->arg=arg;if(!strcmp(name,"dunlin_owner"))owner=task;return task;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task){assert(task);return pdPASS;}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t wait){assert(clear && wait==portMAX_DELAY);return 1;}
void vTaskDelete(TaskHandle_t task){assert(task);deletes++;}
void vTaskDelay(TickType_t ticks){(void)ticks;}
QueueHandle_t xQueueCreateStatic(UBaseType_t capacity,UBaseType_t item,uint8_t *store,StaticQueue_t *queue) {
 if(fail_queue)return NULL;
 *queue=(StaticQueue_t){store,capacity,item,0,0,0};return queue;
}
BaseType_t xQueueSend(QueueHandle_t q,const void *item,TickType_t wait) {
 assert(!wait);if(q->count==q->capacity)return pdFALSE;
 memcpy(q->store+q->tail*q->item,item,q->item);q->tail=(q->tail+1)%q->capacity;q->count++;return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *item,TickType_t wait) {
 assert(wait==portMAX_DELAY);
 if(!q->count){assert(draining);longjmp(drained,1);}
 memcpy(item,q->store+q->head*q->item,q->item);q->head=(q->head+1)%q->capacity;q->count--;return pdTRUE;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s){s->depth=0;return s;}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s){return xSemaphoreCreateMutexStatic(s);}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t wait){(void)wait;if(s->depth)return pdFALSE;s->depth=1;return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s){assert(s->depth==1);s->depth=0;return pdTRUE;}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s,TickType_t wait){(void)wait;s->depth++;return pdTRUE;}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s){assert(s->depth);s->depth--;return pdTRUE;}
int uart_write_bytes(int port,const void *data,size_t size){(void)port;(void)data;return (int)size;}
int uart_wait_tx_done(int port,TickType_t wait){(void)port;(void)wait;return 0;}
int uart_read_bytes(int port,void *data,size_t size,TickType_t wait){(void)port;(void)data;(void)size;(void)wait;return 0;}
static ae_reply hal(void *user,const ae_request *r){(void)user;(void)r;io_calls++;return (ae_reply){AE_OK,"observed"};}
static void done(void *user,uint32_t id,const ae_reply *reply){assert(user==&completed);assert(completed<16);identifiers[completed]=id;statuses[completed++]=reply->status;}
static void drain(void){assert(owner);draining=1;if(!setjmp(drained))owner->run(owner->arg);draining=0;}
static ae_status submit(const char *text,uint32_t *id){return ae_esp_submit(text,strlen(text),done,&completed,id);}
int main(int argc,char **argv) {
 if(argc>1){if(!strcmp(argv[1],"owner"))fail_create=1;else if(!strcmp(argv[1],"serial"))fail_create=2;else fail_queue=1;}
 ae_pool pool={0};ak_platform host={.user=&pool,.alloc=ae_pool_alloc,.free=ae_pool_free};ak_context *ctx=ak_create(&host);assert(ctx);
 ae_io_v1 device={AE_ABI,NULL,hal};ae_io_plugin io={.hal=&device,.valid_pins=UINT64_C(1)<<18};ak_scope *plugin;
 assert(ak_install(ctx,&ae_io_descriptor,&io,&plugin)==AK_OK);
 ae_status start=ae_esp_rtos_start(ak_root(ctx),&io);uint32_t one,two,three;
 if(argc>1){assert(start==(fail_queue?AE_IO_ERROR:AE_LIMIT));assert(submit("status",&one)==AE_BUSY);assert(!completed && !io_calls);assert(deletes==(fail_create==2));}
 else {
  assert(start==AE_OK);assert(ae_esp_rtos_start(ak_root(ctx),&io)==AE_BUSY);assert(submit("gpio write 18 1",&one)==AE_OK);assert(submit("io allow 18",&two)==AE_OK);
  assert(submit("gpio read 18",&three)==AE_BUSY && three==0);assert(!completed && !io_calls);
  drain();assert(completed==2 && identifiers[0]==one && identifiers[1]==two && statuses[0]==AE_DENIED && statuses[1]==AE_OK && !io_calls);
  assert(submit("gpio write 18 1",&three)==AE_OK);assert(io_calls==0);drain();assert(io_calls==1 && completed==3 && statuses[2]==AE_OK);
  assert(submit("unknown",&three)==AE_OK);drain();assert(completed==4 && statuses[3]==AE_INVALID && io_calls==1);
  isr=1;assert(submit("status",&three)==AE_INVALID);isr=0;
  char oversized[AE_LINE_MAX+1];memset(oversized,'x',sizeof oversized);assert(ae_esp_submit(oversized,sizeof oversized,done,NULL,&three)==AE_INVALID);
  for(unsigned i=0;i<10000;i++){assert(ae_esp_submit("gpio read 18",12,NULL,NULL,&three)==AE_OK);drain();}
  assert(io_calls==10001);
 }
 assert(ak_destroy(ctx)==AK_OK && !pool.live);
 puts(argc>1?"PASS executor setup failure rejects jobs without effects":"PASS production executor: bounded FIFO, no effect before owner, busy/ISR/length rejection, callbacks and 10000 reusable jobs");return 0;
}
