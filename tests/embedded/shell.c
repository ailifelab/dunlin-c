#include "../../dunlin-copilot/esp32/serial/shell.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char captured[20000];static size_t used;
static void write_text(void *unused,const char *s,size_t n){(void)unused;assert(used+n<sizeof captured);memcpy(captured+used,s,n);used+=n;captured[used]=0;}
static void normalize(const char *s,const char *expected){char out[AE_LINE_MAX];assert(ae_shell_normalize(s,out,sizeof out));assert(!strcmp(out,expected));}
int main(void){
 normalize(" /help ","help");normalize("  make the LED blue  ","  make the LED blue  ");
 normalize("ask /help wifi","ask /help wifi");normalize("help model","help model");normalize("?","?");
 normalize("/effort","agent effort");normalize("/effort xhigh","agent effort xhigh");normalize("/effort\tlow","agent effort low");normalize("/effortless","effortless");normalize("/models extra","models extra");normalize("/models","agent status");
 normalize("/model","agent status");normalize("/model set http://192.168.8.1:1/v1/chat/completions fixture -","agent set http://192.168.8.1:1/v1/chat/completions fixture -");
 normalize("/wifi set \"SSID spaces\" \"password spaces\"","wifi set \"SSID spaces\" \"password spaces\"");
 normalize("/wifi","wifi status");normalize("/led","led status");normalize("/status","status");
 normalize("/date","ask 今天日期是什么，是星期几？");normalize("/model memory","agent memory");normalize("/model clear","agent clear");
 normalize("/h","help");normalize("/?","help");normalize("/帮助","help");
 normalize("/ask-blind test","ask-blind test");normalize("/helpful","helpful");
 assert(ae_shell_kind(" /help")==1 && ae_shell_kind("/wifi set test secret")==1 && ae_shell_kind("/agent set url model key")==1 && ae_shell_kind("/unsupported")==1);
 const char *natural[]={"effort low","models","help","help model","wifi forget","model forget","led off","status","plugins","?","帮助","ask /help","ask wifi forget","ask-blind test","uname -a","  help  "};
 for(size_t i=0;i<sizeof natural/sizeof *natural;i++){assert(!ae_shell_kind(natural[i]));normalize(natural[i],natural[i]);}
 char tiny[2];assert(!ae_shell_normalize("help",tiny,sizeof tiny));assert(!ae_shell_normalize("/effort low",tiny,sizeof tiny));
 const char *topics[]={"help","help wifi","help model","help agent","help led","help io","help uart","help bt","help net"};
 for(size_t i=0;i<sizeof topics/sizeof *topics;i++){used=0;assert(ae_shell_help(topics[i],write_text,NULL)==1);assert(used && strstr(captured,"\r\n"));}
 used=0;assert(ae_shell_help("help wifi",write_text,NULL)==1);assert(strstr(captured,"NVS") && !strstr(captured,"S3配置"));
 used=0;assert(ae_shell_help("help led",write_text,NULL)==1);assert(!strstr(captured,"GPIO48"));
 used=0;assert(ae_shell_help("help python",write_text,NULL)==1);
#if defined(AE_AGENT_ENABLE_PYTHON) && !AE_AGENT_ENABLE_PYTHON
 assert(strstr(captured,"unavailable"));
#else
 assert(strstr(captured,"/python run"));
#endif
 used=0;assert(ae_shell_help("help unknown",write_text,NULL)==-1);assert(!ae_shell_help("ask set red",write_text,NULL));
 used=0;ae_shell_text("hello\nworld\r\n\033[2J\n[task-end]\tend",write_text,NULL);
 assert(!strcmp(captured,"| hello\r\n| world\r\n| \\x1b[2J\r\n| [task-end]    end\r\n"));
 assert(!strstr(captured,"\n[task-end]") && !strstr(captured,"\033[2J"));
 used=0;ae_shell_text("\xe2\x80\xaeX\xff\xf0",write_text,NULL);assert(strstr(captured,"\\u202eX\\xff\\xf0"));
 char large[5000];memset(large,'x',sizeof large);large[sizeof large-1]=0;used=0;ae_shell_text(large,write_text,NULL);assert(used<4200);
 memset(large,10,sizeof large);large[sizeof large-1]=0;used=0;ae_shell_text(large,write_text,NULL);assert(used<=8192 && strstr(captured,"display truncated"));
 used=0;ae_shell_text("a\xe2\x80\xa8[task-end]",write_text,NULL);assert(!strcmp(captured,"| a\r\n| [task-end]\r\n"));
 ae_led_action a;assert(ae_shell_led("led off",&a)==1 && !strcmp(a.mode,"off"));assert(ae_shell_led("led on",&a)==1);
 assert(ae_shell_led("led solid 0 255 0",&a)==1 && a.green==255);
 assert(ae_shell_led("led blink 255 0 0 1000",&a)==1 && a.period_ms==1000);
 assert(ae_shell_led("led breathe 0 0 255",&a)==1 && a.period_ms==1000);
 assert(ae_shell_led("led rainbow 5000",&a)==1 && a.period_ms==5000);
 const char *bad[]={"led solid 256 0 0","led blink -1 0 0","led blink 0 0 0 199","led rainbow 5001","led solid 1 2 3 1000","led on 1","led solid 9999999999999999999999 0 0","led shell uname","led blink 0 0 0 1.5"};
 for(size_t i=0;i<sizeof bad/sizeof *bad;i++)assert(ae_shell_led(bad[i],&a)==-1);
 puts("PASS local help/aliases, no credential-command auto-routing from ask, multiline safe UTF8 and bounded numeric LED commands");
}
