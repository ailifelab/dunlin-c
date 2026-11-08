#ifndef DUNLIN_DESKTOP_TRAJECTORY_H
#define DUNLIN_DESKTOP_TRAJECTORY_H
#include "internal.h"
struct json_object *dc_trajectory_request(dc_store *,struct json_object *);
int dc_trajectory_schema(dc_store *);
char *dc_trajectory_chat_key(dc_store *,const char *);
dl_trace_span *dc_trajectory_begin(dc_store *,const char *,const char *,const char *,const char *,long long,const char *);
#endif
