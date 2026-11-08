#ifndef DUNLIN_RUNTIME_H
#define DUNLIN_RUNTIME_H
#include "kernel.h"
#include "agent.h"
#include "protocol.h"
#ifdef __cplusplus
extern "C" {
#endif

#define DUNLIN_VERSION "0.2.12"
typedef struct dunlin_runtime dunlin_runtime;
/* "agent/event" uses AK_EMIT. All fields are borrowed during the callback. */
typedef struct { const char *session_id, *type, *data; size_t length; } dunlin_event_v1;
/* Service keys below all use version 1. Plugin implementations own their
 * tables. Serialized calls on the same context; never unload during a call. */
typedef struct { dunlin_model_fn complete; dunlin_model_config config; } dunlin_model_v1;
typedef struct { ah_run_fn run; } dunlin_loop_v1;
typedef struct { ah_result (*run)(const ah_agent *,const char *,const char *,const ah_png *); } dunlin_png_loop_v1;
typedef struct { ah_result (*run)(const ah_agent_v2 *,const char *,const char *); } dunlin_loop_v2;
typedef struct { ah_result (*run)(const ah_agent_v2 *,const char *,const char *,const ah_png *); } dunlin_png_loop_v2;
typedef struct { ah_session_service service; struct json_object *(*list)(const ah_session_service *); } dunlin_sessions_v1;
typedef struct { void *user; int (*approve)(void *,const char *,const char *); } dunlin_permission_v1;
typedef enum {
    DUNLIN_PERMISSION_REQUEST=0,
    DUNLIN_PERMISSION_AGENT=1,
    DUNLIN_PERMISSION_FULL_ACCESS=2
} dunlin_permission_mode;
typedef enum {
    DUNLIN_PERMISSION_DECISION_ASK=0,
    DUNLIN_PERMISSION_DECISION_ALLOW=1,
    DUNLIN_PERMISSION_DECISION_DENY=2
} dunlin_permission_decision;
/* Platform or product policy adapter. Unknown tools should return ASK. */
typedef struct {
    void *user;
    dunlin_permission_decision (*decide)(void *,const char *tool,const char *exact_arguments);
} dunlin_permission_adapter_v1;
/* Versioned native service. FULL_ACCESS skips requests but never bypasses a
 * tool's own validation, availability or platform sandbox. */
typedef struct {
    void *request_user;
    int (*request)(void *,const char *,const char *);
    dunlin_permission_mode mode;
    const dunlin_permission_adapter_v1 *adapter;
} dunlin_permission_v2;
int dunlin_permission_authorize(const dunlin_permission_v2 *,const char *tool,const char *exact_arguments);
const char *dunlin_permission_mode_name(dunlin_permission_mode);
int dunlin_permission_mode_parse(const char *,dunlin_permission_mode *);
/* tools = ah_tool_service, clock = uint64_t (*)(void *) inside this table. */
typedef struct { uint64_t (*now_ms)(void *); void *user; } dunlin_clock_v1;
/* Bind externally assembled services ("loop","model","sessions","tools",
 * "permission","clock") to one turn. NULL input resumes a checkpoint.
 * Observer .approve and .now_ms are supplied by installed services instead. */
ah_result dunlin_turn(ak_scope *, const ah_limits *, const ah_hooks *observer,
                    const char *session_id, const char *input);

ah_result dunlin_turn_png(ak_scope *, const ah_limits *, const ah_hooks *, const char *session_id,
                        const char *input, const ah_png *image);

/* Linux preset. Relative directories resolve beside the configuration file.
 * Config path and hooks need only survive open; hooks.user through close. */
dunlin_runtime *dunlin_open(const char *config, const ah_hooks *, char *error,size_t size);
/* Optional immutable transports, borrowed through close. Existing open/API
 * remains unchanged. Useful for host credential resolution without global env. */
struct dunlin_http_v1;
struct dunlin_http_v2;
dunlin_runtime *dunlin_open_with_transports(const char *,const ah_hooks *,
    const struct dunlin_http_v1 *,const struct dunlin_http_v2 *,char *,size_t);
/* Optional per-request derived view. Called after preset instructions, before
 * transport; never mutates the durable history or tool grants. Returned view
 * is owned. aux_config uses the actual upstream transport and is suitable for
 * a bounded text-only summary request. No ABI change to the v1 model table. */
typedef struct {
    void *user;
    dunlin_model_status (*project)(void *,const dunlin_model_config *,const dunlin_protocol_options *,
        struct json_object *,struct json_object *,const dunlin_model_events *,
        struct json_object **,char *,size_t);
} dunlin_context_projector_v1;
int dunlin_set_context_projector(dunlin_runtime *,const dunlin_context_projector_v1 *);
/* Idle-only, default off. Additional portable model.request/response/end events
 * for an explicitly installed local observer. Never emits transport headers or
 * provider-private continuation fields. Observer owns retention/redaction. */
int dunlin_set_model_trace(dunlin_runtime *,int enabled);
/* Idle-only borrowed observer pipeline. NULL restores the scope service.
 * It must preserve any service pipeline the host chooses to wrap. */
int dunlin_set_tool_pipeline(dunlin_runtime *,const ah_tool_pipeline_v1 *);
/* On an idle desktop preset require this registered tool on the first proposal of each new turn. NULL restores automatic choice. */
int dunlin_require_tool(dunlin_runtime *, const char *name);
/* Idle-only. The adapter is borrowed until replaced or runtime close; NULL
 * restores the built-in workspace policy (write_file only in AGENT mode). */
int dunlin_set_permission_mode(dunlin_runtime *,dunlin_permission_mode);
dunlin_permission_mode dunlin_get_permission_mode(const dunlin_runtime *);
int dunlin_set_permission_adapter(dunlin_runtime *,const dunlin_permission_adapter_v1 *);
int dunlin_close(dunlin_runtime *);
/* Install additional plugins on an idle runtime. Builtins are owned by the
 * preset. Omit a builtin from the profile before installing its replacement. */
ak_context *dunlin_kernel(dunlin_runtime *);
ak_scope *dunlin_scope(dunlin_runtime *);
const ah_limits *dunlin_limits(dunlin_runtime *);
/* Borrowed canonical configured workspace, valid until close. */
const char *dunlin_workspace(const dunlin_runtime *);
/* Owned inventory; never includes endpoint credentials or key environment names. */
struct json_object *dunlin_models(dunlin_runtime *);
const char *dunlin_current_model(const dunlin_runtime *);
/* Model/settings mutations are idle-only and preserve the current state on error. */
int dunlin_select_model(dunlin_runtime *,const char *id,char *error,size_t size);
int dunlin_show_reasoning(const dunlin_runtime *);
int dunlin_set_show_reasoning(dunlin_runtime *,int enabled);
const char *dunlin_reasoning_effort(const dunlin_runtime *);
int dunlin_set_reasoning_effort(dunlin_runtime *,const char *effort,char *error,size_t size);
/* Pass the original public conversation ID, never an earlier derived ID.
 * Profiles isolate storage by alias, protocol, model and endpoint; no history is copied.
 * Without models, the legacy ID is unchanged. out requires up to 65 bytes. */
int dunlin_session_for_model(const dunlin_runtime *,const char *conversation,char *out,
    size_t out_size,char *error,size_t error_size);
/* Use this entry for model profiles: map public conversation IDs once, retain
 * atomic conversation metadata, and expose public IDs in events and inventory. */
ah_result dunlin_turn_for_model(dunlin_runtime *,const ah_hooks *,const char *conversation,
    const char *input,const ah_png *,const ah_tool_service *);
struct json_object *dunlin_conversations(dunlin_runtime *); /* owned current-profile ID array */
/* Read-only projection of the last committed snapshot. Each owned array item has
 * sequence, type and a data object. Provider-private continuation state is omitted.
 * This is a derived view, not an append-only log or a replacement recovery format. */
struct json_object *dunlin_session_events(dunlin_runtime *,const char *conversation,
    char *error,size_t error_size);
/* Host-side tool adapters share the same loop, services and event contract.
 * NULL tools uses the registered service. The adapter is borrowed for this call. */
ah_result dunlin_turn_with_tools(ak_scope *,const ah_limits *,const ah_hooks *,
    const char *session_id,const char *input,const ah_png *,const ah_tool_service *tools);
struct json_object *dunlin_plugins(dunlin_runtime *); /* owned array of active preset plugins */
/* Additive desktop seam: caller owns session service through the synchronous call.
 * Existing permission, event, model and tool contracts are unchanged. */
ah_result dunlin_turn_with_sessions(ak_scope *, const ah_limits *, const ah_hooks *,
    const char *, const char *, const ah_session_service *);
/* Optional monotonic host clock, borrowed for the synchronous call. Desktop
 * excludes human approval wait while retaining execution/model deadlines. */
ah_result dunlin_turn_with_session_clock(ak_scope *,const ah_limits *,const ah_hooks *,
    const char *,const char *,const ah_session_service *,const dunlin_clock_v1 *);
/* Additive host composition: all adapters borrowed for this synchronous turn. */
ah_result dunlin_turn_with_adapters(ak_scope *,const ah_limits *,const ah_hooks *,
    const char *,const char *,const ah_png *,const ah_tool_service *,
    const ah_session_service *,const dunlin_clock_v1 *);
int dunlin_init_config(const char *path, char *error, size_t size);
#ifdef __cplusplus
}
#endif
#endif
