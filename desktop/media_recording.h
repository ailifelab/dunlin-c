#ifndef DUNLIN_DESKTOP_MEDIA_RECORDING_H
#define DUNLIN_DESKTOP_MEDIA_RECORDING_H
#include "internal.h"
#include "dunlin/media_recording.h"
typedef struct dc_media_capture dc_media_capture;
int dc_recording_schema(dc_store *);
int dc_recording_drop_chat(dc_store *,const char *);
int dc_recording_collect(dc_store *);
struct json_object *dc_recording_request(dc_store *,struct json_object *);
#endif
