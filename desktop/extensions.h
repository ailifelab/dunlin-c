#ifndef DUNLIN_EXTENSIONS_INTERNAL_H
#define DUNLIN_EXTENSIONS_INTERNAL_H
#include "internal.h"
int dc_extensions_schema(dc_store *);
void dc_extensions_enforce(dc_store *,const dc_hooks *);
void dc_extensions_close(dc_store *);
struct json_object *dc_extensions_request(dc_store *,struct json_object *,const dc_hooks *);
/* Only called after catalogue archive verification; runs the same host gates. */
const char *dc_extension_gate(dc_store *,struct json_object *engines,const char *kind);
struct json_object *dc_extension_import(dc_store *,const char *source,const char *path,struct json_object *expected_manifest);
#endif
