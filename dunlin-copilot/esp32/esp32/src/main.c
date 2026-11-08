#include "hal.h"
#include "agent_host.h"
#include "driver/uart.h"
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include <stdarg.h>
#endif
#include "esp_heap_caps.h"
#include "esp_system.h"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "esp_psram.h"
#include "python_host.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
/* No PSRAM required. All DUNLIN-owned storage fixed; SDK/radio heaps are separate. */
static ae_pool pool;
static ae_io_plugin io;
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
/* Keep SDK logging on the same buffered driver, bounded when the host is absent. */
static int bounded_log(const char *format,va_list args){
 char text[256];int n=vsnprintf(text,sizeof text,format,args);
 if(n>0)usb_serial_jtag_write_bytes(text,(size_t)n<sizeof text?(size_t)n:sizeof text-1,pdMS_TO_TICKS(20));
 return n;
}
#endif
void app_main(void) {
#ifdef CONFIG_IDF_TARGET_ESP32S3
 printf("DUNLIN PSRAM initialized=%d bytes=%u external_free=%u\n",esp_psram_is_initialized(),(unsigned)esp_psram_get_size(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
 ak_platform host={.user=&pool,.alloc=ae_pool_alloc,.free=ae_pool_free,.pump=ae_esp_pump,.now_ms=ae_esp_now_ms};
 ak_context *kernel=ak_create(&host);
 if(!kernel){printf("DUNLIN kernel pool exhausted\n");return;}
 if(!ae_kernel_probe(kernel)){printf("DUNLIN kernel probe failed\n");ak_destroy(kernel);return;}
 io.hal=&ae_esp_hal;io.valid_pins=ae_esp_valid_pins();ak_scope *scope=NULL;
 if(ak_install(kernel,&ae_io_descriptor,&io,&scope)!=AK_OK){printf("DUNLIN IO install failed\n");ak_destroy(kernel);return;}
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
 usb_serial_jtag_driver_config_t usb={.tx_buffer_size=2048,.rx_buffer_size=512};
 esp_err_t e=usb_serial_jtag_driver_install(&usb);
 if(e==ESP_OK){usb_serial_jtag_vfs_use_driver();esp_log_set_vprintf(bounded_log);}
#else
 uart_config_t cfg={.baud_rate=115200,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
 esp_err_t e=uart_param_config(UART_NUM_0,&cfg);
 if(e==ESP_OK)e=ae_esp_console_uart_start();
#endif
 if(e!=ESP_OK){printf("console unavailable: %s\n",esp_err_to_name(e));ak_destroy(kernel);return;}
 char banner[240];
 const char *instructions="UTF-8 lines. /help for local commands; other text goes to model. No input echo. Conversation uses bounded RAM history.";
 int n=snprintf(banner,sizeof banner,"DUNLIN FreeRTOS serial ABI %u (%s). %s\r\nDUNLIN pool=%u bytes; free heap=%u\r\n> ",AE_ABI,CONFIG_IDF_TARGET,instructions,(unsigned)sizeof pool,(unsigned)esp_get_free_heap_size());
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
 usb_serial_jtag_write_bytes(banner,(size_t)n<sizeof banner?(size_t)n:sizeof banner-1,pdMS_TO_TICKS(100));
#else
 uart_write_bytes(UART_NUM_0,banner,(size_t)n<sizeof banner?(size_t)n:sizeof banner-1);
#endif
#ifdef DUNLIN_BOARD_LED_PIN
 if(!ae_board_led_configure(DUNLIN_BOARD_LED_PIN))printf("onboard LED unavailable; serial and agent remain available\n");
#endif
 ae_status status=ae_esp_rtos_start(ak_root(kernel),&io);
 if(status!=AE_OK){printf("DUNLIN task setup failed: %s\n",ae_status_name(status));ak_destroy(kernel);}
 /* app_main's IDF task returns; RX and one kernel owner now run independently. */
}
