#include "profile.h"
#include "hal.h"
#include "agent_host.h"
#include "nvs.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
static int restoring;
int ae_profile_restoring_wifi(void){return restoring;}
#define MAGIC UINT32_C(0x41484b31)
typedef struct {uint32_t magic,version;ae_saved_wifi data;} wifi_record;
typedef struct {uint32_t magic,version;ae_saved_model data;} model_record;
typedef struct {uint32_t magic,version;char url[128],model[96],key[128];} model_record_v1;
static void wipe(void *p,size_t n){volatile unsigned char *b=p;while(n--)*b++=0;}
static int terminated(const char *s,size_t cap){return memchr(s,0,cap)!=NULL;}
static int load(const char *key,void *data,size_t size){
 if(ae_esp_radio_prepare()!=ESP_OK)return -1;
 nvs_handle_t h;esp_err_t e=nvs_open("dunlin_agent",NVS_READONLY,&h);if(e==ESP_ERR_NVS_NOT_FOUND)return 0;if(e!=ESP_OK)return -1;
 size_t actual=size;e=nvs_get_blob(h,key,data,&actual);nvs_close(h);
 if(e==ESP_ERR_NVS_NOT_FOUND)return 0;
 return e==ESP_OK && actual==size?1:-1;
}
static esp_err_t save(const char *key,const void *data,size_t size){
 esp_err_t e=ae_esp_radio_prepare();if(e!=ESP_OK)return e;
 nvs_handle_t h;e=nvs_open("dunlin_agent",NVS_READWRITE,&h);if(e!=ESP_OK)return e;
 e=nvs_set_blob(h,key,data,size);if(e==ESP_OK)e=nvs_commit(h);
 nvs_close(h);return e;
}
int ae_profile_load_wifi(ae_saved_wifi *out){
 wifi_record r={0};memset(out,0,sizeof *out);int found=load("wifi",&r,sizeof r);
 if(found==1 && (r.magic!=MAGIC || r.version!=1 || !terminated(r.data.ssid,sizeof r.data.ssid) || !r.data.ssid[0] || !terminated(r.data.password,sizeof r.data.password)))found=-1;
 if(found==1)*out=r.data;
 wipe(&r,sizeof r);return found;
}
int ae_profile_load_model(ae_saved_model *out){
 model_record r={0};memset(out,0,sizeof *out);int found=load("model",&r,sizeof r);
 if(found==1 && (r.magic!=MAGIC || r.version!=2 || !terminated(r.data.url,sizeof r.data.url) || !r.data.url[0] || !terminated(r.data.model,sizeof r.data.model) || !r.data.model[0] || !terminated(r.data.key,sizeof r.data.key) || !terminated(r.data.effort,sizeof r.data.effort) || (strcmp(r.data.effort,"low") && strcmp(r.data.effort,"xhigh"))))found=-1;
 if(found==1)*out=r.data;
 else if(found<0){
  model_record_v1 old={0};int legacy=load("model",&old,sizeof old);
  if(legacy==1 && old.magic==MAGIC && old.version==1 && terminated(old.url,sizeof old.url) && old.url[0] && terminated(old.model,sizeof old.model) && old.model[0] && terminated(old.key,sizeof old.key)){
   strcpy(out->url,old.url);strcpy(out->model,old.model);strcpy(out->key,old.key);strcpy(out->effort,"low");found=1;
  }
  wipe(&old,sizeof old);
 }
 wipe(&r,sizeof r);return found;
}
esp_err_t ae_profile_save_wifi(const char *ssid,const char *password){
 if(!ssid || !password || !*ssid || strlen(ssid)>=33 || strlen(password)>=65)return ESP_ERR_INVALID_ARG;
 wifi_record r={.magic=MAGIC,.version=1};strcpy(r.data.ssid,ssid);strcpy(r.data.password,password);esp_err_t e=save("wifi",&r,sizeof r);wipe(&r,sizeof r);return e;
}
esp_err_t ae_profile_save_model(const char *url,const char *model,const char *key){
 return ae_profile_save_model_options(url,model,key,"low");
}
esp_err_t ae_profile_save_model_options(const char *url,const char *model,const char *key,const char *effort){
 if(!url || !model || !key || !effort || !*url || !*model || strlen(url)>=128 || strlen(model)>=96 || strlen(key)>=256 || (strcmp(effort,"low") && strcmp(effort,"xhigh")))return ESP_ERR_INVALID_ARG;
 model_record r={.magic=MAGIC,.version=2};strcpy(r.data.url,url);strcpy(r.data.model,model);strcpy(r.data.key,key);strcpy(r.data.effort,effort);esp_err_t e=save("model",&r,sizeof r);wipe(&r,sizeof r);return e;
}
esp_err_t ae_profile_forget(const char *key){
 if(strcmp(key,"wifi") && strcmp(key,"model"))return ESP_ERR_INVALID_ARG;
 esp_err_t e=ae_esp_radio_prepare();if(e!=ESP_OK)return e;
 nvs_handle_t h;e=nvs_open("dunlin_agent",NVS_READWRITE,&h);if(e!=ESP_OK)return e;
 e=nvs_erase_key(h,key);if(e==ESP_ERR_NVS_NOT_FOUND)e=ESP_OK;
 if(e==ESP_OK)e=nvs_commit(h);
 nvs_close(h);return e;
}
void ae_esp_profile_start_wifi(void *data){
 ae_io_plugin *io=data;ae_saved_wifi wifi;int found=ae_profile_load_wifi(&wifi);
 if(found==1){
  ae_request request={.op=AE_WIFI_SET};strcpy(request.name,wifi.ssid);strcpy(request.secret,wifi.password);
  restoring=1;ae_reply r=io->service.call(io->service.user,&request);restoring=0;wipe(&request,sizeof request);
  if(r.status==AE_OK){request.op=AE_WIFI_CONNECT;r=io->service.call(io->service.user,&request);}
  ae_esp_console_print(r.status==AE_OK?"[启动] 已恢复Wi-Fi配置，正在连接；/wifi status可查看状态。":"[启动] Wi-Fi恢复失败；请检查/wifi status或重新配置。");
 }else if(found<0)ae_esp_console_print("[启动] 保存的Wi-Fi配置无效或NVS不可用，未自动连接。");
 wipe(&wifi,sizeof wifi);
}
