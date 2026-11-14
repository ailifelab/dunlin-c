#ifndef DUNLIN_CONFIG_IMPORT_H
#define DUNLIN_CONFIG_IMPORT_H
#include <json-c/json.h>
/* 1: normalized DSH subset, 0: another source format, -1: recognized but invalid.
 * All output arrays are caller-owned. Diagnostic contains only a field path and
 * stable code, never a credential value. No files, includes, code or networking. */
int dc_config_import_dsh(struct json_object *document,struct json_object *existing_providers,
 struct json_object *models,struct json_object *providers,struct json_object *warnings,
 struct json_object *source_providers,struct json_object **diagnostic);
void dc_config_import_fresh_alias(struct json_object *,struct json_object *,struct json_object *,struct json_object *);
#endif
