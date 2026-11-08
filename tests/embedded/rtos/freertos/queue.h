#ifndef TEST_QUEUE_H
#define TEST_QUEUE_H
#include "FreeRTOS.h"
typedef struct {unsigned char *store;size_t capacity,item,head,tail,count;} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t,UBaseType_t,uint8_t *,StaticQueue_t *);
BaseType_t xQueueSend(QueueHandle_t,const void *,TickType_t);
BaseType_t xQueueReceive(QueueHandle_t,void *,TickType_t);
#endif
