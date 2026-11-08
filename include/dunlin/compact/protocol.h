#ifndef DUNLIN_PROTOCOL_H
#define DUNLIN_PROTOCOL_H
#include "dunlin/compact/model.h"
#include "dunlin/compact/http2.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    DUNLIN_PROTOCOL_CHAT, DUNLIN_PROTOCOL_RESPONSES, DUNLIN_PROTOCOL_ANTHROPIC,
    DUNLIN_PROTOCOL_OLLAMA, DUNLIN_PROTOCOL_GEMINI
} dunlin_protocol;
typedef enum {
    DUNLIN_REASONING_DEFAULT, DUNLIN_REASONING_NONE, DUNLIN_REASONING_LOW,
    DUNLIN_REASONING_MEDIUM, DUNLIN_REASONING_HIGH, DUNLIN_REASONING_XHIGH
} dunlin_reasoning_level;
enum { DUNLIN_REASONING_CAN_DISABLE = 1, DUNLIN_REASONING_CAN_EFFORT = 2,
       DUNLIN_REASONING_CAN_XHIGH = 4 };
enum {
    DUNLIN_USAGE_INPUT = 1, DUNLIN_USAGE_OUTPUT = 2, DUNLIN_USAGE_TOTAL = 4,
    DUNLIN_USAGE_CACHED_INPUT = 8, DUNLIN_USAGE_CACHE_CREATION = 16,
    DUNLIN_USAGE_REASONING = 32
};
typedef struct {
    /* JSON body bytes handed to transport, and 2xx body bytes delivered to the
     * parser (including SSE/NDJSON framing), excluding HTTP headers. A failed
     * HTTP response body is not delivered by v1/v2: its size is unknown. These
     * counters are not token estimates or complete network traffic counters. */
    size_t request_bytes, response_bytes;
    int response_bytes_known; /* false for non-2xx or before response headers */
    uint64_t input_tokens, output_tokens, total_tokens, cached_input_tokens;
    uint64_t cache_creation_tokens, reasoning_tokens;
    unsigned present; /* Missing statistics are unknown, never implied zero. */
} dunlin_model_metrics;
typedef struct {
    dunlin_protocol protocol;
    unsigned max_tokens; /* zero: provider default; Messages uses explicit 1024 */
    int has_temperature;
    double temperature;
    const char *required_tool; /* one named function, or NULL */
    dunlin_reasoning_level reasoning_effort;
    unsigned reasoning_supported; /* explicit model capabilities above; default zero */
    int show_reasoning;
    void *user;
    /* Public provider text only; event is reasoning.delta or reasoning.summary.
     * Each borrowed text value is complete valid UTF-8. Never receives signatures
     * or opaque/encrypted reasoning. Hiding does not discard continuation state. */
    void (*reasoning)(void *, const char *event, const char *text, size_t length);
    dunlin_model_metrics *metrics; /* reset and populated for this invocation, including failures */
    const dunlin_http_v2 *http; /* native protocols; NULL selects desktop libcurl v2 */
    const char *anthropic_version; /* NULL pins 2026-10-21 */
    /* Chat metadata has two incompatible documented conventions. Zero preserves
     * v1 id/name fragments. One accepts complete id/name fields: repeat identical
     * values, ignore null/empty metadata, reject conflicting identities. */
    int chat_metadata_complete;
} dunlin_protocol_options;
/* v1 inputs/ownership remain unchanged. Native replies may contain the reserved
 * _dunlin_provider continuation object, which must survive durable checkpoints.
 * A provider's opaque state must not be moved to a different protocol. */
dunlin_model_status dunlin_protocol_proposal(const dunlin_model_config *,
    const dunlin_protocol_options *, struct json_object *messages, struct json_object *tools,
    const dunlin_model_events *, struct json_object **assistant, char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
