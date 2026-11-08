#include "dunlin/compact/protocol.h"
#include "dunlin/compact/json.h"
#include <json-c/json.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void protocol_responses_tests(void);
typedef struct json_object J;
typedef struct {
    dunlin_model_config cfg;
    dunlin_protocol_options opt;
    dunlin_model_metrics metrics;
    dunlin_model_events events;
    dunlin_http_v2 http;
    const char *wire;
    J *request;
    long code;
    dunlin_http_result transport;
    size_t sent, cancel_after;
    int posts;
    char shown[256], summary[512], error[192];
    size_t shown_size, summary_size;
} rp_fixture;

static J *rf_field(J *o, const char *key)
{
    J *v = NULL;
    json_object_object_get_ex(o, key, &v);
    return v;
}
static J *rf_at(J *o, size_t i) { return json_object_array_get_idx(o, i); }
static const char *rf_str(J *o, const char *key)
{
    J *v = rf_field(o, key);
    assert(json_object_is_type(v, json_type_string));
    return json_object_get_string(v);
}
static J *rf_parse(const char *s)
{
    J *o = dunlin_json_parse(s, strlen(s));
    assert(o);
    return o;
}
static J *rf_clone(J *o)
{
    return rf_parse(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
}
static void rf_capture(char *buffer, size_t *used, size_t capacity, const char *text, size_t n)
{
    assert(*used + n < capacity);
    memcpy(buffer + *used, text, n);
    *used += n;
    buffer[*used] = 0;
}
static void rf_text(void *opaque, const char *text, size_t n)
{
    rp_fixture *f = opaque;
    rf_capture(f->shown, &f->shown_size, sizeof(f->shown), text, n);
}
static void rf_summary(void *opaque, const char *event, const char *text, size_t n)
{
    rp_fixture *f = opaque;
    assert(!strcmp(event, "reasoning.summary"));
    rf_capture(f->summary, &f->summary_size, sizeof(f->summary), text, n);
}
static int rf_cancel(void *opaque)
{
    rp_fixture *f = opaque;
    return f->cancel_after && f->sent >= f->cancel_after;
}
static dunlin_http_result rf_post(void *opaque, const dunlin_http_request_v2 *request, long *code)
{
    rp_fixture *f = opaque;
    ++f->posts;
    assert(!strcmp(request->request.url, "http://fixture/v1/responses"));
    assert(!request->request.bearer && request->header_count == 0);
    assert(!strcmp(request->accept, f->cfg.stream ? "text/event-stream" : "application/json"));
    f->request = rf_parse(request->request.json);
    *code = f->code;
    for (size_t i = 0; f->wire[i]; ++i) {
        ++f->sent;
        if (!request->request.body(request->request.user, f->wire + i, 1)) return DUNLIN_HTTP_FAILED;
    }
    return f->transport;
}
static void rf_init(rp_fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->cfg = (dunlin_model_config){"http://fixture/v1", "fixture-model", NULL, 2000, 0, 65536, NULL};
    f->http = (dunlin_http_v2){rf_post, f};
    f->opt = (dunlin_protocol_options){.protocol = DUNLIN_PROTOCOL_RESPONSES, .show_reasoning = 1,
        .user = f, .reasoning = rf_summary, .metrics = &f->metrics, .http = &f->http};
    f->events = (dunlin_model_events){f, rf_cancel, rf_text};
    f->code = 200;
    f->transport = DUNLIN_HTTP_OK;
}
static J *rf_run(rp_fixture *f, J *history, J *tools, const char *wire, dunlin_model_status expected)
{
    if (f->request) json_object_put(f->request);
    f->request = NULL;
    f->wire = wire;
    f->posts = 0;
    f->sent = f->shown_size = f->summary_size = 0;
    f->shown[0] = f->summary[0] = f->error[0] = 0;
    J *assistant = NULL;
    dunlin_model_status got = dunlin_protocol_proposal(&f->cfg, &f->opt, history, tools,
        &f->events, &assistant, f->error, sizeof(f->error));
    if (got != expected) fprintf(stderr, "Responses expected %d, got %d: %s\n", expected, got, f->error);
    assert(got == expected);
    assert((assistant != NULL) == (expected == DUNLIN_MODEL_OK));
    return assistant;
}

static const char *rf_text_response =
    "{\"id\":\"resp_text\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"id\":\"message_1\","
    "\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"你好\",\"annotations\":[]}]}]}";
static const char *rf_tool_response =
    "{\"id\":\"resp_tools\",\"status\":\"completed\",\"output\":["
    "{\"type\":\"reasoning\",\"id\":\"reason_1\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"public summary\"},"
    "{\"type\":\"summary_text\",\"text\":\"second summary\"}],\"encrypted_content\":\"opaque_do_not_display\"},"
    "{\"type\":\"function_call\",\"id\":\"item_1\",\"call_id\":\"call_1\",\"name\":\"echo\","
    "\"arguments\":\"{\\\"text\\\":\\\"你好\\\"}\",\"status\":\"completed\"}],"
    "\"usage\":{\"input_tokens\":20,\"output_tokens\":12,\"total_tokens\":32,"
    "\"input_tokens_details\":{\"cached_tokens\":3},\"output_tokens_details\":{\"reasoning_tokens\":7}}}";

static void rf_wire_append(char **wire, const char *s)
{
    size_t old = *wire ? strlen(*wire) : 0, extra = strlen(s);
    char *next = realloc(*wire, old + extra + 1);
    assert(next);
    memcpy(next + old, s, extra + 1);
    *wire = next;
}
static void rf_event(char **wire, J *event)
{
    rf_wire_append(wire, "event: ");
    rf_wire_append(wire, rf_str(event, "type"));
    rf_wire_append(wire, "\r\ndata: ");
    rf_wire_append(wire, json_object_to_json_string_ext(event, JSON_C_TO_STRING_PLAIN));
    rf_wire_append(wire, "\r\n\r\n");
    json_object_put(event);
}
static void rf_event_text(char **wire, const char *json) { rf_event(wire, rf_parse(json)); }
static void rf_item_event(char **wire, const char *type, size_t index, J *item)
{
    J *event = json_object_new_object();
    json_object_object_add(event, "type", json_object_new_string(type));
    json_object_object_add(event, "output_index", json_object_new_int64((int64_t)index));
    json_object_object_add(event, "item", json_object_get(item));
    rf_event(wire, event);
}
static void rf_terminal_event(char **wire, const char *type, J *response)
{
    J *event = json_object_new_object();
    json_object_object_add(event, "type", json_object_new_string(type));
    json_object_object_add(event, "response", json_object_get(response));
    rf_event(wire, event);
}

/* Wire fixtures describe public events independently of the adapter's tracking
 * state. Each HTTP body is delivered byte by byte, including UTF-8 boundaries. */
static char *rf_tool_stream(int broken)
{
    char *wire = NULL;
    J *final = rf_parse(rf_tool_response), *output = rf_field(final, "output");
    rf_wire_append(&wire, ": heartbeat\r\n\r\n");
    rf_event_text(&wire, "{\"type\":\"response.created\",\"response\":{\"id\":\"resp_tools\",\"status\":\"in_progress\",\"output\":[]}}");
    J *initial = rf_clone(rf_at(output, 0));
    json_object_object_add(initial, "summary", json_object_new_array());
    json_object_object_del(initial, "encrypted_content");
    rf_item_event(&wire, "response.output_item.added", 0, initial);
    json_object_put(initial);
    rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_part.added\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":0,\"part\":{\"type\":\"summary_text\",\"text\":\"\"}}");
    rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":0,\"delta\":\"public \"}");
    rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":0,\"delta\":\"summary\"}");
    rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_text.done\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":0,\"text\":\"public summary\"}");
    rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_part.added\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":1,\"part\":{\"type\":\"summary_text\",\"text\":\"\"}}");
    rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":1,\"delta\":\"second summary\"}");
    rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_text.done\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":1,\"text\":\"second summary\"}");
    rf_item_event(&wire, "response.output_item.done", 0, rf_at(output, 0));
    initial = rf_clone(rf_at(output, 1));
    json_object_object_add(initial, "arguments", json_object_new_string(""));
    json_object_object_add(initial, "status", json_object_new_string("in_progress"));
    rf_item_event(&wire, "response.output_item.added", 1, initial);
    json_object_put(initial);
    rf_event_text(&wire, broken == 1 ?
        "{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"wrong_item\",\"output_index\":1,\"delta\":\"{}\"}" :
        "{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"item_1\",\"output_index\":1,\"delta\":\"{\\\"text\\\":\\\"你\"}");
    rf_event_text(&wire, broken == 6 ?
        "{\"type\":\"response.function_call_arguments.delta\",\"response_id\":\"different_response\",\"item_id\":\"item_1\",\"output_index\":1,\"delta\":\"好\\\"}\"}" :
        "{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"item_1\",\"output_index\":1,\"delta\":\"好\\\"}\"}");
    rf_event_text(&wire, "{\"type\":\"response.function_call_arguments.done\",\"item_id\":\"item_1\",\"output_index\":1,\"arguments\":\"{\\\"text\\\":\\\"你好\\\"}\"}");
    rf_item_event(&wire, "response.output_item.done", 1, rf_at(output, 1));
    if (broken == 2) json_object_object_add(rf_at(output, 1), "call_id", json_object_new_string("different_call"));
    if (broken == 3) json_object_object_add(rf_at(output, 1), "arguments", json_object_new_string("{}"));
    if (broken == 5) json_object_object_add(final, "id", json_object_new_string("different_response"));
    if (broken != 4) rf_terminal_event(&wire, "response.completed", final);
    json_object_put(final);
    return wire;
}

