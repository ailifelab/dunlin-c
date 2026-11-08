#include "dunlin/compact/protocol.h"
#include "dunlin/compact/json.h"
#include <json-c/json.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct json_object J;
typedef struct {
    const char *wire;
    long code;
    dunlin_http_result transport_result;
    size_t sent, cancel_after;
    int cancel_now, posts;
    char shown[2048], thought[2048], error[192];
    size_t shown_len, thought_len;
    J *request;
    char *request_wire;
    dunlin_http_v1 v1;
    dunlin_http_v2 v2;
    dunlin_model_config cfg;
    dunlin_protocol_options options;
    dunlin_model_events events;
    dunlin_model_metrics metrics;
} fixture;

void protocol_responses_tests(void);

static J *field(J *o, const char *key)
{
    J *v = NULL;
    json_object_object_get_ex(o, key, &v);
    return v;
}
static J *at(J *o, size_t index) { return json_object_array_get_idx(o, index); }
static const char *string(J *o, const char *key)
{
    J *v = field(o, key);
    assert(json_object_is_type(v, json_type_string));
    return json_object_get_string(v);
}
static J *parse(const char *s)
{
    J *o = dunlin_json_parse(s, strlen(s));
    assert(o);
    return o;
}
static J *snapshot(J *o)
{
    return parse(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
}
static void append_output(char *buffer, size_t *used, const char *s, size_t n)
{
    assert(*used + n < 2048);
    memcpy(buffer + *used, s, n);
    *used += n;
    buffer[*used] = 0;
}
static void show(void *ptr, const char *s, size_t n)
{
    fixture *f = ptr;
    append_output(f->shown, &f->shown_len, s, n);
}
static void reasoning(void *ptr, const char *event, const char *s, size_t n)
{
    fixture *f = ptr;
    assert(!strcmp(event, "reasoning.delta") || !strcmp(event, "reasoning.summary"));
    append_output(f->thought, &f->thought_len, s, n);
}
static int cancelled(void *ptr)
{
    fixture *f = ptr;
    return f->cancel_now || (f->cancel_after && f->sent >= f->cancel_after);
}
static dunlin_http_result deliver(fixture *f, const dunlin_http_request *r, long *code)
{
    ++f->posts;
    assert(!r->bearer);
    assert(r->stream == f->cfg.stream);
    assert(r->timeout_ms == f->cfg.timeout_ms);
    const char *suffix = f->options.protocol == DUNLIN_PROTOCOL_OLLAMA ? "/chat" :
        f->options.protocol == DUNLIN_PROTOCOL_ANTHROPIC ? "/messages" :
        f->options.protocol == DUNLIN_PROTOCOL_GEMINI ?
            (f->cfg.stream ? "/models/fixture-model:streamGenerateContent?alt=sse" :
                             "/models/fixture-model:generateContent") : "/chat/completions";
    char expected[96];
    snprintf(expected, sizeof expected, "%s%s", f->cfg.base_url, suffix);
    assert(!strcmp(r->url, expected));
    f->request = parse(r->json);
    size_t n = strlen(r->json);
    f->request_wire = malloc(n + 1);
    assert(f->request_wire);
    memcpy(f->request_wire, r->json, n + 1);
    *code = f->code;
    if (f->code >= 200 && f->code < 300) {
        for (size_t i = 0; f->wire[i]; ++i) {
            ++f->sent;
            if (!r->body(r->user, f->wire + i, 1)) return DUNLIN_HTTP_FAILED;
        }
    }
    return f->transport_result;
}
static dunlin_http_result post_v1(void *ptr, const dunlin_http_request *r, long *code)
{
    fixture *f = ptr;
    assert(f->options.protocol == DUNLIN_PROTOCOL_CHAT);
    return deliver(f, r, code);
}
static dunlin_http_result post_v2(void *ptr, const dunlin_http_request_v2 *r, long *code)
{
    fixture *f = ptr;
    const char *accept = !f->cfg.stream ? "application/json" :
        f->options.protocol == DUNLIN_PROTOCOL_OLLAMA ? "application/x-ndjson" : "text/event-stream";
    assert(!strcmp(r->accept, accept));
    if (f->options.protocol == DUNLIN_PROTOCOL_ANTHROPIC) {
        assert(r->header_count == 1);
        assert(!strcmp(r->headers[0].name, "anthropic-version"));
        assert(!strcmp(r->headers[0].value, f->options.anthropic_version ? f->options.anthropic_version : "2026-10-21"));
    } else assert(r->header_count == 0);
    return deliver(f, &r->request, code);
}
static void init(fixture *f, dunlin_protocol protocol, int stream)
{
    memset(f, 0, sizeof *f);
    f->code = 200;
    f->v1 = (dunlin_http_v1){post_v1, f};
    f->v2 = (dunlin_http_v2){post_v2, f};
    f->cfg = (dunlin_model_config){
        protocol == DUNLIN_PROTOCOL_OLLAMA ? "http://fixture/api" : "http://fixture/v1",
        "fixture-model", NULL, 2000, stream, 32768, protocol == DUNLIN_PROTOCOL_CHAT ? &f->v1 : NULL};
    f->options = (dunlin_protocol_options){.protocol = protocol, .show_reasoning = 1,
        .user = f, .reasoning = reasoning, .metrics = &f->metrics, .http = &f->v2};
    f->events = (dunlin_model_events){f, cancelled, show};
}
static void clear(fixture *f)
{
    if (f->request) json_object_put(f->request);
    free(f->request_wire);
    f->request = NULL;
    f->request_wire = NULL;
}
static J *run(fixture *f, J *messages, J *tools, const char *wire, dunlin_model_status expected)
{
    clear(f);
    f->wire = wire;
    f->shown_len = f->thought_len = f->sent = 0;
    f->shown[0] = f->thought[0] = f->error[0] = 0;
    f->posts = 0;
    J *assistant = NULL;
    dunlin_model_status status = dunlin_protocol_proposal(&f->cfg, &f->options, messages, tools,
        &f->events, &assistant, f->error, sizeof f->error);
    if (status != expected) fprintf(stderr, "protocol %d expected %d got %d: %s\n",
        f->options.protocol, expected, status, f->error);
    assert(status == expected);
    assert((assistant != NULL) == (status == DUNLIN_MODEL_OK));
    assert(!strstr(f->error, "secret-provider-detail"));
    if (f->posts && f->options.metrics) assert(f->metrics.request_bytes == strlen(f->request_wire));
    if (status == DUNLIN_MODEL_OK && f->options.metrics) {
        assert(f->metrics.response_bytes_known);
        assert(f->metrics.response_bytes == strlen(wire));
    }
    return assistant;
}
static J *messages(void)
{
    return parse("[{\"role\":\"system\",\"content\":\"bound\"},{\"role\":\"user\",\"content\":\"compute\"}]");
}
static J *tools(void)
{
    return parse("[{\"type\":\"function\",\"function\":{\"name\":\"echo\",\"description\":\"return text\",\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}},"
                 "{\"type\":\"function\",\"function\":{\"name\":\"calc\",\"parameters\":{\"type\":\"object\"}}}]");
}
static void result(J *history, const char *id, const char *value)
{
    J *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string("tool"));
    json_object_object_add(m, "tool_call_id", json_object_new_string(id));
    json_object_object_add(m, "content", json_object_new_string(value));
    json_object_array_add(history, m);
}

