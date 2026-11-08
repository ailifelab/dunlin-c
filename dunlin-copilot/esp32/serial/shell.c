#include "shell.h"
#include <stdio.h>
#include <string.h>
static int word(const char *s,const char *w){size_t n=strlen(w);return !strncmp(s,w,n) && (!s[n] || s[n]==' ' || s[n]=='\t');}
int ae_shell_kind(const char *line){
 line+=strspn(line," \t");
 return *line=='/';
}
int ae_shell_normalize(const char *input,char *out,size_t cap){
 size_t size=strlen(input);if(size>=AE_LINE_MAX)return 0;
 if(!ae_shell_kind(input)){
  int n=snprintf(out,cap,"%s",input);return n>=0 && (size_t)n<cap;
 }
 input+=strspn(input," \t")+1;size=strlen(input);
 while(size && (input[size-1]==' ' || input[size-1]=='\t'))size--;
 char line[AE_LINE_MAX];memcpy(line,input,size);line[size]=0;const char *s=line;
 int n;
 if(!strcmp(s,"?") || !strcmp(s,"h") || !strcmp(s,"帮助"))n=snprintf(out,cap,"help");
 else if(!strcmp(s,"date"))n=snprintf(out,cap,"ask 今天日期是什么，是星期几？");
 else if(!strcmp(s,"models"))n=snprintf(out,cap,"agent status");
 else if(word(s,"effort")){
  const char *p=s+6;p+=strspn(p," \t");n=snprintf(out,cap,"agent effort%s%s",*p?" ":"",p);
 }else if(word(s,"model")){
  const char *p=s+5;p+=strspn(p," \t");n=snprintf(out,cap,"agent %s",*p?p:"status");
 }else if(!strcmp(s,"python"))n=snprintf(out,cap,"python status");
 else if(!strcmp(s,"wifi"))n=snprintf(out,cap,"wifi status");
 else if(!strcmp(s,"led"))n=snprintf(out,cap,"led status");
 else n=snprintf(out,cap,"%s",s);
 return n>=0 && (size_t)n<cap;
}
static void emit(ae_write_fn write,void *user,const char *s){write(user,s,strlen(s));}
int ae_shell_help(const char *line,ae_write_fn write,void *user){
 if(!word(line,"help"))return 0;
 const char *topic=line+4;topic+=strspn(topic," \t");
 if(!*topic){
  emit(write,user,"\r\n本地命令（不请求模型）：\r\n  /help /h /? /帮助         查看帮助\r\n  /help wifi|model|python|led|io|uart|bt|net\r\n  /date                     实时日期和星期（上海时区）\r\n  /model memory | clear     对话容量/清除RAM对话\r\n  /status                   内存/任务状态\r\n  /plugins                 内核插件\r\n  /wifi status              联网状态\r\n  /model /model status /models 模型配置（不显示密钥）\r\n  /effort [low|xhigh]        查询或设置思考强度\r\n  /python status           MicroPython状态\r\n  /led status               板载灯当前状态\r\n  直接输入自然语言          调用模型（无需ask）\r\n  /ask-blind 自然语言       省略system设备类型的测试\r\n命令必须以/开头；其余文字直接交给模型。/wifi /model /led默认查看状态。\r\n");return 1;
 }
#if defined(AE_AGENT_ENABLE_PYTHON) && !AE_AGENT_ENABLE_PYTHON
 if(!strcmp(topic,"python")){emit(write,user,"\r\nMicroPython unavailable on this board.\r\n");return 1;}
#endif
 if(!strcmp(topic,"python"))emit(write,user,"\r\nMicroPython：\r\n  /python status\r\n  /python run print(1+2)\r\n  /python network [trusted|allowlist]\r\n  /python hosts\r\n  /python allow example.com\r\n模块json、dunlin；dunlin.http_get(HTTPS_URL)、dunlin.info()、dunlin.led(mode[,r,g,b,period_ms])。\r\n脚本有内存/时间/输出限制；无文件系统、pip或Linux shell。默认trusted允许证书链/主机名验证通过的HTTPS，无需逐个加域名。allowlist可选限制模式。\r\n模式与新增域名只保存在RAM；模型不能自行改策略。\r\n");
 else if(!strcmp(topic,"wifi"))emit(write,user,"\r\n配网：\r\n  /wifi set \"SSID\" \"PASSWORD\"\r\n  /wifi connect | disconnect | status\r\n  /wifi forget\r\n配置保存到NVS，上电恢复；/wifi forget清除保存的凭据。密码不回显。\r\n");
 else if(!strcmp(topic,"model") || !strcmp(topic,"agent"))emit(write,user,"\r\n模型配置：\r\n  /model set http://LAN_IP:PORT/v1/chat/completions MODEL KEY|-\r\n  /model status\r\n  /model memory             查看RAM对话容量\r\n  /model clear              清除对话，不清配置\r\n  /date                     实时日期/星期（上海时区）\r\n  /model forget\r\nESP32-S3 OpenAI: /model set https://api.openai.com/v1/responses gpt-6.1-sol KEY\r\n  /effort [low|xhigh]      (ESP32-S3 OpenAI Responses)\r\n  /model effort low|xhigh (兼容旧命令)\r\n  /model time UNIX_SECONDS  (ESP32-S3 HTTPS requires valid UTC)\r\n兼容 /agent set/forget；配置保存到NVS。KEY为现有API密钥，无认证用-；不回显密钥。\r\n");
 else if(!strcmp(topic,"led"))emit(write,user,"\r\n板载RGB（仅已确认并配置的引脚）：\r\n  /led status | on | off\r\n  /led solid R G B\r\n  /led blink R G B [周期ms]\r\n  /led breathe R G B [周期ms]\r\n  /led rainbow [周期ms]\r\nRGB 0..255；周期200..5000，默认1000；亮度上限约25%。on恢复前一效果；模型忙时不并发控灯。\r\n");
 else if(!strcmp(topic,"io"))emit(write,user,"\r\n/io allow|deny PIN; /gpio read PIN; /gpio write PIN 0|1\r\n/pwm PIN HZ DUTY; /pwm stop PIN\r\n仅板级白名单引脚，初始不授权；板载RGB使用led命令。\r\n");
 else if(!strcmp(topic,"uart"))emit(write,user,"\r\n/uart open BAUD TX RX; /uart send HEX; /uart read; /uart close\r\nUART1独立于控制台；先/io allow TX/RX。\r\n");
 else if(!strcmp(topic,"bt"))emit(write,user,"\r\n/bt name NAME; /bt start|stop|status; /bt send HEX; /bt read\r\nBLE GATT，不支持经典蓝牙SPP。\r\n");
 else if(!strcmp(topic,"net"))emit(write,user,"\r\n/net send IPV4 PORT HEX\r\nUDP最多96字节；submitted不代表对端已收到。\r\n");
 else{emit(write,user,"\r\n未知帮助主题；输入 /help 查看本机命令。\r\n");return -1;}
 return 1;
}
static unsigned utf8(const unsigned char *s,unsigned *point){
 unsigned first=*s,n=first<0x80?1:first>=0xc2 && first<=0xdf?2:first>=0xe0 && first<=0xef?3:first>=0xf0 && first<=0xf4?4:0;
 if(!n)return 0;
 unsigned value=first & (n==1?127:n==2?31:n==3?15:7);
 for(unsigned i=1;i<n;i++){if(!s[i] || (s[i]&0xc0)!=0x80)return 0;value=(value<<6)|(s[i]&63);}
 if((n==2 && value<0x80) || (n==3 && value<0x800) || (n==4 && value<0x10000) || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return 0;
 *point=value;return n;
}
typedef struct {ae_write_fn write;void *user;char data[256];size_t used,total;} display;
static void flush(display *d){if(d->used){d->write(d->user,d->data,d->used);d->used=0;}}
static void append(display *d,const char *s,size_t size){
 if(d->used+size>sizeof d->data)flush(d);
 memcpy(d->data+d->used,s,size);d->used+=size;d->total+=size;
}
void ae_shell_text(const char *text,ae_write_fn write,void *user){
 display d={.write=write,.user=user};const unsigned char *s=(const unsigned char *)text;size_t consumed=0;
 append(&d,"| ",2);
 while(*s && consumed<4096 && d.total+16<8128){
  if(*s=='\r' || *s=='\n'){
   unsigned n=s[0]=='\r' && s[1]=='\n'?2:1;if(consumed+n>4096)break;
   s+=n;consumed+=n;append(&d,"\r\n| ",4);continue;
  }
  if(*s=='\t'){append(&d,"    ",4);s++;consumed++;continue;}
  unsigned point=0,n=utf8(s,&point);char escaped[16];
  if(!n || *s<32 || *s==127){int size=snprintf(escaped,sizeof escaped,"\\x%02x",*s);append(&d,escaped,(size_t)size);s++;consumed++;continue;}
  if(consumed+n>4096)break;
  if(point==0x2028 || point==0x2029)append(&d,"\r\n| ",4);
  else if((point>=0x80 && point<=0x9f) || (point>=0x202a && point<=0x202e) || (point>=0x2066 && point<=0x2069)){
   int size=snprintf(escaped,sizeof escaped,"\\u%04x",point);append(&d,escaped,(size_t)size);
  }else append(&d,(const char *)s,n);
  s+=n;consumed+=n;
 }
 if(*s)append(&d," ...[display truncated]",strlen(" ...[display truncated]"));
 append(&d,"\r\n",2);flush(&d);
}
static int numeric(const char *s,unsigned min,unsigned max,unsigned *out){
 unsigned value=0;if(!*s)return 0;
 for(;*s;s++){if(*s<'0' || *s>'9' || value>(max-(unsigned)(*s-'0'))/10)return 0;value=value*10+(unsigned)(*s-'0');}
 if(value<min || value>max)return 0;
 *out=value;return 1;
}
int ae_shell_led(const char *line,ae_led_action *out){
 if(!word(line,"led"))return 0;
 char scratch[AE_LINE_MAX];size_t length=strlen(line);if(length>=sizeof scratch)return -1;memcpy(scratch,line,length+1);
 char *args[7];unsigned count=0;char *p=scratch;
 while(*p){while(*p==' ' || *p=='\t')p++;if(!*p)break;if(count==7)return -1;args[count++]=p;while(*p && *p!=' ' && *p!='\t')p++;if(*p)*p++=0;}
 if(count<2)return -1;
 *out=(ae_led_action){"solid",32,32,32,1000};
 const char *modes[]={"off","on","solid","blink","breathe","rainbow"};unsigned mode=0;
 for(;mode<6;mode++)if(!strcmp(args[1],modes[mode]))break;
 if(mode==6)return -1;
 out->mode=modes[mode];
 if(mode<2)return count==2?1:-1;
 if(mode==5)return count==2 || (count==3 && numeric(args[2],200,5000,&out->period_ms))?1:-1;
 if(count!=5 && (mode==2 || count!=6))return -1;
 if(!numeric(args[2],0,255,&out->red) || !numeric(args[3],0,255,&out->green) || !numeric(args[4],0,255,&out->blue))return -1;
 if(count==6 && !numeric(args[5],200,5000,&out->period_ms))return -1;
 return 1;
}
