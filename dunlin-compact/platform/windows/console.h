#ifndef DUNLIN_WINDOWS_CONSOLE_H
#define DUNLIN_WINDOWS_CONSOLE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int dunlin_console_install(void);
void dunlin_console_reset(void);
int dunlin_console_cancelled(void *unused);
BOOL WINAPI dunlin_console_control(DWORD signal);
#endif
