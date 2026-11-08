#ifndef DUNLIN_PUBLIC_PNG_H
#define DUNLIN_PUBLIC_PNG_H
#include "agent.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Desktop model-message helper. Non-NULL prompt and image required.
 * Returns an owned json-c content array or NULL for invalid/oversized PNG.
 * Only bounded PNG envelope/dimension validation, not full image decoding.
 * Caller releases via json_object_put(). Not part of the minimal MCU kernel. */
struct json_object *ah_png_content(const char *prompt,const ah_png *image);
#ifdef __cplusplus
}
#endif
#endif
