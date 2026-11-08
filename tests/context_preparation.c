#include "dunlin/context_prep.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){
 dc_source_span spans[4];int partial=0;
 const char *s="你好。不要删除 src/v5.1 文件。只有我确认后才能执行。更正：模型改为 B。";
 size_t n=dc_prepare_text_spans(s,strlen(s),spans,4,&partial);assert(n==3&&!partial);
 assert(spans[0].flags&DC_SPAN_NEGATION);assert(spans[1].flags&DC_SPAN_CONDITION);assert(spans[2].flags&DC_SPAN_CORRECTION);
 for(size_t i=0;i<n;i++){assert(spans[i].start<spans[i].end&&spans[i].end<=strlen(s));if(i)assert(spans[i].start>=spans[i-1].end);}
 assert(!strncmp(s+spans[0].start,"不要删除",strlen("不要删除")));
 n=dc_prepare_text_spans("notebook is fine.",17,spans,4,&partial);assert(n==0&&!partial);
 n=dc_prepare_text_spans("Do NOT delete. Only after approval! Actually use model B.",55,spans,1,&partial);assert(n==1&&partial&&(spans[0].flags&DC_SPAN_CORRECTION));
 const char bad[]={'a',(char)0xed,(char)0xa0,(char)0x80};assert(!dc_prepare_text_spans(bad,sizeof bad,spans,4,&partial)&&partial);
 const char nul[]={'n','o','t',0,'a'};assert(!dc_prepare_text_spans(nul,sizeof nul,spans,4,&partial)&&partial);
 char long_text[1000];memset(long_text,'x',sizeof long_text);memcpy(long_text,"never ",6);assert(!dc_prepare_text_spans(long_text,sizeof long_text,spans,4,&partial)&&partial);
 assert(!dc_prepare_text_spans("Do not delete.",14,NULL,0,&partial)&&partial);
 /* All byte strings must remain bounded even when they are invalid UTF-8. */
 unsigned state=1234567;char noise[128];for(int i=0;i<20000;i++){size_t len=(size_t)i%128;for(size_t j=0;j<len;j++){state=state*1664525u+1013904223u;noise[j]=(char)(state>>24);}n=dc_prepare_text_spans(noise,len,spans,4,&partial);assert(n<=4);for(size_t j=0;j<n;j++)assert(spans[j].start<spans[j].end&&spans[j].end<=len);}
 puts("PASS exact UTF-8 spans, lexical boundaries, priority, omission, invalid text and bounded random inputs");return 0;
}
