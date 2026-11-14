#ifndef DUNLIN_GRANT_BROKER_H
#define DUNLIN_GRANT_BROKER_H
#include <stdint.h>
typedef struct sqlite3 sqlite3;
typedef struct dl_grant_broker dl_grant_broker;
typedef enum {DL_GRANT_REVOKED=0,DL_GRANT_NEEDS_USER=1,DL_GRANT_RETRY=2,DL_GRANT_UNKNOWN=3} dl_grant_revoke_result;
/* Adapters own protocol/OS-specific actions. No shell command or secret is
 * accepted from a model here. resource is an opaque host-authorized reference.
 * Synchronous adapter may not reenter this broker. No DB transaction spans it. */
typedef dl_grant_revoke_result (*dl_grant_revoke_fn)(void *,const char *resource);
typedef struct {const char *protocol;void *user;dl_grant_revoke_fn revoke;} dl_grant_adapter;
/* SQLite connection borrowed, host serializes access and binds a principal.
 * recover=1 only on actual writer startup; interrupted revocations become unknown,
 * never silently replayed; active local leases expire conservatively at startup.
 * An observed expiry is durable and cannot revive on wall-clock rollback.
 * now is caller-supplied Unix seconds. */
dl_grant_broker *dl_grant_open(sqlite3 *,const char *principal,int recover);
void dl_grant_close(dl_grant_broker *);
int dl_grant_register(dl_grant_broker *,const dl_grant_adapter *);
int dl_grant_issue(dl_grant_broker *,const char *id,const char *chat,const char *protocol,
 const char *resource,const char *scope_hash,int64_t now,int64_t expires);
int dl_grant_valid(dl_grant_broker *,const char *id,const char *chat,const char *scope_hash,int64_t now);
/* Revocation denies future local use before invoking the adapter. Return 1
 * means a durable result, not necessarily successful remote revocation. */
int dl_grant_revoke(dl_grant_broker *,const char *id,int64_t now);
/* Owned JSON array includes actual state, generation, start/expiry/revoke times.
 * free with dl_grant_free. No resource reference or secret in public records. */
char *dl_grant_list(dl_grant_broker *,const char *chat);
/* Prior attempts remain available after a successful retry. No credentials or request bodies. */
char *dl_grant_attempts(dl_grant_broker *,const char *id);
void dl_grant_free(void *);
#endif
