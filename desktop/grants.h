#ifndef DUNLIN_GRANTS_INTERNAL_H
#define DUNLIN_GRANTS_INTERNAL_H
#include "internal.h"
int dc_grants_schema(dc_store *);
long long dc_grant_begin(dc_store *,const char *chat,const char *tool,const char *args);
int dc_grant_decide(dc_store *,long long id,int allow,unsigned long long elapsed);
int dc_grant_end(dc_store *,long long id);
void dc_grants_finish(dc_store *,const char *chat);
struct json_object *dc_grants_request(dc_store *,struct json_object *);
#endif
