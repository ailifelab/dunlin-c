#ifndef DUNLIN_AGENT_H
#define DUNLIN_AGENT_H
#include "model.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DUNLIN_AGENT_ABI 1u
typedef enum { AH_DONE=0, AH_FAILED=1, AH_CANCELLED=2, AH_DENIED=3,
               AH_BUDGET=4, AH_UNKNOWN=5, AH_BUSY=6, AH_TIMEOUT=7,
               AH_INVALID_ARGUMENTS=8, /* check() only, safe before execution */
               AH_EXEC_FAILED=9 /* call() only, known nonzero exit, effects possible */ } ah_status;

typedef struct {
    void *user;
    int (*cancelled)(void *);
    int (*approve)(void *, const char *name, const char *exact_arguments);
    /* Synchronous, borrowed UTF-8; delta is ephemeral. */
    void (*event)(void *, const char *type, const char *text, size_t len);
    uint64_t (*now_ms)(void *);
} ah_hooks;
typedef struct {
    unsigned max_rounds, max_messages, max_calls;
    size_t max_context_bytes, max_message_bytes, max_output_bytes;
    unsigned task_timeout_ms, tool_timeout_ms;
} ah_limits;
typedef struct {
    void *ctx;
    struct json_object *(*schema)(void *); /* owned JSON array */
    ah_status (*check)(void *, const char *name, struct json_object *args,
                       int *approval, int *effect, char *error, size_t size);
    ah_status (*call)(void *, const char *name, struct json_object *args, const ah_hooks *,
                      size_t max_output, unsigned timeout_ms, char **result);
} ah_tool_service;
typedef struct {
    void *ctx;
    ah_status (*open)(void *, const char *id, void **lease, struct json_object **state);
    int (*save)(void *lease, struct json_object *state);
    void (*close)(void *lease);
} ah_session_service;
typedef struct {
    dunlin_model_fn model;
    const dunlin_model_config *model_config;
    const ah_tool_service *tools;
    const ah_session_service *sessions;
    ah_hooks hooks;
    ah_limits limits;
} ah_agent;
typedef struct {
    void *ctx;
    ah_status (*pre_execute)(void *,const char *,const char *,const struct json_object *,
        struct json_object **,char *,size_t);
    ah_status (*post_execute)(void *,const char *,const char *,ah_status,const char *,char **);
    void (*finalized)(void *,const char *,const char *,ah_status);
} ah_tool_pipeline_v1;
/* v1 remains the first, unchanged member for source and binary isolation. */
typedef struct { ah_agent base; const ah_tool_pipeline_v1 *pipeline; } ah_agent_v2;
/* Borrowed PNG bytes supplied explicitly by the host. */
typedef struct { const unsigned char *data; size_t size; } ah_png;
typedef struct { ah_status status; char *text; } ah_result;
typedef ah_result (*ah_run_fn)(const ah_agent *, const char *session_id, const char *input);
/* NULL input resumes a checkpoint. Caller frees result.text with free(). */
ah_result ah_agent_run(const ah_agent *, const char *session_id, const char *input);
ah_result ah_agent_run_png(const ah_agent *, const char *session_id, const char *input,const ah_png *);
ah_result ah_agent_run_v2(const ah_agent_v2 *,const char *,const char *);
ah_result ah_agent_run_png_v2(const ah_agent_v2 *,const char *,const char *,const ah_png *);
const char *ah_status_name(ah_status);
int ah_utf8_text(const char *, size_t); /* valid UTF-8 with no NUL bytes */
ah_limits ah_default_limits(void);
#ifdef __cplusplus
}
#endif
#endif