static const char ollama_text[] = "{\"message\":{\"role\":\"assistant\",\"content\":\"你好\"},\"done\":true,\"done_reason\":\"stop\",\"prompt_eval_count\":10,\"eval_count\":3,\"prompt_eval_cached_count\":4}";
static const char ollama_stream_text[] = "{\"message\":{\"role\":\"assistant\",\"content\":\"你\"},\"done\":false}\r\n"
    "{\"message\":{\"role\":\"assistant\",\"content\":\"好\"},\"done\":true,\"done_reason\":\"stop\"}";
static const char ollama_plain_tools[] = "{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"thinking\":\"public plan\",\"extra\":\"kept\",\"tool_calls\":["
    "{\"function\":{\"index\":0,\"name\":\"echo\",\"arguments\":{\"text\":\"甲\"}}},"
    "{\"function\":{\"index\":1,\"name\":\"echo\",\"arguments\":{\"text\":\"乙\"}}}]},\"done\":true,\"done_reason\":\"stop\"}";
static const char ollama_stream_tools[] = "{\"message\":{\"role\":\"assistant\",\"thinking\":\"public plan\",\"extra\":\"kept\"},\"done\":false}\n"
    "{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"index\":0,\"name\":\"echo\",\"arguments\":{\"text\":\"甲\"}}}]},\"done\":false}\n"
    "{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"index\":1,\"name\":\"echo\",\"arguments\":{\"text\":\"乙\"}}}]},\"done\":false}\n"
    "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true,\"done_reason\":\"stop\"}\n";

