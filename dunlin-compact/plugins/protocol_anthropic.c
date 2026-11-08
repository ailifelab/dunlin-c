#include "protocol_internal.h"
#include "dunlin/compact/json.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Messages is a separate wire protocol. Only client function tools are exposed
 * to the Loop; native content is retained for signed thinking continuation. */
enum { AN_MAX_CALLS = 64 };
typedef struct {
    ahp_context ctx;
    ahp_json *output, *block;
    ahp_buffer fragment, signature;
    size_t index;
    int started, stopped, done, expects_tools, input_delta;
    uint64_t uncached, cached, created, output_tokens, reasoning;
    unsigned usage_present;
} an_response;

static int an_bad(ahp_context *c, const char *why)
{
    return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, why);
}

static ahp_json *an_object(ahp_context *c)
{
    ahp_json *v = json_object_new_object();
    if (!v) ahp_fail(c, DUNLIN_MODEL_LIMIT, "model response allocation failed");
    return v;
}

static ahp_json *an_array(ahp_context *c)
{
    ahp_json *v = json_object_new_array();
    if (!v) ahp_fail(c, DUNLIN_MODEL_LIMIT, "model response allocation failed");
    return v;
}

static int an_string(ahp_context *c, ahp_json *o, const char *key, const char *s)
{
    ahp_json *v = json_object_new_string(s);
    if (!v) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model response allocation failed");
    json_object_object_add(o, key, v);
    return 1;
}

static int an_copy(ahp_context *c, ahp_json *o, const char *key, ahp_json *v)
{
    ahp_json *copy = ahp_clone(v);
    if (!copy) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model response allocation failed");
    json_object_object_add(o, key, copy);
    return 1;
}

static const char *an_text_value(ahp_json *value)
{
    if (!json_object_is_type(value, json_type_string)) return NULL;
    const char *s = json_object_get_string(value);
    return s && strlen(s) == (size_t)json_object_get_string_len(value) ? s : NULL;
}

static const char *an_str(ahp_json *o, const char *key)
{
    return an_text_value(ahp_field(o, key));
}

static int an_usage_field(an_response *r, ahp_json *usage, const char *key,
                          unsigned flag, uint64_t *value)
{
    /* Messages input_tokens is uncached input, not normalized total input. */
    if (!ahp_metric(&r->ctx, usage, key, flag == DUNLIN_USAGE_INPUT ? 0 : flag, value)) return 0;
    if (ahp_field(usage, key)) r->usage_present |= flag;
    return 1;
}

static int an_usage(an_response *r, ahp_json *usage)
{
    if (!usage) return 1;
    if (!json_object_is_type(usage, json_type_object))
        return an_bad(&r->ctx, "invalid Messages usage object");
    if (!an_usage_field(r, usage, "input_tokens", DUNLIN_USAGE_INPUT, &r->uncached) ||
        !an_usage_field(r, usage, "output_tokens", DUNLIN_USAGE_OUTPUT, &r->output_tokens) ||
        !an_usage_field(r, usage, "cache_read_input_tokens", DUNLIN_USAGE_CACHED_INPUT, &r->cached) ||
        !an_usage_field(r, usage, "cache_creation_input_tokens", DUNLIN_USAGE_CACHE_CREATION, &r->created))
        return 0;
    ahp_json *details = ahp_field(usage, "output_tokens_details");
    if (details && (!json_object_is_type(details, json_type_object) ||
        !an_usage_field(r, details, "thinking_tokens", DUNLIN_USAGE_REASONING, &r->reasoning)))
        return an_bad(&r->ctx, "invalid Messages reasoning usage");
    unsigned input_fields = DUNLIN_USAGE_INPUT | DUNLIN_USAGE_CACHED_INPUT | DUNLIN_USAGE_CACHE_CREATION;
    int input_known = (r->usage_present & input_fields) == input_fields;
    uint64_t input = 0;
    if (input_known) {
        if (r->uncached > UINT64_MAX - r->cached ||
            r->uncached + r->cached > UINT64_MAX - r->created)
            return an_bad(&r->ctx, "Messages input token count overflow");
        input = r->uncached + r->cached + r->created;
    }
    if (input_known && (r->usage_present & DUNLIN_USAGE_OUTPUT) && input > UINT64_MAX - r->output_tokens)
        return an_bad(&r->ctx, "Messages total token count overflow");
    if ((r->usage_present & (DUNLIN_USAGE_OUTPUT | DUNLIN_USAGE_REASONING)) ==
        (DUNLIN_USAGE_OUTPUT | DUNLIN_USAGE_REASONING) && r->reasoning > r->output_tokens)
        return an_bad(&r->ctx, "Messages reasoning usage exceeds output usage");
    dunlin_model_metrics *m = r->ctx.options->metrics;
    if (m) {
        if (input_known) {
            m->input_tokens = input;
            m->present |= DUNLIN_USAGE_INPUT;
        }
        m->output_tokens = r->output_tokens;
        m->cached_input_tokens = r->cached;
        m->cache_creation_tokens = r->created;
        m->reasoning_tokens = r->reasoning;
        if (input_known && (r->usage_present & DUNLIN_USAGE_OUTPUT)) {
            m->total_tokens = input + r->output_tokens;
            m->present |= DUNLIN_USAGE_TOTAL;
        }
    }
    return 1;
}

