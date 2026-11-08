#include "dunlin/message_projection.h"
#include <json-c/json.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(const char *raw,int expected,const char *text,const char *kind){
 struct json_object *m=json_tokener_parse(raw);assert(m);const char *original=json_object_to_json_string_ext(m,JSON_C_TO_STRING_PLAIN);char *before=malloc(strlen(original)+1);assert(before);strcpy(before,original);
 char *out=(char*)1;const char *format=(char*)1;int rc=dl_message_reasoning(m,&out,&format);assert(rc==expected);
 if(rc==1){assert(out&&format&&!strcmp(out,text)&&!strcmp(format,kind));assert(!strstr(out,"PRIVATE"));}else assert(!out&&!format);
 assert(!strcmp(before,json_object_to_json_string_ext(m,JSON_C_TO_STRING_PLAIN)));free(before);dl_message_projection_free(out);json_object_put(m);
}
int main(void){
 check("{\"role\":\"assistant\",\"content\":\"Answer\",\"reasoning_content\":\"公开思路 🐦\"}",1,"公开思路 🐦","reasoning");
 check("{\"role\":\"user\",\"reasoning_content\":\"not model reasoning\"}",0,NULL,NULL);
 check("{\"role\":\"assistant\",\"content\":\"<think>ordinary original text</think>\",\"thinking\":\"PRIVATE\"}",0,NULL,NULL);
 check("{\"role\":\"assistant\",\"reasoning_content\":null}",-1,NULL,NULL);
 check("{\"role\":\"assistant\",\"reasoning_content\":false}",-1,NULL,NULL);
 check("{\"role\":\"assistant\",\"reasoning_content\":\"\"}",0,NULL,NULL);
 check("{\"role\":\"assistant\",\"reasoning_content\":\"first\\u0000PRIVATE\"}",-1,NULL,NULL);
 check("{\"role\":\"assistant\",\"_dunlin_provider\":{\"protocol\":\"responses\",\"output\":[{\"type\":\"reasoning\",\"id\":\"PRIVATE\",\"encrypted_content\":\"PRIVATE\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"One\"},{\"type\":\"summary_text\",\"text\":\"\"},{\"type\":\"summary_text\",\"text\":\"Two\"}]},{\"type\":\"message\",\"content\":\"PRIVATE\"}]}}",1,"One\nTwo","summary");
 check("{\"role\":\"assistant\",\"_dunlin_provider\":{\"protocol\":\"responses\",\"output\":[{\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":\"PRIVATE\"}]}}",0,NULL,NULL);
 check("{\"role\":\"assistant\",\"_dunlin_provider\":{\"protocol\":\"responses\",\"output\":[{\"type\":\"reasoning\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"valid prefix\"},{\"type\":\"text\",\"text\":\"PRIVATE\"}]}]}}",-1,NULL,NULL);
 check("{\"role\":\"assistant\",\"_dunlin_provider\":{\"protocol\":\"anthropic\",\"output\":[{\"type\":\"thinking\",\"thinking\":\"Plan A\",\"signature\":\"PRIVATE\"},{\"type\":\"redacted_thinking\",\"data\":\"PRIVATE\"},{\"type\":\"thinking\",\"thinking\":\"Plan B\"},{\"type\":\"text\",\"text\":\"Answer\"}]}}",1,"Plan A\nPlan B","reasoning");
 check("{\"role\":\"assistant\",\"_dunlin_provider\":{\"protocol\":\"ollama\",\"output\":{\"thinking\":\"Local plan\",\"content\":\"Answer\"}}}",1,"Local plan","reasoning");
 check("{\"role\":\"assistant\",\"_dunlin_provider\":{\"protocol\":\"gemini\",\"output\":{\"parts\":[{\"thought\":true,\"text\":\"First\",\"thoughtSignature\":\"PRIVATE\"},{\"thought\":true,\"text\":\" part\"},{\"text\":\"Answer\"},{\"thoughtSignature\":\"PRIVATE\"}]}}}",1,"First part","reasoning");
 check("{\"role\":\"assistant\",\"_dunlin_provider\":{\"protocol\":\"gemini\",\"output\":{\"parts\":[{\"thought\":\"true\",\"text\":\"PRIVATE\"}]}}}",-1,NULL,NULL);
 check("{\"role\":\"assistant\",\"reasoning_content\":\"PRIVATE fallback\",\"_dunlin_provider\":{\"protocol\":\"unknown\",\"output\":{\"thinking\":\"PRIVATE\"}}}",0,NULL,NULL);
 check("{\"role\":\"assistant\",\"_dunlin_provider\":{\"protocol\":\"anthropic\",\"output\":{}}}",-1,NULL,NULL);
 struct json_object *m=json_object_new_object();json_object_object_add(m,"role",json_object_new_string("assistant"));
 const unsigned char *invalid[]={ (unsigned char*)"\xc0\xaf",(unsigned char*)"\xed\xa0\x80",(unsigned char*)"\xf4\x90\x80\x80",(unsigned char*)"\xe2\x82",(unsigned char*)"\x80" };
 for(size_t i=0;i<sizeof invalid/sizeof *invalid;i++){json_object_object_add(m,"reasoning_content",json_object_new_string((const char*)invalid[i]));char *s=NULL;const char *k=NULL;assert(dl_message_reasoning(m,&s,&k)==-1&&!s&&!k);}
 size_t cap=4u*1024u*1024u;char *large=malloc(cap+2);assert(large);memset(large,'x',cap+1);large[cap+1]=0;
 json_object_object_add(m,"reasoning_content",json_object_new_string_len(large,(int)cap));char *s=NULL;const char *k=NULL;assert(dl_message_reasoning(m,&s,&k)==1&&strlen(s)==cap);dl_message_projection_free(s);
 json_object_object_add(m,"reasoning_content",json_object_new_string(large));assert(dl_message_reasoning(m,&s,&k)==-1&&!s&&!k);free(large);json_object_put(m);
 puts("PASS public protocol whitelist, private opaque exclusion, exact Unicode/source preservation, invalid types/NUL/UTF-8 and bounded complete projection");return 0;
}
