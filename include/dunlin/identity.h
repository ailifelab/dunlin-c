#ifndef DUNLIN_IDENTITY_H
#define DUNLIN_IDENTITY_H
typedef struct dc_identity_catalog dc_identity_catalog;
/* Trusted host profile router. Each identity uses a separate C data domain.
 * Serial host queue; no model-controlled identity selection. This is local OS
 * account profile separation, not authentication between hostile OS users. */
dc_identity_catalog *dc_identity_open(const char *root);
void dc_identity_close(dc_identity_catalog *);
/* JSON op list/create/select/remove/restore. Switch requires expected epoch.
 * Caller opens destination store successfully before committing select, and
 * invalidates old UI/runtime/plugin projections at its safe boundary. */
char *dc_identity_request(dc_identity_catalog *,const char *json);
#endif
