#ifndef DUNLIN_HTTP2_H
#define DUNLIN_HTTP2_H
#include "dunlin/compact/http.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { const char *name, *value; } dunlin_http_header;
/* A separate transport ABI: never cast a v1 request to this type. Borrow all
 * fields for post(). Headers must contain no control characters. */
typedef struct {
    dunlin_http_request request;
    const char *accept;
    const dunlin_http_header *headers;
    size_t header_count;
} dunlin_http_request_v2;
typedef struct dunlin_http_v2 {
    dunlin_http_result (*post)(void *, const dunlin_http_request_v2 *, long *status);
    void *user;
} dunlin_http_v2;
extern const dunlin_http_v2 dunlin_curl_http_v2;
#ifdef __cplusplus
}
#endif
#endif
