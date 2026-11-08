#ifndef DUNLIN_STATIC_CONFIG_H
#define DUNLIN_STATIC_CONFIG_H
#include <stddef.h>
struct json_object;
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
 DC_STATIC_OK=0,DC_STATIC_SYNTAX,DC_STATIC_LIMIT,DC_STATIC_TAG,
 DC_STATIC_ALIAS,DC_STATIC_DUPLICATE,DC_STATIC_KEY,DC_STATIC_DOCUMENT,
 DC_STATIC_ROOT,DC_STATIC_ENCODING,DC_STATIC_MEMORY
} dc_static_config_code;
typedef struct {dc_static_config_code code;size_t line,column;} dc_static_config_error;
/* Owned JSON object/array; no files, includes, environment or code resolution.
 * UTF-8 <=1 MiB, one document, <=32 levels and <=65536 nodes. Scalar resolution
 * uses JSON-compatible plain values; quoted and block scalars remain strings.
 * Aliases, anchors, explicit tags and duplicate/merge keys are rejected. */
struct json_object *dc_static_config_parse(const char *,size_t,dc_static_config_error *);
#ifdef __cplusplus
}
#endif
#endif
