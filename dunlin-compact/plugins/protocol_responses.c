#include "protocol_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { RP_MAX_CALLS = 64 };
typedef struct { ahp_buffer text; size_t shown; int added, done; } rp_piece;
typedef struct {
    ahp_json *added, *done;
    ahp_buffer arguments;
    int arguments_done;
    rp_piece *content, *summary;
    size_t content_count, content_capacity, summary_count, summary_capacity;
} rp_item;
typedef struct {
    ahp_context ctx;
    rp_item *items;
    size_t count, capacity;
    ahp_json *created, *output;
    ahp_buffer response_id;
    int done;
    size_t shown_summary_item, shown_summary_part;
    int summary_shown;
} rp_response;

static int rp_bad(ahp_context *c, const char *why)
{
    return ahp_fail(c, DUNLIN_MODEL_PROTOCOL, why);
}

static const char *rp_str(ahp_json *o, const char *key)
{
    ahp_json *v = ahp_field(o, key);
    if (!json_object_is_type(v, json_type_string)) return NULL;
    const char *s = json_object_get_string(v);
    return s && strlen(s) == (size_t)json_object_get_string_len(v) ? s : NULL;
}

static int rp_same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

static int rp_identity(rp_response *r, const char *id)
{
    if (!id || !*id || (r->response_id.n && !rp_same(r->response_id.s, id)))
        return rp_bad(&r->ctx, "conflicting Responses response identity");
    return r->response_id.n || ahp_append(&r->ctx, &r->response_id, id, strlen(id));
}

static ahp_json *rp_object(ahp_context *c)
{
    ahp_json *v = json_object_new_object();
    if (!v) ahp_fail(c, DUNLIN_MODEL_LIMIT, "model allocation failed");
    return v;
}

static ahp_json *rp_array(ahp_context *c)
{
    ahp_json *v = json_object_new_array();
    if (!v) ahp_fail(c, DUNLIN_MODEL_LIMIT, "model allocation failed");
    return v;
}

static int rp_string(ahp_context *c, ahp_json *o, const char *key, const char *s)
{
    ahp_json *v = s ? json_object_new_string(s) : NULL;
    if (!v) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model allocation failed");
    json_object_object_add(o, key, v);
    return 1;
}

static int rp_copy(ahp_context *c, ahp_json *o, const char *key, ahp_json *source)
{
    ahp_json *v = ahp_clone(source);
    if (!v) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model allocation failed");
    json_object_object_add(o, key, v);
    return 1;
}

static int rp_grow(ahp_context *c, void **data, size_t *capacity, size_t need, size_t size)
{
    if (need <= *capacity) return 1;
    if (need > SIZE_MAX / size) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "too many Responses output items");
    size_t cap = *capacity && *capacity <= SIZE_MAX / 2 ? *capacity * 2 : 4;
    if (cap < need) cap = need;
    if (cap > SIZE_MAX / size) cap = need;
    void *next = realloc(*data, cap * size);
    if (!next) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model allocation failed");
    memset((char *)next + *capacity * size, 0, (cap - *capacity) * size);
    *data = next;
    *capacity = cap;
    return 1;
}

static int rp_index(ahp_json *event, const char *name, size_t *value)
{
    ahp_json *v = ahp_field(event, name);
    if (!json_object_is_type(v, json_type_int) || json_object_get_int64(v) < 0 ||
        (uint64_t)json_object_get_int64(v) > SIZE_MAX) return 0;
    *value = (size_t)json_object_get_int64(v);
    return 1;
}

static int rp_prefix(const ahp_buffer *buffer, const char *whole, int exact)
{
    if (!whole) return 0;
    size_t n = strlen(whole);
    return n >= buffer->n && (!exact || n == buffer->n) &&
           (!buffer->n || !memcmp(buffer->s, whole, buffer->n));
}

static int rp_complete_text(ahp_context *c, ahp_buffer *buffer, const char *whole)
{
    if (!rp_prefix(buffer, whole, 0)) return rp_bad(c, "Responses completed text contradicts its fragments");
    return ahp_append(c, buffer, whole + buffer->n, strlen(whole) - buffer->n);
}

static int rp_known_type(ahp_json *item)
{
    const char *type = rp_str(item, "type");
    return ahp_is(type, "message") || ahp_is(type, "function_call") || ahp_is(type, "reasoning");
}

static void rp_show_summary(rp_response *r, size_t item, size_t part, const char *text)
{
    if (!text || !*text) return;
    if (r->summary_shown && (r->shown_summary_item != item || r->shown_summary_part != part))
        ahp_reasoning(&r->ctx, "reasoning.summary", "\n");
    ahp_reasoning(&r->ctx, "reasoning.summary", text);
    r->shown_summary_item = item;
    r->shown_summary_part = part;
    r->summary_shown = 1;
}

