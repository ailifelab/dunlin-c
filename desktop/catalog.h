#ifndef DUNLIN_CATALOG_INTERNAL_H
#define DUNLIN_CATALOG_INTERNAL_H
#include "internal.h"
int dc_catalog_schema(dc_store *);
struct json_object *dc_catalog_request(dc_store *,struct json_object *,const dc_hooks *);
#endif
