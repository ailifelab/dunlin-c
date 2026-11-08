#ifndef DUNLIN_PREPARATION_INTERNAL_H
#define DUNLIN_PREPARATION_INTERNAL_H
#include "internal.h"
#include "dunlin/context_prep.h"
int dc_preparation_schema(dc_store *);
int dc_preparation_put(dc_store *,const char *chat,int sequence,const char *text);
size_t dc_preparation_get(dc_store *,const char *chat,int sequence,const char *text,dc_source_span out[4]);
/* Owned source messages for occasional checkpoint quotation checks. Read once
 * per compaction; never derive original quotes from augmented request text. */
struct json_object *dc_preparation_messages(dc_store *,const char *chat);
#endif
