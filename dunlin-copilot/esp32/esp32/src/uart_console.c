#include "hal.h"
#include "driver/uart.h"
#include "freertos/queue.h"
/* Bounded burst absorption, not unbounded flow control. The event queue must
 * hold data notifications plus the first overflow when the RX task is printing. */
#define RX_CAPACITY 4096
static QueueHandle_t events;
int ae_esp_console_uart_start(void){
 return uart_driver_install(UART_NUM_0,RX_CAPACITY,512,64,&events,0);
}
int ae_esp_console_uart_fault(void){
 if(!events)return 0;
 uart_event_t event;int fault=0;
 for(unsigned i=0;i<64 && xQueueReceive(events,&event,0)==pdTRUE;i++){
  if(event.type==UART_FIFO_OVF || event.type==UART_BUFFER_FULL || event.type==UART_FRAME_ERR || event.type==UART_PARITY_ERR)fault=1;
 }
 /* Data events can fill an event queue while console output holds the lock.
  * Saturation is therefore a conservative fault even without an error event. */
 size_t buffered=0;
 if(uart_get_buffered_data_len(UART_NUM_0,&buffered)==0 && buffered>=RX_CAPACITY-256)fault=1;
 if(fault)uart_flush_input(UART_NUM_0);
 return fault;
}