static const char gemini_text[] = "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"你好\"}]},\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":7,\"candidatesTokenCount\":3,\"totalTokenCount\":10,\"cachedContentTokenCount\":2}}";
static const char gemini_tool[] = "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"thought\":true,\"text\":\"public plan\",\"thoughtSignature\":\"opaque-signature\"},{\"functionCall\":{\"name\":\"echo\",\"args\":{\"text\":\"甲\"}}}]},\"finishReason\":\"STOP\"}]}";
static const char gemini_stream[] = "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"你\"}]}}]}\n\n"
    "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"好\"}]},\"finishReason\":\"STOP\"}]}\n\n";

static void gemini_tests(void)
{
    fixture f; J *defs=tools();
    for(int stream=0;stream<=1;stream++) {
        init(&f,DUNLIN_PROTOCOL_GEMINI,stream);J *history=messages();
        f.options.max_tokens=64;f.options.has_temperature=1;f.options.temperature=0;
        J *a=run(&f,history,defs,stream?gemini_stream:gemini_text,DUNLIN_MODEL_OK);
        assert(!strcmp(string(a,"content"),"你好"));
        assert(!strcmp(string(field(f.request,"systemInstruction"),"role"),"user"));
        assert(json_object_get_int(field(field(f.request,"generationConfig"),"maxOutputTokens"))==64);
        assert(json_object_array_length(field(at(field(f.request,"tools"),0),"functionDeclarations"))==2);
        if(stream)assert(!strcmp(f.shown,"你好"));
        else assert(f.metrics.input_tokens==7&&f.metrics.output_tokens==3&&f.metrics.total_tokens==10&&f.metrics.cached_input_tokens==2);
        json_object_put(a);clear(&f);json_object_put(history);
    }
    init(&f,DUNLIN_PROTOCOL_GEMINI,0);J *history=messages();
    J *a=run(&f,history,defs,gemini_tool,DUNLIN_MODEL_OK),*calls=field(a,"tool_calls");
    assert(json_object_array_length(calls)==1);
    assert(!strcmp(f.thought,"public plan")&&!strcmp(string(a,"content"),""));
    const char *id=string(at(calls,0),"id");char saved_id[64];snprintf(saved_id,sizeof saved_id,"%s",id);
    assert(strstr(json_object_to_json_string_ext(field(field(a,"_dunlin_provider"),"output"),JSON_C_TO_STRING_PLAIN),"opaque-signature"));
    json_object_array_add(history,a);result(history,saved_id,"observed result");
    a=run(&f,history,defs,gemini_text,DUNLIN_MODEL_OK);
    J *contents=field(f.request,"contents"),*response=field(at(field(at(contents,2),"parts"),0),"functionResponse");
    assert(!strcmp(string(response,"name"),"echo"));
    assert(!strcmp(string(field(response,"response"),"output"),"observed result"));
    assert(strstr(f.request_wire,"opaque-signature")&&!strstr(f.request_wire,"_dunlin_provider"));
    json_object_put(a);
    run(&f,history,defs,"{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"partial\"}]},\"finishReason\":\"MAX_TOKENS\"}]}",DUNLIN_MODEL_LIMIT);
    run(&f,history,defs,"{\"promptFeedback\":{\"blockReason\":\"SAFETY\"}}",DUNLIN_MODEL_PROTOCOL);
    clear(&f);json_object_put(history);json_object_put(defs);
}

