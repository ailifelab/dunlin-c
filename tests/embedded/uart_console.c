#include "../../dunlin-copilot/esp32/esp32/src/hal.h"
#include "driver/uart.h"
#include <assert.h>
#include <stdio.h>
static int fail_install,flushes;static size_t buffered;static uart_event_t pending[64];static unsigned count,at;static StaticQueue_t queue;
int uart_driver_install(int port,int rx,int tx,int n,QueueHandle_t *out,int flags){
 assert(port==UART_NUM_0 && rx>=96*8 && rx<=4096 && tx>0 && n>=64 && !flags);*out=fail_install?NULL:&queue;return fail_install?-1:0;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *out,TickType_t wait){assert(q==&queue && wait==0);if(at==count)return pdFALSE;*(uart_event_t *)out=pending[at++];return pdTRUE;}
int uart_get_buffered_data_len(int port,size_t *out){assert(port==UART_NUM_0);*out=buffered;return 0;}
int uart_flush_input(int port){assert(port==UART_NUM_0);flushes++;buffered=0;return 0;}
int main(void){
 assert(!ae_esp_console_uart_fault());fail_install=1;assert(ae_esp_console_uart_start()!=0);assert(!ae_esp_console_uart_fault());fail_install=0;assert(!ae_esp_console_uart_start());
 pending[0]=(uart_event_t){.type=UART_DATA};count=1;at=0;buffered=768;assert(!ae_esp_console_uart_fault() && !flushes);
 const int errors[]={UART_FIFO_OVF,UART_BUFFER_FULL,UART_FRAME_ERR,UART_PARITY_ERR};
 for(unsigned i=0;i<4;i++){pending[0].type=errors[i];at=0;assert(ae_esp_console_uart_fault());}assert(flushes==4);
 count=64;at=0;for(unsigned i=0;i<count;i++)pending[i].type=UART_DATA;buffered=3980;assert(ae_esp_console_uart_fault() && flushes==5);
 assert(!ae_esp_console_uart_fault());
 puts("PASS UART bounded burst capacity, install failure, clean data, FIFO/ring/frame/parity fault flush, saturated data-only event queue fallback and recovery");
}
