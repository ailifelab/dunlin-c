#ifndef DUNLIN_DESKTOP_MODEL_SWITCH_H
#define DUNLIN_DESKTOP_MODEL_SWITCH_H
#include "internal.h"
/* C-owned next-turn route review/apply; no credentials, HTTP or tools. */
struct json_object *dc_model_switch_request(dc_store *,struct json_object *);
#endif