static int an_finish(an_response *r, const char *reason)
{
    if (!reason) return an_bad(&r->ctx, "missing Messages stop_reason");
    if (ahp_is(reason, "max_tokens") || ahp_is(reason, "model_context_window_exceeded"))
        return ahp_fail(&r->ctx, DUNLIN_MODEL_LIMIT, "Messages response was truncated");
    int kind = ahp_is(reason, "tool_use") ? 1 : ahp_is(reason, "end_turn") ? 2 :
               ahp_is(reason, "stop_sequence") ? 3 : 0;
    if (!kind)
        return an_bad(&r->ctx, "unsupported Messages stop_reason");
    if (r->stopped && r->stopped != kind)
        return an_bad(&r->ctx, "conflicting Messages stop_reason");
    r->stopped = kind;
    r->expects_tools = kind == 1;
    return 1;
}

/* Also used to validate native history before replaying it. Unknown content,
 * including server_tool_use, must never become an executable client proposal. */
static ahp_json *an_normalize(ahp_context *c, ahp_json *content, int expects_tools)
{
    ahp_json *msg = NULL, *calls = NULL;
    ahp_buffer text = {0};
    if (!json_object_is_type(content, json_type_array)) {
        an_bad(c, "missing Messages content array");
        return NULL;
    }
    msg = an_object(c);
    calls = an_array(c);
    if (!msg || !calls || !an_string(c, msg, "role", "assistant")) goto fail;
    for (size_t i = 0; i < (size_t)json_object_array_length(content); ++i) {
        ahp_json *block = json_object_array_get_idx(content, i);
        const char *type = an_str(block, "type");
        if (ahp_is(type, "text")) {
            const char *s = an_str(block, "text");
            if (!s) { an_bad(c, "invalid Messages text block"); goto fail; }
            if (!ahp_append(c, &text, s, strlen(s))) goto fail;
        } else if (ahp_is(type, "tool_use")) {
            const char *id = an_str(block, "id"), *name = an_str(block, "name");
            ahp_json *input = ahp_field(block, "input"), *caller = ahp_field(block, "caller");
            if (!id || !*id || !name || !*name || !json_object_is_type(input, json_type_object) ||
                ahp_field(block, "toolset_name") ||
                (caller && !ahp_is(an_str(caller, "type"), "direct"))) {
                an_bad(c, "unsupported or malformed Messages client tool"); goto fail;
            }
            if ((size_t)json_object_array_length(calls) >= AN_MAX_CALLS) {
                ahp_fail(c, DUNLIN_MODEL_LIMIT, "too many model tool calls"); goto fail;
            }
            ahp_json *call = an_object(c), *fn = an_object(c);
            if (!call || !fn) {
                if (call) json_object_put(call);
                if (fn) json_object_put(fn);
                goto fail;
            }
            json_object_object_add(call, "function", fn);
            if (!an_string(c, call, "type", "function") || !an_string(c, call, "id", id) ||
                !an_string(c, fn, "name", name) ||
                !an_string(c, fn, "arguments", json_object_to_json_string_ext(input, JSON_C_TO_STRING_PLAIN))) {
                json_object_put(call); goto fail;
            }
            json_object_array_add(calls, call);
        } else if (ahp_is(type, "thinking")) {
            const char *signature = an_str(block, "signature");
            if (!an_str(block, "thinking") || !signature || !*signature) {
                an_bad(c, "incomplete Messages thinking block"); goto fail;
            }
        } else if (ahp_is(type, "redacted_thinking")) {
            const char *data = an_str(block, "data");
            if (!data || !*data) { an_bad(c, "invalid Messages opaque thinking block"); goto fail; }
        } else {
            an_bad(c, "unsupported Messages content block"); goto fail;
        }
    }
    if (!an_string(c, msg, "content", text.s ? text.s : "")) goto fail;
    if (json_object_array_length(calls)) {
        json_object_object_add(msg, "tool_calls", calls);
        calls = NULL;
    }
    if (!ahp_valid_assistant(c, msg, expects_tools)) goto fail;
    if (calls) json_object_put(calls);
    ahp_buffer_free(&text);
    return msg;
fail:
    if (msg) json_object_put(msg);
    if (calls) json_object_put(calls);
    ahp_buffer_free(&text);
    return NULL;
}