static void rp_show_summary_piece(rp_response *r, size_t item, size_t part, rp_piece *piece)
{
    if (piece->shown == piece->text.n) return;
    rp_show_summary(r, item, part, piece->text.s + piece->shown);
    piece->shown = piece->text.n;
}

/* The terminal output has been fully validated before this fallback. A provider
 * may supply a public summary only in a completed snapshot, or complete a prefix
 * without sending every delta. Opaque reasoning fields never enter this path. */
static void rp_show_summary_remaining(rp_response *r)
{
    for (size_t i = 0; i < (size_t)json_object_array_length(r->output); ++i) {
        ahp_json *item = json_object_array_get_idx(r->output, i);
        if (!ahp_is(rp_str(item, "type"), "reasoning")) continue;
        ahp_json *summary = ahp_field(item, "summary");
        for (size_t j = 0; j < (size_t)json_object_array_length(summary); ++j) {
            const char *s = rp_str(json_object_array_get_idx(summary, j), "text");
            rp_piece *piece = i < r->count && j < r->items[i].summary_count ? &r->items[i].summary[j] : NULL;
            rp_show_summary(r, i, j, s + (piece ? piece->shown : 0));
            if (piece) piece->shown = strlen(s);
        }
    }
}

static int rp_usage(ahp_context *c, ahp_json *usage)
{
    if (!usage) return 1;
    if (!json_object_is_type(usage, json_type_object)) return rp_bad(c, "invalid Responses usage object");
    dunlin_model_metrics ignored = {0}, *m = c->options->metrics ? c->options->metrics : &ignored;
    if (!ahp_metric(c, usage, "input_tokens", DUNLIN_USAGE_INPUT, &m->input_tokens) ||
        !ahp_metric(c, usage, "output_tokens", DUNLIN_USAGE_OUTPUT, &m->output_tokens) ||
        !ahp_metric(c, usage, "total_tokens", DUNLIN_USAGE_TOTAL, &m->total_tokens)) return 0;
    ahp_json *in = ahp_field(usage, "input_tokens_details"), *out = ahp_field(usage, "output_tokens_details");
    if ((in && !json_object_is_type(in, json_type_object)) ||
        (out && !json_object_is_type(out, json_type_object)))
        return rp_bad(c, "invalid Responses token detail object");
    return ahp_metric(c, in, "cached_tokens", DUNLIN_USAGE_CACHED_INPUT, &m->cached_input_tokens) &&
           ahp_metric(c, out, "reasoning_tokens", DUNLIN_USAGE_REASONING, &m->reasoning_tokens);
}

