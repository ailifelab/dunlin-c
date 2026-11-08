#include "protocol_internal.h"
#include "dunlin/compact/json.h"
#include <stdint.h>
#include <string.h>

enum { OLLAMA_MAX_CALLS = 64 };

typedef struct {
    ahp_context *ctx;
    ahp_buffer content, thinking;
    ahp_json *native, *call[OLLAMA_MAX_CALLS];
    size_t calls;
    int done, saw_message, saw_thinking;
} ollama_response;

static int put(ahp_context *c, ahp_json *o, const char *key, ahp_json *v)
{
    if (!v) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed");
    json_object_object_add(o, key, v);
    return 1;
}

static int text(ahp_context *c, ahp_json *o, const char *key, const char *v)
{
    return put(c, o, key, json_object_new_string(v));
}

static int push(ahp_context *c, ahp_json *a, ahp_json *v)
{
    if (!v) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed");
    if (json_object_array_add(a, v) == 0) return 1;
    json_object_put(v);
    return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed");
}

/* json_object_equal is unavailable in the oldest supported json-c. */
static int same_json(ahp_json *a, ahp_json *b)
{
    if (!a || !b) return a == b;
    enum json_type type = json_object_get_type(a);
    if (type != json_object_get_type(b)) return 0;
    switch (type) {
    case json_type_null: return 1;
    case json_type_boolean: return json_object_get_boolean(a) == json_object_get_boolean(b);
    case json_type_int: return json_object_get_int64(a) == json_object_get_int64(b);
    case json_type_double: return json_object_get_double(a) == json_object_get_double(b);
    case json_type_string:
        return json_object_get_string_len(a) == json_object_get_string_len(b) &&
               !strcmp(json_object_get_string(a), json_object_get_string(b));
    case json_type_array:
        if (json_object_array_length(a) != json_object_array_length(b)) return 0;
        for (size_t i = 0; i < (size_t)json_object_array_length(a); ++i)
            if (!same_json(json_object_array_get_idx(a, i), json_object_array_get_idx(b, i))) return 0;
        return 1;
    case json_type_object:
        if (json_object_object_length(a) != json_object_object_length(b)) return 0;
        json_object_object_foreach(a, key, value) {
            ahp_json *other = NULL;
            if (!json_object_object_get_ex(b, key, &other) || !same_json(value, other)) return 0;
        }
        return 1;
    }
    return 0;
}

static int input_error(ahp_context *c)
{
    return ahp_fail(c, DUNLIN_MODEL_INVALID, "invalid Ollama message or tool result pairing");
}

static int present(ahp_json *o, const char *key)
{
    ahp_json *value = NULL;
    return json_object_object_get_ex(o, key, &value);
}

static ahp_json *input_call(ahp_context *c, ahp_json *call, size_t index)
{
    ahp_json *fn = ahp_field(call, "function"), *args = NULL, *native = NULL, *out = NULL;
    const char *id = ahp_str(call, "id"), *name = ahp_str(fn, "name"), *encoded = ahp_str(fn, "arguments");
    if (!ahp_is(ahp_str(call, "type"), "function") || !id || !*id || !name || !*name || !encoded)
        goto invalid;
    args = dunlin_json_parse(encoded, strlen(encoded));
    if (!json_object_is_type(args, json_type_object)) goto invalid;
    native = json_object_new_object();
    out = json_object_new_object();
    if (!native || !out) {
        ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed");
        goto failed;
    }
    if (!text(c, native, "type", "function") || !text(c, out, "name", name) ||
        !put(c, out, "index", json_object_new_int64((int64_t)index))) goto failed;
    json_object_object_add(out, "arguments", args);
    args = NULL;
    json_object_object_add(native, "function", out);
    return native;
invalid:
    input_error(c);
failed:
    if (args) json_object_put(args);
    if (out) json_object_put(out);
    if (native) json_object_put(native);
    return NULL;
}

