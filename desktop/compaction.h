#ifndef DUNLIN_COMPACTION_INTERNAL_H
#define DUNLIN_COMPACTION_INTERNAL_H
#include "internal.h"
#include "dunlin/compact/runtime.h"
typedef struct dc_compactor dc_compactor;
int dc_compaction_schema(dc_store *);
dc_compactor *dc_compactor_open(dc_store *,const char *chat,struct json_object *settings,const dc_hooks *,char *error,size_t);
const dunlin_context_projector_v1 *dc_compactor_service(dc_compactor *);
void dc_compactor_close(dc_compactor *);
struct json_object *dc_compaction_request(dc_store *,struct json_object *);
#endif