static ahp_json *rp_normalize(ahp_context *c, ahp_json *output, int show_summary)
{
    ahp_json *message = NULL, *calls = NULL;
    ahp_buffer text = {0};
    int summary_shown = 0;
    if (!json_object_is_type(output, json_type_array)) {
        rp_bad(c, "missing Responses output array"); return NULL;
    }
    message = rp_object(c);
    calls = rp_array(c);
    if (!message || !calls || !rp_string(c, message, "role", "assistant")) goto fail;
    for (size_t i = 0; i < (size_t)json_object_array_length(output); ++i) {
        ahp_json *item = json_object_array_get_idx(output, i);
        const char *id = rp_str(item, "id"), *type = rp_str(item, "type"), *status = rp_str(item, "status");
        if (!rp_known_type(item) || !id || !*id ||
            (ahp_field(item, "status") && !ahp_is(status, "completed"))) {
            rp_bad(c, "unsupported or incomplete Responses output item"); goto fail;
        }
        for (size_t j = 0; j < i; ++j)
            if (rp_same(id, rp_str(json_object_array_get_idx(output, j), "id"))) {
                rp_bad(c, "duplicate Responses output item id"); goto fail;
            }
        if (ahp_is(type, "message")) {
            ahp_json *content = ahp_field(item, "content");
            if (!ahp_is(rp_str(item, "role"), "assistant") || !json_object_is_type(content, json_type_array)) {
                rp_bad(c, "invalid Responses assistant message"); goto fail;
            }
            for (size_t j = 0; j < (size_t)json_object_array_length(content); ++j) {
                ahp_json *part = json_object_array_get_idx(content, j);
                const char *s = rp_str(part, "text");
                if (!ahp_is(rp_str(part, "type"), "output_text") || !s) {
                    rp_bad(c, "unsupported Responses message content"); goto fail;
                }
                if (!ahp_append(c, &text, s, strlen(s))) goto fail;
            }
        } else if (ahp_is(type, "function_call")) {
            const char *call_id = rp_str(item, "call_id"), *name = rp_str(item, "name"), *args = rp_str(item, "arguments");
            if (!call_id || !*call_id || !name || !*name || !args) {
                rp_bad(c, "incomplete Responses function call"); goto fail;
            }
            if ((size_t)json_object_array_length(calls) >= RP_MAX_CALLS) {
                ahp_fail(c, DUNLIN_MODEL_LIMIT, "too many model tool calls"); goto fail;
            }
            ahp_json *call = rp_object(c), *fn = rp_object(c);
            if (!call || !fn) {
                if (call) json_object_put(call);
                if (fn) json_object_put(fn);
                goto fail;
            }
            json_object_object_add(call, "function", fn);
            if (!rp_string(c, call, "type", "function") || !rp_string(c, call, "id", call_id) ||
                !rp_string(c, fn, "name", name) || !rp_string(c, fn, "arguments", args)) {
                json_object_put(call); goto fail;
            }
            json_object_array_add(calls, call);
        } else {
            ahp_json *summary = ahp_field(item, "summary"), *encrypted = ahp_field(item, "encrypted_content");
            if (!json_object_is_type(summary, json_type_array) || (encrypted && !rp_str(item, "encrypted_content"))) {
                rp_bad(c, "invalid Responses reasoning item"); goto fail;
            }
            for (size_t j = 0; j < (size_t)json_object_array_length(summary); ++j) {
                ahp_json *part = json_object_array_get_idx(summary, j);
                const char *s = rp_str(part, "text");
                if (!ahp_is(rp_str(part, "type"), "summary_text") || !s) {
                    rp_bad(c, "unsupported Responses reasoning summary"); goto fail;
                }
                if (show_summary && *s) {
                    if (summary_shown) ahp_reasoning(c, "reasoning.summary", "\n");
                    ahp_reasoning(c, "reasoning.summary", s);
                    summary_shown = 1;
                }
            }
        }
    }
    size_t count = (size_t)json_object_array_length(calls);
    if (!rp_string(c, message, "content", text.s ? text.s : "")) goto fail;
    if (count) { json_object_object_add(message, "tool_calls", calls); calls = NULL; }
    if (!ahp_valid_assistant(c, message, count > 0)) goto fail;
    if (calls) json_object_put(calls);
    ahp_buffer_free(&text);
    return message;
fail:
    if (message) json_object_put(message);
    if (calls) json_object_put(calls);
    ahp_buffer_free(&text);
    return NULL;
}

static int rp_parts_match(ahp_context *c, ahp_json *parts, const rp_piece *seen,
                          size_t count, const char *type)
{
    if (!json_object_is_type(parts, json_type_array) || (size_t)json_object_array_length(parts) < count)
        return rp_bad(c, "Responses snapshot lost a streamed content part");
    for (size_t i = 0; i < count; ++i) {
        ahp_json *part = json_object_array_get_idx(parts, i);
        if (!ahp_is(rp_str(part, "type"), type) || !rp_prefix(&seen[i].text, rp_str(part, "text"), seen[i].done))
            return rp_bad(c, "Responses snapshot contradicts streamed text");
    }
    return 1;
}

static int rp_snapshot_parts(ahp_json *a, ahp_json *b)
{
    if (!json_object_is_type(a, json_type_array) || !json_object_is_type(b, json_type_array) ||
        json_object_array_length(a) != json_object_array_length(b)) return 0;
    for (size_t i = 0; i < (size_t)json_object_array_length(a); ++i) {
        ahp_json *pa = json_object_array_get_idx(a, i), *pb = json_object_array_get_idx(b, i);
        if (!rp_same(rp_str(pa, "type"), rp_str(pb, "type")) || !rp_same(rp_str(pa, "text"), rp_str(pb, "text"))) return 0;
    }
    return 1;
}

