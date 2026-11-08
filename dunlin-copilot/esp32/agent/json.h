#ifndef AE_JSON_H
#define AE_JSON_H
#include "cJSON.h"
#include <stddef.h>
/* Private bounded framing/depth/duplicate-key guard shared by device adapters. */
cJSON *ae_json_parse_bounded(const char *,size_t);
#endif
