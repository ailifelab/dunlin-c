#ifndef DUNLIN_EXTENSION_SERVICES_H
#define DUNLIN_EXTENSION_SERVICES_H
#include "plugin_port.h"
#include <stdint.h>
/* Optional environment provider. Core services hold no interpreter or DSH DTO.
 * The descriptor is borrowed for the store lifetime; register while idle. */
typedef struct {
 uint32_t abi_version;
 const char *engine;
 const char *version;
 const char *baseline;
 dc_worker *(*prepare)(const char *directory,const char *assets,const char *selected_engine,
  int confirm_upgrade,const dc_hooks *,char **error,char **actual_engine);
} dc_environment_provider_v1;
int dc_extensions_register_environment(dc_store *,const dc_environment_provider_v1 *);
/* Inspect a candidate in a temporary worker without downloading or enabling it. */
typedef int (*dc_environment_detector)(const char *directory,const char *assets,const char *selected_engine,
 const dc_hooks *,char **error,char **actual_engine);
int dc_extensions_register_detector(dc_store *,dc_environment_detector);
#endif