static int rp_item_matches(rp_response *r, rp_item *seen, ahp_json *snapshot)
{
    const char *type = rp_str(seen->added, "type");
    if (!rp_same(type, rp_str(snapshot, "type")) ||
        !rp_same(rp_str(seen->added, "id"), rp_str(snapshot, "id")))
        return rp_bad(&r->ctx, "Responses snapshot changed output item identity");
    if (ahp_is(type, "function_call")) {
        if (!rp_same(rp_str(seen->added, "call_id"), rp_str(snapshot, "call_id")) ||
            !rp_same(rp_str(seen->added, "name"), rp_str(snapshot, "name")) ||
            !rp_prefix(&seen->arguments, rp_str(snapshot, "arguments"), seen->arguments_done) ||
            (seen->done && !rp_same(rp_str(seen->done, "arguments"), rp_str(snapshot, "arguments"))))
            return rp_bad(&r->ctx, "Responses snapshot contradicts streamed function call");
    } else if (ahp_is(type, "message")) {
        if (!ahp_is(rp_str(snapshot, "role"), "assistant"))
            return rp_bad(&r->ctx, "Responses snapshot changed assistant role");
        if (!rp_parts_match(&r->ctx, ahp_field(snapshot, "content"), seen->content, seen->content_count, "output_text")) return 0;
        if (seen->done && !rp_snapshot_parts(ahp_field(seen->done, "content"), ahp_field(snapshot, "content")))
            return rp_bad(&r->ctx, "Responses final message changed completed content");
    } else {
        if (!rp_parts_match(&r->ctx, ahp_field(snapshot, "summary"), seen->summary, seen->summary_count, "summary_text")) return 0;
        if (ahp_field(seen->added, "encrypted_content") &&
            !rp_same(rp_str(seen->added, "encrypted_content"), rp_str(snapshot, "encrypted_content")))
            return rp_bad(&r->ctx, "Responses snapshot changed opaque reasoning state");
        if (seen->done && (!rp_snapshot_parts(ahp_field(seen->done, "summary"), ahp_field(snapshot, "summary")) ||
            (ahp_field(seen->done, "encrypted_content") &&
             !rp_same(rp_str(seen->done, "encrypted_content"), rp_str(snapshot, "encrypted_content")))))
            return rp_bad(&r->ctx, "Responses final reasoning changed completed state");
    }
    return 1;
}

static rp_item *rp_event_item(rp_response *r, ahp_json *event)
{
    size_t index;
    if (!rp_index(event, "output_index", &index) || index >= r->count ||
        !rp_same(rp_str(event, "item_id"), rp_str(r->items[index].added, "id"))) {
        rp_bad(&r->ctx, "Responses event has an unknown output item identity"); return NULL;
    }
    rp_item *item = &r->items[index];
    if (item->done) { rp_bad(&r->ctx, "Responses delta after output_item.done"); return NULL; }
    return item;
}

static rp_piece *rp_get_piece(rp_response *r, rp_item *item, ahp_json *event, int summary)
{
    size_t index;
    rp_piece **pieces = summary ? &item->summary : &item->content;
    size_t *count = summary ? &item->summary_count : &item->content_count;
    size_t *capacity = summary ? &item->summary_capacity : &item->content_capacity;
    if (!rp_index(event, summary ? "summary_index" : "content_index", &index) || index > *count) {
        rp_bad(&r->ctx, "out-of-order Responses content index"); return NULL;
    }
    if (index == *count) {
        if (!rp_grow(&r->ctx, (void **)pieces, capacity, *count + 1, sizeof(**pieces))) return NULL;
        ++*count;
    }
    return &(*pieces)[index];
}

static int rp_terminal(rp_response *r, ahp_json *response, const char *type)
{
    if (!rp_identity(r, rp_str(response, "id"))) return 0;
    if (!rp_usage(&r->ctx, ahp_field(response, "usage"))) return 0;
    if (!ahp_is(type, "response.completed")) {
        if (ahp_is(type, "response.incomplete") &&
            ahp_is(rp_str(ahp_field(response, "incomplete_details"), "reason"), "max_output_tokens"))
            return ahp_fail(&r->ctx, DUNLIN_MODEL_LIMIT, "Responses output token limit reached");
        return rp_bad(&r->ctx, "Responses generation did not complete");
    }
    if (!ahp_is(rp_str(response, "status"), "completed") || ahp_field(response, "error") ||
        ahp_field(response, "incomplete_details"))
        return rp_bad(&r->ctx, "invalid Responses completed status");
    ahp_json *output = ahp_field(response, "output");
    if (!json_object_is_type(output, json_type_array) || (size_t)json_object_array_length(output) < r->count)
        return rp_bad(&r->ctx, "Responses terminal lacks streamed output items");
    for (size_t i = 0; i < r->count; ++i)
        if (!rp_item_matches(r, &r->items[i], json_object_array_get_idx(output, i))) return 0;
    r->output = json_object_get(output);
    r->done = 1;
    return 1;
}

