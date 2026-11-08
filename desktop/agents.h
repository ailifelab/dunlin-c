#ifndef DUNLIN_DESKTOP_AGENTS_H
#define DUNLIN_DESKTOP_AGENTS_H
#include "internal.h"
#include "dunlin/compact/agent.h"
/* Decorates borrowed project tools only while an admitted task is executing. */
int dc_agents_tools(dc_store *,const ah_tool_service *,ah_tool_service *);
void dc_agents_tools_close(ah_tool_service *);
int dc_agents_initialize(dc_store *);
struct json_object *dc_agents_request(dc_store *,struct json_object *);
/* Called inside the canonical input append transaction. */
int dc_agents_chat_prepare(dc_store *,const char *,const char *,char **expanded,void **context);
void dc_agents_chat_finish(dc_store *,void *context);
void dc_agents_input_committed(dc_store *);
int dc_agents_commit_input(dc_store *,const char *chat,int64_t sequence,struct json_object *message);
int dc_agents_forget_chat(dc_store *,const char *chat);
#endif
