#include "dunlin/compact/model.h"
#include "dunlin/compact/http.h"
#include "dunlin/compact/json.h"
#include "protocol_internal.h"
#include <json-c/json.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct json_object json;
typedef struct { char *s; size_t n, cap; } buffer;
typedef struct { buffer id, name, arguments; int typed; } tool_fragments;
enum { MAX_TOOL_INDICES = 64 };
typedef struct {
    const dunlin_model_config *cfg;
    const dunlin_model_events *events;
    const dunlin_protocol_options *options;
    dunlin_model_status status;
    const char *reason;
    size_t received, calls;
    int cr, done, finished, expects_tools;
    buffer body, line, data, content, reasoning;
    tool_fragments call[MAX_TOOL_INDICES];
} response;

static int fail(response *r, dunlin_model_status code, const char *reason)
{
    r->status = code;
    r->reason = reason;
    return 0;
}

static int append(response *r, buffer *b, const char *s, size_t n)
{
    if (n > r->cfg->max_response_bytes - b->n)
        return fail(r, DUNLIN_MODEL_LIMIT, "model response exceeds byte limit");
    size_t need = b->n + n + 1;
    if (b->cap < need) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        if (cap < need) cap = need;
        if (cap > r->cfg->max_response_bytes + 1) cap = r->cfg->max_response_bytes + 1;
        char *p = realloc(b->s, cap);
        if (!p) return fail(r, DUNLIN_MODEL_LIMIT, "model response allocation failed");
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
    return 1;
}

static json *field(json *o, const char *key)
{
    json *v = NULL;
    json_object_object_get_ex(o, key, &v);
    return v;
}
static const char *str(json *o, const char *key)
{
    json *v = field(o, key);
    return json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL;
}
static int is(const char *str_value, const char *s)
{
    return str_value && !strcmp(str_value, s);
}
static int finish(response *r, const char *reason)
{
    if (!reason) return 1;
    if (is(reason, "length")) return fail(r, DUNLIN_MODEL_LIMIT, "model stopped at its token limit");
    if (!is(reason, "stop") && !is(reason, "tool_calls"))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "unsupported completion finish_reason");
    r->finished = 1;
    r->expects_tools = is(reason, "tool_calls");
    return 1;
}

static int usage(response *r,json *o)
{
    json *u=field(o,"usage");
    if(!r->options || !u) return 1;
    if(!json_object_is_type(u,json_type_object))
        return fail(r,DUNLIN_MODEL_PROTOCOL,"invalid model usage object");
    dunlin_model_metrics scratch={0},*m=r->options->metrics?r->options->metrics:&scratch;
    ahp_context c={r->cfg,r->options,r->events,DUNLIN_MODEL_OK,{0}};
    int ok=ahp_metric(&c,u,"prompt_tokens",DUNLIN_USAGE_INPUT,&m->input_tokens) &&
        ahp_metric(&c,u,"completion_tokens",DUNLIN_USAGE_OUTPUT,&m->output_tokens) &&
        ahp_metric(&c,u,"total_tokens",DUNLIN_USAGE_TOTAL,&m->total_tokens) &&
        ahp_metric(&c,field(u,"prompt_tokens_details"),"cached_tokens",DUNLIN_USAGE_CACHED_INPUT,&m->cached_input_tokens) &&
        ahp_metric(&c,field(u,"completion_tokens_details"),"reasoning_tokens",DUNLIN_USAGE_REASONING,&m->reasoning_tokens);
    return ok?1:fail(r,DUNLIN_MODEL_PROTOCOL,"invalid provider token statistic");
}
static int reasoning_delta(response *r,json *delta)
{
    json *v=field(delta,"reasoning_content");
    if(!r->options || !v) return 1;
    if(!json_object_is_type(v,json_type_string))return fail(r,DUNLIN_MODEL_PROTOCOL,"invalid reasoning content fragment");
    const char *s=json_object_get_string(v);size_t n=(size_t)json_object_get_string_len(v);
    if(!append(r,&r->reasoning,s,n))return 0;
    if(n && r->options->show_reasoning && r->options->reasoning)
        r->options->reasoning(r->options->user,"reasoning.delta",s,n);
    return 1;
}

