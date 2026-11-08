#ifndef DUNLIN_CLI_VIEW_H
#define DUNLIN_CLI_VIEW_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct {
    int enabled,ansi,color,drawn,executing,output_seen;
    uint64_t started,last_draw;
    size_t used;
    char partial[512],latest[512],rendered[1024];
} cli_view;
void cli_view_init(cli_view *,int);
void cli_view_clear(cli_view *);
void cli_view_heading(cli_view *,const char *);
void cli_view_activity(void *,const char *,const char *,size_t);
void cli_view_reason(cli_view *,const char *,size_t);
void cli_view_wait(cli_view *);
#endif
