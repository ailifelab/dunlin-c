#ifndef TEST_UART_H
#define TEST_UART_H
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
typedef struct {int type;size_t size;int timeout_flag;} uart_event_t;
enum {UART_DATA,UART_FIFO_OVF,UART_BUFFER_FULL,UART_FRAME_ERR,UART_PARITY_ERR};
int uart_driver_install(int,int,int,int,QueueHandle_t *,int);
int uart_flush_input(int);
int uart_get_buffered_data_len(int,size_t *);
#define UART_NUM_0 0
int uart_write_bytes(int,const void *,size_t);
int uart_wait_tx_done(int,TickType_t);
int uart_read_bytes(int,void *,size_t,TickType_t);
#endif
