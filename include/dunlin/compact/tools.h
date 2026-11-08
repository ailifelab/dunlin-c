#ifndef DUNLIN_TOOLS_H
#define DUNLIN_TOOLS_H
#include "agent.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Open an existing workspace; hold directory fd through service lifetime. */
int ah_linux_tools(ah_tool_service *out, const char *workspace);
/* Additive read-only POSIX host query. Requires a service from ah_linux_tools;
 * returns 1 and held-directory device/inode, or 0 with zero outputs. No path
 * reopening or process/tool execution. Existing service/agent ABI unchanged. */
int ah_linux_tools_root_identity(const ah_tool_service *,uint64_t *device,uint64_t *inode);
int ah_linux_tool_enable(ah_tool_service *, const char *name, int enable);
void ah_linux_tools_close(ah_tool_service *);
int ah_windows_tools(ah_tool_service *out, const char *workspace);
int ah_windows_tool_enable(ah_tool_service *, const char *name, int enable);
void ah_windows_tools_close(ah_tool_service *);
#ifdef __cplusplus
}
#endif
#endif