static int rp_event(void *opaque, ahp_json *event, const char *event_name)
{
    rp_response *r = opaque;
    const char *type = rp_str(event, "type");
    if (!type || (event_name && *event_name && !rp_same(event_name, type)))
        return rp_bad(&r->ctx, "invalid Responses SSE event type");
    if (r->done) return rp_bad(&r->ctx, "Responses event after terminal completion");
    ahp_json *event_id = NULL;
    if (json_object_object_get_ex(event, "response_id", &event_id) &&
        !rp_identity(r, rp_str(event, "response_id"))) return 0;
    if (ahp_is(type, "error")) return rp_bad(&r->ctx, "Responses provider reported a stream error");
    if (ahp_is(type, "response.completed") || ahp_is(type, "response.failed") || ahp_is(type, "response.incomplete"))
        return rp_terminal(r, ahp_field(event, "response"), type);
    if (ahp_is(type, "response.created") || ahp_is(type, "response.in_progress")) {
        ahp_json *response = ahp_field(event, "response");
        if (!rp_identity(r, rp_str(response, "id"))) return 0;
        if (r->created && ahp_is(type, "response.created"))
            return rp_bad(&r->ctx, "conflicting Responses lifecycle identity");
        if (!r->created) r->created = json_object_get(response);
        return 1;
    }
    if (ahp_is(type, "response.output_item.added")) {
        size_t index;
        ahp_json *source = ahp_field(event, "item");
        const char *id = rp_str(source, "id");
        if (!rp_index(event, "output_index", &index) || index != r->count || !id || !*id || !rp_known_type(source))
            return rp_bad(&r->ctx, "unsupported or out-of-order Responses output item");
        for (size_t i = 0; i < r->count; ++i)
            if (rp_same(id, rp_str(r->items[i].added, "id")))
                return rp_bad(&r->ctx, "duplicate Responses streaming item id");
        if (!rp_grow(&r->ctx, (void **)&r->items, &r->capacity, r->count + 1, sizeof(*r->items))) return 0;
        rp_item *item = &r->items[r->count++];
        item->added = json_object_get(source);
        if (ahp_is(rp_str(source, "type"), "function_call")) {
            const char *call_id = rp_str(source, "call_id"), *name = rp_str(source, "name"), *args = rp_str(source, "arguments");
            if (!call_id || !*call_id || !name || !*name || !args)
                return rp_bad(&r->ctx, "incomplete Responses streaming function identity");
            return ahp_append(&r->ctx, &item->arguments, args, strlen(args));
        }
        int summary = ahp_is(rp_str(source, "type"), "reasoning");
        ahp_json *initial_parts = ahp_field(source, summary ? "summary" : "content");
        if (!json_object_is_type(initial_parts, json_type_array) ||
            (!summary && !ahp_is(rp_str(source, "role"), "assistant")))
            return rp_bad(&r->ctx, "invalid Responses initial content");
        for (size_t i = 0; i < (size_t)json_object_array_length(initial_parts); ++i) {
            ahp_json *part = json_object_array_get_idx(initial_parts, i);
            const char *s = rp_str(part, "text");
            rp_piece **pieces = summary ? &item->summary : &item->content;
            size_t *count = summary ? &item->summary_count : &item->content_count;
            size_t *capacity = summary ? &item->summary_capacity : &item->content_capacity;
            if (!ahp_is(rp_str(part, "type"), summary ? "summary_text" : "output_text") || !s)
                return rp_bad(&r->ctx, "unsupported Responses initial content");
            if (!rp_grow(&r->ctx, (void **)pieces, capacity, *count + 1, sizeof(**pieces))) return 0;
            rp_piece *piece = &(*pieces)[(*count)++];
            piece->added = 1;
            if (summary) {
                if (!ahp_append(&r->ctx, &piece->text, s, strlen(s))) return 0;
                rp_show_summary_piece(r, index, i, piece);
            } else if (!ahp_text(&r->ctx, &piece->text, s)) return 0;
        }
        return 1;
    }
    if (ahp_is(type, "response.output_item.done")) {
        size_t index;
        ahp_json *source = ahp_field(event, "item");
        if (!rp_index(event, "output_index", &index) || index >= r->count || r->items[index].done)
            return rp_bad(&r->ctx, "out-of-order Responses output_item.done");
        rp_item *item = &r->items[index];
        if (!rp_item_matches(r, item, source)) return 0;
        item->done = json_object_get(source);
        return 1;
    }
    if (ahp_is(type, "response.function_call_arguments.delta") || ahp_is(type, "response.function_call_arguments.done")) {
        rp_item *item = rp_event_item(r, event);
        if (!item) return 0;
        if (!ahp_is(rp_str(item->added, "type"), "function_call") || item->arguments_done ||
            (ahp_field(event, "call_id") && !rp_same(rp_str(event, "call_id"), rp_str(item->added, "call_id"))) ||
            (ahp_field(event, "name") && !rp_same(rp_str(event, "name"), rp_str(item->added, "name"))))
            return rp_bad(&r->ctx, "Responses function delta conflicts with its item");
        if (ahp_is(type, "response.function_call_arguments.done")) {
            if (!rp_complete_text(&r->ctx, &item->arguments, rp_str(event, "arguments"))) return 0;
            item->arguments_done = 1;
            return 1;
        }
        const char *delta = rp_str(event, "delta");
        if (!delta) return rp_bad(&r->ctx, "invalid Responses function argument delta");
        return ahp_append(&r->ctx, &item->arguments, delta, strlen(delta));
    }
    int summary = ahp_is(type, "response.reasoning_summary_part.added") || ahp_is(type, "response.reasoning_summary_part.done") ||
                  ahp_is(type, "response.reasoning_summary_text.delta") || ahp_is(type, "response.reasoning_summary_text.done");
    int text = ahp_is(type, "response.content_part.added") || ahp_is(type, "response.content_part.done") ||
               ahp_is(type, "response.output_text.delta") || ahp_is(type, "response.output_text.done");
    if (summary || text) {
        rp_item *item = rp_event_item(r, event);
        if (!item) return 0;
        if (!ahp_is(rp_str(item->added, "type"), summary ? "reasoning" : "message"))
            return rp_bad(&r->ctx, "Responses text event has the wrong item type");
        rp_piece *piece = rp_get_piece(r, item, event, summary);
        if (!piece) return 0;
        int added = ahp_is(type, "response.content_part.added") || ahp_is(type, "response.reasoning_summary_part.added");
        int part_done = ahp_is(type, "response.content_part.done") || ahp_is(type, "response.reasoning_summary_part.done");
        int text_done = ahp_is(type, "response.output_text.done") || ahp_is(type, "response.reasoning_summary_text.done");
        if (added || part_done) {
            ahp_json *part = ahp_field(event, "part");
            const char *s = rp_str(part, "text");
            if (!ahp_is(rp_str(part, "type"), summary ? "summary_text" : "output_text") || !s)
                return rp_bad(&r->ctx, "unsupported Responses streamed content part");
            if (added) {
                if (piece->added || piece->done || piece->text.n)
                    return rp_bad(&r->ctx, "duplicate Responses content_part.added");
                piece->added = 1;
                if (summary) {
                    if (!ahp_append(&r->ctx, &piece->text, s, strlen(s))) return 0;
                    rp_show_summary_piece(r, (size_t)(item - r->items), (size_t)(piece - item->summary), piece);
                    return 1;
                }
                return ahp_text(&r->ctx, &piece->text, s);
            }
            if (piece->done && !rp_prefix(&piece->text, s, 1))
                return rp_bad(&r->ctx, "Responses content_part.done contradicts text.done");
            if (!rp_complete_text(&r->ctx, &piece->text, s)) return 0;
            piece->done = 1;
            if (summary) rp_show_summary_piece(r, (size_t)(item - r->items), (size_t)(piece - item->summary), piece);
            return 1;
        }
        if (piece->done) return rp_bad(&r->ctx, "Responses text delta after text.done");
        if (text_done) {
            if (!rp_complete_text(&r->ctx, &piece->text, rp_str(event, "text"))) return 0;
            piece->done = 1;
            if (summary) rp_show_summary_piece(r, (size_t)(item - r->items), (size_t)(piece - item->summary), piece);
            return 1;
        }
        const char *delta = rp_str(event, "delta");
        if (!delta) return rp_bad(&r->ctx, "invalid Responses text delta");
        if (summary) {
            if (!ahp_append(&r->ctx, &piece->text, delta, strlen(delta))) return 0;
            rp_show_summary_piece(r, (size_t)(item - r->items), (size_t)(piece - item->summary), piece);
            return 1;
        }
        return ahp_text(&r->ctx, &piece->text, delta);
    }
    /* Additional informational event types do not establish completion. Their
     * output items must still be supported and match the authoritative snapshot. */
    return 1;
}

