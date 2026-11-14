#define _XOPEN_SOURCE 700
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include "../../desktop/package_limits.h"
/* npm repeats UTF-8 filenames in local PAX records. Accept only metadata
 * that agrees with the following regular header. Never interpret overrides. */
static int redundant_pax(const char *data,size_t length,const unsigned char *header){
 if(length>4096 || (header[156] && header[156]!='0'))return 0;
 for(size_t pos=0;pos<length;){
  size_t start=pos,record=0;
  while(pos<length && data[pos]>='0' && data[pos]<='9'){
   record=record*10+(size_t)(data[pos++]-'0');
   if(record>length-start)return 0;
  }
  size_t end=start+record;
  if(pos>=end || data[pos++]!=' ' || end<=pos || data[end-1]!='\n')return 0;
  const char *field=data+pos;size_t n=end-pos-1;
  if(n>5 && !memcmp(field,"path=",5)){
   size_t bytes=strnlen((const char*)header,100);
   if(n-5!=bytes || memcmp(field+5,header,bytes))return 0;
  }else if(n>5 && !memcmp(field,"size=",5)){
   size_t declared=0,actual=0;
   for(size_t j=5;j<n;j++){if(field[j]<'0'||field[j]>'9')return 0;declared=declared*10+field[j]-'0';if(declared>DC_PACKAGE_MAX_FILE)return 0;}
   for(size_t j=124;j<135 && header[j] && header[j]!=' ';j++){if(header[j]<'0'||header[j]>'7')return 0;actual=actual*8+header[j]-'0';}
   if(declared!=actual)return 0;
  }else if(n>6 && !memcmp(field,"mtime=",6)){
   for(size_t j=6;j<n;j++)if((field[j]<'0'||field[j]>'9')&&field[j]!='.')return 0;
  }else return 0;
  pos=end;
 }
 return length>0;
}
int dc_npm_extract(int root,const char *bytes,size_t length){
size_t count=0;int valid=1;
 for(size_t offset=0;offset+512<=length;){unsigned char *h=(unsigned char*)bytes+offset;if(!h[0])break;unsigned long sum=0,stored=0,size=0;for(size_t j=0;j<512;j++)sum+=(j>=148&&j<156)?32:h[j];
  for(size_t j=148;j<154;j++){if(h[j]<'0'||h[j]>'7'){valid=0;break;}stored=stored*8+h[j]-'0';}if(!valid||sum!=stored){valid=0;break;}
  for(size_t j=124;j<135&&h[j]&&h[j]!=' ';j++){if(h[j]<'0'||h[j]>'7'){valid=0;break;}size=size*8+h[j]-'0';}if(!valid||size>DC_PACKAGE_MAX_FILE||size>length-offset-512){valid=0;break;}
  if(h[156]=='x'){
   size_t next=offset+512+((size+511)/512)*512;
   if(next+512>length || !redundant_pax(bytes+offset+512,size,(const unsigned char*)bytes+next))return 0;
   offset=next;continue;
  }
  char path[101];memcpy(path,h,100);path[100]=0;char type=h[156];if(h[345]||strncmp(path,"package/",8)){valid=0;break;}char *relative=path+8;if(type=='5'){offset+=512;continue;}if(type!=0&&type!='0'){valid=0;break;}if(++count>=DC_PACKAGE_MAX_FILES||!*relative||*relative=='/'||strchr(relative,'\\')){valid=0;break;}
  int parent=dup(root);char *at=relative,*slash;while(parent>=0&&(slash=strchr(at,'/'))){*slash=0;if(!*at||!strcmp(at,".")||!strcmp(at,"..")){close(parent);parent=-1;break;}if(mkdirat(parent,at,0700)&&errno!=EEXIST){close(parent);parent=-1;break;}int next=openat(parent,at,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(parent);parent=next;at=slash+1;}
  if(parent<0||!*at||!strcmp(at,".")||!strcmp(at,"..")||!strcmp(relative,"dunlin.json")){if(parent>=0)close(parent);valid=0;break;}int file=openat(parent,at,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);close(parent);size_t written=0;while(file>=0&&written<size){ssize_t n=write(file,bytes+offset+512+written,size-written);if(n<0&&errno==EINTR)continue;if(n<=0)break;written+=(size_t)n;}if(file<0||written!=size){if(file>=0)close(file);valid=0;break;}close(file);offset+=512+((size+511)/512)*512;
 }

 return valid;
}