static int an_index(an_response *r, ahp_json *event)
{
    ahp_json *idx = ahp_field(event, "index");
    return json_object_is_type(idx, json_type_int) && json_object_get_int64(idx) >= 0 &&
           (uint64_t)json_object_get_int64(idx) == r->index;
}

static int an_event(void *opaque, ahp_json *event, const char *event_name)
{
    an_response *r = opaque;
    const char *type = an_str(event, "type");
    if (!type || (event_name && *event_name && !ahp_is(event_name, type)))
        return an_bad(&r->ctx, "invalid Messages SSE event type");
    if (r->done) return an_bad(&r->ctx, "Messages event after message_stop");
    if (ahp_is(type, "error"))
        return an_bad(&r->ctx, "Messages provider reported a stream error");
    if (ahp_is(type, "ping")) return 1;
    if (ahp_is(type, "message_start")) {
        ahp_json *message = ahp_field(event, "message"), *content = ahp_field(message, "content");
        const char *id = an_str(message, "id");
        if (r->started || !ahp_is(an_str(message, "type"), "message") ||
            !ahp_is(an_str(message, "role"), "assistant") || !id || !*id ||
            !json_object_is_type(content, json_type_array) || json_object_array_length(content) ||
            ahp_field(message, "stop_reason"))
            return an_bad(&r->ctx, "invalid Messages message_start");
        r->started = 1;
        return an_usage(r, ahp_field(message, "usage"));
    }
    if (ahp_is(type, "content_block_start")) {
        if (!r->started || r->stopped || r->block || !an_index(r, event))
            return an_bad(&r->ctx, "out-of-order Messages content block");
        ahp_json *block = ahp_field(event, "content_block");
        const char *kind = an_str(block, "type");
        if (!ahp_is(kind, "text") && !ahp_is(kind, "tool_use") &&
            !ahp_is(kind, "thinking") && !ahp_is(kind, "redacted_thinking"))
            return an_bad(&r->ctx, "unsupported Messages content block");
        ahp_json *copy = ahp_clone(block);
        if (!copy) return ahp_fail(&r->ctx, DUNLIN_MODEL_LIMIT, "model response allocation failed");
        json_object_array_add(r->output, copy);
        r->block = copy;
        if (ahp_is(kind, "text") || ahp_is(kind, "thinking")) {
            const char *s = an_str(block, ahp_is(kind, "text") ? "text" : "thinking");
            if (!s || !ahp_append(&r->ctx, &r->fragment, s, strlen(s)))
                return r->ctx.status == DUNLIN_MODEL_OK ? an_bad(&r->ctx, "invalid Messages block text") : 0;
            if (ahp_is(kind, "text") && r->ctx.events && r->ctx.events->delta && *s)
                r->ctx.events->delta(r->ctx.events->user, s, strlen(s));
            if (ahp_is(kind, "thinking")) {
                ahp_reasoning(&r->ctx, "reasoning.delta", s);
                const char *sig = an_str(block, "signature");
                if (ahp_field(block, "signature") && !sig)
                    return an_bad(&r->ctx, "invalid Messages thinking signature");
                if (sig && !ahp_append(&r->ctx, &r->signature, sig, strlen(sig))) return 0;
            }
        } else if (ahp_is(kind, "tool_use") &&
                   !json_object_is_type(ahp_field(block, "input"), json_type_object))
            return an_bad(&r->ctx, "invalid Messages tool input");
        return 1;
    }
    if (ahp_is(type, "content_block_delta")) {
        if (!r->block || r->stopped || !an_index(r, event))
            return an_bad(&r->ctx, "Messages delta outside its content block");
        ahp_json *delta = ahp_field(event, "delta");
        const char *kind = an_str(r->block, "type"), *dt = an_str(delta, "type"), *s = NULL;
        if (ahp_is(kind, "text") && ahp_is(dt, "text_delta")) {
            s = an_str(delta, "text");
            if (!s) return an_bad(&r->ctx, "invalid Messages text delta");
            return ahp_text(&r->ctx, &r->fragment, s);
        }
        if (ahp_is(kind, "thinking") && ahp_is(dt, "thinking_delta")) {
            s = an_str(delta, "thinking");
            if (!s || !ahp_append(&r->ctx, &r->fragment, s, strlen(s)))
                return r->ctx.status == DUNLIN_MODEL_OK ? an_bad(&r->ctx, "invalid Messages thinking delta") : 0;
            ahp_reasoning(&r->ctx, "reasoning.delta", s);
            return 1;
        }
        if (ahp_is(kind, "thinking") && ahp_is(dt, "signature_delta")) {
            s = an_str(delta, "signature");
            if (!s) return an_bad(&r->ctx, "invalid Messages signature delta");
            return ahp_append(&r->ctx, &r->signature, s, strlen(s));
        }
        if (ahp_is(kind, "tool_use") && ahp_is(dt, "input_json_delta")) {
            s = an_str(delta, "partial_json");
            if (!s || json_object_object_length(ahp_field(r->block, "input")))
                return an_bad(&r->ctx, "invalid Messages tool input delta");
            r->input_delta = 1;
            return ahp_append(&r->ctx, &r->fragment, s, strlen(s));
        }
        return an_bad(&r->ctx, "unsupported Messages content delta");
    }
    if (ahp_is(type, "content_block_stop")) {
        if (!r->block || r->stopped || !an_index(r, event))
            return an_bad(&r->ctx, "invalid Messages content_block_stop");
        const char *kind = an_str(r->block, "type");
        if (ahp_is(kind, "text") || ahp_is(kind, "thinking")) {
            if (!an_string(&r->ctx, r->block, ahp_is(kind, "text") ? "text" : "thinking",
                           r->fragment.s ? r->fragment.s : "")) return 0;
            if (ahp_is(kind, "thinking") && (!r->signature.n ||
                !an_string(&r->ctx, r->block, "signature", r->signature.s)))
                return r->ctx.status == DUNLIN_MODEL_OK ? an_bad(&r->ctx, "missing Messages thinking signature") : 0;
        } else if (ahp_is(kind, "tool_use") && r->input_delta) {
            ahp_json *input = dunlin_json_parse(r->fragment.s, r->fragment.n);
            if (!json_object_is_type(input, json_type_object)) {
                if (input) json_object_put(input);
                return an_bad(&r->ctx, "incomplete or invalid Messages tool input JSON");
            }
            json_object_object_add(r->block, "input", input);
        }
        ahp_buffer_free(&r->fragment);
        ahp_buffer_free(&r->signature);
        r->input_delta = 0;
        r->block = NULL;
        ++r->index;
        return 1;
    }
    if (ahp_is(type, "message_delta")) {
        ahp_json *delta = ahp_field(event, "delta");
        if (!r->started || r->block || !json_object_is_type(delta, json_type_object))
            return an_bad(&r->ctx, "invalid Messages message_delta");
        if (!an_usage(r, ahp_field(event, "usage"))) return 0;
        if (ahp_field(delta, "stop_reason") && !an_finish(r, an_str(delta, "stop_reason"))) return 0;
        return 1;
    }
    if (ahp_is(type, "message_stop")) {
        if (!r->started || !r->stopped || r->block)
            return an_bad(&r->ctx, "incomplete Messages stream");
        r->done = 1;
        return 1;
    }
    /* The versioned API may add informational event types. Unknown content and
     * delta types above remain errors because silently dropping them loses state. */
    return 1;
}