static int rp_calls_equal(ahp_json *a, ahp_json *b)
{
    size_t n = a ? (size_t)json_object_array_length(a) : 0;
    if (n != (b ? (size_t)json_object_array_length(b) : 0)) return 0;
    for (size_t i = 0; i < n; ++i) {
        ahp_json *ca = json_object_array_get_idx(a, i), *cb = json_object_array_get_idx(b, i);
        ahp_json *fa = ahp_field(ca, "function"), *fb = ahp_field(cb, "function");
        if (!rp_same(rp_str(ca, "id"), rp_str(cb, "id")) || !rp_same(rp_str(fa, "name"), rp_str(fb, "name")) ||
            !rp_same(rp_str(fa, "arguments"), rp_str(fb, "arguments"))) return 0;
    }
    return 1;
}

static int rp_message(ahp_context *c, ahp_json *input, const char *role, const char *text)
{
    ahp_json *message = rp_object(c);
    if (!message) return 0;
    json_object_array_add(input, message);
    return rp_string(c, message, "role", role) && rp_string(c, message, "content", text);
}

static int rp_history(ahp_context *c, ahp_json *request, ahp_json *messages)
{
    ahp_json *input = rp_array(c);
    const char *pending[RP_MAX_CALLS] = {0};
    unsigned char matched[RP_MAX_CALLS] = {0};
    size_t pending_count = 0, remaining = 0;
    if (!input) return 0;
    json_object_object_add(request, "input", input);
    for (size_t i = 0; i < (size_t)json_object_array_length(messages); ++i) {
        ahp_json *message = json_object_array_get_idx(messages, i);
        const char *role = rp_str(message, "role"), *text = rp_str(message, "content");
        if (ahp_is(role, "tool")) {
            const char *id = rp_str(message, "tool_call_id");
            if (!id || !*id || !text || !remaining) goto invalid;
            size_t j = 0;
            while (j < pending_count && !rp_same(pending[j], id)) ++j;
            if (j == pending_count || matched[j]) goto invalid;
            matched[j] = 1;
            --remaining;
            ahp_json *result = rp_object(c);
            if (!result) return 0;
            json_object_array_add(input, result);
            if (!rp_string(c, result, "type", "function_call_output") ||
                !rp_string(c, result, "call_id", id) || !rp_string(c, result, "output", text)) return 0;
        } else if (ahp_is(role, "assistant")) {
            if (remaining) goto invalid;
            ahp_json *calls = ahp_field(message, "tool_calls");
            int has_calls = json_object_is_type(calls, json_type_array) && json_object_array_length(calls);
            if (!ahp_valid_assistant(c, message, has_calls)) return 0;
            ahp_json *native = ahp_native(c, message, "responses");
            if (c->status != DUNLIN_MODEL_OK) return 0;
            if (native) {
                ahp_json *check = rp_normalize(c, native, 0);
                if (!check) return 0;
                int equal = rp_calls_equal(calls, ahp_field(check, "tool_calls")) &&
                            (!text || rp_same(text, rp_str(check, "content")));
                json_object_put(check);
                if (!equal) goto invalid;
                for (size_t j = 0; j < (size_t)json_object_array_length(native); ++j) {
                    ahp_json *copy = ahp_clone(json_object_array_get_idx(native, j));
                    if (!copy) return ahp_fail(c, DUNLIN_MODEL_LIMIT, "model allocation failed");
                    json_object_array_add(input, copy);
                }
            } else {
                if (text && (*text || !has_calls) && !rp_message(c, input, "assistant", text)) return 0;
                for (size_t j = 0; has_calls && j < (size_t)json_object_array_length(calls); ++j) {
                    ahp_json *call = json_object_array_get_idx(calls, j), *fn = ahp_field(call, "function");
                    ahp_json *item = rp_object(c);
                    if (!item) return 0;
                    json_object_array_add(input, item);
                    if (!rp_string(c, item, "type", "function_call") || !rp_string(c, item, "call_id", rp_str(call, "id")) ||
                        !rp_string(c, item, "name", rp_str(fn, "name")) || !rp_string(c, item, "arguments", rp_str(fn, "arguments"))) return 0;
                }
            }
            pending_count = remaining = 0;
            memset(matched, 0, sizeof(matched));
            for (size_t j = 0; has_calls && j < (size_t)json_object_array_length(calls); ++j) {
                pending[pending_count++] = rp_str(json_object_array_get_idx(calls, j), "id");
                ++remaining;
            }
        } else if (ahp_is(role, "user") || ahp_is(role, "system") || ahp_is(role, "developer")) {
            if (!text || (remaining && ahp_is(role, "user"))) goto invalid;
            if (!rp_message(c, input, role, text)) return 0;
        } else goto invalid;
    }
    if (remaining || !json_object_array_length(input)) goto invalid;
    return 1;
invalid:
    return ahp_fail(c, DUNLIN_MODEL_INVALID, "unsupported or unpaired Responses history");
}

