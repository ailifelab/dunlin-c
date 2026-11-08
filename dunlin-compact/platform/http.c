#include "dunlin/compact/http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    CURL *easy;
    const dunlin_http_request *request;
} transfer;

static size_t receive_body(char *data, size_t size, size_t items, void *ptr)
{
    transfer *t = ptr;
    size_t length = size * items;
    long code = 0;
    if (curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK)
        return 0;
    if (code < 200 || code >= 300)
        return 0;
    return t->request->body(t->request->user, data, length) ? length : 0;
}

static int progress(void *ptr, curl_off_t down, curl_off_t got, curl_off_t up, curl_off_t sent)
{
    (void) down; (void) got; (void) up; (void) sent;
    const dunlin_http_request *r = ptr;
    return r->cancelled && r->cancelled(r->user);
}

static int add_header(struct curl_slist **headers, const char *value)
{
    struct curl_slist *next = curl_slist_append(*headers, value);
    if (!next) return 0;
    *headers = next;
    return 1;
}

static dunlin_http_result curl_post(void *unused, const dunlin_http_request *r, long *status)
{
    (void) unused;
    *status = 0;
    transfer t = { curl_easy_init(), r };
    if (!t.easy) return DUNLIN_HTTP_FAILED;
    dunlin_http_result result = DUNLIN_HTTP_FAILED;
    struct curl_slist *headers = NULL;
    char *auth = NULL;
    /* JSON is already buffered. Older curl otherwise waits for 100 Continue
     * before sending bodies over 1 KiB to many local HTTP/1.0 servers. */
    if (!add_header(&headers, "Expect:") || !add_header(&headers, "Content-Type: application/json") ||
        !add_header(&headers, r->stream ? "Accept: text/event-stream" : "Accept: application/json"))
        goto done;
    if (r->bearer) {
        size_t n = strlen(r->bearer);
        auth = malloc(n + sizeof("Authorization: Bearer "));
        if (!auth) goto done;
        memcpy(auth, "Authorization: Bearer ", sizeof("Authorization: Bearer ") - 1);
        memcpy(auth + sizeof("Authorization: Bearer ") - 1, r->bearer, n + 1);
        if (!add_header(&headers, auth)) goto done;
    }
#define SET(option, value) do { if (curl_easy_setopt(t.easy, option, value) != CURLE_OK) goto done; } while (0)
    SET(CURLOPT_URL, r->url);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "http,https");
#else
    SET(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_TIMEOUT_MS, r->timeout_ms);
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_POSTFIELDS, r->json);
    SET(CURLOPT_WRITEFUNCTION, receive_body);
    SET(CURLOPT_WRITEDATA, &t);
    SET(CURLOPT_XFERINFOFUNCTION, progress);
    SET(CURLOPT_XFERINFODATA, (void *)r);
    SET(CURLOPT_NOPROGRESS, 0L);
#undef SET
    CURLcode code = curl_easy_perform(t.easy);
    curl_easy_getinfo(t.easy, CURLINFO_RESPONSE_CODE, status);
    if (code == CURLE_OK) result = DUNLIN_HTTP_OK;
    else if (code == CURLE_OPERATION_TIMEDOUT) result = DUNLIN_HTTP_EXPIRED;
    else if (code == CURLE_ABORTED_BY_CALLBACK) result = DUNLIN_HTTP_ABORTED;
done:
    curl_easy_cleanup(t.easy);
    curl_slist_free_all(headers);
    free(auth);
    return result;
}
const dunlin_http_v1 dunlin_curl_http = { curl_post, NULL };
