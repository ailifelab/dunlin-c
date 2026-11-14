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
/* Optional reverse admission channel. Only explicitly opted-in desktop hosts
 * receive this private FD. The callback runs on the waiting C caller, must not
 * call the same worker recursively, and returns an owned JSON object (malloc).
 * Requests are nonce/sequence/current-RPC bound. The enclosing call is unknown
 * after interruption; neither transport nor caller may replay a callback. */
typedef char *(*dc_worker_native_fn)(void *user,const char *request_json,const dc_hooks *);
char *dc_worker_call_native(dc_worker *,const char *method,const char *params_json,const dc_hooks *,dc_worker_native_fn,void *user);
int dc_worker_alive(dc_worker *);
char *dc_worker_capabilities(dc_worker *);
void dc_worker_close(dc_worker *);
#endif
