#include "../../dunlin-copilot/esp32/esp32/src/profile.h"
#include "../../dunlin-copilot/esp32/esp32/src/hal.h"
#include "../../dunlin-copilot/esp32/esp32/src/agent_host.h"
#include "nvs.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
static unsigned char stored[2][512];static size_t sizes[2];static int fail,sets,commits,io_calls,io_fail;
static int slot(const char *key){assert(!strcmp(key,"wifi") || !strcmp(key,"model"));return !strcmp(key,"model");}
int ae_esp_radio_prepare(void){return fail==1?ESP_FAIL:ESP_OK;}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){assert(!strcmp(ns,"dunlin_agent"));(void)mode;*h=1;return fail==2?ESP_FAIL:ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *data,size_t *size){assert(h==1);int i=slot(key);if(!sizes[i])return ESP_ERR_NVS_NOT_FOUND;if(*size<sizes[i])return ESP_ERR_NVS_INVALID_LENGTH;memcpy(data,stored[i],sizes[i]);*size=sizes[i];return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size){assert(h==1 && size<512);sets++;if(fail==3)return ESP_FAIL;int i=slot(key);memcpy(stored[i],data,size);sizes[i]=size;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);commits++;return fail==4?ESP_FAIL:ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1);}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key){assert(h==1);int i=slot(key);if(!sizes[i])return ESP_ERR_NVS_NOT_FOUND;sizes[i]=0;return ESP_OK;}
void ae_esp_console_print(const char *s){assert(s);}
static ae_reply io_call(void *unused,const ae_request *r){
 (void)unused;io_calls++;
 if(io_calls==1){assert(r->op==AE_WIFI_SET && ae_profile_restoring_wifi());assert(!strcmp(r->name,"fixture-ssid"));}
 else assert(r->op==AE_WIFI_CONNECT && !ae_profile_restoring_wifi());
 return (ae_reply){io_fail?AE_IO_ERROR:AE_OK,"fixture"};
}
int main(void){
 ae_saved_wifi w;ae_saved_model m;
 assert(!ae_profile_load_wifi(&w) && !ae_profile_load_model(&m));
 char long_key[181];memset(long_key,'k',sizeof long_key-1);long_key[sizeof long_key-1]=0;
 assert(ae_profile_save_model("http://192.168.8.1:1235/v1/chat/completions","fixture-model",long_key)==ESP_OK);
 assert(ae_profile_load_model(&m)==1 && !strcmp(m.key,long_key));
 assert(ae_profile_forget("model")==ESP_OK);
 assert(ae_profile_save_wifi("fixture-ssid","fixture-password")==ESP_OK);
 assert(ae_profile_save_model("http://192.168.8.1:1235/v1/chat/completions","fixture-model","fixture-key")==ESP_OK);
 assert(ae_profile_load_wifi(&w)==1 && !strcmp(w.password,"fixture-password"));
 assert(ae_profile_load_model(&m)==1 && !strcmp(m.key,"fixture-key"));
 assert(ae_profile_save_model_options("http://192.168.8.1:1235/v1/chat/completions","fixture-model",long_key,"xhigh")==ESP_OK);
 assert(ae_profile_load_model(&m)==1 && !strcmp(m.key,long_key) && !strcmp(m.effort,"xhigh"));
 assert(ae_profile_save_model_options("x","y","z","max")!=ESP_OK);
 struct {uint32_t magic,version;char url[128],model[96],key[128];} old={.magic=UINT32_C(0x41484b31),.version=1,.url="http://192.168.8.1:1235/v1/chat/completions",.model="legacy",.key="legacy-key"};
 memcpy(stored[1],&old,sizeof old);sizes[1]=sizeof old;
 assert(ae_profile_load_model(&m)==1 && !strcmp(m.model,"legacy") && !strcmp(m.key,"legacy-key") && !strcmp(m.effort,"low"));
 memset(stored[1]+232,'x',128);assert(ae_profile_load_model(&m)==-1 && !m.key[0]);
 for(int i=1;i<=4;i++){fail=i;assert(ae_profile_save_wifi("fixture","fixture-password")!=ESP_OK);assert(ae_profile_save_model("http://192.168.8.1:1235/v1/chat/completions","fixture","key")!=ESP_OK);}fail=0;
 assert(ae_profile_save_wifi("fixture-ssid","fixture-password")==ESP_OK);stored[0][0]^=1;assert(ae_profile_load_wifi(&w)==-1 && !w.ssid[0]);
 assert(ae_profile_save_wifi("fixture-ssid","fixture-password")==ESP_OK);sizes[0]--;assert(ae_profile_load_wifi(&w)==-1 && !w.ssid[0]);
 assert(ae_profile_save_wifi("fixture-ssid","fixture-password")==ESP_OK);memset(stored[0]+8,'x',33);assert(ae_profile_load_wifi(&w)==-1);
 assert(ae_profile_save_wifi("fixture-ssid","fixture-password")==ESP_OK);stored[0][4]^=1;assert(ae_profile_load_wifi(&w)==-1);
 for(int field=0;field<3;field++){
  assert(ae_profile_save_model("http://192.168.8.1:1235/v1/chat/completions","fixture","key")==ESP_OK);
  size_t offset=field==0?8:field==1?136:232,length=field==1?96:field==2?256:128;
  memset(stored[1]+offset,'x',length);assert(ae_profile_load_model(&m)==-1 && !m.url[0]);
 }
 assert(ae_profile_save_wifi("fixture-ssid","fixture-password")==ESP_OK);
 assert(ae_profile_save_model("http://192.168.8.1:1235/v1/chat/completions","fixture","key")==ESP_OK);
 ae_io_plugin io={.service={AE_ABI,NULL,io_call}};int previous_sets=sets;
 ae_esp_profile_start_wifi(&io);assert(io_calls==2 && !ae_profile_restoring_wifi() && sets==previous_sets);
 io_calls=0;io_fail=1;ae_esp_profile_start_wifi(&io);assert(io_calls==1 && !ae_profile_restoring_wifi());
 assert(ae_profile_forget("wifi")==ESP_OK && !ae_profile_load_wifi(&w));assert(ae_profile_load_model(&m)==1);assert(ae_profile_forget("model")==ESP_OK && !ae_profile_load_model(&m));assert(ae_profile_forget("other")==ESP_ERR_INVALID_ARG);
 assert(sets>0 && commits>0);puts("PASS profile roundtrip, missing/version/truncated/unterminated records, isolated namespace, init/open/set/commit failures and independent forget");
}
