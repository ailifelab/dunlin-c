#ifndef DUNLIN_DESKTOP_EXTENSION_TOOLS_H
#define DUNLIN_DESKTOP_EXTENSION_TOOLS_H
#include "internal.h"
#include "dunlin/compact/agent.h"
/* Desktop-only optional host adapter. Borrowed base; no runtime ABI change. */
struct json_object *dc_extension_tools_snapshot(dc_store *);
int dc_extension_tools_open(dc_store *,const char *chat,const ah_tool_service *base,struct json_object *snapshot,ah_tool_service *out);
void dc_extension_tools_close(ah_tool_service *);
#endif