static int add_field(response *r, buffer *b, json *o, const char *key)
{
    json *v = field(o, key);
    if (!v) return 1;
    if (!json_object_is_type(v, json_type_string))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "non-string tool call fragment");
    return append(r, b, json_object_get_string(v), (size_t)json_object_get_string_len(v));
}

static int add_metadata(response *r,buffer *b,json *o,const char *key)
{
    if(!r->options || !r->options->chat_metadata_complete)return add_field(r,b,o,key);
    json *v=field(o,key);
    if(!v)return 1;
    if(!json_object_is_type(v,json_type_string))return fail(r,DUNLIN_MODEL_PROTOCOL,"invalid complete tool metadata");
    const char *s=json_object_get_string(v);size_t n=(size_t)json_object_get_string_len(v);
    if(!n)return 1;
    if(b->n)return b->n==n && !memcmp(b->s,s,n)?1:fail(r,DUNLIN_MODEL_PROTOCOL,"conflicting complete tool metadata");
    return append(r,b,s,n);
}

static int tool_delta(response *r, json *part)
{
    json *idx = field(part, "index");
    if (!json_object_is_type(idx, json_type_int) || json_object_get_int64(idx) < 0 ||
        json_object_get_int64(idx) >= MAX_TOOL_INDICES)
        return fail(r, DUNLIN_MODEL_PROTOCOL, "out-of-range or missing tool delta index");
    size_t i = (size_t)json_object_get_int64(idx);
    if (i >= r->calls) r->calls = i + 1;
    tool_fragments *c = &r->call[i];
    if (field(part, "type")) {
        if (!is(str(part, "type"), "function"))
            return fail(r, DUNLIN_MODEL_PROTOCOL, "unsupported tool call type");
        c->typed = 1;
    }
    json *fn = field(part, "function");
    if (fn && !json_object_is_type(fn, json_type_object))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "invalid function delta");
    return add_metadata(r, &c->id, part, "id") && add_metadata(r, &c->name, fn, "name") &&
           add_field(r, &c->arguments, fn, "arguments");
}

static int chunk(response *r, json *o)
{
    json *remote_error = field(o,"error");
    if (remote_error) {
        const char *why = is(str(remote_error,"code"),"incomplete_tool_call") ?
            "model emitted an incomplete tool call" :
            is(str(remote_error,"code"),"invalid_tool_call") ?
            "model emitted an invalid tool call" : "model reported an error in stream";
        return fail(r, (is(str(remote_error,"code"),"incomplete_tool_call") ||
                        is(str(remote_error,"code"),"invalid_tool_call"))?DUNLIN_MODEL_TOOL_FORMAT:DUNLIN_MODEL_PROTOCOL, why);
    }
    if(!usage(r,o))return 0;
    json *choices = field(o, "choices");
    if (!json_object_is_type(choices, json_type_array))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "missing choices array in model stream");
    size_t count = (size_t)json_object_array_length(choices);
    if (!count && r->finished && field(o, "usage")) return 1;
    if (r->finished || count != 1)
        return fail(r, DUNLIN_MODEL_PROTOCOL, "expected one active streaming choice");
    json *choice = json_object_array_get_idx(choices, 0);
    json *idx = field(choice, "index");
    if (idx && (!json_object_is_type(idx, json_type_int) || json_object_get_int64(idx) != 0))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "unexpected choice index");
    json *delta = field(choice, "delta");
    if (!json_object_is_type(delta, json_type_object) || field(delta, "function_call") ||
        (field(delta, "role") && !is(str(delta, "role"), "assistant")))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "invalid assistant delta or legacy function_call");
    if(!reasoning_delta(r,delta))return 0;
    if (field(delta, "content")) {
        const char *s = str(delta, "content");
        if (!s) return fail(r, DUNLIN_MODEL_PROTOCOL, "non-string content delta");
        size_t n = strlen(s);
        if (!append(r, &r->content, s, n)) return 0;
        if (r->events && r->events->delta && n) r->events->delta(r->events->user, s, n);
    }
    json *calls = field(delta, "tool_calls");
    if (calls) {
        if (!json_object_is_type(calls, json_type_array))
            return fail(r, DUNLIN_MODEL_PROTOCOL, "invalid tool_calls delta");
        for (size_t i = 0; i < (size_t)json_object_array_length(calls); ++i)
            if (!tool_delta(r, json_object_array_get_idx(calls, i))) return 0;
    }
    return finish(r, str(choice, "finish_reason"));
}

