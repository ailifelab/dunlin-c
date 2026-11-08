#include "../../dunlin-copilot/esp32/agent/agent.h"
#include "../../dunlin-copilot/esp32/common/http_date.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static ae_agent_memory memory;static unsigned requests,clocks;static int failure,truncated,tool_clock,long_answer;static char printed[2048];
static int exchange(void *u,const char *s,char *out,size_t cap){
 (void)u;requests++;cJSON *r=cJSON_Parse(s);assert(r);cJSON *msgs=cJSON_GetObjectItemCaseSensitive(r,"messages");
 assert(cJSON_GetArraySize(msgs)==3+(int)memory.count*2+(tool_clock==2?2:0));
 for(unsigned i=0;i<memory.count;i++){
  cJSON *a=cJSON_GetArrayItem(msgs,2+2*i),*b=cJSON_GetArrayItem(msgs,3+2*i);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(a,"role")->valuestring,"user"));assert(!strcmp(cJSON_GetObjectItemCaseSensitive(b,"role")->valuestring,"assistant"));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(a,"content")->valuestring,memory.turns[i].user));assert(!strcmp(cJSON_GetObjectItemCaseSensitive(b,"content")->valuestring,memory.turns[i].assistant));
 }
 assert(cJSON_GetObjectItemCaseSensitive(r,"max_tokens")->valueint==1024);cJSON_Delete(r);
 if(failure)return 0;
 if(long_answer){char value[1801];for(int i=0;i<600;i++)memcpy(value+3*i,"中",3);value[1800]=0;snprintf(out,cap,"{\"choices\":[{\"message\":{\"content\":\"%s\"}}]}",value);return 1;}
 if(tool_clock==1){tool_clock=2;snprintf(out,cap,"{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"time1\",\"function\":{\"name\":\"current_time\",\"arguments\":\"{}\"}}]}}]}");
 }else snprintf(out,cap,"{\"choices\":[{\"finish_reason\":\"%s\",\"message\":{\"content\":\"西湖七天；保留真实回答%d\"}}]}",truncated?"length":"stop",requests);
 return 1;
}
static int led(void *u,const ae_led_action *a,char *o,size_t n){(void)u;(void)a;(void)o;(void)n;assert(0);return 0;}
static int clock_read(void *u,char *o,size_t n){(void)u;clocks++;return !failure && ae_date_shanghai(1794067200LL,o,n);}
static void print(void *u,const char *s){(void)u;snprintf(printed,sizeof printed,"%s",s);}
int main(void){
 ae_agent a={NULL,"{}",1,exchange,led,print,NULL,NULL,&memory,clock_read};
 assert(ae_agent_turn(&a,"fixture","杭州西湖七天")==AE_AGENT_REPLIED);
 assert(ae_agent_turn(&a,"fixture","我从昆明出发")==AE_AGENT_REPLIED);
 assert(ae_agent_turn(&a,"fixture","那目的地呢")==AE_AGENT_REPLIED);
 assert(memory.count==3 && strstr(memory.turns[0].user,"西湖") && strstr(memory.turns[1].user,"昆明") && strstr(memory.turns[0].assistant,"真实回答1"));
 failure=1;assert(ae_agent_turn(&a,"fixture","failed")==AE_AGENT_STOPPED && memory.count==3);failure=0;
 truncated=1;assert(ae_agent_turn(&a,"fixture","long")==AE_AGENT_TRUNCATED && strstr(printed,"回复未完"));truncated=0;
 for(int i=0;i<10;i++)assert(ae_agent_turn(&a,"fixture","bounded")==AE_AGENT_REPLIED);assert(memory.count==AE_MEMORY_TURNS);
 ae_agent_memory_clear(&memory);assert(memory.count==0 && !memory.turns[0].user[0]);
 unsigned old=requests;assert(ae_agent_turn(&a,"fixture","今天日期是什么，是星期几？")==AE_AGENT_OBSERVED && requests==old && clocks==1 && strstr(printed,"2026-11-08 星期日"));
 failure=1;assert(ae_agent_turn(&a,"fixture","能查到日期吗")==AE_AGENT_STOPPED && requests==old && strstr(printed,"不会使用模型猜测"));failure=0;
 tool_clock=1;assert(ae_agent_turn(&a,"fixture","read the clock")==AE_AGENT_OBSERVED && requests==old+2 && clocks==3);tool_clock=0;
 long_answer=1;assert(ae_agent_turn(&a,"fixture","long UTF8 answer")==AE_AGENT_REPLIED);long_answer=0;
 const char *excerpt=memory.turns[memory.count-1].assistant;const char *marker=strstr(excerpt," [history excerpt truncated]");assert(marker && (size_t)(marker-excerpt)%3==0 && strlen(excerpt)<1024);for(const char *q=excerpt;q<marker;q+=3)assert(!memcmp(q,"中",3));
 const char *tail=marker+strlen(" [history excerpt truncated] ");assert(strlen(tail)%3==0 && strlen(tail)>0);for(const char *q=tail;*q;q+=3)assert(!memcmp(q,"中",3));
 char date[160];assert(ae_date_shanghai(1709222399LL,date,sizeof date) && strstr(date,"2024-02-29 星期四"));assert(ae_date_shanghai(1709222400LL,date,sizeof date) && strstr(date,"2024-03-01 星期五"));assert(!ae_date_shanghai(0,date,sizeof date));assert(!ae_date_shanghai(1794067200LL,date,4));
 puts("PASS multi-turn user+assistant preservation, eviction/clear, failed request isolation, explicit length stop, real clock callback without model guessing, tool clock and failed-clock denial, timezone midnight/leap weekday");
}
