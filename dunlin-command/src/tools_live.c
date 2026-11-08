/* CLI-only instance of the existing supervisor. Include system declarations
 * before renaming read, so libc aliases/fortification never alter the shim. */
#define _GNU_SOURCE
#include <unistd.h>
#include "live.h"
#define read cli_live_read
#define ah_linux_tools cli_live_tools
#define ah_linux_tool_enable cli_live_tool_enable
#define ah_linux_tools_close cli_live_tools_close
#define ah_linux_tools_root_identity cli_live_tools_root_identity
#include "../../dunlin-compact/platform/tools.c"
