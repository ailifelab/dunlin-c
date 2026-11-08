#ifndef DUNLIN_INDEX_CONTROL_H
#define DUNLIN_INDEX_CONTROL_H
#include "internal.h"
int dc_index_schema(dc_store *);
/* Retry durable cache deletions before new indexing; serialized host queue.
 * Returns deletions completed, or -1 with the failed intent still durable. */
int dc_index_cleanup_deleted(dc_store *,unsigned limit);
int dc_index_paused(dc_store *);
size_t dc_index_quota(dc_store *);
struct json_object *dc_index_request(dc_store *,struct json_object *);
#endif
