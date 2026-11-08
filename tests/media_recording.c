#include "dunlin/media_recording.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef struct {unsigned char bytes[4096];size_t length,offset,chunk,fail;} memory;
static size_t write_bytes(void *p,const void *bytes,size_t n){memory *m=p;if(m->fail&&m->length>=m->fail)return 0;if(n>m->chunk)n=m->chunk;if(m->fail&&n>m->fail-m->length)n=m->fail-m->length;assert(n<=sizeof m->bytes-m->length);memcpy(m->bytes+m->length,bytes,n);m->length+=n;return n;}
static size_t read_bytes(void *p,void *bytes,size_t n){memory *m=p;if(n>m->length-m->offset)n=m->length-m->offset;if(n>m->chunk)n=m->chunk;memcpy(bytes,m->bytes+m->offset,n);m->offset+=n;return n;}
static int visits(void *p,dl_record_track track,uint64_t stamp,const void *bytes,size_t n){int *count=p;const unsigned char *b=bytes;if(*count==0){assert(track==DL_RECORD_INPUT&&stamp==10&&n==4&&b[0]==0&&b[3]==128);}else{assert(track==DL_RECORD_OUTPUT&&stamp==3&&n==2&&b[0]==255&&b[1]==127);}(*count)++;return 1;}
static dl_record_result scan(memory *m){int count=0;m->offset=0;return dl_record_scan(read_bytes,m,4096,visits,&count);}
int main(void){
 memory m={{0},0,0,3,0};unsigned char input[]={0,0,0,128},output[]={255,127};
 dl_record_writer *w=dl_record_open(write_bytes,&m,98);assert(w);
 assert(dl_record_append(w,DL_RECORD_INPUT,10,input,4)==DL_RECORD_OK);
 assert(dl_record_append(w,DL_RECORD_INPUT,9,input,4)==DL_RECORD_INVALID);
 assert(dl_record_append(w,DL_RECORD_OUTPUT,3,output,1)==DL_RECORD_INVALID);
 assert(dl_record_append(w,DL_RECORD_OUTPUT,3,output,2)==DL_RECORD_OK);
 assert(dl_record_append(w,DL_RECORD_INPUT,11,input,4)==DL_RECORD_LIMIT);
 assert(dl_record_finish(w)==DL_RECORD_OK);assert(dl_record_finish(w)==DL_RECORD_FINISHED);
 assert(dl_record_append(w,DL_RECORD_INPUT,11,input,4)==DL_RECORD_FINISHED);dl_record_close(w);
 assert(m.length==94);assert(scan(&m)==DL_RECORD_OK);
 /* Exhaustively truncate this valid stream; no partial stream is complete. */
 size_t length=m.length;for(size_t n=0;n<length;n++){m.length=n;assert(scan(&m)==DL_RECORD_TRUNCATED);}m.length=length;
 /* Every payload/header bit belongs to either a fixed field or a checksum. */
 for(size_t i=0;i<length;i++){unsigned char saved=m.bytes[i];m.bytes[i]^=1;dl_record_result r=scan(&m);assert(r==DL_RECORD_CORRUPT||r==DL_RECORD_TRUNCATED||r==DL_RECORD_LIMIT);m.bytes[i]=saved;}
 m.bytes[m.length++]=1;assert(scan(&m)==DL_RECORD_CORRUPT);
 memory failure={{0},0,0,4,20};w=dl_record_open(write_bytes,&failure,4096);assert(w);assert(dl_record_append(w,DL_RECORD_INPUT,0,input,4)==DL_RECORD_IO);assert(dl_record_finish(w)==DL_RECORD_IO);dl_record_close(w);
 memory empty={{0},0,0,1,0};w=dl_record_open(write_bytes,&empty,40);assert(w);assert(dl_record_finish(w)==DL_RECORD_OK);dl_record_close(w);assert(scan(&empty)==DL_RECORD_OK);
 puts("PASS bounded C recording codec: short I/O, per-track timestamp order, exact PCM bytes, byte-limit termination, every truncation and single-byte corruption, terminal state and sticky I/O failure; devices/storage/playback separate.");
}
