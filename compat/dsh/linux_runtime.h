#ifndef DUNLIN_DSH_LINUX_RUNTIME_H
#define DUNLIN_DSH_LINUX_RUNTIME_H

#include "dunlin/plugin_port.h"
#include <limits.h>

typedef struct {
    char directory[PATH_MAX];
    char loader[PATH_MAX];
    char launcher[PATH_MAX];
    char preload[PATH_MAX];
} dc_dsh_node_abi;

#if defined(__linux__)
/* Plan a private loader only for the verified upstream Node executable on an
   older GNU system. Detection checks existing files without downloading. */
typedef int (*dc_dsh_runtime_fetch)(const char *, const char *, const char *, const dc_hooks *);
const char *dc_dsh_linux_node_abi(const char *node, const char *home, int detect_only,
                                 const dc_hooks *, dc_dsh_runtime_fetch, dc_dsh_node_abi *);
const char *dc_dsh_linux_child_launcher(const char *node, const char *home, int detect_only, dc_dsh_node_abi *);
#endif
#endif
