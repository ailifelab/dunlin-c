#ifndef DUNLIN_ESP32_H
#define DUNLIN_ESP32_H
#include "embedded.h"
#ifdef __cplusplus
extern "C" {
#endif
/* ESP-IDF platform adapters; the kernel itself does not include this header. */
extern const ae_io_v1 ae_esp_hal;
uint64_t ae_esp_valid_pins(void);
uint64_t ae_esp_now_ms(void *);
int ae_esp_pump(void *); /* yield one FreeRTOS tick while polling a future */
/* One startup attempt per boot, after UART0 driver and services are configured.
 * root/io must live until reset. On success only owner may call that kernel/IO.
 * AE_BUSY on another attempt. No stop API yet; stop hardware commands then reset. */
ae_status ae_esp_rtos_start(ak_scope *root,ae_io_plugin *io);

/* ESP-IDF FreeRTOS host API. Implemented only by the ESP32 component.
 * Task context only (never ISR). Copies line; no wait for queue space.
 * AE_OK means queued, not executed. At most two waiting jobs and one active job.
 * Completion runs on the single kernel owner task; borrowed reply only during
 * callback. Callback must not block; may enqueue another job. user must remain
 * alive until callback. No callback after a rejected submission. */
typedef void (*ae_esp_completion_fn)(void *user,uint32_t id,const ae_reply *reply);
ae_status ae_esp_submit(const char *line,size_t length,ae_esp_completion_fn,void *user,uint32_t *id);
#ifdef __cplusplus
}
#endif
#endif
