#ifndef DUNLIN_ESP_HAL_H
#define DUNLIN_ESP_HAL_H
#include <dunlin/compact/esp32.h>
/* Owned by one console task. Radio callbacks never invoke the kernel. */
int ae_esp_wifi_ipv4_ready(void);
int ae_esp_radio_prepare(void); /* lazy NVS for SDK radio only, never auto-erases */
ae_reply ae_esp_ble(const ae_request *);
void app_main(void);
int ae_esp_console_uart_start(void);
int ae_esp_console_uart_fault(void);
#endif