static int an_append_message(ahp_context *c, ahp_json *messages, const char *role,
                             ahp_json *content)
{
    size_t n = (size_t)json_object_array_length(messages);
    ahp_json *last = n ? json_object_array_get_idx(messages, n - 1) : NULL;
    if (ahp_is(an_str(last, "role"), role)) {
        ahp_json *dest = ahp_field(last, "content");
        for (size_t i = 0; i < (size_t)json_object_array_length(content); ++i)
            json_object_array_add(dest, json_object_get(json_object_array_get_idx(content, i)));
        return 1;
    }
    ahp_json *message = an_object(c);
    if (!message || !an_string(c, message, "role", role)) {
        if (message) json_object_put(message);
        return 0;
    }
    json_object_object_add(message, "content", json_object_get(content));
    json_object_array_add(messages, message);
    return 1;
}

static ahp_json *an_text_blocks(ahp_context *c, const char *s)
{
    ahp_json *blocks = an_array(c), *block = an_object(c);
    if (!blocks || !block || !an_string(c, block, "type", "text") ||
        !an_string(c, block, "text", s)) {
        if (blocks) json_object_put(blocks);
        if (block) json_object_put(block);
        return NULL;
    }
    json_object_array_add(blocks, block);
    return blocks;
}

static int an_json_equal(ahp_json *a, ahp_json *b)
{
    if (!a || !b) return a == b;
    enum json_type type = json_object_get_type(a);
    if (type != json_object_get_type(b)) return 0;
    if (type == json_type_object) {
        if (json_object_object_length(a) != json_object_object_length(b)) return 0;
        json_object_object_foreach(a, key, value) {
            ahp_json *other = NULL;
            if (!json_object_object_get_ex(b, key, &other) || !an_json_equal(value, other)) return 0;
        }
        return 1;
    }
    if (type == json_type_array) {
        size_t n = (size_t)json_object_array_length(a);
        if (n != (size_t)json_object_array_length(b)) return 0;
        for (size_t i = 0; i < n; ++i)
            if (!an_json_equal(json_object_array_get_idx(a, i), json_object_array_get_idx(b, i))) return 0;
        return 1;
    }
    if (type == json_type_string)
        return json_object_get_string_len(a) == json_object_get_string_len(b) &&
               !memcmp(json_object_get_string(a), json_object_get_string(b),
                       (size_t)json_object_get_string_len(a));
    if (type == json_type_int) return json_object_get_int64(a) == json_object_get_int64(b);
    if (type == json_type_double) return json_object_get_double(a) == json_object_get_double(b);
    if (type == json_type_boolean) return json_object_get_boolean(a) == json_object_get_boolean(b);
    return type == json_type_null;
}

