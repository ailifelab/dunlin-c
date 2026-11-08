#include "agent_host.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
/* GPIO is operator-confirmed, never selected by model. Single driver task owns
 * RMT and bounded electrical transactions; animation never blocks the agent. */
static rmt_channel_handle_t channel;
static rmt_encoder_handle_t encoder;
static TaskHandle_t task;
static SemaphoreHandle_t lock,applied;
static int pin=-1,enabled,driver_error;
static unsigned r=32,g=32,b=32,period=1000,generation,ack;
static char effect[12]="solid";
static esp_err_t pixel(unsigned red,unsigned green,unsigned blue) {
 unsigned bytes[3]={green,red,blue};static rmt_symbol_word_t symbols[25];unsigned at=0;
 for(unsigned j=0;j<3;j++)for(int bit=7;bit>=0;bit--){int one=(bytes[j]>>bit)&1;symbols[at++]=(rmt_symbol_word_t){.level0=1,.duration0=one?9:3,.level1=0,.duration1=one?3:9};}
 symbols[24]=(rmt_symbol_word_t){.level0=0,.duration0=400,.level1=0,.duration1=400};
 rmt_transmit_config_t cfg={.loop_count=0};esp_err_t e=rmt_transmit(channel,encoder,symbols,sizeof symbols,&cfg);
 return e==ESP_OK?rmt_tx_wait_all_done(channel,20):e;
}
static void animate(void *unused) {
 (void)unused;
 for(;;){
  xSemaphoreTake(lock,portMAX_DELAY);
  unsigned red=r,green=g,blue=b,p=period,gen=generation;int on=enabled;char mode[12];memcpy(mode,effect,sizeof mode);
  xSemaphoreGive(lock);
  unsigned phase=(unsigned)((esp_timer_get_time()/1000)%p),scale=64;
  if(!on)scale=0;
  else if(!strcmp(mode,"blink"))scale=phase<p/2?64:0;
  else if(!strcmp(mode,"breathe"))scale=phase<p/2?phase*128/p:(p-phase)*128/p;
  else if(!strcmp(mode,"rainbow")){
   unsigned wheel=phase*768/p,part=wheel/256,x=wheel%256;
   red=part==0?255-x:part==1?0:x;green=part==0?x:part==1?255-x:0;blue=part==0?0:part==1?x:255-x;
  }
  esp_err_t e=driver_error?driver_error:pixel(red*scale/255,green*scale/255,blue*scale/255);
  xSemaphoreTake(lock,portMAX_DELAY);driver_error=e;if(gen!=ack){ack=gen;xSemaphoreGive(applied);}xSemaphoreGive(lock);
  ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(30));
 }
}
int ae_board_led_pin(void){return pin;}
int ae_board_led_configure(unsigned selected) {
 if(pin>=0)return pin==(int)selected;
 if(selected!=38 && selected!=48)return 0;
 lock=xSemaphoreCreateMutex();applied=xSemaphoreCreateBinary();
 if(!lock || !applied)goto fail;
 rmt_tx_channel_config_t cfg={.gpio_num=(int)selected,.clk_src=RMT_CLK_SRC_DEFAULT,.resolution_hz=10000000,.mem_block_symbols=64,.trans_queue_depth=1};
 rmt_copy_encoder_config_t copy={0};
 if(rmt_new_tx_channel(&cfg,&channel)!=ESP_OK || rmt_new_copy_encoder(&copy,&encoder)!=ESP_OK || rmt_enable(channel)!=ESP_OK)goto fail;
 if(xTaskCreate(animate,"dunlin_led",3072,NULL,3,&task)!=pdPASS)goto fail;
 pin=(int)selected;return 1;
fail:
 if(encoder){rmt_del_encoder(encoder);encoder=NULL;}
 if(channel){rmt_disable(channel);rmt_del_channel(channel);channel=NULL;}
 if(lock){vSemaphoreDelete(lock);lock=NULL;}if(applied){vSemaphoreDelete(applied);applied=NULL;}return 0;
}
int ae_board_led_apply(const char *mode,unsigned red,unsigned green,unsigned blue,unsigned ms,char *out,size_t cap) {
 if(pin<0){snprintf(out,cap,"unsupported: onboard LED pin has not been confirmed by operator; no GPIO changed");return 0;}
 xSemaphoreTake(lock,portMAX_DELAY);xSemaphoreTake(applied,0);
 if(!strcmp(mode,"off"))enabled=0;
 else if(!strcmp(mode,"on"))enabled=1;
 else{enabled=1;r=red;g=green;b=blue;period=ms;snprintf(effect,sizeof effect,"%s",mode);}
 unsigned wanted=++generation;xSemaphoreGive(lock);xTaskNotifyGive(task);
 if(xSemaphoreTake(applied,pdMS_TO_TICKS(250))!=pdTRUE){snprintf(out,cap,"unknown: LED driver acknowledgement timeout; do not replay automatically");return 0;}
 xSemaphoreTake(lock,portMAX_DELAY);int ok=ack==wanted && driver_error==ESP_OK;
 snprintf(out,cap,"%s: pin=%d on=%d mode=%s rgb=%u,%u,%u period_ms=%u; brightness capped at 25%%; electrical transmit only, no optical sensor",ok?"applied":"io_error",pin,enabled,effect,r,g,b,period);xSemaphoreGive(lock);return ok;
}

int ae_board_led_status(char *out,size_t cap){
 if(pin<0){snprintf(out,cap,"LED is not configured");return 0;}
 xSemaphoreTake(lock,portMAX_DELAY);
 snprintf(out,cap,"pin=%d on=%d mode=%s rgb=%u,%u,%u period_ms=%u driver=%s",pin,enabled,effect,r,g,b,period,driver_error?"error":"ready");
 xSemaphoreGive(lock);return 1;
}
