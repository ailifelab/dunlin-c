#ifndef DUNLIN_HTTP_H
#define DUNLIN_HTTP_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { DUNLIN_HTTP_OK, DUNLIN_HTTP_FAILED, DUNLIN_HTTP_EXPIRED, DUNLIN_HTTP_ABORTED } dunlin_http_result;
/* Version 1 synchronous transport. Deliver only successful response bodies. The
 * host must enforce TLS verification and must not log secrets or follow redirects.
 * Return the HTTP status independently of transport success (zero before headers). */
typedef struct {
    const char *url, *json, *bearer;
    int stream;
    long timeout_ms;
    void *user;
    int (*body)(void *, const char *, size_t);     /* zero aborts the request */
    int (*cancelled)(void *);
} dunlin_http_request;
typedef struct dunlin_http_v1 {
    dunlin_http_result (*post)(void *, const dunlin_http_request *, long *status);
    void *user;
} dunlin_http_v1;
extern const dunlin_http_v1 dunlin_curl_http;
#ifdef __cplusplus
}
#endif
#endif
