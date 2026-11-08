#include "http_date.h"
#include <stdio.h>
#include <string.h>
int ae_http_date_epoch(const char *text,int64_t *epoch){
 char dayname[4],month[4],zone[4],extra;unsigned day,year,hour,minute,second;
 if(!text || !epoch || sscanf(text,"%3s, %u %3s %u %u:%u:%u %3s %c",dayname,&day,month,&year,&hour,&minute,&second,zone,&extra)!=8 || strcmp(zone,"GMT") || year<2024 || year>2099 || hour>23 || minute>59 || second>59)return 0;
 const char *months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};unsigned m=0;
 for(;m<12;m++)if(!strcmp(month,months[m]))break;
 if(m==12)return 0;
 const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};unsigned max=days[m]+(m==1 && year%4==0 && (year%100!=0 || year%400==0));if(!day || day>max)return 0;
 /* Gregorian civil-date conversion; independent of global TZ/newlib timegm. */
 int y=(int)year;unsigned mon=m+1;y-=mon<=2;int era=y/400;unsigned yoe=(unsigned)(y-era*400);
 unsigned doy=(153*(mon+(mon>2?-3:9))+2)/5+day-1;unsigned doe=yoe*365+yoe/4-yoe/100+doy;
 int64_t total=(int64_t)era*146097+doe-719468;*epoch=total*86400+hour*3600+minute*60+second;return 1;
}

int ae_date_shanghai(int64_t epoch,char *out,size_t cap){
 if(!out || !cap || epoch<1704067200LL || epoch>4102444799LL)return 0;
 int64_t local=epoch+8*3600,days=local/86400;unsigned rem=(unsigned)(local%86400);
 unsigned weekday=(unsigned)((days+4)%7),year=1970,month=0;int64_t left=days;
 for(;;){unsigned n=365+(year%4==0 && (year%100!=0 || year%400==0));if(left<n)break;left-=n;year++;}
 const unsigned md[]={31,28,31,30,31,30,31,31,30,31,30,31};
 for(;month<11;month++){unsigned n=md[month]+(month==1 && year%4==0 && (year%100!=0 || year%400==0));if(left<n)break;left-=n;}
 const char *wd[]={"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
 int n=snprintf(out,cap,"%04u-%02u-%02u %s %02u:%02u:%02u（Asia/Shanghai，UTC+08:00）",year,month+1,(unsigned)left+1,wd[weekday],rem/3600,rem/60%60,rem%60);
 return n>=0 && (size_t)n<cap;
}
