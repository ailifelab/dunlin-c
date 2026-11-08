#include "dunlin/compact/http.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/model.h"
#include <json-c/json.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *body;
    char text[32];
    size_t len;
    long code;
} fixture;

static void delta(void *ctx,const char *str,size_t len)
{
    fixture *f = ctx;
    assert(f->len + len < sizeof(f->text));
    memcpy(f->text + f->len, str, len);
    f->len += len;
}

static dunlin_http_result post(void *ctx, const dunlin_http_request *r, long *code)
{
    fixture *f = ctx;
    assert(!strcmp(r->url,"http://test/v1/chat/completions"));
    assert(r->bearer == NULL);
    *code = f->code?f->code:200;
    for (size_t i = 0; f->body[i]; ++i)
        if (!r->body(r->user, f->body+i, 1)) return DUNLIN_HTTP_FAILED;
    return DUNLIN_HTTP_OK;
}
static long test_code;
static void exercise(const char *wire, int stream, dunlin_model_status expected)
{
    fixture f = { .body = wire, .code=test_code };
    dunlin_http_v1 http = {post, &f};
    dunlin_model_config cfg = {"http://test/v1/", "unit", NULL, 2000, stream, 8192, &http};
    struct json_object *messages = json_object_new_array(), *assistant = NULL;
    char error[128];
    dunlin_model_events events = {&f, NULL, delta};
    dunlin_model_status status = dunlin_chat_completion(&cfg, messages, NULL, &events,
                                                   &assistant, error, sizeof(error));
    if (status != expected) fprintf(stderr,"got %d instead of %d (%s)\n",status, expected,error);
    assert(status == expected);
    if(expected==DUNLIN_MODEL_TOOL_FORMAT && strstr(wire,"\"invalid_tool_call\""))
        assert(!strcmp(error,"model emitted an invalid tool call"));
    assert((assistant != NULL) == (expected == DUNLIN_MODEL_OK));
    if (assistant) {
        struct json_object *content = NULL;
        assert(json_object_object_get_ex(assistant,"content",&content));
        assert(!strcmp(json_object_get_string(content), "你好"));
        if (stream) assert(!strcmp(f.text, "你好"));
        json_object_put(assistant);
    }
    json_object_put(messages);
}
int main(void)
{
    const char *valid_urls[] = {"http://localhost", "https://host.docker.internal:65535/v1",
        "http://model-1.:1/a%20b","http://127.0.0.1:8080/v1","http://[::1]/v1",
        "https://[2001:db8::1]:443/v1","http://[::ffff:192.0.2.128]:80"};
    const char *bad_urls[] = {"http:///v1","http://:123/v1","http://[::1/v1","http://::1",
        "http://[::1]x/v1","http://[::dead::beef]","http://0.0.0.999", "http://0177.0.0.1",
        "http://bad..name", "http://-host/", "http://host../", "http://host:0", "http://host:",
        "http://host:65536", "http://host:abc", "http://host/%xz", "http://a@host", "http://host?a=1"};
    for (size_t i=0;i<sizeof(valid_urls)/sizeof(valid_urls[0]);++i) assert(dunlin_model_url_valid(valid_urls[i]));
    for (size_t i=0;i<sizeof(bad_urls)/sizeof(bad_urls[0]);++i) assert(!dunlin_model_url_valid(bad_urls[i]));
    exercise(": keepalive\r\ndata: {\"choices\":[{\r\ndata: \"delta\":{\"content\":\"你\"}}]}\r\n\r\n"
             "data: {\"choices\":[{\"delta\":{\"content\":\"好\"},\"finish_reason\":\"stop\"}]}\r\r"
             "data: [DONE]\n\n", 1, DUNLIN_MODEL_OK);
    exercise("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"你好\"},\"finish_reason\":\"stop\"}]}",0,DUNLIN_MODEL_OK);
    exercise("bad",0,DUNLIN_MODEL_PROTOCOL);
    exercise("data: [DONE]\n\n",1,DUNLIN_MODEL_PROTOCOL);
    exercise("data: {\"choices\":[{\"delta\":{\"content\":\"x\"},\"finish_reason\":\"stop\"}]}\n\n",
             1,DUNLIN_MODEL_PROTOCOL);
    exercise("data: {}\n\n",1,DUNLIN_MODEL_PROTOCOL);
    exercise("data: {\"error\":{\"code\":\"incomplete_tool_call\"}}\n\n",1,DUNLIN_MODEL_TOOL_FORMAT);
    exercise("data: {\"error\":{\"code\":\"invalid_tool_call\",\"message\":\"private server detail\"}}\n\n",1,DUNLIN_MODEL_TOOL_FORMAT);
    exercise("{\"error\":{\"code\":\"invalid_tool_call\"}}",0,DUNLIN_MODEL_TOOL_FORMAT);
    test_code=400;
    exercise("data: {\"error\":{\"code\":\"invalid_tool_call\"}}\n\n",1,DUNLIN_MODEL_HTTP);
    test_code=0;
    exercise("data: xxx\n\n",1,DUNLIN_MODEL_PROTOCOL);
    exercise("{\"choices\":[{\"message\":null,\"finish_reason\":\"stop\"}]}",0,DUNLIN_MODEL_PROTOCOL);
    exercise("{\"choices\":[{\"finish_reason\":\"length\"}]}",0,DUNLIN_MODEL_LIMIT);
    const char *invalid[] = {"{\"a\":0,\"\\u0061\":1}","{\"child\":{\"a\":0,\"a\":1}}","[]extra",
                             "[1,]","['foo']","[01]","[9223372036854775808]","[-9223372036854775809]","[1.]","[NaN]","[\"\\u0000\"]",
                             "[\"\\uD800\"]","[\"\\uFFFF\\uDC00\"]","[\"\300\200\"]"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        assert(!dunlin_json_parse(invalid[i],strlen(invalid[i])));
    puts("model byte-by-byte UTF-8/SSE, error frames, JSON admission passed");
}