static int an_calls_equal(ahp_json *a, ahp_json *b)
{
    size_t n = a ? (size_t)json_object_array_length(a) : 0;
    if (n != (b ? (size_t)json_object_array_length(b) : 0)) return 0;
    for (size_t i = 0; i < n; ++i) {
        ahp_json *ca = json_object_array_get_idx(a, i), *cb = json_object_array_get_idx(b, i);
        ahp_json *fa = ahp_field(ca, "function"), *fb = ahp_field(cb, "function");
        const char *aa = an_str(fa, "arguments"), *ab = an_str(fb, "arguments");
        if (!ahp_is(an_str(ca, "id"), an_str(cb, "id")) ||
            !ahp_is(an_str(fa, "name"), an_str(fb, "name")) || !aa || !ab) return 0;
        ahp_json *pa = dunlin_json_parse(aa, strlen(aa)), *pb = dunlin_json_parse(ab, strlen(ab));
        int same = pa && pb && an_json_equal(pa, pb);
        if (pa) json_object_put(pa);
        if (pb) json_object_put(pb);
        if (!same) return 0;
    }
    return 1;
}

static ahp_json *an_assistant_content(ahp_context *c, ahp_json *message)
{
    ahp_json *native = ahp_native(c, message, "anthropic");
    if (c->status != DUNLIN_MODEL_OK) return NULL;
    ahp_json *calls = ahp_field(message, "tool_calls");
    int has_calls = json_object_is_type(calls, json_type_array) && json_object_array_length(calls);
    if (!ahp_valid_assistant(c, message, has_calls)) return NULL;
    if (native) {
        ahp_json *check = an_normalize(c, native, has_calls);
        if (!check) return NULL;
        ahp_json *native_calls = ahp_field(check, "tool_calls");
        int equal = an_calls_equal(calls, native_calls);
        const char *text = an_str(message, "content");
        equal = equal && (!text || ahp_is(text, an_str(check, "content")));
        json_object_put(check);
        if (!equal) {
            ahp_fail(c, DUNLIN_MODEL_INVALID, "Messages continuation disagrees with assistant history");
            return NULL;
        }
        ahp_json *copy = ahp_clone(native);
        if (!copy) ahp_fail(c, DUNLIN_MODEL_LIMIT, "model request allocation failed");
        return copy;
    }
    ahp_json *content = an_array(c);
    if (!content) return NULL;
    const char *text = an_str(message, "content");
    if (text && *text) {
        ahp_json *block = an_object(c);
        if (!block || !an_string(c, block, "type", "text") || !an_string(c, block, "text", text)) {
            if (block) json_object_put(block);
            json_object_put(content); return NULL;
        }
        json_object_array_add(content, block);
    }
    for (size_t i = 0; has_calls && i < (size_t)json_object_array_length(calls); ++i) {
        ahp_json *call = json_object_array_get_idx(calls, i), *fn = ahp_field(call, "function");
        const char *args = an_str(fn, "arguments");
        ahp_json *input = args ? dunlin_json_parse(args, strlen(args)) : NULL;
        if (!json_object_is_type(input, json_type_object)) {
            if (input) json_object_put(input);
            ahp_fail(c, DUNLIN_MODEL_INVALID, "Messages history requires complete tool input objects");
            json_object_put(content); return NULL;
        }
        ahp_json *block = an_object(c);
        if (!block) { json_object_put(input); json_object_put(content); return NULL; }
        json_object_object_add(block, "input", input);
        if (!an_string(c, block, "type", "tool_use") || !an_string(c, block, "id", an_str(call, "id")) ||
            !an_string(c, block, "name", an_str(fn, "name"))) {
            json_object_put(block); json_object_put(content); return NULL;
        }
        json_object_array_add(content, block);
    }
    if (!json_object_array_length(content)) {
        json_object_put(content);
        return an_text_blocks(c, "");
    }
    return content;
}

