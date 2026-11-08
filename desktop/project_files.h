#ifndef DUNLIN_PROJECT_FILES_H
#define DUNLIN_PROJECT_FILES_H
#include "dunlin/compact/agent.h"
#include "dunlin/file_journal.h"
/* Serialized C file tools. Root fd is held for lifetime. No symlink traversal.
 * policy ask/readOnly/noTools; every write/command is still Loop-approved. */
int dc_project_tools_open(ah_tool_service *,const char *root,const char *policy);
/* Checks both held file and command roots; no reopening/dispatch. */
int dc_project_tools_identity(const ah_tool_service *,const char *device,const char *inode);
void dc_project_tools_close(ah_tool_service *);
void dc_project_tools_skills(ah_tool_service *,const char *data_directory,struct json_object *resolved);
void dc_project_tools_journal(ah_tool_service *,const dc_file_journal_v1 *);
/* Host-only explicit restore path, not registered as a model tool. Request
 * path/expected_sha256, text or remove=true; journal is mandatory. */
ah_status dc_project_file_restore(const char *root,struct json_object *,const dc_file_journal_v1 *,char **);
/* 1 readable UTF-8 regular <=1 MiB, 0 safely observed absent, -1 unsafe/unreadable. */
int dc_project_file_digest(const char *root,const char *path,char sha256[65]);
/* Remove only a verified owned deterministic staging prefix; missing is clean.
 * Returns 1 clean, 0 pending/unsafe. Never removes the user's target file. */
int dc_project_file_discard_stage(const char *root,const char *path,const char *operation_id,const char *intended);
struct json_object *dc_project_file_query(const char *root,struct json_object *request);
struct json_object *dc_project_file_query_bound(const char *root,const char *device,const char *inode,struct json_object *request);
#endif
