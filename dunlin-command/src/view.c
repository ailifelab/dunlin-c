#define _XOPEN_SOURCE 700
#include "view.h"
#include "../../dunlin-compact/platform/clock.h"
#include <dunlin/compact/session.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <wchar.h>
#include <locale.h>
#include <stdio.h>

/* Encode controls and invalid UTF-8, and crop by terminal cells before styling. */
static void safe_line(char *out,size_t capacity,const char *text,int cells) {
    size_t used=0;mbstate_t state={0};
    while(*text && cells>0 && used+8<capacity) {
        unsigned char byte=(unsigned char)*text;
        wchar_t wide;
        size_t count=mbrtowc(&wide,text,strlen(text),&state);
        if(count==(size_t)-2) break;
        if(count>0 && count!=(size_t)-1 && ((wide>=0x80 && wide<=0x9f) ||
           (wide>=0x202a && wide<=0x202e) || (wide>=0x2066 && wide<=0x2069))) {
            if(cells<6) break;
            used+=(size_t)snprintf(out+used,capacity-used,"\\u%04x",(unsigned)wide);
            text+=count;cells-=6;memset(&state,0,sizeof state);continue;
        }
        if(count==(size_t)-1 || byte<32 || byte==127) {
            if(cells<4) break;
            used+=(size_t)snprintf(out+used,capacity-used,"\\x%02x",byte);
            text++;cells-=4;memset(&state,0,sizeof state);continue;
        }
        int width=wcwidth(wide);if(width<0) width=1;
        if(width>cells || used+count>=capacity) break;
        memcpy(out+used,text,count);used+=count;text+=count;cells-=width;
    }
    out[used]=0;
}
static int columns(void) {
    struct winsize size={0};
    return !ioctl(STDERR_FILENO,TIOCGWINSZ,&size) && size.ws_col?size.ws_col:80;
}
void cli_view_init(cli_view *view,int enabled) {
    memset(view,0,sizeof *view);view->enabled=enabled;
    if(!enabled) return;
    setlocale(LC_CTYPE,"");
    if(MB_CUR_MAX==1 && !setlocale(LC_CTYPE,"C.UTF-8")) setlocale(LC_CTYPE,"en_US.UTF-8");
    const char *term=getenv("TERM");
    view->ansi=isatty(STDERR_FILENO) && term &&
        (!strncmp(term,"xterm",5) || !strncmp(term,"screen",6) || !strncmp(term,"tmux",4) ||
         !strncmp(term,"rxvt",4) || !strcmp(term,"linux") || !strncmp(term,"vt",2) || !strcmp(term,"ansi"));
    view->color=view->ansi && !getenv("NO_COLOR");
}
void cli_view_clear(cli_view *view) {
    if(view->enabled && view->drawn && view->ansi) fputs("\r\033[2K",stderr);
    view->drawn=0;view->rendered[0]=0;fflush(stderr);
}
void cli_view_heading(cli_view *view,const char *text) {
    if(!view->enabled) return;
    cli_view_clear(view);
    fprintf(stderr,"\n%s%s%s\n",view->color?"\033[1;36m":"",text,view->color?"\033[0m":"");
}
static void draw(cli_view *view,int force) {
    if(!view->enabled || !view->ansi) return;
    uint64_t now=ah_os_now_ms(NULL);
    if(!force && now-view->last_draw<150) return;
    char raw[1024],line[1024];
    snprintf(raw,sizeof raw,"    %s · %llus  %s",view->executing?"执行":"思考",
        (unsigned long long)((now-view->started)/1000),view->used?view->partial:view->latest);
    safe_line(line,sizeof line,raw,columns()-1);
    if(!strcmp(line,view->rendered)) return;
    fprintf(stderr,"\r\033[2K%s%s%s",view->color?"\033[2m":"",line,view->color?"\033[0m":"");
    snprintf(view->rendered,sizeof view->rendered,"%s",line);
    view->drawn=1;view->last_draw=now;fflush(stderr);
}
static void append(cli_view *view,const char *data,size_t size) {
    for(size_t i=0;i<size;i++) {
        if(data[i]=='\n' || data[i]=='\r') {
            if(view->used) {memcpy(view->latest,view->partial,view->used);view->latest[view->used]=0;}
            view->used=0;view->partial[0]=0;
        } else {
            if(!data[i]) {
                if(view->used+4>=sizeof view->partial) view->used=0;
                memcpy(view->partial+view->used,"\\x00",4);view->used+=4;view->partial[view->used]=0;
                continue;
            }
            if(view->used==sizeof view->partial-1) view->used=0;
            view->partial[view->used++]=data[i];view->partial[view->used]=0;
        }
    }
}
void cli_view_activity(void *user,const char *type,const char *data,size_t size) {
    cli_view *view=user;if(!view->enabled) return;
    if(!strcmp(type,"start")) {
        cli_view_clear(view);view->executing=1;view->started=ah_os_now_ms(NULL);view->last_draw=0;
        view->used=0;view->output_seen=0;view->partial[0]=view->latest[0]=0;
        char preview[512],command[2048];
        size_t count=size<sizeof preview-1?size:sizeof preview-1;
        memcpy(preview,data,count);preview[count]=0;
        safe_line(command,sizeof command,preview,10000);
        fprintf(stderr,"  %s$ %s%s%s\n",view->color?"\033[36m":"",command,count<size?" …":"",view->color?"\033[0m":"");
        snprintf(view->latest,sizeof view->latest,"等待命令输出");draw(view,1);
    } else if(!strcmp(type,"output")) {view->output_seen=1;append(view,data,size);draw(view,0);
    } else if(!strcmp(type,"tick")) {draw(view,0);
    } else if(!strcmp(type,"finish")) {
        cli_view_clear(view);
        char last[1024];safe_line(last,sizeof last,view->output_seen?(view->used?view->partial:view->latest):"无输出",columns()-20);
        fprintf(stderr,"    %s%s%s  %s\n",view->color?(!strcmp(data,"completed")?"\033[32m":"\033[31m"):"",data,view->color?"\033[0m":"",last);
        view->executing=0;view->used=0;view->partial[0]=view->latest[0]=0;view->started=ah_os_now_ms(NULL);fflush(stderr);
    } else if(!strcmp(type,"duplicate")) {
        cli_view_clear(view);fputs("    已拦截重复查询，复用先前观察并继续。\n",stderr);
    }
}
void cli_view_reason(cli_view *view,const char *data,size_t size) {
    if(!view->enabled) return;
    if(view->executing) return;
    if(!view->started) view->started=ah_os_now_ms(NULL);
    append(view,data,size);draw(view,0);
}
void cli_view_wait(cli_view *view) {
    if(!view->enabled) return;
    if(!view->started) view->started=ah_os_now_ms(NULL);
    draw(view,0);
}
