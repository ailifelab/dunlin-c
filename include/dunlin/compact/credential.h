#ifndef DUNLIN_CREDENTIAL_H
#define DUNLIN_CREDENTIAL_H
#include "agent.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DUNLIN_CREDENTIAL_ABI 1u
typedef enum {
    DUNLIN_CREDENTIAL_PROMPT_EACH_TIME=0,
    DUNLIN_CREDENTIAL_CACHE=1,
    DUNLIN_CREDENTIAL_MANUAL=2
} dunlin_credential_mode;
typedef enum {
    DUNLIN_CREDENTIAL_OK=0,
    DUNLIN_CREDENTIAL_DENIED=1,
    DUNLIN_CREDENTIAL_UNAVAILABLE=2,
    DUNLIN_CREDENTIAL_MANUAL_REQUIRED=3,
    DUNLIN_CREDENTIAL_INVALID=4
} dunlin_credential_status;

/* This request deliberately has no secret field. purpose is a stable UI/audit
 * label; executable and argv bind authorization to the exact operation. */
typedef struct {
    const char *purpose;
    const char *executable;
    const char *const *argv;
    size_t argc;
} dunlin_credential_request;
typedef struct {
    dunlin_credential_mode mode;
    unsigned cache_seconds; /* CACHE only: 1..86400 */
} dunlin_credential_policy;
/* The trusted platform broker owns all credential UI and OS cache interaction.
 * It must never return a password or place one in result, events or arguments. */
typedef struct {
    void *user;
    dunlin_credential_status (*authorize)(void *,const dunlin_credential_request *,
        const dunlin_credential_policy *);
    ah_status (*execute)(void *,const dunlin_credential_request *,const ah_hooks *,
        size_t max_output,unsigned timeout_ms,char **result);
    void (*revoke)(void *,const char *purpose);
} dunlin_credential_broker_v1;

const char *dunlin_credential_mode_name(dunlin_credential_mode);
int dunlin_credential_mode_parse(const char *,dunlin_credential_mode *);
dunlin_credential_status dunlin_credential_run(const dunlin_credential_broker_v1 *,
    const dunlin_credential_policy *,const dunlin_credential_request *,const ah_hooks *,
    size_t max_output,unsigned timeout_ms,ah_status *execution_status,char **result);

#ifdef __cplusplus
}
#endif
#endif
