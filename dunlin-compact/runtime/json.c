#include "dunlin/compact/json.h"
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <string.h>

typedef struct {
    const unsigned char *p, *end;
    size_t keys;
} cursor;

static void space(cursor *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        ++c->p;
}

static int take(cursor *c, unsigned char ch)
{
    if (c->p == c->end || *c->p != ch) return 0;
    ++c->p;
    return 1;
}

static int literal(cursor *c, const char *s)
{
    size_t n = strlen(s);
    if (n > (size_t)(c->end - c->p) || memcmp(c->p, s, n)) return 0;
    c->p += n;
    return 1;
}

static int hex4(cursor *c)
{
    if (c->end - c->p < 4) return -1;
    int value = 0;
    for (int i = 0; i < 4; ++i) {
        int ch = *c->p++;
        if (ch >= '0' && ch <= '9') ch -= '0';
        else if ((ch | 32) >= 'a' && (ch | 32) <= 'f') ch = (ch | 32) - 'a' + 10;
        else return -1;
        value = value * 16 + ch;
    }
    return value;
}

static int string(cursor *c)
{
    if (!take(c, '"')) return 0;
    while (c->p < c->end) {
        unsigned char ch = *c->p++;
        if (ch == '"') return 1;
        if (ch < 32) return 0;
        if (ch == '\\') {
            if (c->p == c->end) return 0;
            ch = *c->p++;
            if (ch != 'u') {
                if (!ch || !strchr("\"\\/bfnrt", ch)) return 0;
                continue;
            }
            int u = hex4(c);
            if (u <= 0 || (u >= 0xdc00 && u <= 0xdfff)) return 0;
            if (u >= 0xd800 && u <= 0xdbff) {
                if (!literal(c, "\\u")) return 0;
                int low = hex4(c);
                if (low < 0xdc00 || low > 0xdfff) return 0;
            }
        } else if (ch >= 0x80) {
            if (ch < 0xc2 || ch > 0xf4) return 0;
            int count = ch < 0xe0 ? 1 : ch < 0xf0 ? 2 : 3;
            if (c->end - c->p < count) return 0;
            unsigned char next = *c->p;
            if ((ch == 0xe0 && next < 0xa0) || (ch == 0xed && next >= 0xa0) ||
                (ch == 0xf0 && next < 0x90) || (ch == 0xf4 && next >= 0x90)) return 0;
            while (count--) if ((*c->p++ & 0xc0) != 0x80) return 0;
        }
    }
    return 0;
}

static int digits(cursor *c)
{
    const unsigned char *start = c->p;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') ++c->p;
    return c->p != start;
}

static int value(cursor *c, int depth)
{
    space(c);
    if (depth > 30 || c->p == c->end) return 0;
    unsigned char ch = *c->p;
    if (ch == '{' || ch == '[') {
        int obj = ch == '{';
        unsigned char close = obj ? '}' : ']';
        ++c->p;
        space(c);
        if (take(c, close)) return 1;
        do {
            space(c);
            if (obj) {
                if (!string(c)) return 0;
                space(c);
                if (!take(c, ':')) return 0;
                ++c->keys;
            }
            if (!value(c, depth + 1)) return 0;
            space(c);
            if (take(c, close)) return 1;
        } while (take(c, ','));
        return 0;
    }
    if (ch == '"') return string(c);
    if (ch == 't') return literal(c, "true");
    if (ch == 'f') return literal(c, "false");
    if (ch == 'n') return literal(c, "null");
    int negative = take(c, '-');
    const unsigned char *start = c->p;
    if (!take(c, '0') && !digits(c)) return 0;
    if (c->p == c->end || (*c->p != '.' && *c->p != 'e' && *c->p != 'E')) {
        /* Bionic json-c clamps overflowing integers. Reject a value whose
         * argument could silently change between approval and execution. */
        size_t n = (size_t)(c->p - start);
        const char *limit = negative ? "9223372036854775808" : "9223372036854775807";
        return n < 19 || (n == 19 && memcmp(start, limit, 19) <= 0);
    }
    if (take(c, '.') && !digits(c)) return 0;
    if (take(c, 'e') || take(c, 'E')) {
        if (!take(c, '+')) take(c, '-');
        if (!digits(c)) return 0;
    }
    return 1;
}

static int count_keys(struct json_object *o, size_t *count)
{
    if (json_object_is_type(o, json_type_object)) {
        json_object_object_foreach(o, key, child) {
            (void)key;
            ++*count;
            if (!count_keys(child, count)) return 0;
        }
    } else if (json_object_is_type(o, json_type_array)) {
        for (size_t i = 0; i < (size_t)json_object_array_length(o); ++i)
            if (!count_keys(json_object_array_get_idx(o, i), count)) return 0;
    } else if (json_object_is_type(o, json_type_double) && !isfinite(json_object_get_double(o)))
        return 0;
    return 1;
}

struct json_object *dunlin_json_parse(const char *bytes, size_t length)
{
    if (!bytes || length > INT_MAX || !length) return NULL;
    const unsigned char *p = (const unsigned char *)bytes;
    cursor c = {p, p + length, 0};
    space(&c);
    if (c.p == c.end || (*c.p != '{' && *c.p != '[') || !value(&c, 0)) return NULL;
    space(&c);
    if (c.p != c.end) return NULL;
    struct json_tokener *tok = json_tokener_new();
    if (!tok) return NULL;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    struct json_object *o = json_tokener_parse_ex(tok, bytes, (int)length);
    int ok = json_tokener_get_error(tok) == json_tokener_success;
    json_tokener_free(tok);
    size_t keys = 0;
    if (!ok || !o || !count_keys(o, &keys) || keys != c.keys) {
        if (o) json_object_put(o);
        return NULL;
    }
    return o;
}
