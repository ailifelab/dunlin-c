#ifndef DUNLIN_CLI_POLICY_H
#define DUNLIN_CLI_POLICY_H
#include <dunlin/compact/tools.h>
#include "live.h"
typedef struct {
    ah_tool_service service, runner;
    const ah_tool_service *base;
    const char *workspace;
    int automatic, native_commands, guard_probes;
    char *last_probe;
    cli_activity activity;
    void *activity_user;
    struct json_object *checked, *normalized;
    char *checked_name;
    char *last_name, *last_args, *last_result;
    ah_status last_status;
} cli_tools;
int cli_tools_open(cli_tools *,const ah_tool_service *,const char *workspace,int automatic);
void cli_tools_close(cli_tools *);
void cli_tools_begin_turn(cli_tools *,const char *);
/* Owned normalized shell arguments, NULL if automatic approval is not justified. */
struct json_object *cli_readonly(const char *name,struct json_object *args);
#endif
