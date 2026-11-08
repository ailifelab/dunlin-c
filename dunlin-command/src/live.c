#include "live.h"
#include <unistd.h>
#include <errno.h>
static _Thread_local cli_live_scope *active;
void cli_live_enter(cli_live_scope *scope,cli_activity activity,void *user) {
    *scope=(cli_live_scope){active,getpid(),activity,user};active=scope;
}
void cli_live_leave(cli_live_scope *scope) {active=scope->previous;}
ssize_t cli_live_read(int fd,void *data,size_t size) {
    ssize_t count=read(fd,data,size);
    int saved=errno;
    if(active && active->owner==getpid() && active->activity) {
        if(count>0) active->activity(active->user,"output",data,(size_t)count);
        else if(count<0 && saved==EAGAIN) active->activity(active->user,"tick",NULL,0);
    }
    errno=saved;return count;
}
