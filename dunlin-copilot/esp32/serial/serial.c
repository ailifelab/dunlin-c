#include <dunlin/compact/embedded.h>
#include <stdio.h>
#include <string.h>
void ae_serial_init(ae_serial *s,ak_scope *scope,ae_io_plugin *io,ae_write_fn write,void *user) {
 if(!s)return;
 memset(s,0,sizeof *s);s->scope=scope;s->io=io;s->write=write;s->user=user;
}
void ae_serial_set_submit(ae_serial *s,ae_submit_fn submit,void *user) {
 if(!s)return;
 s->submit=submit;s->submit_user=user;
}
void ae_serial_feed(ae_serial *s,const unsigned char *data,size_t size) {
 if(!s || !s->write || !data)return;
 for(size_t i=0;i<size;i++) {
  unsigned ch=data[i];
  if(ch=='\n' && s->swallow_lf){s->swallow_lf=0;continue;}
  s->swallow_lf=0;
  if(ch==3) {memset(s->line,0,sizeof s->line);s->length=0;s->dropping=0;s->escape=0;s->write(s->user,"cancelled\r\n> ",sizeof "cancelled\r\n> "-1);continue;}
  if(ch=='\r' || ch=='\n') {
   ae_reply r=s->dropping?(ae_reply){AE_LIMIT,"line discarded: overflow/control sequence"}:(s->submit?s->submit(s->submit_user,s->line,s->length):ae_execute(s->scope,s->io,s->line,s->length));
   char out[AE_REPLY_MAX+40];int n=snprintf(out,sizeof out,"%s: %s\r\n> ",ae_status_name(r.status),r.text);
   if(n>0)s->write(s->user,out,(size_t)n<sizeof out?(size_t)n:sizeof out-1);
   memset(s->line,0,sizeof s->line);s->length=0;s->dropping=0;s->escape=0;s->swallow_lf=ch=='\r';continue;
  }
  if(ch==8 || ch==127){
   if(!s->dropping && s->length){
    size_t start=s->length-1;
    while(start && ((unsigned char)s->line[start]&0xc0)==0x80)start--;
    memset(s->line+start,0,s->length-start);s->length=start;
   }
   continue;
  }
  if(s->dropping)continue;
  if(ch==0 || (ch<32 && ch!='\t')){s->dropping=1;continue;}
  if(s->length+1>=sizeof s->line){s->dropping=1;continue;}
  s->line[s->length++]=(char)ch;
 }
}
