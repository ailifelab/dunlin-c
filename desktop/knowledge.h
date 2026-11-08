#ifndef DUNLIN_KNOWLEDGE_INTERNAL_H
#define DUNLIN_KNOWLEDGE_INTERNAL_H
#include "internal.h"
int dc_knowledge_schema(dc_store *);
struct json_object *dc_knowledge_request(dc_store *,struct json_object *);
struct json_object *dc_knowledge_snapshot(dc_store *,const char *chat,const char *query,struct json_object *options,const char **error);
int dc_knowledge_snapshot_valid(dc_store *,struct json_object *snapshot);
int dc_knowledge_drop_chat(dc_store *,const char *);
#endif
