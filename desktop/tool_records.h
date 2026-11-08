#ifndef DUNLIN_TOOL_RECORDS_H
#define DUNLIN_TOOL_RECORDS_H
#include "internal.h"
#include "dunlin/compact/agent.h"
typedef struct {
 dc_store *store;
 const char *chat,*run;
 long long proposal,record;
 char call[257];
 int returned,denied;
 ah_status outcome;
 unsigned long long started_ms,returned_ms;
 const ah_tool_pipeline_v1 *inner;
 const ah_tool_service *tools;
} dc_tool_record_run;
int dc_tool_records_schema(dc_store *);
/* Called inside the source checkpoint transaction, with newly appended range. */
int dc_tool_records_append(dc_store *,const char *,const char *,struct json_object *,size_t,dc_tool_record_run *);
int dc_tool_records_import(dc_store *);
ah_tool_pipeline_v1 dc_tool_records_pipeline(dc_tool_record_run *);
void dc_tool_records_event(dc_tool_record_run *,const char *,const char *,size_t);
int dc_tool_records_approval(dc_tool_record_run *,long long,int,unsigned long long);
void dc_tool_records_finish(dc_tool_record_run *,ah_status);
struct json_object *dc_tool_records_request(dc_store *,struct json_object *);
#endif