static int native_matches(ahp_json *native, ahp_json *converted)
{
    ahp_json *a = ahp_field(native, "function"), *b = ahp_field(converted, "function");
    const char *name = ahp_str(a, "name");
    return json_object_is_type(native, json_type_object) && name &&
           ahp_is(name, ahp_str(b, "name")) &&
           (!ahp_field(native, "type") || ahp_is(ahp_str(native, "type"), "function")) &&
           (!present(a, "index") || same_json(ahp_field(a, "index"), ahp_field(b, "index"))) &&
           same_json(ahp_field(a, "arguments"), ahp_field(b, "arguments"));
}

static ahp_json *history(ahp_context *c, ahp_json *messages)
{
    ahp_json *result = json_object_new_array(), *pending = NULL;
    size_t paired = 0;
    if (!result) { ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed"); return NULL; }
    for (size_t i = 0; i < (size_t)json_object_array_length(messages); ++i) {
        ahp_json *m = json_object_array_get_idx(messages, i), *out = NULL;
        const char *role = ahp_str(m, "role"), *content = ahp_str(m, "content");
        if (pending && paired < (size_t)json_object_array_length(pending) && !ahp_is(role, "tool"))
            goto invalid;
        ahp_json *native = ahp_native(c, m, "ollama");
        if (c->status != DUNLIN_MODEL_OK) goto failed;
        if (native && !ahp_is(role, "assistant")) goto invalid;
        if (ahp_is(role, "tool")) {
            if (!pending || paired >= (size_t)json_object_array_length(pending) || !content)
                goto invalid;
            ahp_json *call = json_object_array_get_idx(pending, paired);
            if (!ahp_is(ahp_str(m, "tool_call_id"), ahp_str(call, "id"))) goto invalid;
            out = json_object_new_object();
            if (!out) goto allocation;
            if (!text(c, out, "role", "tool") || !text(c, out, "content", content) ||
                !text(c, out, "tool_name", ahp_str(ahp_field(call, "function"), "name"))) {
                json_object_put(out); goto failed;
            }
            ++paired;
        } else if (ahp_is(role, "assistant")) {
            ahp_json *calls = ahp_field(m, "tool_calls"), *native_calls = ahp_field(native, "tool_calls");
            if ((calls && !json_object_is_type(calls, json_type_array)) ||
                (ahp_field(m, "content") && !content)) goto invalid;
            size_t n = calls ? (size_t)json_object_array_length(calls) : 0;
            if (n > OLLAMA_MAX_CALLS || (!n && !content)) goto invalid;
            if (native && (!ahp_is(ahp_str(native, "role"), "assistant") ||
                !ahp_str(native, "content") || !ahp_is(ahp_str(native, "content"), content ? content : "") ||
                (ahp_field(native, "thinking") && !ahp_str(native, "thinking")) ||
                (native_calls && !json_object_is_type(native_calls, json_type_array)) ||
                (native_calls ? (size_t)json_object_array_length(native_calls) : 0) != n)) goto invalid;
            out = native ? ahp_clone(native) : json_object_new_object();
            if (!out) goto allocation;
            if (!native && (!text(c, out, "role", "assistant") || !text(c, out, "content", content ? content : ""))) {
                json_object_put(out); goto failed;
            }
            ahp_json *translated = n && !native ? json_object_new_array() : NULL;
            if (n && !native && !translated) { json_object_put(out); goto allocation; }
            if (translated) json_object_object_add(out, "tool_calls", translated);
            for (size_t j = 0; j < n; ++j) {
                ahp_json *call = json_object_array_get_idx(calls, j);
                ahp_json *converted = input_call(c, call, j);
                if (!converted) { json_object_put(out); goto failed; }
                for (size_t k = 0; k < j; ++k) {
                    if (ahp_is(ahp_str(call, "id"), ahp_str(json_object_array_get_idx(calls, k), "id"))) {
                        json_object_put(converted); json_object_put(out); goto invalid;
                    }
                }
                if (native) {
                    int same = native_matches(json_object_array_get_idx(native_calls, j), converted);
                    json_object_put(converted);
                    if (!same) { json_object_put(out); goto invalid; }
                } else if (!push(c, translated, converted)) { json_object_put(out); goto failed; }
            }
            pending = calls;
            paired = 0;
        } else if (ahp_is(role, "system") || ahp_is(role, "user")) {
            if (!content || ahp_field(m, "tool_calls")) goto invalid;
            out = json_object_new_object();
            if (!out) goto allocation;
            if (!text(c, out, "role", role) || !text(c, out, "content", content)) {
                json_object_put(out); goto failed;
            }
            pending = NULL;
            paired = 0;
        } else goto invalid;
        if (!push(c, result, out)) goto failed;
    }
    if (pending && paired < (size_t)json_object_array_length(pending)) goto invalid;
    return result;
allocation:
    ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed");
    goto failed;
invalid:
    input_error(c);
failed:
    json_object_put(result);
    return NULL;
}

static int response_call(ollama_response *r, ahp_json *call)
{
    ahp_context *c = r->ctx;
    ahp_json *fn = ahp_field(call, "function"), *args = ahp_field(fn, "arguments"), *idx = ahp_field(fn, "index");
    const char *name = ahp_str(fn, "name");
    if (!json_object_is_type(call, json_type_object) || !name || !*name ||
        !json_object_is_type(args, json_type_object) ||
        (ahp_field(call, "type") && !ahp_is(ahp_str(call, "type"), "function")))
        return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "invalid Ollama function call");
    size_t index = r->calls;
    if (idx) {
        if (!json_object_is_type(idx, json_type_int) || json_object_get_int64(idx) < 0 ||
            json_object_get_int64(idx) >= OLLAMA_MAX_CALLS)
            return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "invalid Ollama function index");
        index = (size_t)json_object_get_int64(idx);
    }
    if (index >= OLLAMA_MAX_CALLS)
        return ahp_fail(c, DUNLIN_MODEL_LIMIT, "too many Ollama function calls");
    if (r->call[index]) return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "duplicate Ollama function index");
    r->call[index] = ahp_clone(call);
    if (!r->call[index]) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed");
    if (index >= r->calls) r->calls = index + 1;
    return 1;
}

