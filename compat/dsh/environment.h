#ifndef DUNLIN_DSH_ENVIRONMENT_H
#define DUNLIN_DSH_ENVIRONMENT_H
#include "dunlin/plugin_port.h"
/* Entire optional compatibility boundary. Never edits global engines. */
dc_worker *dc_dsh_prepare(const char *private_directory,const char *assets,
 const char *selected_node,int confirm_upgrade,const dc_hooks *,char **error,char **actual_node);
int dc_dsh_detect(const char *private_directory,const char *assets,const char *selected_node,
 const dc_hooks *,char **error,char **actual_node);
#endif
