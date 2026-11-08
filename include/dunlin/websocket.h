#ifndef DUNLIN_WEBSOCKET_H
#define DUNLIN_WEBSOCKET_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct dl_websocket dl_websocket;
/* Serialized host worker. TLS certificate/hostname checks are always enabled;
 * no redirects, cookies or ambient credentials. ws is loopback-only and must
 * be explicitly enabled. The key is borrowed during open, never logged. */
dl_websocket *dl_ws_open(const char *url,const char *key,int allow_loopback,
    size_t max_message,void *user,int (*cancelled)(void *));
/* Bounded send; any partial-send error makes the connection terminal. */
int dl_ws_send(dl_websocket *,const char *,size_t);
/* Nonblocking. 1 complete owned text message (free with free), 0 pending,
 * -1 terminal error/close. Fragmented messages retain bounded buffered state. */
int dl_ws_receive(dl_websocket *,char **text,size_t *bytes);
void dl_ws_close(dl_websocket *);
#ifdef __cplusplus
}
#endif
#endif