static int line_end(response *r)
{
    int ok = 1;
    if (!r->line.n && r->data.n) {
        /* SSE appends LF after every data line and drops the final LF. */
        r->data.s[--r->data.n] = 0;
        if (r->done) ok = fail(r, DUNLIN_MODEL_PROTOCOL, "data after [DONE]");
        else if (r->data.n == 6 && !memcmp(r->data.s, "[DONE]", 6)) r->done = 1;
        else {
            json *o = dunlin_json_parse(r->data.s, r->data.n);
            if (!o) ok = fail(r, DUNLIN_MODEL_PROTOCOL, "malformed JSON in SSE event");
            else {
                ok = chunk(r, o);
                json_object_put(o);
            }
        }
        r->data.n = 0;
    } else if (r->line.n >= 4 && !memcmp(r->line.s, "data", 4) &&
               (r->line.n == 4 || r->line.s[4] == ':')) {
        size_t start = r->line.n == 4 ? 4 : 5;
        if (r->line.s[start] == ' ') ++start;
        ok = append(r, &r->data, r->line.s + start, r->line.n - start) &&
             append(r, &r->data, "\n", 1);
    }
    r->line.n = 0;
    return ok;
}

static int cancelled(void *user)
{
    response *r = user;
    return r->events && r->events->cancelled && r->events->cancelled(r->events->user);
}
static int body(void *user, const char *s, size_t n)
{
    response *r = user;
    if (cancelled(r)) return fail(r, DUNLIN_MODEL_CANCELLED, "model request cancelled");
    if(r->options && r->options->metrics && n<=SIZE_MAX-r->received)
        r->options->metrics->response_bytes=r->received+n;
    if (n > r->cfg->max_response_bytes - r->received)
        return fail(r, DUNLIN_MODEL_LIMIT, "model response exceeds byte limit");
    r->received += n;
    if (!r->cfg->stream) return append(r, &r->body, s, n);
    while (n) {
        char ch = *s;
        if (ch == '\r' || ch == '\n') {
            if (!(r->cr && ch == '\n') && !line_end(r)) return 0;
            r->cr = ch == '\r';
            ++s; --n;
        } else {
            r->cr = 0;
            size_t i = 0;
            while (i < n && s[i] != '\r' && s[i] != '\n') ++i;
            if (!append(r, &r->line, s, i)) return 0;
            s += i; n -= i;
        }
    }
    return 1;
}

static json *stream_message(response *r)
{
    if (!r->done || !r->finished || r->line.n || r->data.n) {
        fail(r, DUNLIN_MODEL_PROTOCOL, "incomplete SSE stream (finish_reason and [DONE] required)");
        return NULL;
    }
    json *msg = json_object_new_object();
    json_object_object_add(msg, "role", json_object_new_string("assistant"));
    json_object_object_add(msg, "content", r->content.s ? json_object_new_string(r->content.s) : NULL);
    if(r->reasoning.s)json_object_object_add(msg,"reasoning_content",json_object_new_string(r->reasoning.s));
    if (r->calls) {
        json *array = json_object_new_array();
        json_object_object_add(msg, "tool_calls", array);
        for (size_t i = 0; i < r->calls; ++i) {
            tool_fragments *c = &r->call[i];
            if (!c->typed || !c->id.s || !c->name.s || !c->arguments.s) {
                fail(r, DUNLIN_MODEL_PROTOCOL, "incomplete streamed function call");
                json_object_put(msg);
                return NULL;
            }
            json *call = json_object_new_object(), *fn = json_object_new_object();
            json_object_object_add(call, "type", json_object_new_string("function"));
            json_object_object_add(call, "id", json_object_new_string(c->id.s));
            json_object_object_add(fn, "name", json_object_new_string(c->name.s));
            json_object_object_add(fn, "arguments", json_object_new_string(c->arguments.s));
            json_object_object_add(call, "function", fn);
            json_object_array_add(array, call);
        }
    }
    return msg;
}

