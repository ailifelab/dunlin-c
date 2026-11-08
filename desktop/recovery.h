#ifndef DUNLIN_RECOVERY_INTERNAL_H
#define DUNLIN_RECOVERY_INTERNAL_H
#include "internal.h"
int dc_recovery_schema(dc_store *);
struct json_object *dc_recovery_request(dc_store *,struct json_object *);
#endif
