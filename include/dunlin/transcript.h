#ifndef DUNLIN_TRANSCRIPT_H
#define DUNLIN_TRANSCRIPT_H
#include "desktop.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Portable backup of visible user/assistant text only. The bounded importer
 * creates an ordinary conversation in this identity's private workspace.
 * All results are owned UTF-8 JSON. dc_free releases them. */
char *dc_transcript_export(dc_store *,const char *chat_id);
char *dc_transcript_preview(const char *archive);
int dc_transcript_restore(dc_store *,const char *new_id,const char *expected_identity,const char *archive);
#ifdef __cplusplus
}
#endif
#endif
