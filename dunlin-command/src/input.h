#ifndef DUNLIN_CLI_INPUT_H
#define DUNLIN_CLI_INPUT_H
#include <signal.h>
#include <stddef.h>
#include <histedit.h>
typedef struct { EditLine *editor; History *history; volatile sig_atomic_t *cancelled; int invalid,nul; } cli_input;
int cli_input_open(cli_input *,volatile sig_atomic_t *);
/* 1=line, 0=EOF, -1=error, -2=invalid UTF-8, -3=message limit/NUL. */
int cli_input_read(cli_input *,size_t max,char **line);
void cli_input_close(cli_input *);
#endif