static int rp_tools(ahp_context *c, ahp_json *request, ahp_json *tools)
{
    if (!tools || !json_object_array_length(tools)) return 1;
    ahp_json *native = rp_array(c);
    if (!native) return 0;
    json_object_object_add(request, "tools", native);
    for (size_t i = 0; i < (size_t)json_object_array_length(tools); ++i) {
        ahp_json *tool = json_object_array_get_idx(tools, i), *fn = ahp_field(tool, "function");
        const char *name = rp_str(fn, "name"), *description = rp_str(fn, "description");
        ahp_json *schema = ahp_field(fn, "parameters");
        if (!ahp_is(rp_str(tool, "type"), "function") || !name || !*name ||
            !json_object_is_type(schema, json_type_object) || (ahp_field(fn, "description") && !description))
            return ahp_fail(c, DUNLIN_MODEL_INVALID, "unsupported Responses tool schema");
        ahp_json *out = rp_object(c);
        if (!out) return 0;
        json_object_array_add(native, out);
        if (!rp_string(c, out, "type", "function") || !rp_string(c, out, "name", name) ||
            !rp_copy(c, out, "parameters", schema) ||
            (description && !rp_string(c, out, "description", description))) return 0;
        /* Existing C tool schemas are validated by the Loop, not rewritten to
         * the stricter JSON Schema subset implicitly used by Responses. */
        json_object_object_add(out, "strict", json_object_new_boolean(0));
    }
    return 1;
}

