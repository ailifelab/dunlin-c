#ifndef DUNLIN_CONTEXT_PREP_H
#define DUNLIN_CONTEXT_PREP_H
#include <stddef.h>
#include <stdint.h>
enum {DC_SPAN_NEGATION=1,DC_SPAN_CONDITION=2,DC_SPAN_CORRECTION=4,DC_SPAN_IDENTIFIER=8};
typedef struct {size_t start,end;uint32_t flags;} dc_source_span;
/* Allocation-free lexical candidates, not facts/summaries/permissions.
 * Exact UTF-8 source byte offsets. Up to 16 spans, each <=384 bytes. Large,
 * invalid or omitted candidates set partial; no truncated paraphrases. */
size_t dc_prepare_text_spans(const char *,size_t,dc_source_span *,size_t,int *partial);
#endif
