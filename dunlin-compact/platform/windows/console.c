#include "console.h"
static volatile LONG stopped;
BOOL WINAPI dunlin_console_control(DWORD signal) {
    if(signal!=CTRL_C_EVENT&&signal!=CTRL_BREAK_EVENT&&signal!=CTRL_CLOSE_EVENT)return FALSE;
    InterlockedExchange(&stopped,1);return TRUE;
}
int dunlin_console_install(void) {return SetConsoleCtrlHandler(dunlin_console_control,TRUE)!=0;}
void dunlin_console_reset(void) {InterlockedExchange(&stopped,0);}
int dunlin_console_cancelled(void *unused) {(void)unused;return InterlockedCompareExchange(&stopped,0,0)!=0;}
