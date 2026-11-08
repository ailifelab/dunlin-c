#ifndef DUNLIN_CHAT_WORKSPACE_H
#define DUNLIN_CHAT_WORKSPACE_H
#include "internal.h"
/* Desktop C orchestration; caller serializes the store. No UI/model execution. */
int dc_chat_workspace_writer(dc_store *);
int dc_chat_workspaces_schema(dc_store *);
/* First format upgrade only; no transaction may be active. Empty stores
 * return an empty relative path. Does not read a credential service. */
int dc_workspace_backup_before_upgrade(dc_store *,char relative_path[160],uint64_t *bytes);
/* General source-format copy. component is a bounded ASCII identifier. */
int dc_backup_before_upgrade(dc_store *,const char *component,char relative_path[160],uint64_t *bytes);
struct json_object *dc_workspace_migration_view(dc_store *);
/* Capture/move/relocate run inside the caller's source/metadata transaction. */
int dc_chat_workspace_capture(dc_store *,const char *chat,const char *path);
int dc_chat_workspaces_relocate(dc_store *,const char *project,const char *path,
                              const char *device,const char *inode);
struct json_object *dc_chat_workspace_view(dc_store *,const char *chat);
struct json_object *dc_chat_workspace_request(dc_store *,struct json_object *);
/* Existing project-root validation, also used for explicit chat relink targets. */
int dc_workspace_root(dc_store *,const char *,char *,char device[32],char inode[32]);
#endif
