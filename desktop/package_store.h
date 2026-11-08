#ifndef DUNLIN_PACKAGE_STORE_H
#define DUNLIN_PACKAGE_STORE_H
#include "internal.h"
/* Copy a bounded, link-free directory into private immutable content storage.
 * Files and manifest are data here; no lifecycle scripts are executed. */
struct json_object *dc_package_snapshot(dc_store *,const char *directory);
int dc_package_verify(const char *directory,struct json_object *files);
/* Caller owns returned bounded UTF-8 bytes; verified against the recorded file. */
char *dc_package_read_text(const char *directory,struct json_object *files,const char *relative);
#endif
