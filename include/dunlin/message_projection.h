#ifndef DUNLIN_MESSAGE_PROJECTION_H
#define DUNLIN_MESSAGE_PROJECTION_H
#ifdef __cplusplus
extern "C" {
#endif
struct json_object;
/* Read-only projection of explicitly public provider reasoning text.
 * 1: caller owns *text (free with dl_message_projection_free); *kind is a static
 *    "reasoning" or "summary" string. 0: no public text. -1: recognized public
 *    data is invalid/too large or allocation failed. No partial text is returned.
 * Never decodes private continuation, signatures or encrypted content. No I/O. */
int dl_message_reasoning(struct json_object *message, char **text, const char **kind);
void dl_message_projection_free(void *text);
#ifdef __cplusplus
}
#endif
#endif