static int usage(ahp_context *c, ahp_json *o)
{
    dunlin_model_metrics scratch = {0};
    dunlin_model_metrics *m = c->options->metrics ? c->options->metrics : &scratch;
    if (!ahp_metric(c, o, "prompt_eval_count", DUNLIN_USAGE_INPUT, &m->input_tokens) ||
        !ahp_metric(c, o, "eval_count", DUNLIN_USAGE_OUTPUT, &m->output_tokens) ||
        !ahp_metric(c, o, "prompt_eval_cached_count", DUNLIN_USAGE_CACHED_INPUT, &m->cached_input_tokens)) return 0;
    if (ahp_field(o, "prompt_eval_count") && ahp_field(o, "eval_count")) {
        if (m->input_tokens > UINT64_MAX - m->output_tokens)
            return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "Ollama token statistics overflow");
        m->total_tokens = m->input_tokens + m->output_tokens;
        m->present |= DUNLIN_USAGE_TOTAL;
    }
    return 1;
}

static int consume(void *user, ahp_json *o, const char *event)
{
    (void)event;
    ollama_response *r = user;
    ahp_context *c = r->ctx;
    if (!json_object_is_type(o, json_type_object))
        return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "invalid Ollama response object");
    if (present(o, "error")) return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "Ollama reported a model error");
    if (r->done) return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "Ollama data after completed response");
    ahp_json *done = ahp_field(o, "done"), *msg = ahp_field(o, "message");
    if (!json_object_is_type(done, json_type_boolean))
        return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "missing Ollama completion marker");
    int finished = json_object_get_boolean(done);
    if (!msg && !(finished && r->saw_message))
        return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "missing Ollama assistant message");
    if (msg) {
        if (!json_object_is_type(msg, json_type_object) ||
            (present(msg, "role") && !ahp_is(ahp_str(msg, "role"), "assistant")) ||
            (!r->saw_message && !ahp_is(ahp_str(msg, "role"), "assistant")))
            return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "invalid Ollama assistant message");
        if (!c->cfg->stream) {
            ahp_json *copy = ahp_clone(msg);
            if (!copy) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed");
            json_object_put(r->native);
            r->native = copy;
        }
        r->saw_message = 1;
        json_object_object_foreach(msg, key, value) {
            if (!strcmp(key, "role") || !strcmp(key, "content") || !strcmp(key, "thinking") || !strcmp(key, "tool_calls")) continue;
            ahp_json *previous = NULL;
            if (json_object_object_get_ex(r->native, key, &previous)) {
                if (!same_json(previous, value)) return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "conflicting Ollama message metadata");
            } else if (value) {
                /* Retain an immutable field; ahp_clone parses containers only. */
                if (!put(c, r->native, key, json_object_get(value))) return 0;
            } else json_object_object_add(r->native, key, NULL);
        }
        if (present(msg, "content")) {
            const char *s = ahp_str(msg, "content");
            if (!s) return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "invalid Ollama content");
            if (c->cfg->stream ? !ahp_text(c, &r->content, s) : !ahp_append(c, &r->content, s, strlen(s))) return 0;
        }
        if (present(msg, "thinking")) {
            const char *s = ahp_str(msg, "thinking");
            if (!s) return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "invalid Ollama thinking content");
            if (!ahp_append(c, &r->thinking, s, strlen(s))) return 0;
            r->saw_thinking = 1;
            ahp_reasoning(c, "reasoning.delta", s);
        }
        ahp_json *calls = ahp_field(msg, "tool_calls");
        if (present(msg, "tool_calls")) {
            if (!json_object_is_type(calls, json_type_array))
                return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "invalid Ollama tool_calls");
            for (size_t i = 0; i < (size_t)json_object_array_length(calls); ++i)
                if (!response_call(r, json_object_array_get_idx(calls, i))) return 0;
        }
    }
    if (finished) {
        if (!usage(c, o)) return 0;
        const char *reason = ahp_str(o, "done_reason");
        if (present(o, "done_reason") && !reason)
            return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "invalid Ollama completion reason");
        if (ahp_is(reason, "length")) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "Ollama stopped at its token limit");
        if (reason && !ahp_is(reason, "stop")) return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "unsupported Ollama completion reason");
        r->done = 1;
    }
    return 1;
}

