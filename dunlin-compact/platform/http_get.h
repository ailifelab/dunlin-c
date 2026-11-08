#ifndef DUNLIN_HTTP_GET_H
#define DUNLIN_HTTP_GET_H

#include "dunlin/compact/agent.h"
#include <stddef.h>

/* Public HTTPS, GET-only transport used by the read-only tool. */
int ah_https_url_check(const char *url, char *error, size_t size);
ah_status ah_https_get(const char *url, const ah_hooks *hooks, size_t max_output,
                       unsigned timeout_ms, char **result);

#endif
