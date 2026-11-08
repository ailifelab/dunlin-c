#ifndef TEST_SEMPHR_H
#define TEST_SEMPHR_H
#include "FreeRTOS.h"
typedef struct {unsigned depth;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *);
BaseType_t xSemaphoreTake(SemaphoreHandle_t,TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t,TickType_t);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t);
#endif
