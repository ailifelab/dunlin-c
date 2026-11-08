#include "console.h"
#include <assert.h>
#include <stdio.h>
int main(void){dunlin_console_reset();assert(!dunlin_console_cancelled(NULL));assert(!dunlin_console_control(99));assert(dunlin_console_control(CTRL_C_EVENT));assert(dunlin_console_cancelled(NULL));dunlin_console_reset();puts("Windows console cancellation state passed");return 0;}