static void ollama_tests(void)
{
    fixture f;
    init(&f, DUNLIN_PROTOCOL_OLLAMA, 0);
    J *history = messages(), *defs = tools();
    f.options.max_tokens = 32;
    f.options.has_temperature = 1;
    f.options.temperature = 0;
    f.options.reasoning_effort = DUNLIN_REASONING_NONE;
    f.options.reasoning_supported = DUNLIN_REASONING_CAN_DISABLE;
    J *a = run(&f, history, defs, ollama_text, DUNLIN_MODEL_OK);
    assert(!strcmp(string(a, "content"), "你好"));
    assert(json_object_get_int(field(field(f.request, "options"), "num_predict")) == 32);
    assert(json_object_get_double(field(field(f.request, "options"), "temperature")) == 0);
    assert(json_object_is_type(field(f.request, "think"), json_type_boolean));
    assert(!json_object_get_boolean(field(f.request, "think")));
    assert(f.metrics.present == (DUNLIN_USAGE_INPUT | DUNLIN_USAGE_OUTPUT | DUNLIN_USAGE_TOTAL | DUNLIN_USAGE_CACHED_INPUT));
    assert(f.metrics.input_tokens == 10 && f.metrics.output_tokens == 3 && f.metrics.total_tokens == 13 && f.metrics.cached_input_tokens == 4);
    printf("{\"source\":\"deterministic Ollama mock fixture\",\"request_bytes\":%zu,\"response_bytes\":%zu,\"response_bytes_known\":%s,\"input_tokens\":%" PRIu64 ",\"output_tokens\":%" PRIu64 ",\"total_tokens\":%" PRIu64 ",\"present\":%u}\n",
        f.metrics.request_bytes, f.metrics.response_bytes, f.metrics.response_bytes_known ? "true" : "false",
        f.metrics.input_tokens, f.metrics.output_tokens, f.metrics.total_tokens, f.metrics.present);
    json_object_put(a);
    f.cfg.stream = 1;
    a = run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_OK);
    assert(!strcmp(f.shown, "你好") && f.metrics.present == 0);
    json_object_put(a);
    clear(&f);
    json_object_put(history);
    for (int stream = 0; stream <= 1; ++stream) {
        init(&f, DUNLIN_PROTOCOL_OLLAMA, stream);
        history = messages();
        f.options.show_reasoning = 0;
        a = run(&f, history, defs, stream ? ollama_stream_tools : ollama_plain_tools, DUNLIN_MODEL_OK);
        assert(f.thought_len == 0 && f.shown_len == 0);
        J *calls = field(a, "tool_calls"), *native = field(field(a, "_dunlin_provider"), "output");
        assert(json_object_array_length(calls) == 2);
        assert(strcmp(string(at(calls, 0), "id"), string(at(calls, 1), "id")));
        assert(!strcmp(string(native, "thinking"), "public plan"));
        assert(!strcmp(string(native, "extra"), "kept"));
        J *args = parse(string(field(at(calls, 0), "function"), "arguments"));
        assert(!strcmp(string(args, "text"), "甲"));
        json_object_put(args);
        json_object_array_add(history, a);
        result(history, string(at(calls, 0), "id"), "first");
        result(history, string(at(calls, 1), "id"), "second");
        J *durable = snapshot(history);
        json_object_put(history);
        history = durable;
        a = run(&f, history, defs, stream ? ollama_stream_text : ollama_text, DUNLIN_MODEL_OK);
        J *sent = field(f.request, "messages");
        assert(!strcmp(string(at(sent, 2), "thinking"), "public plan"));
        assert(!strcmp(string(at(sent, 3), "tool_name"), "echo"));
        assert(!strcmp(string(at(sent, 3), "content"), "first"));
        assert(!strcmp(string(at(sent, 4), "tool_name"), "echo"));
        assert(!strcmp(string(at(sent, 4), "content"), "second"));
        assert(!strstr(f.request_wire, "_dunlin_provider") && !strstr(f.request_wire, "tool_call_id"));
        json_object_put(a);
        J *tampered = snapshot(history);
        J *changed = field(at(field(at(tampered, 2), "tool_calls"), 0), "function");
        json_object_object_add(changed, "arguments", json_object_new_string("{\"text\":\"changed\"}"));
        run(&f, tampered, defs, ollama_text, DUNLIN_MODEL_INVALID);
        assert(!f.posts);
        json_object_put(tampered);
        tampered = snapshot(history);
        J *raw = field(field(at(tampered, 2), "_dunlin_provider"), "output");
        changed = field(at(field(raw, "tool_calls"), 0), "function");
        json_object_object_add(changed, "index", json_object_new_int(1));
        run(&f, tampered, defs, ollama_text, DUNLIN_MODEL_INVALID);
        assert(!f.posts);
        json_object_put(tampered);
        json_object_object_add(at(history, 3), "tool_call_id", json_object_new_string("wrong"));
        run(&f, history, defs, ollama_text, DUNLIN_MODEL_INVALID);
        assert(!f.posts);
        json_object_put(history);
        clear(&f);
    }
    init(&f, DUNLIN_PROTOCOL_OLLAMA, 1);
    history = messages();
    f.options.required_tool = "echo";
    run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_REQUIRED_TOOL_MISSING);
    assert(f.shown_len == 0 && json_object_array_length(field(f.request, "tools")) == 1);
    f.options.required_tool = "absent";
    run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_INVALID);
    assert(!f.posts);
    f.options.required_tool = NULL;
    f.options.reasoning_effort = DUNLIN_REASONING_HIGH;
    run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_INVALID);
    assert(!f.posts);
    f.options.reasoning_supported = DUNLIN_REASONING_CAN_EFFORT;
    a = run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_OK);
    assert(!strcmp(string(f.request, "think"), "high"));
    json_object_put(a);
    f.options.reasoning_effort = DUNLIN_REASONING_DEFAULT;
    f.options.reasoning_supported = 0;
    const char *bad[] = {
        "{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":false}\n",
        "{\"message\":{\"role\":\"assistant\",\"content\":null},\"done\":true}\n",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"x\"}}\n",
        "{\"error\":\"secret-provider-detail\"}\n",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":true}\n{}\n",
        "{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"name\":\"echo\",\"arguments\":\"{}\"}}]},\"done\":true}\n",
        "{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"index\":0,\"name\":\"echo\",\"arguments\":{}}},{\"function\":{\"index\":0,\"name\":\"echo\",\"arguments\":{}}}]},\"done\":true}\n"
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) run(&f, history, defs, bad[i], DUNLIN_MODEL_PROTOCOL);
    run(&f, history, defs, "{\"message\":{\"role\":\"assistant\",\"content\":\"partial\"},\"done\":true,\"done_reason\":\"length\",\"eval_count\":3}\n", DUNLIN_MODEL_LIMIT);
    assert((f.metrics.present & DUNLIN_USAGE_OUTPUT) && f.metrics.output_tokens == 3);
    f.cfg.max_response_bytes = 16;
    run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_LIMIT);
    f.cfg.max_response_bytes = 32768;
    f.cancel_now = 1;
    run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_CANCELLED);
    assert(!f.posts && !f.metrics.response_bytes_known);
    f.cancel_now = 0;
    f.cancel_after = 10;
    run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_CANCELLED);
    assert(f.metrics.response_bytes_known);
    f.cancel_after = 0;
    f.code = 429;
    run(&f, history, defs, "", DUNLIN_MODEL_RATE_LIMIT);
    assert(!f.metrics.response_bytes_known);
    f.code = 400;
    run(&f, history, defs, "", DUNLIN_MODEL_HTTP);
    assert(!f.metrics.response_bytes_known);
    f.code = 200;
    f.transport_result = DUNLIN_HTTP_EXPIRED;
    run(&f, history, defs, "", DUNLIN_MODEL_TIMEOUT);
    f.transport_result = DUNLIN_HTTP_FAILED;
    run(&f, history, defs, "", DUNLIN_MODEL_TRANSPORT);
    f.transport_result = DUNLIN_HTTP_OK;
    f.options.reasoning_effort = (dunlin_reasoning_level)99;
    run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_INVALID);
    assert(!f.posts);
    f.options.reasoning_effort = DUNLIN_REASONING_DEFAULT;
    f.cfg.http = &f.v1;
    f.options.http = NULL;
    run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_INVALID);
    assert(!f.posts);
    f.cfg.http = NULL;
    f.options.http = &f.v2;
    f.options.metrics = NULL;
    a = run(&f, history, defs, ollama_stream_text, DUNLIN_MODEL_OK);
    json_object_put(a);
    clear(&f);
    json_object_put(history);
    json_object_put(defs);
}

