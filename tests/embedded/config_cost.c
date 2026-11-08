/* Controlled host microbenchmark: configuration access only, not whole-system cost. */
#define _POSIX_C_SOURCE 200809L
#include "cJSON.h"
#include "../../dunlin-copilot/esp32/esp32/config/python_defaults.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
static size_t live,peak,allocations;
static void *allocate(size_t n){size_t *p=malloc(n+sizeof(size_t));if(!p)return NULL;*p=n;live+=n;if(live>peak)peak=live;allocations++;return p+1;}
static void release(void *v){if(v){size_t *p=(size_t *)v-1;live-=*p;free(p);}}
static double clock_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
typedef struct {uint32_t heap,script_ms,job_ms,code,out,lines,http,max_hosts,ntp_wait;char hosts[AP_DEFAULT_HOST_COUNT][96],ntp[96],profile[48],version[16];} binary;
static const binary defaults={AP_VM_HEAP_BYTES,AP_SCRIPT_MS,AP_JOB_MS,AP_CODE_BYTES,AP_STDOUT_BYTES,AP_STDOUT_LINES,AP_HTTPS_TIMEOUT_MS,AP_MAX_ALLOWED_HOSTS,AP_NTP_WAIT_MS,AP_DEFAULT_HOSTS,AP_NTP_SERVER,"verified-esp32s3-2101","1.26.1"};
static volatile uint64_t sink;
__attribute__((noinline)) static void copy_blob(binary *out){memcpy(out,&defaults,sizeof *out);__asm__ volatile("" ::: "memory");}
int main(void){
 FILE *f=fopen("embedded/esp32/config/python.json","rb");assert(f);char text[2048];size_t bytes=fread(text,1,sizeof text-1,f);text[bytes]=0;fclose(f);
 const unsigned direct_n=1000000,json_n=10000;double began=clock_ms();
 for(unsigned i=0;i<direct_n;i++)sink+=defaults.heap+defaults.script_ms+defaults.hosts[0][0];
 double direct=clock_ms()-began;binary cached;began=clock_ms();
 for(unsigned i=0;i<direct_n;i++){copy_blob(&cached);sink+=cached.heap+cached.script_ms+cached.hosts[0][0];}
 double blob=clock_ms()-began;cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);began=clock_ms();
 for(unsigned i=0;i<json_n;i++){cJSON *j=cJSON_Parse(text);assert(j);sink+=(unsigned)cJSON_GetObjectItemCaseSensitive(j,"vm_heap_bytes")->valueint+(unsigned)cJSON_GetObjectItemCaseSensitive(j,"script_ms")->valueint+cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j,"default_allowed_hosts"),0)->valuestring[0];cJSON_Delete(j);assert(!live);}
 double parse=clock_ms()-began;cJSON_InitHooks(NULL);
 printf("{\"scope\":\"native host configuration CPU and tracked parser allocation only; no filesystem/NVS flash IO, model, RTOS, GPU or whole-system memory\",\"input_json_bytes\":%zu,\"typed_record_bytes\":%zu,\"const_lookup_ns\":%.2f,\"typed_copy_ns\":%.2f,\"json_parse_free_ns\":%.2f,\"json_peak_live_payload_bytes\":%zu,\"json_allocations_per_parse\":%.1f,\"const_or_copy_dynamic_allocations\":0,\"repetitions\":{\"const_copy\":%u,\"json\":%u}}\n",bytes,sizeof(binary),direct*1e6/direct_n,blob*1e6/direct_n,parse*1e6/json_n,peak,(double)allocations/json_n,direct_n,json_n);
}