static int an_history(ahp_context *c, ahp_json *request, ahp_json *history)
{
    ahp_json *messages = an_array(c), *system = an_array(c);
    const char *pending[AN_MAX_CALLS] = {0};
    unsigned char matched[AN_MAX_CALLS] = {0};
    size_t pending_count = 0, remaining = 0;
    if (!messages || !system) {
        if (messages) json_object_put(messages);
        if (system) json_object_put(system);
        return 0;
    }
    json_object_object_add(request, "messages", messages);
    for (size_t i = 0; i < (size_t)json_object_array_length(history); ++i) {
        ahp_json *source = json_object_array_get_idx(history, i), *content = NULL;
        const char *role = an_str(source, "role"), *text = an_str(source, "content");
        if (ahp_is(role, "system")) {
            if (!text) goto invalid;
            if (!*text) continue;
            ahp_json *blocks = an_text_blocks(c, text);
            if (!blocks) goto failed;
            json_object_array_add(system, json_object_get(json_object_array_get_idx(blocks, 0)));
            json_object_put(blocks);
            continue;
        }
        if (ahp_is(role, "tool")) {
            const char *id = an_str(source, "tool_call_id");
            if (!remaining || !id || !*id || !text) goto invalid;
            size_t match = 0;
            while (match < pending_count && !ahp_is(pending[match], id)) ++match;
            if (match == pending_count || matched[match]) goto invalid;
            matched[match] = 1;
            --remaining;
            content = an_array(c);
            ahp_json *block = an_object(c);
            if (!content || !block || !an_string(c, block, "type", "tool_result") ||
                !an_string(c, block, "tool_use_id", id) || !an_string(c, block, "content", text)) {
                if (content) json_object_put(content);
                if (block) json_object_put(block);
                goto failed;
            }
            ahp_json *is_error = ahp_field(source, "is_error");
            if (is_error && !json_object_is_type(is_error, json_type_boolean)) {
                json_object_put(block); json_object_put(content); goto invalid;
            }
            if (is_error) json_object_object_add(block, "is_error", json_object_get(is_error));
            json_object_array_add(content, block);
            role = "user";
        } else if (ahp_is(role, "assistant")) {
            if (remaining) goto invalid;
            content = an_assistant_content(c, source);
            if (!content) goto failed;
            pending_count = remaining = 0;
            memset(matched, 0, sizeof(matched));
            for (size_t j = 0; j < (size_t)json_object_array_length(content); ++j) {
                ahp_json *block = json_object_array_get_idx(content, j);
                if (!ahp_is(an_str(block, "type"), "tool_use")) continue;
                if (pending_count == AN_MAX_CALLS) { json_object_put(content); goto invalid; }
                pending[pending_count++] = an_str(block, "id");
                ++remaining;
            }
        } else if (ahp_is(role, "user")) {
            if (remaining || !text) goto invalid;
            content = an_text_blocks(c, text);
            if (!content) goto failed;
        } else goto invalid;
        int ok = an_append_message(c, messages, role, content);
        json_object_put(content);
        if (!ok) goto failed;
    }
    if (remaining || !json_object_array_length(messages)) goto invalid;
    if (json_object_array_length(system)) {
        json_object_object_add(request, "system", system);
        system = NULL;
    }
    if (system) json_object_put(system);
    return 1;
invalid:
    ahp_fail(c, DUNLIN_MODEL_INVALID, "unsupported or unpaired Messages history");
failed:
    if (system) json_object_put(system);
    return 0;
}

