#ifndef DUNLIN_PROTOCOL_INTERNAL_H
#define DUNLIN_PROTOCOL_INTERNAL_H
#include "dunlin/compact/protocol.h"
#include <json-c/json.h>
typedef struct json_object ahp_json;
typedef struct { char *s; size_t n, capacity; } ahp_buffer;
typedef struct {
    const dunlin_model_config *cfg;
    const dunlin_protocol_options *options;
    const dunlin_model_events *events;
    dunlin_model_status status;
    char reason[160];
} ahp_context;
typedef enum { AHP_SSE, AHP_NDJSON } ahp_framing;
/* A callback borrows one complete JSON event and its optional SSE event name.
 * Return zero after ahp_fail() to abort. HTTP errors never expose remote bodies. */
typedef int (*ahp_consume)(void *, ahp_json *, const char *event);
int ahp_fail(ahp_context *, dunlin_model_status, const char *);
int ahp_append(ahp_context *, ahp_buffer *, const char *, size_t);
void ahp_buffer_free(ahp_buffer *);
ahp_json *ahp_field(ahp_json *, const char *);
const char *ahp_str(ahp_json *, const char *);
int ahp_is(const char *, const char *);
ahp_json *ahp_clone(ahp_json *);
/* Return native output, NULL if absent; mismatched/malformed metadata sets error. */
ahp_json *ahp_native(ahp_context *, ahp_json *message, const char *protocol);
int ahp_metadata(ahp_context *, ahp_json *assistant, const char *protocol, ahp_json *native);
int ahp_metric(ahp_context *, ahp_json *usage, const char *field,
               unsigned flag, uint64_t *value);
void ahp_reasoning(ahp_context *, const char *event, const char *text);
int ahp_text(ahp_context *, ahp_buffer *, const char *text);
int ahp_call_id(ahp_context *, char output[49]);
const char *ahp_effort(dunlin_reasoning_level);
/* Caller owns *plain on nonstream success. suffix starts with '/'. Extra headers
 * are protocol constants, not arbitrary user/model content. Streaming requires
 * callback-defined terminal events; exchange alone is not completion evidence. */
int ahp_exchange(ahp_context *, const char *suffix, ahp_json *request,
    ahp_framing, const dunlin_http_header *, size_t header_count,
    ahp_consume, void *state, ahp_json **plain);
int ahp_valid_assistant(ahp_context *, ahp_json *, int expects_tools);
dunlin_model_status ahp_result(ahp_context *, char *error, size_t error_size);
dunlin_model_status ahp_responses(const dunlin_model_config *, const dunlin_protocol_options *,
    ahp_json *, ahp_json *, const dunlin_model_events *, ahp_json **, char *, size_t);
dunlin_model_status ahp_anthropic(const dunlin_model_config *, const dunlin_protocol_options *,
    ahp_json *, ahp_json *, const dunlin_model_events *, ahp_json **, char *, size_t);
dunlin_model_status ahp_ollama(const dunlin_model_config *, const dunlin_protocol_options *,
    ahp_json *, ahp_json *, const dunlin_model_events *, ahp_json **, char *, size_t);
dunlin_model_status ahp_gemini(const dunlin_model_config *, const dunlin_protocol_options *,
    ahp_json *, ahp_json *, const dunlin_model_events *, ahp_json **, char *, size_t);
dunlin_model_status ahp_chat(const dunlin_model_config *, const dunlin_protocol_options *,
    ahp_json *, ahp_json *, const dunlin_model_events *, ahp_json **, char *, size_t);
#endif
