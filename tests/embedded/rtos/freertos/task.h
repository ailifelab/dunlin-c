#ifndef TEST_TASK_H
#define TEST_TASK_H
#include "FreeRTOS.h"
typedef struct {void (*run)(void *);void *arg;} StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
TaskHandle_t xTaskCreateStatic(void (*)(void *),const char *,uint32_t,void *,UBaseType_t,StackType_t *,StaticTask_t *);
BaseType_t xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(BaseType_t,TickType_t);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
#endif
