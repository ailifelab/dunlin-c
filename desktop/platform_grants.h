#ifndef DUNLIN_PLATFORM_GRANTS_H
#define DUNLIN_PLATFORM_GRANTS_H
#include "internal.h"
int dc_platform_grants_schema(dc_store *);
struct json_object *dc_platform_grants_request(dc_store *,struct json_object *);
#endif
