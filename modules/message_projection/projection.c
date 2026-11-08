#include "dunlin/message_projection.h"
#include <json-c/json.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct json_object J;
typedef struct { char *text; size_t len,capacity; } buffer;
#define PUBLIC_LIMIT (4u * 1024u * 1024u)
static J *field(J *o,const char *key){J *v=NULL;if(json_object_is_type(o,json_type_object))json_object_object_get_ex(o,key,&v);return v;}
static int present(J *o,const char *key){J *v=NULL;return json_object_is_type(o,json_type_object)&&json_object_object_get_ex(o,key,&v);}
static int is(J *v,const char *s){return json_object_is_type(v,json_type_string)&&(size_t)json_object_get_string_len(v)==strlen(s)&&!memcmp(json_object_get_string(v),s,strlen(s));}
/* Strict scalar UTF-8. Reject embedded NUL, overlong encodings and surrogates. */
static int utf8(const unsigned char *s,size_t n){
 for(size_t i=0;i<n;){unsigned char a=s[i++];uint32_t code;size_t more;
  if(!a)return 0;if(a<0x80)continue;
  if(a>=0xc2&&a<=0xdf){code=a&31;more=1;}
  else if(a>=0xe0&&a<=0xef){code=a&15;more=2;}
  else if(a>=0xf0&&a<=0xf4){code=a&7;more=3;}else return 0;
  if(more>n-i)return 0;size_t count=more;
  while(more--){unsigned char b=s[i++];if((b&0xc0)!=0x80)return 0;code=(code<<6)|(b&63);}
  if((count==1&&code<0x80)||(count==2&&code<0x800)||(count==3&&code<0x10000)||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return 0;
 }return 1;
}
static int append(buffer *b,J *v,int separate){
 if(!json_object_is_type(v,json_type_string))return 0;
 const char *s=json_object_get_string(v);size_t n=(size_t)json_object_get_string_len(v);
 if(n>PUBLIC_LIMIT||!utf8((const unsigned char*)s,n))return 0;
 if(!n)return 1;size_t gap=separate&&b->len?1:0;
 if(n>PUBLIC_LIMIT-b->len||gap>PUBLIC_LIMIT-b->len-n)return 0;
 size_t needed=b->len+gap+n+1;
 if(needed>b->capacity){size_t cap=b->capacity?b->capacity:256;while(cap<needed)cap*=2;if(cap>PUBLIC_LIMIT+1)cap=PUBLIC_LIMIT+1;char *p=realloc(b->text,cap);if(!p)return 0;b->text=p;b->capacity=cap;}
 if(gap)b->text[b->len++]='\n';memcpy(b->text+b->len,s,n);b->len+=n;b->text[b->len]=0;return 1;
}
int dl_message_reasoning(J *message,char **text,const char **kind){
 if(!text||!kind)return -1;*text=NULL;*kind=NULL;
 if(!is(field(message,"role"),"assistant"))return 0;
 buffer b={0};int ok=1;const char *format="reasoning";J *meta=field(message,"_dunlin_provider");
 if(meta){
  J *protocol=field(meta,"protocol"),*output=field(meta,"output");
  if(is(protocol,"responses")){
   format="summary";ok=json_object_is_type(output,json_type_array);
   for(size_t i=0;ok&&i<json_object_array_length(output);i++){
    J *item=json_object_array_get_idx(output,i);if(!is(field(item,"type"),"reasoning"))continue;
    J *summary=field(item,"summary");ok=json_object_is_type(summary,json_type_array);
    for(size_t j=0;ok&&j<json_object_array_length(summary);j++){J *part=json_object_array_get_idx(summary,j);ok=is(field(part,"type"),"summary_text")&&append(&b,field(part,"text"),1);}
   }
  }else if(is(protocol,"anthropic")){
   ok=json_object_is_type(output,json_type_array);
   for(size_t i=0;ok&&i<json_object_array_length(output);i++){J *part=json_object_array_get_idx(output,i);if(is(field(part,"type"),"thinking"))ok=append(&b,field(part,"thinking"),1);}
  }else if(is(protocol,"ollama")){
   ok=json_object_is_type(output,json_type_object);if(ok&&present(output,"thinking"))ok=append(&b,field(output,"thinking"),0);
  }else if(is(protocol,"gemini")){
   J *parts=field(output,"parts");ok=json_object_is_type(parts,json_type_array);
   for(size_t i=0;ok&&i<json_object_array_length(parts);i++){
    J *part=json_object_array_get_idx(parts,i),*thought=field(part,"thought"),*v=field(part,"text");
    if(present(part,"thought")&&!json_object_is_type(thought,json_type_boolean)){ok=0;break;}
    if(json_object_get_boolean(thought)&&present(part,"text"))ok=append(&b,v,0);
   }
  }
  /* Unknown/opaque provider formats do not fall back to arbitrary message data. */
 }else if(present(message,"reasoning_content"))ok=append(&b,field(message,"reasoning_content"),0);
 if(!ok){free(b.text);return -1;}
 if(!b.len){free(b.text);return 0;}*text=b.text;*kind=format;return 1;
}
void dl_message_projection_free(void *text){free(text);}