static char *rf_text_stream(int contradictory)
{
    char *wire = NULL;
    J *final = rf_parse(rf_text_response), *item = rf_at(rf_field(final, "output"), 0), *initial = rf_clone(item);
    json_object_object_add(initial, "content", json_object_new_array());
    json_object_object_add(initial, "status", json_object_new_string("in_progress"));
    rf_event_text(&wire, "{\"type\":\"response.created\",\"response\":{\"id\":\"resp_text\",\"status\":\"in_progress\",\"output\":[]}}");
    rf_item_event(&wire, "response.output_item.added", 0, initial);
    json_object_put(initial);
    rf_event_text(&wire, "{\"type\":\"response.content_part.added\",\"item_id\":\"message_1\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}");
    rf_event_text(&wire, "{\"type\":\"response.output_text.delta\",\"item_id\":\"message_1\",\"output_index\":0,\"content_index\":0,\"delta\":\"你\"}");
    rf_event_text(&wire, "{\"type\":\"response.output_text.delta\",\"item_id\":\"message_1\",\"output_index\":0,\"content_index\":0,\"delta\":\"好\"}");
    if (contradictory) json_object_object_add(rf_at(rf_field(item, "content"), 0), "text", json_object_new_string("changed"));
    rf_terminal_event(&wire, "response.completed", final);
    json_object_put(final);
    return wire;
}

