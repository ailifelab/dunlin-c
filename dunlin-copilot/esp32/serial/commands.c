#include <dunlin/compact/embedded.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
const char *ae_status_name(ae_status s) {
 static const char *names[]={"ok","invalid","denied","unsupported","busy","io_error","limit"};
 return (unsigned)s<sizeof names/sizeof *names?names[s]:"invalid";
}
static int tokenize(char *s,char **args,size_t *count) {
 size_t n=0;char *read=s,*write=s;
 while(*read) {
  while(*read==' ' || *read=='\t')read++;
  if(!*read)break;
  if(n==8)return 0;
  args[n++]=write;int quote=0;
  while(*read && (quote || (*read!=' ' && *read!='\t'))) {
   unsigned char ch=(unsigned char)*read++;
   if(ch=='"'){quote=!quote;continue;}
   if(ch=='\\') {
    ch=(unsigned char)*read++;if(!ch)return 0;
    if(ch=='n')ch='\n';else if(ch=='r')ch='\r';else if(ch=='t')ch='\t';
    else if(ch!='\\' && ch!='"' && ch!=' ')return 0;
   } else if(ch<32 || ch==127)return 0;
   *write++=(char)ch;
  }
  if(quote)return 0;
  while(*read==' ' || *read=='\t')read++;
  *write++=0;
 }
 *count=n;return 1;
}
static int number(const char *s,unsigned *value) {
 unsigned v=0;if(!*s)return 0;
 for(;*s;s++){if(*s<'0'||*s>'9'||v>(UINT_MAX-(unsigned)(*s-'0'))/10)return 0;v=v*10+(unsigned)(*s-'0');}
 *value=v;return 1;
}
static int copy(char *out,size_t cap,const char *in) {size_t n=strlen(in);if(n>=cap)return 0;memcpy(out,in,n+1);return 1;}
static int hex(ae_request *r,const char *in) {
 size_t n=strlen(in);if(n%2 || n/2>AE_DATA_MAX)return 0;
 for(size_t i=0;i<n;i++) {
  unsigned char c=(unsigned char)in[i];int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
  if(v<0)return 0;
  if(!(i%2))r->data[i/2]=(unsigned char)(v<<4);else r->data[i/2]|=(unsigned char)v;
 }
 r->length=n/2;return 1;
}
static ae_reply execute(ak_scope *scope,ae_io_plugin *io,char *line) {
 char *a[8];size_t n=0;ae_request r={0};
 ae_reply bad={AE_INVALID,"invalid syntax; use help"};
 if(!tokenize(line,a,&n))return bad;
 if(!n)return (ae_reply){AE_OK,""};
 if(n==1 && (!strcmp(a[0],"help") || !strcmp(a[0],"/help")))return (ae_reply){AE_OK,"help io|uart|wifi|bt|net; status; plugins. io grants start empty. No command echo/history; double-quote values containing spaces."};
 if(n==2 && !strcmp(a[0],"help")) {
  if(!strcmp(a[1],"io"))return (ae_reply){AE_OK,"io allow|deny PIN; gpio read PIN; gpio write PIN 0|1; pwm PIN HZ DUTY (1..20000 Hz, 0..1023); pwm stop PIN"};
  if(!strcmp(a[1],"uart"))return (ae_reply){AE_OK,"uart open BAUD TX RX (UART1 8N1, 1200..115200); uart send HEX; uart read; uart close"};
  if(!strcmp(a[1],"wifi"))return (ae_reply){AE_OK,"wifi set \"SSID\" \"PASSWORD\"; wifi connect|disconnect|status|forget. RAM only; passwords are never echoed. Open WiFi password: \"\"."};
  if(!strcmp(a[1],"bt"))return (ae_reply){AE_OK,"bt name NAME; bt start|stop|status; bt send HEX; bt read. BLE GATT FFE0 service / FFE1 RX / FFE2 TX notify. No Classic or pairing persistence."};
  if(!strcmp(a[1],"net"))return (ae_reply){AE_OK,"net send IPV4 PORT HEX (UDP <=96 bytes, numeric IPv4, no DNS). Submitted is not proof of delivery."};
  return bad;
 }
 if(n==1 && !strcmp(a[0],"plugins"))return ak_get(scope,AE_IO_SERVICE,AE_ABI)?(ae_reply){AE_OK,"embedded-io active"}:(ae_reply){AE_OK,"embedded-io unavailable"};
 if(n==3 && !strcmp(a[0],"io") && (!strcmp(a[1],"allow") || !strcmp(a[1],"deny"))) {
  if(!number(a[2],&r.pin))return bad;
  return ae_io_allow(io,r.pin,!strcmp(a[1],"allow"))?(ae_reply){AE_OK,"pin policy updated"}:(ae_reply){AE_DENIED,"pin excluded by board policy"};
 }
 if(n==1 && !strcmp(a[0],"status"))r.op=AE_DIAGNOSTICS;
 else if(!strcmp(a[0],"gpio")) {
  if(n==3 && !strcmp(a[1],"read") && number(a[2],&r.pin))r.op=AE_GPIO_READ;
  else if(n==4 && !strcmp(a[1],"write") && number(a[2],&r.pin) && number(a[3],&r.value))r.op=AE_GPIO_WRITE;
  else return bad;
 } else if(!strcmp(a[0],"pwm")) {
  if(n==3 && !strcmp(a[1],"stop") && number(a[2],&r.pin))r.op=AE_PWM_STOP;
  else if(n==4 && number(a[1],&r.pin) && number(a[2],&r.frequency) && number(a[3],&r.value))r.op=AE_PWM;
  else return bad;
 } else if(!strcmp(a[0],"uart")) {
  if(n==5 && !strcmp(a[1],"open") && number(a[2],&r.baud) && number(a[3],&r.tx) && number(a[4],&r.rx))r.op=AE_UART_OPEN;
  else if(n==3 && !strcmp(a[1],"send") && hex(&r,a[2]))r.op=AE_UART_SEND;
  else if(n==2 && !strcmp(a[1],"read"))r.op=AE_UART_READ;
  else if(n==2 && !strcmp(a[1],"close"))r.op=AE_UART_CLOSE;
  else return bad;
 } else if(!strcmp(a[0],"wifi")) {
  if(n==4 && !strcmp(a[1],"set") && copy(r.name,sizeof r.name,a[2]) && copy(r.secret,sizeof r.secret,a[3]))r.op=AE_WIFI_SET;
  else if(n==2 && !strcmp(a[1],"connect"))r.op=AE_WIFI_CONNECT;
  else if(n==2 && !strcmp(a[1],"disconnect"))r.op=AE_WIFI_DISCONNECT;
  else if(n==2 && !strcmp(a[1],"status"))r.op=AE_WIFI_STATUS;
  else if(n==2 && !strcmp(a[1],"forget"))r.op=AE_WIFI_FORGET;
  else return bad;
 } else if(!strcmp(a[0],"bt")) {
  if(n==3 && !strcmp(a[1],"name") && copy(r.name,sizeof r.name,a[2]))r.op=AE_BLE_NAME;
  else if(n==3 && !strcmp(a[1],"send") && hex(&r,a[2]))r.op=AE_BLE_SEND;
  else if(n==2 && !strcmp(a[1],"read"))r.op=AE_BLE_READ;
  else if(n==2 && !strcmp(a[1],"start"))r.op=AE_BLE_START;
  else if(n==2 && !strcmp(a[1],"stop"))r.op=AE_BLE_STOP;
  else if(n==2 && !strcmp(a[1],"status"))r.op=AE_BLE_STATUS;
  else return bad;
 } else if(!strcmp(a[0],"net") && n==5 && !strcmp(a[1],"send") && copy(r.address,sizeof r.address,a[2]) && number(a[3],&r.port) && hex(&r,a[4]))r.op=AE_NET_SEND;
 else return bad;
 const ae_io_v1 *svc=ak_get(scope,AE_IO_SERVICE,AE_ABI);
 ae_reply result=svc && svc->abi==AE_ABI && svc->call?svc->call(svc->user,&r):(ae_reply){AE_UNSUPPORTED,"IO service unavailable"};
 /* Volatile wipe includes password before returning to caller. */
 volatile unsigned char *wipe=(volatile unsigned char *)&r;for(size_t i=0;i<sizeof r;i++)wipe[i]=0;
 result.text[AE_REPLY_MAX-1]=0;return result;
}
ae_reply ae_execute(ak_scope *s,ae_io_plugin *io,const char *line,size_t length) {
 if(!s || !io || !line || length>=AE_LINE_MAX)return (ae_reply){AE_LIMIT,"line exceeds limit"};
 if(memchr(line,0,length))return (ae_reply){AE_INVALID,"NUL in input"};
 char scratch[AE_LINE_MAX];memcpy(scratch,line,length);scratch[length]=0;
 ae_reply r=execute(s,io,scratch);
 volatile char *wipe=scratch;for(size_t i=0;i<sizeof scratch;i++)wipe[i]=0;
 return r;
}
