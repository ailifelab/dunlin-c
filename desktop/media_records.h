#ifndef DUNLIN_DESKTOP_MEDIA_RECORDS_H
#define DUNLIN_DESKTOP_MEDIA_RECORDS_H
#include "internal.h"
int dc_media_collect(dc_store *);
int dc_media_schema(dc_store *);
int dc_media_chat_busy(dc_store *,const char *);
int dc_media_drop_chat(dc_store *,const char *);
struct json_object *dc_media_request(dc_store *,struct json_object *);
#endif
