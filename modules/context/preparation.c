#include "dunlin/context_prep.h"
#include <string.h>
static int utf8(const unsigned char *p,size_t n){
 for(size_t i=0;i<n;){unsigned char c=p[i++];if(!c)return 0;if(c<128)continue;int k=c<224?1:c<240?2:3;if(c<194||c>244||i+(size_t)k>n)return 0;unsigned char d=p[i];if((c==224&&d<160)||(c==237&&d>=160)||(c==240&&d<144)||(c==244&&d>=144))return 0;while(k--)if((p[i++]&192)!=128)return 0;}return 1;
}
static int word(unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';}
static unsigned char lower(unsigned char c){return c>='A'&&c<='Z'?(unsigned char)(c+32):c;}
static int has(const char *p,size_t n,const char *needle){
 size_t m=strlen(needle);for(size_t i=0;i+m<=n;i++){
  size_t j=0;while(j<m&&lower((unsigned char)p[i+j])==lower((unsigned char)needle[j]))j++;
  if(j==m&&((unsigned char)*needle>=128||((!i||!word((unsigned char)p[i-1]))&&(i+m==n||!word((unsigned char)p[i+m])))))return 1;
 }return 0;
}
static unsigned flags(const char *p,size_t n){
 static const char *neg[]={"不要","不能","禁止","不允许","不得","并非","never","not","cannot","don't",NULL};
 static const char *cond[]={"除非","只有","仅当","前提","如果","unless","only","must",NULL};
 static const char *correct[]={"更正","纠正","改为","修正","correction","instead","actually",NULL};unsigned f=0;
 for(size_t i=0;neg[i];i++)if(has(p,n,neg[i])){f|=DC_SPAN_NEGATION;break;}
 for(size_t i=0;cond[i];i++)if(has(p,n,cond[i])){f|=DC_SPAN_CONDITION;break;}
 for(size_t i=0;correct[i];i++)if(has(p,n,correct[i])){f|=DC_SPAN_CORRECTION;break;}
 int digit=0,id=0;for(size_t i=0;i<n;i++){if(p[i]>='0'&&p[i]<='9')digit=1;if(p[i]=='`'||p[i]=='/'||p[i]=='_'||p[i]=='.')id=1;}if(digit&&id)f|=DC_SPAN_IDENTIFIER;return f;
}
static unsigned score(unsigned f){return((f&DC_SPAN_CORRECTION)?8:0)+((f&DC_SPAN_NEGATION)?4:0)+((f&DC_SPAN_CONDITION)?2:0)+((f&DC_SPAN_IDENTIFIER)?1:0);}
size_t dc_prepare_text_spans(const char *text,size_t len,dc_source_span *out,size_t cap,int *partial){
 if(partial)*partial=0;if(!text||len>262144||(cap&&!out)||!utf8((const unsigned char*)text,len)){if(partial)*partial=1;return 0;}if(cap>16)cap=16;size_t used=0,start=0;
 for(size_t i=0;i<len;){unsigned char c=(unsigned char)text[i];size_t width=c<128?1:c<224?2:c<240?3:4;
  int end=c=='\n'||c==';'||c=='!'||c=='?'||(c=='.'&&(i+1==len||text[i+1]==' '||text[i+1]=='\n'))||(width==3&&(!memcmp(text+i,"。",3)||!memcmp(text+i,"！",3)||!memcmp(text+i,"？",3)));
  size_t stop=i+width;if(stop==len)end=1;i=stop;if(!end)continue;size_t a=start,b=stop;while(a<b&&(text[a]==' '||text[a]=='\t'||text[a]=='\r'||text[a]=='\n'))a++;while(b>a&&(text[b-1]==' '||text[b-1]=='\t'||text[b-1]=='\r'||text[b-1]=='\n'))b--;
  unsigned f=flags(text+a,b-a);if(f){if(b-a>384||!cap){if(partial)*partial=1;}else{dc_source_span span={a,b,f};if(used<cap)out[used++]=span;else{size_t weakest=0;for(size_t k=1;k<used;k++)if(score(out[k].flags)<score(out[weakest].flags))weakest=k;if(score(f)>=score(out[weakest].flags))out[weakest]=span;if(partial)*partial=1;}}}start=stop;
 }
 for(size_t i=1;i<used;i++){dc_source_span v=out[i];size_t j=i;while(j&&out[j-1].start>v.start){out[j]=out[j-1];j--;}out[j]=v;}return used;
}
