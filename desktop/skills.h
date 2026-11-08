#ifndef DUNLIN_SKILLS_INTERNAL_H
#define DUNLIN_SKILLS_INTERNAL_H
#include "internal.h"
int dc_skills_schema(dc_store *);
struct json_object *dc_skills_request(dc_store *,struct json_object *);
struct json_object *dc_skills_import_verified(dc_store *,const char *,const char *,const char *,const char *);
/* Owned resolved immutable rows, read once before a run. */
int dc_skills_snapshot_allowed(dc_store *,struct json_object *);
struct json_object *dc_skills_resolve(dc_store *,const char *project,const char *chat);
char *dc_skill_resource(const char *data_directory,struct json_object *resolved,
    struct json_object *arguments,size_t cap);
#endif
