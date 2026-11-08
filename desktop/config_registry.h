#ifndef DUNLIN_CONFIG_REGISTRY_H
#define DUNLIN_CONFIG_REGISTRY_H
#include "internal.h"
int dc_config_schema(dc_store *);
int dc_config_provider_valid(struct json_object *);
struct json_object *dc_config_request(dc_store *,struct json_object *);
#endif
