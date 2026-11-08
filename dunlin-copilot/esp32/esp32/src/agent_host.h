#ifndef AE_AGENT_HOST_H
#define AE_AGENT_HOST_H
#include <dunlin/compact/embedded.h>
/* Called only by the serial owner; async model work has its own bounded task. */
int ae_esp_agent_command(const char *,ae_reply *);
void ae_esp_agent_prompt(const char *,ae_reply *);
void ae_esp_console_print(const char *);
void ae_esp_console_write(void *,const char *,size_t);
void ae_esp_console_model_text(const char *);
int ae_board_led_configure(unsigned);
int ae_board_led_apply(const char *,unsigned,unsigned,unsigned,unsigned,char *,size_t);
int ae_board_led_pin(void);
int ae_board_led_status(char *,size_t);
int ae_esp_model_clock_sync(unsigned);
void ae_esp_device_info(char *,size_t);
#endif
