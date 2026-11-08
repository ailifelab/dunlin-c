#ifndef DUNLIN_PROJECT_CONTEXT_H
#define DUNLIN_PROJECT_CONTEXT_H
#include <json-c/json.h>
#include "dunlin/desktop.h"
/* Explicit choices are bounded pinned {path,sha256} references. legacy!=0
 * preserves earlier C callers that selected all project pins implicitly.
 * Reads only chosen sources; returns owned document array or NULL/error. */
struct json_object *dc_project_references(struct json_object *project,struct json_object *selection,int legacy,const char **error);
char *dc_project_reference_input(const char *input,struct json_object *documents);
int dc_project_reference_schema(dc_store *);
int dc_project_reference_versions(const char *root,struct json_object *paths,struct json_object *versions);
/* Sync/prune inside the caller's project transaction. */
int dc_project_reference_sync(dc_store *,const char *id,struct json_object *paths,struct json_object *versions,int reset);
struct json_object *dc_project_reference_list(dc_store *,struct json_object *project);
#endif
