#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;typedef unsigned UBaseType_t;typedef uint32_t TickType_t;
typedef uint8_t StackType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
int xPortInIsrContext(void);
#endif