static int an_tools(ahp_context *c, ahp_json *request, ahp_json *tools)
{
    if (!tools || !json_object_array_length(tools)) return 1;
    ahp_json *native = an_array(c);
    if (!native) return 0;
    json_object_object_add(request, "tools", native);
    for (size_t i = 0; i < (size_t)json_object_array_length(tools); ++i) {
        ahp_json *tool = json_object_array_get_idx(tools, i), *fn = ahp_field(tool, "function");
        const char *name = an_str(fn, "name"), *description = an_str(fn, "description");
        ahp_json *schema = ahp_field(fn, "parameters");
        if (!ahp_is(an_str(tool, "type"), "function") || !name || !*name ||
            !json_object_is_type(schema, json_type_object) ||
            (ahp_field(fn, "description") && !description))
            return ahp_fail(c, DUNLIN_MODEL_INVALID, "unsupported Messages tool schema");
        ahp_json *out = an_object(c);
        if (!out) return 0;
        json_object_array_add(native, out);
        if (!an_string(c, out, "name", name) || !an_copy(c, out, "input_schema", schema) ||
            (description && !an_string(c, out, "description", description))) return 0;
    }
    return 1;
}

dunlin_model_status ahp_anthropic(const dunlin_model_config *cfg, const dunlin_protocol_options *opt,
    ahp_json *messages, ahp_json *tools, const dunlin_model_events *events,
    ahp_json **assistant, char *error, size_t error_size)
{
    an_response r = { .ctx = { cfg, opt, events, DUNLIN_MODEL_OK, {0} } };
    ahp_json *request = NULL, *plain = NULL, *result = NULL;
    *assistant = NULL;
    if (opt->has_temperature) {
        ahp_fail(&r.ctx, DUNLIN_MODEL_INVALID, "temperature is unsupported by this Messages adapter");
        goto done;
    }
    request = an_object(&r.ctx);
    r.output = an_array(&r.ctx);
    if (!request || !r.output || !an_string(&r.ctx, request, "model", cfg->model)) goto done;
    json_object_object_add(request, "max_tokens", json_object_new_int64(opt->max_tokens ? opt->max_tokens : 1024));
    json_object_object_add(request, "stream", json_object_new_boolean(cfg->stream));
    if (!an_history(&r.ctx, request, messages) || !an_tools(&r.ctx, request, tools)) goto done;
    if (opt->required_tool && *opt->required_tool) {
        ahp_json *choice = an_object(&r.ctx);
        if (!choice) goto done;
        json_object_object_add(request, "tool_choice", choice);
        if (!an_string(&r.ctx, choice, "type", "tool") ||
            !an_string(&r.ctx, choice, "name", opt->required_tool)) goto done;
    }
    if (opt->reasoning_effort == DUNLIN_REASONING_NONE) {
        ahp_json *thinking = an_object(&r.ctx);
        if (!thinking) goto done;
        json_object_object_add(request, "thinking", thinking);
        if (!an_string(&r.ctx, thinking, "type", "disabled")) goto done;
    } else if (opt->reasoning_effort != DUNLIN_REASONING_DEFAULT) {
        const char *effort = ahp_effort(opt->reasoning_effort);
        ahp_json *output = an_object(&r.ctx);
        if (!effort || !output) {
            if (output) json_object_put(output);
            ahp_fail(&r.ctx, DUNLIN_MODEL_INVALID, "unsupported Messages effort"); goto done;
        }
        json_object_object_add(request, "output_config", output);
        if (!an_string(&r.ctx, output, "effort", effort)) goto done;
    }
    const char *version = opt->anthropic_version ? opt->anthropic_version : "2026-10-21";
    dunlin_http_header headers[] = {{"anthropic-version", version}};
    if (!ahp_exchange(&r.ctx, "/messages", request, AHP_SSE, headers, 1, an_event, &r, &plain)) goto done;
    if (cfg->stream) {
        if (!r.done) { an_bad(&r.ctx, "incomplete Messages stream (message_stop required)"); goto done; }
    } else {
        const char *id = an_str(plain, "id");
        if (ahp_field(plain, "error") || !ahp_is(an_str(plain, "type"), "message") ||
            !ahp_is(an_str(plain, "role"), "assistant") || !id || !*id) {
            an_bad(&r.ctx, "invalid Messages response"); goto done;
        }
        if (!an_usage(&r, ahp_field(plain, "usage")) || !an_finish(&r, an_str(plain, "stop_reason"))) goto done;
        ahp_json *content = ahp_field(plain, "content");
        if (!json_object_is_type(content, json_type_array)) {
            an_bad(&r.ctx, "missing Messages content array"); goto done;
        }
        json_object_put(r.output);
        r.output = json_object_get(content);
    }
    result = an_normalize(&r.ctx, r.output, r.expects_tools);
    if (!result) goto done;
    if (!cfg->stream) {
        for (size_t i = 0; i < (size_t)json_object_array_length(r.output); ++i) {
            ahp_json *block = json_object_array_get_idx(r.output, i);
            if (ahp_is(an_str(block, "type"), "thinking"))
                ahp_reasoning(&r.ctx, "reasoning.delta", an_str(block, "thinking"));
        }
    }
    if (!ahp_metadata(&r.ctx, result, "anthropic", r.output)) goto done;
    *assistant = result;
    result = NULL;
done:
    if (result) json_object_put(result);
    if (request) json_object_put(request);
    if (plain) json_object_put(plain);
    if (r.output) json_object_put(r.output);
    ahp_buffer_free(&r.fragment);
    ahp_buffer_free(&r.signature);
    return ahp_result(&r.ctx, error, error_size);
}