dunlin_model_status ahp_responses(const dunlin_model_config *cfg, const dunlin_protocol_options *opt,
    ahp_json *messages, ahp_json *tools, const dunlin_model_events *events,
    ahp_json **assistant, char *error, size_t error_size)
{
    rp_response r = { .ctx = { cfg, opt, events, DUNLIN_MODEL_OK, {0} } };
    ahp_json *request = NULL, *plain = NULL, *result = NULL;
    *assistant = NULL;
    request = rp_object(&r.ctx);
    if (!request || !rp_string(&r.ctx, request, "model", cfg->model) ||
        !rp_history(&r.ctx, request, messages) || !rp_tools(&r.ctx, request, tools)) goto done;
    json_object_object_add(request, "stream", json_object_new_boolean(cfg->stream));
    json_object_object_add(request, "store", json_object_new_boolean(0));
    ahp_json *include = rp_array(&r.ctx);
    if (!include) goto done;
    json_object_object_add(request, "include", include);
    json_object_array_add(include, json_object_new_string("reasoning.encrypted_content"));
    if (opt->max_tokens) json_object_object_add(request, "max_output_tokens", json_object_new_int64(opt->max_tokens));
    if (opt->has_temperature) json_object_object_add(request, "temperature", json_object_new_double(opt->temperature));
    if (opt->required_tool && *opt->required_tool) {
        ahp_json *choice = rp_object(&r.ctx);
        if (!choice) goto done;
        json_object_object_add(request, "tool_choice", choice);
        if (!rp_string(&r.ctx, choice, "type", "function") || !rp_string(&r.ctx, choice, "name", opt->required_tool)) goto done;
    }
    if (opt->reasoning_effort != DUNLIN_REASONING_DEFAULT) {
        ahp_json *reasoning = rp_object(&r.ctx);
        if (!reasoning) goto done;
        json_object_object_add(request, "reasoning", reasoning);
        if (!rp_string(&r.ctx, reasoning, "effort", ahp_effort(opt->reasoning_effort))) goto done;
    }
    if (!ahp_exchange(&r.ctx, "/responses", request, AHP_SSE, NULL, 0, rp_event, &r, &plain)) goto done;
    if (cfg->stream) {
        if (!r.done) { rp_bad(&r.ctx, "incomplete Responses stream (response.completed required)"); goto done; }
    } else {
        const char *status = rp_str(plain, "status");
        const char *type = ahp_is(status, "completed") ? "response.completed" :
                           ahp_is(status, "incomplete") ? "response.incomplete" : "response.failed";
        if (!rp_terminal(&r, plain, type)) goto done;
    }
    result = rp_normalize(&r.ctx, r.output, !cfg->stream);
    if (!result) goto done;
    if (!ahp_metadata(&r.ctx, result, "responses", r.output)) goto done;
    if (cfg->stream) rp_show_summary_remaining(&r);
    *assistant = result;
    result = NULL;
done:
    if (result) json_object_put(result);
    if (request) json_object_put(request);
    if (plain) json_object_put(plain);
    if (r.output) json_object_put(r.output);
    if (r.created) json_object_put(r.created);
    ahp_buffer_free(&r.response_id);
    for (size_t i = 0; i < r.count; ++i) {
        rp_item *item = &r.items[i];
        if (item->added) json_object_put(item->added);
        if (item->done) json_object_put(item->done);
        ahp_buffer_free(&item->arguments);
        for (size_t j = 0; j < item->content_count; ++j) ahp_buffer_free(&item->content[j].text);
        for (size_t j = 0; j < item->summary_count; ++j) ahp_buffer_free(&item->summary[j].text);
        free(item->content);
        free(item->summary);
    }
    free(r.items);
    return ahp_result(&r.ctx, error, error_size);
}