#define AN_START "event: message_start\r\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":4,\"output_tokens\":1,\"cache_read_input_tokens\":2,\"cache_creation_input_tokens\":1}}}\r\n\r\n"
#define AN_TEXT "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n" \
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"你好\"}}\n\n" \
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
#define AN_END "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":3}}\n\n" \
    "data: {\"type\":\"message_stop\"}\n\n"
static const char anthropic_text[] = "{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"你好\"}],\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":4,\"output_tokens\":3,\"cache_read_input_tokens\":2,\"cache_creation_input_tokens\":1}}";
static const char anthropic_plain_tools[] = "{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
    "{\"type\":\"thinking\",\"thinking\":\"public plan\",\"signature\":\"private-signature\"},"
    "{\"type\":\"redacted_thinking\",\"data\":\"private-redacted\"},"
    "{\"type\":\"tool_use\",\"id\":\"call_a\",\"name\":\"echo\",\"input\":{\"text\":\"甲\"}}],\"stop_reason\":\"tool_use\"}";
static const char anthropic_stream_tools[] = AN_START
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"public plan\"}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"private-signature\"}}\n\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"private-redacted\"}}\n\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"call_a\",\"name\":\"echo\",\"input\":{}}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"text\\\":\"}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"甲\\\"}\"}}\n\n"
    "data: {\"type\":\"content_block_stop\",\"index\":2}\n\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":3}}\n\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static void anthropic_tests(void)
{
    fixture f;
    J *defs = tools();
    for (int stream = 0; stream <= 1; ++stream) {
        init(&f, DUNLIN_PROTOCOL_ANTHROPIC, stream);
        J *history = messages();
        f.options.max_tokens = 64;
        J *a = run(&f, history, defs, stream ? AN_START AN_TEXT AN_END : anthropic_text, DUNLIN_MODEL_OK);
        assert(!strcmp(string(a, "content"), "你好"));
        if (stream) assert(!strcmp(f.shown, "你好"));
        assert(json_object_get_int(field(f.request, "max_tokens")) == 64);
        assert(!strcmp(string(at(field(f.request, "system"), 0), "text"), "bound"));
        assert(json_object_array_length(field(f.request, "messages")) == 1);
        assert(!strcmp(string(at(field(f.request, "tools"), 0), "name"), "echo"));
        assert(field(at(field(f.request, "tools"), 0), "input_schema"));
        assert(f.metrics.input_tokens == 7 && f.metrics.output_tokens == 3 && f.metrics.total_tokens == 10);
        assert(f.metrics.cached_input_tokens == 2 && f.metrics.cache_creation_tokens == 1);
        assert(f.metrics.present == (DUNLIN_USAGE_INPUT | DUNLIN_USAGE_OUTPUT | DUNLIN_USAGE_TOTAL | DUNLIN_USAGE_CACHED_INPUT | DUNLIN_USAGE_CACHE_CREATION));
        json_object_put(a);
        a = run(&f, history, defs, stream ? anthropic_stream_tools : anthropic_plain_tools, DUNLIN_MODEL_OK);
        assert(!strcmp(f.thought, "public plan") && !strstr(f.thought, "private"));
        assert(!strcmp(string(at(field(a, "tool_calls"), 0), "id"), "call_a"));
        json_object_array_add(history, a);
        result(history, "call_a", "observed result");
        J *saved = snapshot(history);
        json_object_put(history);
        history = saved;
        f.options.show_reasoning = 0;
        a = run(&f, history, defs, stream ? AN_START AN_TEXT AN_END : anthropic_text, DUNLIN_MODEL_OK);
        J *sent = field(f.request, "messages"), *blocks = field(at(sent, 1), "content");
        assert(!strcmp(string(at(blocks, 0), "signature"), "private-signature"));
        assert(!strcmp(string(at(blocks, 1), "data"), "private-redacted"));
        J *tool_result = at(field(at(sent, 2), "content"), 0);
        assert(!strcmp(string(at(sent, 2), "role"), "user"));
        assert(!strcmp(string(tool_result, "tool_use_id"), "call_a"));
        assert(!strcmp(string(tool_result, "content"), "observed result"));
        assert(!strstr(f.request_wire, "_dunlin_provider") && !f.thought_len);
        json_object_put(a);
        f.options.protocol = DUNLIN_PROTOCOL_OLLAMA;
        run(&f, history, defs, ollama_text, DUNLIN_MODEL_INVALID);
        assert(!f.posts);
        clear(&f);
        json_object_put(history);
    }
    init(&f, DUNLIN_PROTOCOL_ANTHROPIC, 1);
    J *history = messages();
    const char *bad[] = {
        AN_START AN_TEXT,
        AN_START "data: {\"type\":\"message_stop\"}\n\n",
        AN_START "event: message_delta\ndata: {\"type\":\"ping\"}\n\n",
        AN_START "data: {\"type\":\"error\",\"error\":{\"message\":\"secret-provider-detail\"}}\n\n",
        AN_START "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"server_tool_use\",\"id\":\"remote\"}}\n\n",
        AN_START AN_TEXT AN_END "data: {\"type\":\"ping\"}\n\n",
        AN_START AN_TEXT "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"pause_turn\"}}\n\n"
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) run(&f, history, defs, bad[i], DUNLIN_MODEL_PROTOCOL);
    run(&f, history, defs, AN_START AN_TEXT "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"}}\n\n", DUNLIN_MODEL_LIMIT);
    f.cfg.stream = 0;
    run(&f, history, defs, "{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"partial\"}],\"stop_reason\":\"max_tokens\",\"usage\":{\"output_tokens\":17}}", DUNLIN_MODEL_LIMIT);
    assert((f.metrics.present & DUNLIN_USAGE_OUTPUT) && f.metrics.output_tokens == 17);
    run(&f, history, defs, "{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"same\",\"name\":\"echo\",\"input\":{}},{\"type\":\"tool_use\",\"id\":\"same\",\"name\":\"echo\",\"input\":{}}],\"stop_reason\":\"tool_use\"}", DUNLIN_MODEL_PROTOCOL);
    J *a = run(&f, history, defs, "{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"stop_reason\":\"end_turn\"}", DUNLIN_MODEL_OK);
    assert(f.metrics.present == 0);
    json_object_put(a);
    const char *partial_cache[] = {
        "{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":4,\"output_tokens\":3}}",
        "{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":4,\"output_tokens\":3,\"cache_read_input_tokens\":2}}"
    };
    for (size_t i = 0; i < sizeof partial_cache / sizeof partial_cache[0]; ++i) {
        a = run(&f, history, defs, partial_cache[i], DUNLIN_MODEL_OK);
        assert(!(f.metrics.present & (DUNLIN_USAGE_INPUT | DUNLIN_USAGE_TOTAL)));
        assert((f.metrics.present & DUNLIN_USAGE_OUTPUT) && f.metrics.output_tokens == 3);
        assert(!(f.metrics.present & DUNLIN_USAGE_CACHE_CREATION));
        if (i) assert((f.metrics.present & DUNLIN_USAGE_CACHED_INPUT) && f.metrics.cached_input_tokens == 2);
        json_object_put(a);
    }
    f.options.has_temperature = 1;
    f.options.temperature = 0;
    run(&f, history, defs, anthropic_text, DUNLIN_MODEL_INVALID);
    assert(!f.posts);
    f.options.has_temperature = 0;
    f.options.reasoning_effort = DUNLIN_REASONING_NONE;
    f.options.reasoning_supported = DUNLIN_REASONING_CAN_DISABLE;
    a = run(&f, history, defs, anthropic_text, DUNLIN_MODEL_OK);
    assert(!strcmp(string(field(f.request, "thinking"), "type"), "disabled"));
    json_object_put(a);
    f.options.reasoning_effort = DUNLIN_REASONING_HIGH;
    f.options.reasoning_supported = DUNLIN_REASONING_CAN_EFFORT;
    a = run(&f, history, defs, anthropic_text, DUNLIN_MODEL_OK);
    assert(!strcmp(string(field(f.request, "output_config"), "effort"), "high"));
    json_object_put(a);
    clear(&f);
    json_object_put(history);
    json_object_put(defs);
}

