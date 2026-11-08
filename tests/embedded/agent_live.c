/* Existing real model transport; hardware substituted explicitly. Never flashes or
 * accesses GPIO. Production firmware uses esp_http_client and the RMT driver. */
#include "../../dunlin-copilot/esp32/agent/agent.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static struct {char *out;size_t used,cap;unsigned effects;} state;
static size_t receive(char *data,size_t size,size_t count,void *unused) {
 (void)unused;if(count && size>(size_t)-1/count)return 0;size_t n=size*count;
 if(n>=state.cap-state.used)return 0;memcpy(state.out+state.used,data,n);state.used+=n;state.out[state.used]=0;return n;
}
static int exchange(void *unused,const char *body,char *out,size_t capacity) {
 (void)unused;CURL *curl=curl_easy_init();if(!curl)return 0;
 state.out=out;state.cap=capacity;state.used=0;
 struct curl_slist *headers=NULL;headers=curl_slist_append(headers,"Content-Type: application/json");
 const char *key=getenv("OMLX_API_KEY");char bearer[512];
 if(key && strlen(key)<450){snprintf(bearer,sizeof bearer,"Authorization: Bearer %s",key);headers=curl_slist_append(headers,bearer);memset(bearer,0,sizeof bearer);}
 curl_easy_setopt(curl,CURLOPT_URL,"http://127.0.0.1:1234/v1/chat/completions");curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
 curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body);curl_easy_setopt(curl,CURLOPT_TIMEOUT,30L);curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
 curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);CURLcode rc=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
 curl_slist_free_all(headers);curl_easy_cleanup(curl);return rc==CURLE_OK && status==200;
}
static int led(void *unused,const ae_led_action *a,char *out,size_t size) {
 (void)unused;state.effects++;snprintf(out,size,"host fixture recorded mode=%s rgb=%u,%u,%u period_ms=%u; no physical GPIO",a->mode,a->red,a->green,a->blue,a->period_ms);return 1;
}
static void print(void *unused,const char *s){(void)unused;puts(s);fflush(stdout);}
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 const char *device="{\"device\":\"ESP32-S3\",\"os\":\"FreeRTOS\",\"shell\":false,\"linux\":false,\"package_manager\":false,\"led_available\":true,\"hardware_evidence\":\"host substitute, no physical GPIO\"}";
 ae_agent a={NULL,device,strcmp(argv[1],"blind")!=0,exchange,led,print,NULL,NULL,NULL,NULL};
 int status=ae_agent_turn(&a,"Qwen3.5-4B-Uncensored-MLX-4bit",argv[2]);printf("agent_status=%d fixture_effects=%u\n",status,state.effects);return status==AE_AGENT_STOPPED;
}
