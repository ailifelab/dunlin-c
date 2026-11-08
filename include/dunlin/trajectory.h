#ifndef DUNLIN_TRAJECTORY_H
#define DUNLIN_TRAJECTORY_H
#include <stddef.h>
#include <json-c/json.h>
typedef struct dl_trajectory dl_trajectory;
typedef struct dl_trace_span dl_trace_span;
/* Serialized local derived ledger. Does not execute tools, decide permissions,
 * resume runs, mutate source history or send telemetry. Failure is optional to
 * the host: a missing trace is never evidence of an absent external effect. */
dl_trajectory *dl_trajectory_open(const char *private_directory);
/* WAL snapshot reader; never initializes schema or marks writer runs interrupted. */
dl_trajectory *dl_trajectory_open_reader(const char *private_directory);
/* Separate writer connection retaining the same exclusive process lease. */
dl_trajectory *dl_trajectory_open_worker(dl_trajectory *owner);
void dl_trajectory_close(dl_trajectory *);
/* secret is borrowed through finish and redacted before serialization. */
dl_trace_span *dl_trajectory_begin(dl_trajectory *,const char *chat,const char *run,
    const char *model,const char *protocol,long long source_start,const char *secret);
void dl_trajectory_event(dl_trace_span *,const char *type,const char *data,size_t);
void dl_trajectory_finish(dl_trace_span *,const char *status,long long source_end);
/* Owned {ok,error?,data?}; ops trace.status/config/segments/events/export/clear.
 * Host MUST validate current identity and chat existence before querying data. */
struct json_object *dl_trajectory_request(dl_trajectory *,struct json_object *);
int dl_trajectory_forget(dl_trajectory *,const char *chat);
#endif
