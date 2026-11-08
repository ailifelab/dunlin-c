#ifndef DUNLIN_JSON_H
#define DUNLIN_JSON_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct json_object;
/* Return an owned UTF-8 object/array tree. Reject ambiguous keys and NUL.
 * Callers apply a byte limit before reading or allocating the input. */
struct json_object *dunlin_json_parse(const char *bytes, size_t length);
#ifdef __cplusplus
}
#endif
#endif
