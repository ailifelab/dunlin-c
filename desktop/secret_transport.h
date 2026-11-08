#ifndef DUNLIN_SECRET_TRANSPORT_H
#define DUNLIN_SECRET_TRANSPORT_H
#include "dunlin/compact/http2.h"
typedef struct {const char *secret;char endpoint[2080];int header;dunlin_http_v1 v1;dunlin_http_v2 v2;} dc_secret_transport;
int dc_secret_transport_init(dc_secret_transport *,const char *endpoint,const char *protocol,const char *mode,const char *secret);
#endif