static json *plain_message(response *r)
{
    json *o = dunlin_json_parse(r->body.s, r->body.n);
    json *choices = field(o, "choices"), *msg = NULL;
    if(!usage(r,o)){if(o)json_object_put(o);return NULL;}
    if(field(o,"error")) {chunk(r,o);json_object_put(o);return NULL;}
    if (!json_object_is_type(choices, json_type_array) || json_object_array_length(choices) != 1)
        fail(r, DUNLIN_MODEL_PROTOCOL, "malformed completion JSON or missing single choice");
    else {
        json *choice = json_object_array_get_idx(choices, 0);
        const char *reason = str(choice, "finish_reason");
        if (!reason) fail(r, DUNLIN_MODEL_PROTOCOL, "missing finish_reason");
        else if (finish(r, reason)) {
            msg = json_object_get(field(choice, "message"));
            if(!reasoning_delta(r,msg)){json_object_put(msg);msg=NULL;}
        }
    }
    if (o) json_object_put(o);
    return msg;
}

static int valid_message(response *r, json *msg, int strict_arguments)
{
    if (!is(str(msg, "role"), "assistant") || field(msg, "function_call"))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "invalid assistant message or legacy function_call");
    json *calls = field(msg, "tool_calls");
    if (calls && !json_object_is_type(calls, json_type_array))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "expected tool_calls array");
    size_t count = calls ? (size_t)json_object_array_length(calls) : 0;
    if (count > MAX_TOOL_INDICES) return fail(r, DUNLIN_MODEL_LIMIT, "too many model tool calls");
    if ((count > 0) != r->expects_tools || (!count && !str(msg, "content")) ||
        (field(msg, "content") && !str(msg, "content")))
        return fail(r, DUNLIN_MODEL_PROTOCOL, "completion message disagrees with finish reason");
    for (size_t i = 0; i < count; ++i) {
        json *call = json_object_array_get_idx(calls, i), *fn = field(call, "function");
        const char *id = str(call, "id"), *name = str(fn, "name"), *args = str(fn, "arguments");
        if (!is(str(call, "type"), "function") || !id || !*id || !name || !*name || !args)
            return fail(r, DUNLIN_MODEL_PROTOCOL, "malformed function call");
        for (size_t j = 0; j < i; ++j)
            if (is(str(json_object_array_get_idx(calls, j), "id"), id))
                return fail(r, DUNLIN_MODEL_PROTOCOL, "duplicate tool call id");
        if (!strict_arguments) continue; /* The Agent pairs argument errors with this call ID. */
        json *parsed = dunlin_json_parse(args, strlen(args));
        int ok = json_object_is_type(parsed, json_type_object);
        if (parsed) json_object_put(parsed);
        if (!ok) return fail(r, DUNLIN_MODEL_PROTOCOL, "tool arguments must contain an unambiguous JSON object");
    }
    return 1;
}

static int ascii_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int dns_or_ipv4(const char *start, size_t len)
{
    if (!len || len > 254) return 0;
    char host[255];
    memcpy(host, start, len);
    host[len] = 0;
    if (strspn(host, "0123456789.") == len) {
        struct in_addr v4;
        char canonical[INET_ADDRSTRLEN];
        return inet_pton(AF_INET, host, &v4)==1 && inet_ntop(AF_INET,&v4,canonical,sizeof canonical) &&
               !strcmp(host,canonical);
    }
    if (host[len - 1] == '.') --len;
    if (len > 253) return 0;
    for (size_t i = 0; i < len;) {
        size_t begin = i;
        while (i < len && host[i] != '.') {
            if (!ascii_alnum((unsigned char)host[i]) && host[i] != '-') return 0;
            ++i;
        }
        if (i == begin || i - begin > 63 || host[begin] == '-' || host[i-1] == '-') return 0;
        if (i < len && ++i == len) return 0;
    }
    return 1;
}