static void rf_response_id_tests(rp_fixture *f, J *history, J *tools)
{
    static const char *starts[] = {
        "{\"type\":\"response.queued\",\"response_id\":\"resp_text\"}",
        "{\"type\":\"response.queued\",\"response_id\":\"different_response\"}",
        "{\"type\":\"response.queued\",\"response_id\":null}",
        "{\"type\":\"response.queued\",\"response_id\":7}",
        "{\"type\":\"response.queued\",\"response_id\":\"\"}"
    };
    J *response = rf_parse(rf_text_response);
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        char *wire = NULL;
        rf_event_text(&wire, starts[i]);
        rf_terminal_event(&wire, "response.completed", response);
        J *result = rf_run(f, history, tools, wire, i ? DUNLIN_MODEL_PROTOCOL : DUNLIN_MODEL_OK);
        if (result) json_object_put(result);
        free(wire);
    }
    for (int conflict = 0; conflict < 2; ++conflict) {
        char *wire = NULL;
        rf_event_text(&wire, starts[conflict]);
        rf_event_text(&wire, "{\"type\":\"response.created\",\"response_id\":\"resp_text\",\"response\":{\"id\":\"resp_text\",\"status\":\"in_progress\",\"output\":[]}}");
        rf_terminal_event(&wire, "response.completed", response);
        J *result = rf_run(f, history, tools, wire, conflict ? DUNLIN_MODEL_PROTOCOL : DUNLIN_MODEL_OK);
        if (result) json_object_put(result);
        free(wire);
    }
    json_object_put(response);
}

