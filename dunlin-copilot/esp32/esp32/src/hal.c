#include "hal.h"
#include "profile.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
static int nvs_ready;
int ae_esp_radio_prepare(void) {
 if(nvs_ready)return ESP_OK;
 esp_err_t e=nvs_flash_init();if(e==ESP_OK)nvs_ready=1;
 return e; /* Never erase an existing user's NVS to hide initialization errors. */
}
static int pwm_pins[4]={-1,-1,-1,-1};
static int uart_opened,uart_tx=-1,uart_rx=-1,wifi_ready,wifi_configured;
static esp_netif_t *station;
static volatile unsigned wifi_reason;
int ae_esp_wifi_ipv4_ready(void){esp_netif_ip_info_t ip;return station && esp_netif_get_ip_info(station,&ip)==ESP_OK && ip.ip.addr!=0;}
static void wifi_event(void *unused,esp_event_base_t base,int32_t id,void *data) {
 (void)unused;(void)base;
 if(id==WIFI_EVENT_STA_DISCONNECTED && data)wifi_reason=((wifi_event_sta_disconnected_t *)data)->reason;
 else if(id==WIFI_EVENT_STA_CONNECTED)wifi_reason=0;
}
static ae_reply result(esp_err_t e,const char *ok) {
 ae_reply r={e==ESP_OK?AE_OK:AE_IO_ERROR,""};
 snprintf(r.text,sizeof r.text,"%s",e==ESP_OK?ok:esp_err_to_name(e));return r;
}
int ae_esp_pump(void *unused) {(void)unused;vTaskDelay(1);return 0;}
uint64_t ae_esp_now_ms(void *unused) {(void)unused;return (uint64_t)esp_timer_get_time()/1000;}
uint64_t ae_esp_valid_pins(void) {
 /* Conservative candidates. Grants start empty. Check the actual board before use.
  * Excludes boot strap, flash/PSRAM, console UART0 and USB pins on S3. */
#if CONFIG_IDF_TARGET_ESP32
 const unsigned pins[]={18,19,21,22,23,25,26,27,32,33};
#else
 const unsigned pins[]={4,5,6,7,15,16,17,18};
#endif
 uint64_t mask=0;for(size_t i=0;i<sizeof pins/sizeof *pins;i++)mask|=UINT64_C(1)<<pins[i];return mask;
}
static int pwm_index(unsigned pin) {for(int i=0;i<4;i++)if(pwm_pins[i]==(int)pin)return i;return -1;}
static esp_err_t wifi_init(void) {
 if(wifi_ready)return ESP_OK;
 esp_err_t e=ae_esp_radio_prepare();if(e!=ESP_OK)return e;
 if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)<100*1024 || heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)<24*1024)return ESP_ERR_NO_MEM;
 e=esp_netif_init();if(e!=ESP_OK)return e;
 e=esp_event_loop_create_default();if(e!=ESP_OK && e!=ESP_ERR_INVALID_STATE)return e;
 station=esp_netif_create_default_wifi_sta();if(!station)return ESP_ERR_NO_MEM;
 wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT();e=esp_wifi_init(&c);
 if(e!=ESP_OK){esp_netif_destroy_default_wifi(station);station=NULL;return e;}
 if((e=esp_wifi_set_storage(WIFI_STORAGE_RAM))!=ESP_OK || (e=esp_wifi_set_mode(WIFI_MODE_STA))!=ESP_OK || (e=esp_wifi_start())!=ESP_OK) {
  esp_wifi_deinit();esp_netif_destroy_default_wifi(station);station=NULL;return e;
 }
 e=esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL);
 if(e!=ESP_OK){esp_wifi_stop();esp_wifi_deinit();esp_netif_destroy_default_wifi(station);station=NULL;return e;}
 wifi_ready=1;return ESP_OK;
}
static ae_reply call(void *unused,const ae_request *r) {
 (void)unused;
 if(r->op==AE_DIAGNOSTICS) {
 
  const char *config="NVS+RAM";
  ae_reply out={AE_OK,""};snprintf(out.text,sizeof out.text,"heap_free=%u heap_min=%u largest_internal=%u owner_stack_free=%u bytes; config=%s",(unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),(unsigned)uxTaskGetStackHighWaterMark(NULL),config);return out;
 }
 if(r->op>=AE_BLE_NAME && r->op<=AE_BLE_READ)return ae_esp_ble(r);
 if(r->op==AE_GPIO_READ || r->op==AE_GPIO_WRITE || r->op==AE_PWM || r->op==AE_PWM_STOP) {
  if((int)r->pin==uart_tx || (int)r->pin==uart_rx)return (ae_reply){AE_BUSY,"pin belongs to UART"};
  int index=pwm_index(r->pin);esp_err_t e;
  if(r->op==AE_PWM_STOP) {
   if(index<0)return (ae_reply){AE_INVALID,"PWM not active"};
   e=ledc_stop(LEDC_LOW_SPEED_MODE,index,0);if(e==ESP_OK){pwm_pins[index]=-1;gpio_reset_pin(r->pin);}return result(e,"PWM stopped low");
  }
  if(r->op==AE_PWM) {
   if(index<0)for(int i=0;i<4;i++)if(pwm_pins[i]<0){index=i;break;}
   if(index<0)return (ae_reply){AE_LIMIT,"four PWM channels in use"};
   ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=LEDC_TIMER_10_BIT,.timer_num=index,.freq_hz=r->frequency,.clk_cfg=LEDC_AUTO_CLK};
   e=ledc_timer_config(&timer);if(e!=ESP_OK)return result(e,"");
   ledc_channel_config_t channel={.gpio_num=r->pin,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=index,.timer_sel=index,.duty=r->value,.hpoint=0};
   e=ledc_channel_config(&channel);if(e==ESP_OK)pwm_pins[index]=(int)r->pin;return result(e,"PWM configured (10-bit duty)");
  }
  if(index>=0)return (ae_reply){AE_BUSY,"stop PWM before GPIO access"};
  e=gpio_set_direction(r->pin,r->op==AE_GPIO_WRITE?GPIO_MODE_OUTPUT:GPIO_MODE_INPUT);
  if(e!=ESP_OK)return result(e,"");
  if(r->op==AE_GPIO_WRITE)return result(gpio_set_level(r->pin,r->value),"GPIO set");
  ae_reply out={AE_OK,""};snprintf(out.text,sizeof out.text,"level=%d",gpio_get_level(r->pin));return out;
 }
 if(r->op==AE_UART_OPEN) {
  if(uart_opened)return (ae_reply){AE_BUSY,"close UART before reconfigure"};
  if(pwm_index(r->tx)>=0 || pwm_index(r->rx)>=0)return (ae_reply){AE_BUSY,"pin belongs to PWM"};
  uart_config_t c={.baud_rate=r->baud,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
  esp_err_t e=uart_param_config(UART_NUM_1,&c);if(e!=ESP_OK)return result(e,"");
  e=uart_set_pin(UART_NUM_1,r->tx,r->rx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);if(e!=ESP_OK)return result(e,"");
  e=uart_driver_install(UART_NUM_1,256,256,0,NULL,0);
  if(e==ESP_OK){uart_opened=1;uart_tx=r->tx;uart_rx=r->rx;}return result(e,"UART1 open 8N1");
 }
 if(r->op>=AE_UART_SEND && r->op<=AE_UART_CLOSE) {
  if(!uart_opened)return (ae_reply){AE_INVALID,"UART1 closed"};
  if(r->op==AE_UART_CLOSE){esp_err_t e=uart_driver_delete(UART_NUM_1);if(e==ESP_OK){gpio_reset_pin(uart_tx);gpio_reset_pin(uart_rx);uart_opened=0;uart_tx=uart_rx=-1;}return result(e,"UART1 closed");}
  if(r->op==AE_UART_SEND){int n=uart_write_bytes(UART_NUM_1,r->data,r->length);if(n!=(int)r->length)return (ae_reply){AE_IO_ERROR,"UART short write; effect possible"};return result(uart_wait_tx_done(UART_NUM_1,pdMS_TO_TICKS(100)),"UART transmitted");}
  unsigned char data[64];int n=uart_read_bytes(UART_NUM_1,data,sizeof data,pdMS_TO_TICKS(20));
  if(n<0)return (ae_reply){AE_IO_ERROR,"UART read failed"};
  ae_reply out={AE_OK,""};for(int i=0;i<n;i++)snprintf(out.text+2*i,sizeof out.text-2*i,"%02x",data[i]);return out;
 }
 if(r->op>=AE_WIFI_SET && r->op<=AE_WIFI_FORGET) {
  if(r->op==AE_WIFI_STATUS && !wifi_ready)return (ae_reply){AE_OK,"WiFi disabled"};
  esp_err_t e=wifi_init();if(e!=ESP_OK)return result(e,"");
  if(r->op==AE_WIFI_SET) {
   wifi_config_t c={0};memcpy(c.sta.ssid,r->name,strlen(r->name));memcpy(c.sta.password,r->secret,strlen(r->secret));
   c.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;c.sta.sort_method=WIFI_CONNECT_AP_BY_SIGNAL;c.sta.failure_retry_cnt=2;
   c.sta.pmf_cfg.capable=true;c.sta.pmf_cfg.required=false;
   esp_wifi_disconnect();e=esp_wifi_set_config(WIFI_IF_STA,&c);memset(&c,0,sizeof c);
   if(e==ESP_OK)wifi_configured=1;
   if(e==ESP_OK && !ae_profile_restoring_wifi() && ae_profile_save_wifi(r->name,r->secret)!=ESP_OK)return (ae_reply){AE_IO_ERROR,"WiFi configured in RAM but NVS save failed; saved profile unchanged/uncertain"};
   return result(e,"WiFi config saved; use /wifi connect; restored on reboot");
  }
  if(r->op==AE_WIFI_CONNECT)return wifi_configured?result(esp_wifi_connect(),
  "connection requested; use /wifi status"
  ):(ae_reply){AE_INVALID,"configure WiFi first"};
  if(r->op==AE_WIFI_DISCONNECT)return result(esp_wifi_disconnect(),"disconnect requested");
  if(r->op==AE_WIFI_FORGET){wifi_config_t c={0};esp_wifi_disconnect();e=esp_wifi_set_config(WIFI_IF_STA,&c);if(e==ESP_OK)wifi_configured=0;
   if(e==ESP_OK && ae_profile_forget("wifi")!=ESP_OK)return (ae_reply){AE_IO_ERROR,"WiFi RAM cleared but saved profile deletion failed"};
   return result(e,"WiFi credentials cleared");}
  wifi_ap_record_t ap;esp_netif_ip_info_t ip;
  if(esp_wifi_sta_get_ap_info(&ap)!=ESP_OK){ae_reply out={AE_OK,""};snprintf(out.text,sizeof out.text,"WiFi disconnected; SDK reason=%u (0=none, 201=no AP, 202=auth, 204=handshake timeout)",wifi_reason);return out;}
  e=esp_netif_get_ip_info(station,&ip);if(e!=ESP_OK)return result(e,"");
  ae_reply out={AE_OK,""};snprintf(out.text,sizeof out.text,"WiFi associated; ipv4=" IPSTR "; rssi=%d",IP2STR(&ip.ip),ap.rssi);return out;
 }
 if(r->op==AE_NET_SEND) {
  struct sockaddr_in to={.sin_family=AF_INET,.sin_port=htons((uint16_t)r->port)};
  if(inet_pton(AF_INET,r->address,&to.sin_addr)!=1)return (ae_reply){AE_INVALID,"numeric IPv4 required"};
  int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(fd<0)return (ae_reply){AE_IO_ERROR,"UDP socket failed"};
  struct timeval timeout={.tv_sec=0,.tv_usec=100000};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
  int n=sendto(fd,r->data,r->length,0,(struct sockaddr *)&to,sizeof to);close(fd);
  return n==(int)r->length?(ae_reply){AE_OK,"UDP submitted; delivery unconfirmed"}:(ae_reply){AE_IO_ERROR,"UDP send failed; delivery unknown"};
 }
 return (ae_reply){AE_UNSUPPORTED,"operation unavailable"};
}
const ae_io_v1 ae_esp_hal={AE_ABI,NULL,call};
