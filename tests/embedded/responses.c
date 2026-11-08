#include "../../dunlin-copilot/esp32/agent/responses.h"
#include "../../dunlin-copilot/esp32/agent/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
static const char first[]="{\"model\":\"gpt-6.1-sol\",\"temperature\":0,\"enable_thinking\":false,\"messages\":[{\"role\":\"system\",\"content\":\"device\"},{\"role\":\"user\",\"content\":\"off\"}],\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"led_control\",\"description\":\"LED\",\"parameters\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\"}},\"required\":[\"mode\"],\"additionalProperties\":false}}}]}";
static const char call[]="{\"id\":\"resp_first\",\"status\":\"completed\",\"output\":[{\"type\":\"reasoning\",\"encrypted_content\":\"opaque\"},{\"type\":\"function_call\",\"id\":\"fc_NOT_CALL_ID\",\"call_id\":\"call_real\",\"name\":\"led_control\",\"arguments\":\"{\\\"mode\\\":\\\"off\\\"}\"}],\"usage\":{\"input_tokens\":10,\"output_tokens\":12,\"output_tokens_details\":{\"reasoning_tokens\":5}}}";
static const char next[]="{\"model\":\"gpt-6.1-sol\",\"messages\":[{\"role\":\"system\",\"content\":\"device\"},{\"role\":\"user\",\"content\":\"off\"},{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[]},{\"role\":\"tool\",\"tool_call_id\":\"call_real\",\"content\":\"applied off\"}],\"tools\":[]}";
static const char final[]="{\"id\":\"resp_final\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"turned \"},{\"type\":\"output_text\",\"text\":\"off\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":12,\"output_tokens_details\":{\"reasoning_tokens\":5}}}";
typedef union {max_align_t align;size_t size;} head;
static size_t allocations,fail_at,live;
static void *allocate(size_t n){if(++allocations==fail_at)return NULL;head *p=malloc(sizeof *p+n);if(!p)return NULL;live++;return p+1;}
static void release(void *v){if(v){assert(live);live--;free((head *)v-1);}}
int main(void){
 ae_responses s;ae_responses_reset(&s);char out[8192];
 char *wire=ae_responses_request(&s,first,"xhigh",4096);assert(wire);cJSON *j=ae_json_parse_bounded(wire,32768);assert(j);assert(!cJSON_GetObjectItemCaseSensitive(j,"temperature") && !cJSON_GetObjectItemCaseSensitive(j,"enable_thinking"));
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j,"reasoning"),"effort")->valuestring,"xhigh"));assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"store")));cJSON_Delete(j);cJSON_free(wire);
 assert(ae_responses_reply(&s,call,out,sizeof out) && strstr(out,"call_real") && !strstr(out,"fc_NOT_CALL_ID"));
 wire=ae_responses_request(&s,next,"xhigh",4096);assert(wire);j=ae_json_parse_bounded(wire,32768);assert(j);assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"previous_response_id")->valuestring,"resp_first"));cJSON *input=cJSON_GetObjectItemCaseSensitive(j,"input");assert(cJSON_GetArraySize(input)==1);assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(input,0),"call_id")->valuestring,"call_real"));cJSON_Delete(j);cJSON_free(wire);
 /* A function item id is never a valid output correlation id. */
 char wrong[sizeof next];memcpy(wrong,next,sizeof next);char *correlation=strstr(wrong,"call_real");assert(correlation);memcpy(correlation,"call_fake",9);
 assert(!ae_responses_request(&s,wrong,"xhigh",4096));
 assert(ae_responses_reply(&s,final,out,sizeof out) && strstr(out,"turned off") && !s.pending_call[0]);assert(s.input_tokens==20 && s.output_tokens==24 && s.reasoning_tokens==10 && s.usage_known==7);
 ae_responses_reset(&s);assert(!ae_responses_request(&s,next,"low",4096));
 assert(!ae_responses_request(&s,first,"max",4096));
 const char *bad[]={"{}","{\"id\":\"x\",\"status\":\"incomplete\",\"output\":[]}","{\"id\":\"x\",\"status\":\"failed\",\"output\":[]}","{\"id\":\"x\",\"id\":\"y\",\"status\":\"completed\",\"output\":[]}","{\"id\":\"x\",\"status\":\"completed\",\"output\":[{\"type\":\"web_search_call\"}]}"};
 for(size_t i=0;i<sizeof bad/sizeof *bad;i++){ae_responses_reset(&s);assert(!ae_responses_reply(&s,bad[i],out,sizeof out) && !s.previous_id[0] && !s.calls);}
 assert(!ae_responses_reply(&s,call,out,16) && !s.previous_id[0]);
 cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);
 allocations=fail_at=0;ae_responses_reset(&s);wire=ae_responses_request(&s,first,"low",4096);assert(wire);cJSON_free(wire);assert(!live);size_t requests=allocations;
 for(size_t i=1;i<=requests;i++){allocations=0;fail_at=i;ae_responses_reset(&s);wire=ae_responses_request(&s,first,"low",4096);cJSON_free(wire);assert(!live);}
 allocations=fail_at=0;ae_responses_reset(&s);assert(ae_responses_reply(&s,call,out,sizeof out));assert(!live);size_t replies=allocations;
 for(size_t i=1;i<=replies;i++){allocations=0;fail_at=i;ae_responses_reset(&s);(void)ae_responses_reply(&s,call,out,sizeof out);assert(!live);}
 cJSON_InitHooks(NULL);printf("PASS Responses conversion, low/xhigh, exact call_id continuation, no Qwen-only fields, fragments, usage, failed/incomplete/duplicate/size guards; %zu request and %zu reply allocation positions clean\n",requests,replies);
}