int dunlin_model_url_valid(const char *url)
{
    if (!url || strlen(url) > 2048) return 0;
    size_t prefix = !strncmp(url, "https://", 8) ? 8 : !strncmp(url, "http://", 7) ? 7 : 0;
    if (!prefix || strpbrk(url, "?#@\\")) return 0;
    const char *host = url + prefix;
    const char *end = host + strcspn(host, "/");
    const char *port;
    if (*host == '[') {
        const char *bracket = memchr(host, ']', (size_t)(end - host));
        char address[INET6_ADDRSTRLEN];
        if (!bracket || (size_t)(bracket - host - 1) >= sizeof(address)) return 0;
        size_t n = (size_t)(bracket - host - 1);
        memcpy(address, host + 1, n);
        address[n] = 0;
        struct in6_addr v6;
        if (inet_pton(AF_INET6, address, &v6) != 1) return 0;
        port = bracket + 1;
    } else {
        port = memchr(host, ':', (size_t)(end - host));
        if (!port) port = end;
        if (!dns_or_ipv4(host, (size_t)(port - host))) return 0;
    }
    if (port != end) {
        if (*port++ != ':' || port == end) return 0;
        unsigned value = 0;
        while (port != end) {
            if (*port < '0' || *port > '9') return 0;
            value = value * 10 + (unsigned)(*port++ - '0');
            if (value > 65535) return 0;
        }
        if (!value) return 0;
    }
    for (const unsigned char *p = (const unsigned char *)end; *p; ++p) {
        if (*p <= 32 || *p == 127) return 0;
        if (*p == '%') {
            const char *hex = "0123456789abcdefABCDEF";
            if (!p[1] || !strchr(hex, p[1]) || !p[2] || !strchr(hex, p[2])) return 0;
            p += 2;
        }
    }
    return 1;
}

static dunlin_model_status complete(const dunlin_model_config *cfg, json *messages, json *tools,
    const dunlin_model_events *events, json **assistant, char *error, size_t error_size, int strict_arguments,
    const dunlin_protocol_options *options)
{
    response r = { .cfg = cfg, .events = events, .options=options, .status = DUNLIN_MODEL_OK };
    json *request = NULL, *msg = NULL;
    if (assistant) *assistant = NULL;
    if (!assistant || !cfg || !dunlin_model_url_valid(cfg->base_url) || !cfg->model || !*cfg->model ||
        cfg->timeout_ms < 1 || !cfg->max_response_bytes || cfg->max_response_bytes >= INT_MAX ||
        !json_object_is_type(messages, json_type_array) || (tools && !json_object_is_type(tools, json_type_array))) {
        fail(&r, DUNLIN_MODEL_INVALID, "invalid model configuration or request");
        goto done;
    }
    if (cancelled(&r)) { fail(&r, DUNLIN_MODEL_CANCELLED, "model request cancelled"); goto done; }
    const char *key = NULL;
    if (cfg->key_env && *cfg->key_env) {
        key = getenv(cfg->key_env);
        if (!key || !*key || strlen(key) > 16384 || strpbrk(key, "\r\n")) {
            fail(&r, DUNLIN_MODEL_INVALID, "model credential unavailable or invalid");
            goto done;
        }
    }
    char url[2080];
    size_t n = strlen(cfg->base_url);
    while (n && cfg->base_url[n-1] == '/') --n;
    snprintf(url, sizeof(url), "%.*s/chat/completions", (int)n, cfg->base_url);
    request = json_object_new_object();
    json_object_object_add(request, "model", json_object_new_string(cfg->model));
    json_object_object_add(request, "stream", json_object_new_boolean(cfg->stream));
    json_object_object_add(request, "messages", json_object_get(messages));
    if (tools && json_object_array_length(tools))
        json_object_object_add(request, "tools", json_object_get(tools));
    if(options) {
        if(options->max_tokens)json_object_object_add(request,"max_tokens",json_object_new_int64(options->max_tokens));
        if(options->has_temperature)json_object_object_add(request,"temperature",json_object_new_double(options->temperature));
        if(options->reasoning_effort!=DUNLIN_REASONING_DEFAULT)
            json_object_object_add(request,"reasoning_effort",json_object_new_string(ahp_effort(options->reasoning_effort)));
        if(cfg->stream && options->metrics) {
            json *stream_options=json_object_new_object();
            json_object_object_add(stream_options,"include_usage",json_object_new_boolean(1));
            json_object_object_add(request,"stream_options",stream_options);
        }
        if(options->required_tool && *options->required_tool) {
            json *choice=json_object_new_object(),*fn=json_object_new_object();
            json_object_object_add(choice,"type",json_object_new_string("function"));
            json_object_object_add(fn,"name",json_object_new_string(options->required_tool));
            json_object_object_add(choice,"function",fn);
            json_object_object_add(request,"tool_choice",choice);
        }
        if(options->metrics)options->metrics->request_bytes=strlen(json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN));
    }
    dunlin_http_request http = {url, json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN),
                              key, cfg->stream, cfg->timeout_ms, &r, body, cancelled};
    const dunlin_http_v1 *transport = cfg->http ? cfg->http : &dunlin_curl_http;
    long code = 0;
    if (!transport->post) { fail(&r, DUNLIN_MODEL_INVALID, "missing HTTP transport"); goto done; }
    dunlin_http_result rc = transport->post(transport->user, &http, &code);
    if(options && options->metrics)options->metrics->response_bytes_known=code>=200 && code<300;
    char http_error[96];
    if(r.status==DUNLIN_MODEL_TOOL_FORMAT && !(code>=200 && code<300)) {r.status=DUNLIN_MODEL_OK;r.reason=NULL;}
    if (r.status != DUNLIN_MODEL_OK) goto done;
    if (code == 429) fail(&r, DUNLIN_MODEL_RATE_LIMIT, "model HTTP 429 (rate limited)");
    else if (code && (code < 200 || code >= 300)) {
        snprintf(http_error, sizeof(http_error), "model HTTP %ld (check endpoint, credentials and tool support)", code);
        fail(&r, DUNLIN_MODEL_HTTP, http_error);
    } else if (rc == DUNLIN_HTTP_ABORTED || cancelled(&r)) fail(&r, DUNLIN_MODEL_CANCELLED, "model request cancelled");
    else if (rc == DUNLIN_HTTP_EXPIRED) fail(&r, DUNLIN_MODEL_TIMEOUT, "model request timed out");
    else if (rc != DUNLIN_HTTP_OK || !code) fail(&r, DUNLIN_MODEL_TRANSPORT, "model connection or TLS transfer failed");
    else {
        msg = cfg->stream ? stream_message(&r) : plain_message(&r);
        if (r.status == DUNLIN_MODEL_OK && valid_message(&r, msg, strict_arguments)) {
            *assistant = msg;
            msg = NULL;
        }
    }
