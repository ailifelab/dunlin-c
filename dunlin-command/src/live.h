#ifndef DUNLIN_CLI_LIVE_H
#define DUNLIN_CLI_LIVE_H
#include <sys/types.h>
#include <dunlin/compact/tools.h>
typedef void (*cli_activity)(void *,const char *,const char *,size_t);
typedef struct cli_live_scope {
    struct cli_live_scope *previous;
    pid_t owner;
    cli_activity activity;
    void *user;
} cli_live_scope;
void cli_live_enter(cli_live_scope *,cli_activity,void *);
void cli_live_leave(cli_live_scope *);
ssize_t cli_live_read(int,void *,size_t);
int cli_live_tools(ah_tool_service *,const char *);
int cli_live_tool_enable(ah_tool_service *,const char *,int);
void cli_live_tools_close(ah_tool_service *);
#endif