static void chat_tests(void)
{
    fixture f;
    init(&f, DUNLIN_PROTOCOL_CHAT, 1);
    J *history = messages(), *defs = tools();
    /* DeepSeek: usage on the finish chunk. The count includes reasoning. */
    J *a = run(&f, history, defs,
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"public plan\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":5,\"total_tokens\":12,\"prompt_tokens_details\":{\"cached_tokens\":2},\"completion_tokens_details\":{\"reasoning_tokens\":3}}}\n\n"
        "data: [DONE]\n\n", DUNLIN_MODEL_OK);
    assert(!strcmp(f.thought, "public plan") && !strcmp(string(a, "reasoning_content"), "public plan"));
    assert(f.metrics.input_tokens == 7 && f.metrics.output_tokens == 5 && f.metrics.total_tokens == 12 && f.metrics.reasoning_tokens == 3);
    assert(f.metrics.cached_input_tokens == 2);
    assert(json_object_get_boolean(field(field(f.request, "stream_options"), "include_usage")));
    json_object_array_add(history, a);
    /* Qwen: a separate empty-choices usage chunk. */
    f.options.show_reasoning = 0;
    a = run(&f, history, defs,
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":2,\"total_tokens\":9}}\n\n"
        "data: [DONE]\n\n", DUNLIN_MODEL_OK);
    assert(!strcmp(string(at(field(f.request, "messages"), 2), "reasoning_content"), "public plan"));
    assert(f.metrics.total_tokens == 9 && !f.thought_len);
    json_object_put(a);
    /* The v1 fragment contract stays the default, including split names/IDs. */
    a = run(&f, history, defs,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_\",\"type\":\"function\",\"function\":{\"name\":\"ec\",\"arguments\":\"{\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"split\",\"function\":{\"name\":\"ho\",\"arguments\":\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n", DUNLIN_MODEL_OK);
    assert(!strcmp(string(at(field(a, "tool_calls"), 0), "id"), "call_split"));
    assert(!strcmp(string(field(at(field(a, "tool_calls"), 0), "function"), "name"), "echo"));
    json_object_put(a);
    /* Qwen-Omni's documented repeated call ID must not be concatenated. */
    f.options.chat_metadata_complete = 1;
    a = run(&f, history, defs,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_qwen\",\"type\":\"function\",\"function\":{\"name\":\"echo\",\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_qwen\",\"function\":{\"name\":\"echo\",\"arguments\":\"{\\\"text\\\":\\\"ok\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"\",\"function\":{\"name\":null,\"arguments\":\"\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n", DUNLIN_MODEL_OK);
    assert(!strcmp(string(at(field(a, "tool_calls"), 0), "id"), "call_qwen"));
    assert(!strcmp(string(field(at(field(a, "tool_calls"), 0), "function"), "name"), "echo"));
    assert(f.metrics.present == 0);
    json_object_put(a);
    run(&f, history, defs,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"first\",\"type\":\"function\",\"function\":{\"name\":\"echo\",\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"different\",\"function\":{\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n", DUNLIN_MODEL_PROTOCOL);
    f.code = 400;
    run(&f, history, defs, "secret-provider-detail", DUNLIN_MODEL_HTTP);
    assert(!f.metrics.response_bytes_known);
    clear(&f);
    json_object_put(history);
    json_object_put(defs);
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (!strcmp(argv[1], "ollama")) ollama_tests();
        else if (!strcmp(argv[1], "anthropic")) anthropic_tests();
        else if (!strcmp(argv[1], "chat")) chat_tests();
        else if (!strcmp(argv[1], "responses")) protocol_responses_tests();
        else if (!strcmp(argv[1], "gemini")) gemini_tests();
        else return 64;
        printf("%s protocol public-API mock contracts passed\n", argv[1]);
        return 0;
    }
    if (argc != 1) return 64;
    ollama_tests();
    anthropic_tests();
    chat_tests();
    protocol_responses_tests();
    gemini_tests();
    puts("protocol public-API mock request, replay, framing, metrics and failure contracts passed");
    return 0;
}