done:
    if (error && error_size) snprintf(error, error_size, "%s", r.reason ? r.reason : "");
    if (msg) json_object_put(msg);
    if (request) json_object_put(request);
    free(r.body.s); free(r.line.s); free(r.data.s); free(r.content.s);free(r.reasoning.s);
    for (size_t i = 0; i < r.calls; ++i) {
        free(r.call[i].id.s); free(r.call[i].name.s); free(r.call[i].arguments.s);
    }
    return r.status;
}


dunlin_model_status dunlin_chat_completion(const dunlin_model_config *cfg, json *messages, json *tools,
    const dunlin_model_events *events, json **assistant, char *error, size_t error_size)
{
    return complete(cfg,messages,tools,events,assistant,error,error_size,1,NULL);
}

dunlin_model_status dunlin_chat_proposal(const dunlin_model_config *cfg, json *messages, json *tools,
    const dunlin_model_events *events, json **assistant, char *error, size_t error_size)
{
    return complete(cfg,messages,tools,events,assistant,error,error_size,0,NULL);
}

dunlin_model_status ahp_chat(const dunlin_model_config *cfg,const dunlin_protocol_options *options,
    json *messages,json *tools,const dunlin_model_events *events,json **assistant,char *error,size_t size)
{
    ahp_context c={cfg,options,events,DUNLIN_MODEL_OK,{0}};
    json *view=ahp_clone(messages);
    if(!view){ahp_fail(&c,DUNLIN_MODEL_INVALID,"invalid chat message history");return ahp_result(&c,error,size);}
    for(size_t i=0;i<(size_t)json_object_array_length(view);i++) {
        json *msg=json_object_array_get_idx(view,i);
        if(field(msg,"_dunlin_provider")) {
            (void)ahp_native(&c,msg,"chat");
            if(c.status!=DUNLIN_MODEL_OK){json_object_put(view);return ahp_result(&c,error,size);}
            json_object_object_del(msg,"_dunlin_provider");
        }
    }
    dunlin_model_status status=complete(cfg,view,tools,events,assistant,error,size,0,options);
    json_object_put(view);return status;
}
