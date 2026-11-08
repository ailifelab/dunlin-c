#include "https_policy.h"
#include <string.h>
static int equal(const char *a,const char *b){
 for(;*a && *b;a++,b++){unsigned x=(unsigned char)*a,y=(unsigned char)*b;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return 0;}return !*a && !*b;
}
int ae_https_permitted(const char *url,int trusted,const char *const *hosts,size_t count,char *out,size_t cap){
 if(!url || !out || cap<2)return 0;
 out[0]=0;size_t length=0;while(url[length] && length<=768){unsigned c=(unsigned char)url[length];if(c<=32 || c==127 || c=='\\')return 0;length++;}
 if(length>768 || length<8)return 0;
 for(unsigned i=0;i<8;i++){unsigned c=(unsigned char)url[i];if(c>='A'&&c<='Z')c+=32;if(c!=(unsigned char)"https://"[i])return 0;}
 const char *start=url+8,*end=start;while(*end && *end!='/' && *end!='?' && *end!='#')end++;
 if(start==end)return 0;
 const char *host=start,*last=end,*port=NULL;
 if(*start=='['){
  host++;last=host;int colon=0;
  while(last<end && *last!=']'){unsigned c=(unsigned char)*last;if(c==':')colon=1;else if(!((c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F') || c=='.'))return 0;last++;}
  if(last==end || !colon || last==host)return 0;
  if(last+1<end){if(last[1]!=':')return 0;port=last+2;}
 }else{
  last=start;while(last<end && *last!=':'){unsigned c=(unsigned char)*last;if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='.'))return 0;last++;}
  if(last<end)port=last+1;
 }
 size_t n=(size_t)(last-host);if(!n || n>=cap || n>=96)return 0;
 if(port){unsigned value=0;if(port==end)return 0;for(const char *p=port;p<end;p++){if(*p<'0' || *p>'9' || value>(65535u-(unsigned)(*p-'0'))/10)return 0;value=value*10+(unsigned)(*p-'0');}if(!value)return 0;}
 for(size_t i=0;i<n;i++){unsigned c=(unsigned char)host[i];out[i]=(char)(c>='A'&&c<='Z'?c+32:c);}out[n]=0;
 if(trusted)return 1;
 for(size_t i=0;hosts && i<count;i++)if(hosts[i] && equal(out,hosts[i]))return 1;
 return 0;
}