static ahp_json *finish(ollama_response *r)
{
    ahp_context *c = r->ctx;
    if (!r->done || !r->saw_message) {
        ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "incomplete Ollama response");
        return NULL;
    }
    ahp_json *msg = json_object_new_object(), *native_calls = NULL, *calls = NULL;
    if (!msg) { ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed"); return NULL; }
    const char *content = r->content.s ? r->content.s : "";
    if (!text(c, msg, "role", "assistant") || !text(c, msg, "content", content) ||
        !text(c, r->native, "role", "assistant") || !text(c, r->native, "content", content) ||
        (r->saw_thinking && !text(c, r->native, "thinking", r->thinking.s ? r->thinking.s : ""))) goto failed;
    if (r->calls) {
        calls = json_object_new_array();
        native_calls = json_object_new_array();
        if (!calls || !native_calls) {
            if (calls) json_object_put(calls);
            if (native_calls) json_object_put(native_calls);
            ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed");
            goto failed;
        }
        json_object_object_add(msg, "tool_calls", calls);
        json_object_object_add(r->native, "tool_calls", native_calls);
        for (size_t i = 0; i < r->calls; ++i) {
            if (!r->call[i]) { ahp_fail(c, DUNLIN_MODEL_PROTOCOL, "missing Ollama function index"); goto failed; }
            char id[49];
            if (!ahp_call_id(c, id)) goto failed;
            ahp_json *call = json_object_new_object(), *fn = json_object_new_object();
            if (!call || !fn) {
                if (call) json_object_put(call);
                if (fn) json_object_put(fn);
                ahp_fail(c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed"); goto failed;
            }
            json_object_object_add(call, "function", fn);
            ahp_json *native_fn = ahp_field(r->call[i], "function");
            if (!text(c, call, "type", "function") || !text(c, call, "id", id) ||
                !text(c, fn, "name", ahp_str(native_fn, "name")) ||
                !text(c, fn, "arguments", json_object_to_json_string_ext(ahp_field(native_fn, "arguments"), JSON_C_TO_STRING_PLAIN))) {
                json_object_put(call); goto failed;
            }
            if (!push(c, calls, call) || !push(c, native_calls, json_object_get(r->call[i]))) goto failed;
        }
    }
    if (!ahp_valid_assistant(c, msg, r->calls != 0)) goto failed;
    if (!ahp_metadata(c, msg, "ollama", r->native)) goto failed;
    return msg;
failed:
    json_object_put(msg);
    return NULL;
}

dunlin_model_status ahp_ollama(const dunlin_model_config *cfg, const dunlin_protocol_options *options,
    ahp_json *messages, ahp_json *tools, const dunlin_model_events *events,
    ahp_json **assistant, char *error, size_t error_size)
{
    ahp_context c = {.cfg = cfg, .options = options, .events = events, .status = DUNLIN_MODEL_OK};
    ollama_response r = {.ctx = &c};
    ahp_json *request = NULL, *translated = NULL, *plain = NULL, *settings = NULL;
    *assistant = NULL;
    translated = history(&c, messages);
    if (!translated) goto done;
    request = json_object_new_object();
    r.native = json_object_new_object();
    if (!request || !r.native) { ahp_fail(&c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed"); goto done; }
    json_object_object_add(request, "messages", translated);
    translated = NULL;
    if (!text(&c, request, "model", cfg->model) || !put(&c, request, "stream", json_object_new_boolean(cfg->stream))) goto done;
    if (tools && json_object_array_length(tools)) json_object_object_add(request, "tools", json_object_get(tools));
    if (options->max_tokens || options->has_temperature) {
        settings = json_object_new_object();
        if (!settings) { ahp_fail(&c, DUNLIN_MODEL_LIMIT, "model JSON allocation failed"); goto done; }
        json_object_object_add(request, "options", settings);
        if ((options->max_tokens && !put(&c, settings, "num_predict", json_object_new_int64(options->max_tokens))) ||
            (options->has_temperature && !put(&c, settings, "temperature", json_object_new_double(options->temperature)))) goto done;
    }
    if (options->reasoning_effort == DUNLIN_REASONING_NONE) {
        if (!put(&c, request, "think", json_object_new_boolean(0))) goto done;
    } else if (options->reasoning_effort != DUNLIN_REASONING_DEFAULT) {
        if (!text(&c, request, "think", ahp_effort(options->reasoning_effort))) goto done;
    }
    if (!ahp_exchange(&c, "/chat", request, AHP_NDJSON, NULL, 0, consume, &r, &plain)) goto done;
    if (!cfg->stream && !consume(&r, plain, NULL)) goto done;
    *assistant = finish(&r);
done:
    if (plain) json_object_put(plain);
    if (translated) json_object_put(translated);
    if (request) json_object_put(request);
    if (r.native) json_object_put(r.native);
    for (size_t i = 0; i < OLLAMA_MAX_CALLS; ++i) if (r.call[i]) json_object_put(r.call[i]);
    ahp_buffer_free(&r.content);
    ahp_buffer_free(&r.thinking);
    return ahp_result(&c, error, error_size);
}
