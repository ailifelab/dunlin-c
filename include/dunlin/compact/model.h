#ifndef DUNLIN_MODEL_H
#define DUNLIN_MODEL_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct json_object;
struct dunlin_http_v1;
typedef enum {
    DUNLIN_MODEL_OK, DUNLIN_MODEL_INVALID, DUNLIN_MODEL_PROTOCOL, DUNLIN_MODEL_HTTP,
    DUNLIN_MODEL_TRANSPORT, DUNLIN_MODEL_TIMEOUT, DUNLIN_MODEL_CANCELLED,
    DUNLIN_MODEL_LIMIT, DUNLIN_MODEL_RATE_LIMIT,
    DUNLIN_MODEL_REQUIRED_TOOL_MISSING, /* complete proposal discarded; error contains required tool name */
    DUNLIN_MODEL_TOOL_FORMAT /* explicit provider rejection, no accepted proposal */
} dunlin_model_status;
typedef struct {
    const char *base_url;        /* http(s) API root, e.g. http://localhost:8080/v1 */
    const char *model;
    const char *key_env;         /* optional name of the environment variable */
    long timeout_ms;
    int stream;
    size_t max_response_bytes;   /* total wire body, including SSE framing */
    const struct dunlin_http_v1 *http; /* NULL selects the desktop libcurl transport */
} dunlin_model_config;
typedef struct {
    void *user;
    int (*cancelled)(void *user);
    void (*delta)(void *user, const char *utf8, size_t length);
} dunlin_model_events;
/* Borrow all inputs for one synchronous invocation. On success caller owns *assistant.
 * Error messages exclude response body, credentials and URL. error may be NULL. */
typedef dunlin_model_status (*dunlin_model_fn)(const dunlin_model_config *,
    struct json_object *messages, struct json_object *tools,
    const dunlin_model_events *, struct json_object **assistant, char *error, size_t error_size);
/* Validate an HTTP(S) API root without network access; accepts ASCII DNS,
 * canonical IPv4 and bracketed IPv6, plus an optional TCP port. */
int dunlin_model_url_valid(const char *url);
dunlin_model_status dunlin_chat_completion(const dunlin_model_config *,
    struct json_object *messages, struct json_object *tools,
    const dunlin_model_events *, struct json_object **assistant, char *error, size_t error_size);
/* For an Agent Loop that validates arguments before execution: accept complete
 * call envelopes even if the arguments string is not a strict JSON object. */
dunlin_model_status dunlin_chat_proposal(const dunlin_model_config *,
    struct json_object *messages, struct json_object *tools,
    const dunlin_model_events *, struct json_object **assistant, char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
