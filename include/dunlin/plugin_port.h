#ifndef DUNLIN_PLUGIN_PORT_H
#define DUNLIN_PLUGIN_PORT_H
#include "desktop.h"
typedef struct dc_worker dc_worker;
/* Optional desktop process adapter, not part of the microkernel. One serialized
 * caller per worker; cancellation is a separate callback. Absolute executable,
 * argv[], private home, timeoutMs<=120000; no inherited application environment.
 * This is trusted-full-host execution, not an OS sandbox. */
dc_worker *dc_worker_open(const char *configuration_json,const dc_hooks *,char **error);
/* JSON-RPC methods must be declared by the nonce-bound v1 handshake. Owned
 * {ok,data?,error?,effectState}. Interrupted calls are unknown, never replayed. */
char *dc_worker_call(dc_worker *,const char *method,const char *params_json,const dc_hooks *);
int dc_worker_alive(dc_worker *);
char *dc_worker_capabilities(dc_worker *);
void dc_worker_close(dc_worker *);
#endif
