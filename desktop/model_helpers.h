#ifndef DUNLIN_MODEL_HELPERS_H
#define DUNLIN_MODEL_HELPERS_H
#include "internal.h"
int dc_helpers_schema(dc_store *);
/* Called on the serialized extension owner, before entering optional code. */
struct json_object *dc_helpers_plan(dc_store *,const char *call,struct json_object *request);
int dc_helpers_handoff(dc_store *,const char *call);
int dc_helpers_cancel_call(dc_store *,const char *call);
struct json_object *dc_helpers_native(dc_store *,const char *method,struct json_object *args,const char *call);
struct json_object *dc_helpers_pending(dc_store *);
unsigned dc_helpers_running(void *store);
int dc_helpers_uncertain(dc_store *,const char *source,const char *package,const char *instance);
int dc_helpers_chat_busy(dc_store *,const char *chat);
int dc_helpers_drop_chat(dc_store *,const char *chat);
#endif