static void rf_summary_snapshot_tests(rp_fixture *f, J *history, J *tools)
{
    J *response = rf_parse(rf_tool_response);
    J *reasoning = rf_at(rf_field(response, "output"), 0);
    for (int mode = 0; mode < 5; ++mode) {
        char *wire = NULL;
        if (mode) {
            J *initial = rf_clone(reasoning);
            if (mode != 4) json_object_object_add(initial, "summary", json_object_new_array());
            rf_item_event(&wire, "response.output_item.added", 0, initial);
            json_object_put(initial);
        }
        if (mode == 1) rf_item_event(&wire, "response.output_item.done", 0, reasoning);
        if (mode == 2 || mode == 3)
            rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":0,\"delta\":\"public \"}");
        if (mode == 2) {
            rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_text.done\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":0,\"text\":\"public summary\"}");
            rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_part.done\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":0,\"part\":{\"type\":\"summary_text\",\"text\":\"public summary\"}}");
            rf_event_text(&wire, "{\"type\":\"response.reasoning_summary_part.done\",\"item_id\":\"reason_1\",\"output_index\":0,\"summary_index\":1,\"part\":{\"type\":\"summary_text\",\"text\":\"second summary\"}}");
            rf_item_event(&wire, "response.output_item.done", 0, reasoning);
        }
        rf_terminal_event(&wire, "response.completed", response);
        for (int show = 1; show >= 0; --show) {
            f->opt.show_reasoning = show;
            J *result = rf_run(f, history, tools, wire, DUNLIN_MODEL_OK);
            assert(!strcmp(f->summary, show ? "public summary\nsecond summary" : ""));
            assert(!strcmp(rf_str(rf_at(rf_field(rf_field(result, "_dunlin_provider"), "output"), 0),
                                 "encrypted_content"), "opaque_do_not_display"));
            json_object_put(result);
        }
        free(wire);
    }
    f->opt.show_reasoning = 1;
    json_object_put(response);
}

