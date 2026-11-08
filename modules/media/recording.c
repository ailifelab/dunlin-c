#include "dunlin/media_recording.h"
#include <stdlib.h>
#include <string.h>
#define MAX_PACKET 1048576u
static const unsigned char magic[16]={'D','U','N','L','I','N','R','1',0,0,0,0,0,0,0,0};
struct dl_record_writer {dl_record_write_fn write;void *io;uint64_t limit,used,last[3];unsigned seen;dl_record_result state;};
static void put32(unsigned char *p,uint32_t n){for(int i=0;i<4;i++)p[i]=(unsigned char)(n>>(i*8));}
static void put64(unsigned char *p,uint64_t n){for(int i=0;i<8;i++)p[i]=(unsigned char)(n>>(i*8));}
static uint32_t get32(const unsigned char *p){uint32_t n=0;for(int i=0;i<4;i++)n|=(uint32_t)p[i]<<(i*8);return n;}
static uint64_t get64(const unsigned char *p){uint64_t n=0;for(int i=0;i<8;i++)n|=(uint64_t)p[i]<<(i*8);return n;}
static uint32_t crc(uint32_t value,const unsigned char *p,size_t n){while(n--){value^=*p++;for(int i=0;i<8;i++)value=(value>>1)^(0xedb88320u&(0u-(value&1)));}return value;}
static int write_all(dl_record_writer *w,const void *bytes,size_t n){const unsigned char *p=bytes;while(n){size_t k=w->write(w->io,p,n);if(!k||k>n)return 0;p+=k;n-=k;w->used+=k;}return 1;}
static int read_all(dl_record_read_fn read,void *io,void *bytes,size_t n){unsigned char *p=bytes;while(n){size_t k=read(io,p,n);if(!k||k>n)return 0;p+=k;n-=k;}return 1;}
dl_record_writer *dl_record_open(dl_record_write_fn write,void *io,uint64_t limit){
 if(!write||limit<40)return NULL;dl_record_writer *w=calloc(1,sizeof *w);if(!w)return NULL;w->write=write;w->io=io;w->limit=limit;if(!write_all(w,magic,sizeof magic)){free(w);return NULL;}return w;
}
dl_record_result dl_record_append(dl_record_writer *w,dl_record_track track,uint64_t stamp,const void *bytes,size_t n){
 if(!w)return DL_RECORD_INVALID;if(w->state)return w->state;
 if(track<DL_RECORD_INPUT||track>DL_RECORD_VIDEO||!bytes||!n||n>MAX_PACKET||(track!=DL_RECORD_VIDEO&&(n%2)))return DL_RECORD_INVALID;
 unsigned index=(unsigned)track-1;if((w->seen&(1u<<index))&&stamp<w->last[index])return DL_RECORD_INVALID;
 /* Reserve the explicit terminal record even at the byte limit. */
 if(w->used>w->limit||w->limit-w->used<48||(uint64_t)n>w->limit-w->used-48)return DL_RECORD_LIMIT;
 unsigned char header[24]={0};header[0]=(unsigned char)track;put64(header+4,stamp);put32(header+12,(uint32_t)n);put32(header+16,~crc(crc(~0u,header,16),bytes,n));
 if(!write_all(w,header,sizeof header)||!write_all(w,bytes,n)){w->state=DL_RECORD_IO;return w->state;}
 w->seen|=1u<<index;w->last[index]=stamp;return DL_RECORD_OK;
}
dl_record_result dl_record_finish(dl_record_writer *w){
 if(!w)return DL_RECORD_INVALID;if(w->state)return w->state;unsigned char end[24]={0};put32(end+16,~crc(~0u,end,16));
 if(!write_all(w,end,sizeof end)){w->state=DL_RECORD_IO;return w->state;}w->state=DL_RECORD_FINISHED;return DL_RECORD_OK;
}
void dl_record_close(dl_record_writer *w){free(w);}
dl_record_result dl_record_scan(dl_record_read_fn read,void *io,uint64_t limit,dl_record_visit_fn visit,void *user){
 if(!read||!visit||limit<40)return DL_RECORD_INVALID;unsigned char header[24];if(!read_all(read,io,header,16))return DL_RECORD_TRUNCATED;if(memcmp(header,magic,16))return DL_RECORD_CORRUPT;
 uint64_t used=16,last[3]={0};unsigned seen=0;unsigned char *data=NULL;size_t capacity=0;dl_record_result result=DL_RECORD_OK;
 for(;;){
  if(limit-used<24){result=DL_RECORD_LIMIT;break;}if(!read_all(read,io,header,24)){result=DL_RECORD_TRUNCATED;break;}used+=24;
  unsigned track=header[0];uint64_t stamp=get64(header+4);size_t n=get32(header+12);
  if(header[1]||header[2]||header[3]||get32(header+20)||track>3||n>MAX_PACKET){result=DL_RECORD_CORRUPT;break;}
  if(!track){unsigned char extra;if(n||stamp||get32(header+16)!=~crc(~0u,header,16)||read(io,&extra,1)){result=DL_RECORD_CORRUPT;}break;}
  if(!n||(track!=DL_RECORD_VIDEO&&n%2)||((seen&(1u<<(track-1)))&&stamp<last[track-1])){result=DL_RECORD_CORRUPT;break;}
  if((uint64_t)n>limit-used){result=DL_RECORD_LIMIT;break;}if(n>capacity){unsigned char *next=realloc(data,n);if(!next){result=DL_RECORD_IO;break;}data=next;capacity=n;}
  if(!read_all(read,io,data,n)){result=DL_RECORD_TRUNCATED;break;}used+=n;
  if(get32(header+16)!=~crc(crc(~0u,header,16),data,n)){result=DL_RECORD_CORRUPT;break;}
  if(!visit(user,(dl_record_track)track,stamp,data,n)){result=DL_RECORD_IO;break;}seen|=1u<<(track-1);last[track-1]=stamp;
 }
 free(data);return result;
}