void protocol_responses_tests(void)
{
    rp_fixture f;
    rf_init(&f);
    J *history = rf_parse("[{\"role\":\"system\",\"content\":\"Follow the task\"},{\"role\":\"user\",\"content\":\"Echo 你好\"}]");
    J *tools = rf_parse("[{\"type\":\"function\",\"function\":{\"name\":\"echo\",\"description\":\"Echo text\",\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}}]");
    f.opt.max_tokens = 64;
    f.opt.has_temperature = 1;
    f.opt.temperature = 0.2;
    f.opt.required_tool = "echo";
    f.opt.reasoning_effort = DUNLIN_REASONING_LOW;
    f.opt.reasoning_supported = DUNLIN_REASONING_CAN_EFFORT;
    J *assistant = rf_run(&f, history, tools, rf_tool_response, DUNLIN_MODEL_OK);
    assert(!strcmp(rf_str(rf_at(rf_field(assistant, "tool_calls"), 0), "id"), "call_1"));
    assert(!strcmp(f.summary, "public summary\nsecond summary"));
    assert(!strstr(f.summary, "opaque"));
    assert(!json_object_get_boolean(rf_field(f.request, "store")));
    assert(!strcmp(json_object_get_string(rf_at(rf_field(f.request, "include"), 0)), "reasoning.encrypted_content"));
    assert(!strcmp(rf_str(rf_field(f.request, "reasoning"), "effort"), "low"));
    assert(json_object_get_int(rf_field(f.request, "max_output_tokens")) == 64);
    assert(json_object_get_double(rf_field(f.request, "temperature")) == 0.2);
    assert(!strcmp(rf_str(rf_field(f.request, "tool_choice"), "name"), "echo"));
    J *flat = rf_at(rf_field(f.request, "tools"), 0);
    assert(!strcmp(rf_str(flat, "type"), "function") && !rf_field(flat, "function"));
    assert(!json_object_get_boolean(rf_field(flat, "strict")));
    assert(f.metrics.input_tokens == 20 && f.metrics.output_tokens == 12 && f.metrics.total_tokens == 32);
    assert(f.metrics.cached_input_tokens == 3 && f.metrics.reasoning_tokens == 7);
    assert(f.metrics.present == (DUNLIN_USAGE_INPUT | DUNLIN_USAGE_OUTPUT | DUNLIN_USAGE_TOTAL | DUNLIN_USAGE_CACHED_INPUT | DUNLIN_USAGE_REASONING));
    assert(f.metrics.request_bytes == strlen(json_object_to_json_string_ext(f.request, JSON_C_TO_STRING_PLAIN)));
    assert(f.metrics.response_bytes == strlen(rf_tool_response));

    J *continued = rf_clone(history);
    json_object_array_add(continued, json_object_get(assistant));
    json_object_array_add(continued, rf_parse("{\"role\":\"tool\",\"tool_call_id\":\"call_1\",\"content\":\"你好\"}"));
    f.opt.required_tool = NULL;
    f.opt.reasoning_effort = DUNLIN_REASONING_XHIGH;
    f.opt.reasoning_supported = DUNLIN_REASONING_CAN_EFFORT;
    rf_run(&f, history, tools, rf_text_response, DUNLIN_MODEL_INVALID);
    assert(f.posts == 0); /* Legacy levels never silently grants xhigh. */
    f.opt.reasoning_supported = DUNLIN_REASONING_CAN_XHIGH;
    J *xhigh_result = rf_run(&f, history, tools, rf_text_response, DUNLIN_MODEL_OK);
    assert(!strcmp(rf_str(rf_field(f.request, "reasoning"), "effort"), "xhigh"));
    json_object_put(xhigh_result);
    f.opt.reasoning_effort = DUNLIN_REASONING_DEFAULT;
    f.opt.has_temperature = 0;
    J *final = rf_run(&f, continued, tools, rf_text_response, DUNLIN_MODEL_OK);
    assert(!strcmp(rf_str(final, "content"), "你好"));
    assert(!rf_field(f.request, "reasoning") && !rf_field(f.request, "temperature"));
    J *input = rf_field(f.request, "input");
    assert(json_object_array_length(input) == 5);
    assert(!strcmp(rf_str(rf_at(input, 2), "encrypted_content"), "opaque_do_not_display"));
    assert(!strcmp(rf_str(rf_at(input, 3), "id"), "item_1"));
    assert(!strcmp(rf_str(rf_at(input, 4), "type"), "function_call_output"));
    assert(!strcmp(rf_str(rf_at(input, 4), "call_id"), "call_1"));
    assert(f.metrics.present == 0); /* Missing token fields remain unknown. */
    json_object_put(final);
    J *bad = rf_clone(continued);
    json_object_object_add(rf_at(bad, 3), "tool_call_id", json_object_new_string("item_1"));
    rf_run(&f, bad, tools, rf_text_response, DUNLIN_MODEL_INVALID);
    assert(f.posts == 0);
    json_object_put(bad);
    bad = rf_clone(continued);
    json_object_array_add(bad, rf_clone(rf_at(bad, 3)));
    rf_run(&f, bad, tools, rf_text_response, DUNLIN_MODEL_INVALID);
    assert(f.posts == 0);
    json_object_put(bad);
    bad = rf_clone(history);
    json_object_array_add(bad, json_object_get(assistant));
    rf_run(&f, bad, tools, rf_text_response, DUNLIN_MODEL_INVALID);
    assert(f.posts == 0);
    json_object_put(bad);

    f.cfg.stream = 1;
    char *wire = rf_tool_stream(0);
    f.opt.required_tool = "echo";
    final = rf_run(&f, history, tools, wire, DUNLIN_MODEL_OK);
    assert(!strcmp(rf_str(rf_field(rf_at(rf_field(final, "tool_calls"), 0), "function"), "arguments"), "{\"text\":\"你好\"}"));
    assert(!strcmp(f.summary, "public summary\nsecond summary"));
    assert(!strcmp(rf_str(rf_at(rf_field(rf_field(final, "_dunlin_provider"), "output"), 0), "encrypted_content"), "opaque_do_not_display"));
    json_object_put(final);
    f.opt.show_reasoning = 0;
    final = rf_run(&f, history, tools, wire, DUNLIN_MODEL_OK);
    assert(f.summary_size == 0);
    assert(rf_field(final, "_dunlin_provider"));
    json_object_put(final);
    f.opt.show_reasoning = 1;
    free(wire);
    for (int i = 1; i <= 6; ++i) {
        wire = rf_tool_stream(i);
        rf_run(&f, history, tools, wire, DUNLIN_MODEL_PROTOCOL);
        free(wire);
    }
    f.opt.required_tool = NULL;
    rf_response_id_tests(&f, history, tools);
    rf_summary_snapshot_tests(&f, history, tools);
    wire = rf_text_stream(0);
    final = rf_run(&f, history, tools, wire, DUNLIN_MODEL_OK);
    assert(!strcmp(f.shown, "你好") && !strcmp(rf_str(final, "content"), "你好"));
    json_object_put(final);
    f.opt.required_tool = "echo";
    rf_run(&f, history, tools, wire, DUNLIN_MODEL_REQUIRED_TOOL_MISSING);
    assert(f.shown_size == 0);
    f.opt.required_tool = NULL;
    f.cfg.max_response_bytes = 20;
    rf_run(&f, history, tools, wire, DUNLIN_MODEL_LIMIT);
    f.cfg.max_response_bytes = 65536;
    f.cancel_after = 20;
    rf_run(&f, history, tools, wire, DUNLIN_MODEL_CANCELLED);
    f.cancel_after = 0;
    free(wire);
    wire = rf_text_stream(1);
    rf_run(&f, history, tools, wire, DUNLIN_MODEL_PROTOCOL);
    free(wire);
    rf_run(&f, history, tools, "event: error\ndata: {\"type\":\"error\",\"message\":\"private_secret\"}\n\n", DUNLIN_MODEL_PROTOCOL);
    assert(!strstr(f.error, "private_secret"));
    rf_run(&f, history, tools, "data: {\"type\":\"response.completed\"}", DUNLIN_MODEL_PROTOCOL);

    f.cfg.stream = 0;
    rf_run(&f, history, tools, "{\"id\":\"failed\",\"status\":\"failed\",\"error\":{\"message\":\"private_secret\"},\"usage\":{\"input_tokens\":4,\"output_tokens\":2}}", DUNLIN_MODEL_PROTOCOL);
    assert(f.metrics.output_tokens == 2 && !strstr(f.error, "private_secret"));
    rf_run(&f, history, tools, "{\"id\":\"truncated\",\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_output_tokens\"},\"usage\":{\"input_tokens\":5,\"output_tokens\":17}}", DUNLIN_MODEL_LIMIT);
    assert(f.metrics.output_tokens == 17 && (f.metrics.present & DUNLIN_USAGE_OUTPUT));
    rf_run(&f, history, tools, "{\"id\":\"unsupported\",\"status\":\"completed\",\"output\":[{\"type\":\"web_search_call\",\"id\":\"server_call\",\"status\":\"completed\"}]}", DUNLIN_MODEL_PROTOCOL);
    rf_run(&f, history, tools, "{\"id\":\"bad_usage\",\"status\":\"completed\",\"output\":[],\"usage\":{\"input_tokens\":-1}}", DUNLIN_MODEL_PROTOCOL);
    bad = rf_parse(rf_tool_response);
    json_object_object_add(rf_at(rf_field(bad, "output"), 1), "arguments", json_object_new_string("bad JSON"));
    final = rf_run(&f, history, tools, json_object_to_json_string_ext(bad, JSON_C_TO_STRING_PLAIN), DUNLIN_MODEL_OK);
    assert(!strcmp(rf_str(rf_field(rf_at(rf_field(final, "tool_calls"), 0), "function"), "arguments"), "bad JSON"));
    json_object_put(final);
    json_object_put(bad);
    f.opt.reasoning_effort = DUNLIN_REASONING_NONE;
    f.opt.reasoning_supported = 0;
    rf_run(&f, history, tools, rf_text_response, DUNLIN_MODEL_INVALID);
    assert(f.posts == 0);
    f.opt.reasoning_supported = DUNLIN_REASONING_CAN_DISABLE;
    final = rf_run(&f, history, tools, rf_text_response, DUNLIN_MODEL_OK);
    assert(!strcmp(rf_str(rf_field(f.request, "reasoning"), "effort"), "none"));
    json_object_put(final);
    f.opt.reasoning_effort = DUNLIN_REASONING_DEFAULT;
    f.code = 429;
    rf_run(&f, history, tools, rf_text_response, DUNLIN_MODEL_RATE_LIMIT);
    f.code = 503;
    rf_run(&f, history, tools, rf_text_response, DUNLIN_MODEL_HTTP);
    f.code = 200;
    f.transport = DUNLIN_HTTP_EXPIRED;
    rf_run(&f, history, tools, "", DUNLIN_MODEL_TIMEOUT);
    if (f.request) json_object_put(f.request);
    json_object_put(assistant);
    json_object_put(continued);
    json_object_put(history);
    json_object_put(tools);
}
