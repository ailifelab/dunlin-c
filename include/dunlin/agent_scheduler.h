#ifndef DUNLIN_AGENT_SCHEDULER_H
#define DUNLIN_AGENT_SCHEDULER_H
#include <stddef.h>
#include <stdint.h>
typedef struct sqlite3 sqlite3;
typedef struct dl_agent_scheduler dl_agent_scheduler;
typedef enum { DL_SCHED_OK, DL_SCHED_EMPTY, DL_SCHED_INVALID, DL_SCHED_CONFLICT,
    DL_SCHED_LIMIT, DL_SCHED_STALE, DL_SCHED_STORAGE, DL_SCHED_TRANSACTION } dl_sched_status;
typedef struct {
    char id[65], project[65], chat[65], parent[65], model[65], resource[129];
    char state[24];
    int write_access, depth;
    int64_t generation;
    char *task;
} dl_agent_job;
/* Independent durable queue; no executor, model, tools, credentials or UI.
 * Caller authenticates identifiers and canonical resource identity, serializes
 * each borrowed connection, and supplies approvals separately for every worker.
 * IDs: ASCII alphanumeric, '-' and '_', 1..64 (parent may be empty).
 * Resource: host-derived workspace identity, 1..128 bytes, same alphabet plus ':'.
 * principal and capacity (1..64) are permanently bound to this queue database.
 * Multiple connections may claim concurrently; they share the stored capacity.
 * Open does not recover active workers. recover is ONLY for an exclusive owner
 * after proving all previous workers have stopped. No interrupted task replays.
 */
dl_agent_scheduler *dl_agent_scheduler_open(sqlite3 *,const char *principal,unsigned capacity);
void dl_agent_scheduler_close(dl_agent_scheduler *);
/* Optional host-owned capacity shared with another executor on the same DB.
 * Called inside claim/wake's write transaction. Must only read this borrowed
 * connection, never reenter the scheduler; return >=capacity on read failure.
 * Host installs it on every claiming handle; NULL preserves standalone use. */
void dl_agent_scheduler_external_load(dl_agent_scheduler *,unsigned (*)(void *),void *);
dl_sched_status dl_agent_enqueue(dl_agent_scheduler *,const dl_agent_job *);
/* Claim oldest eligible queued job. Readers may overlap; a writer exclusively
 * owns its resource across projects. One chat may have only one active job.
 * Output is owned; initialize/reset with {0}, then clear after use. */
dl_sched_status dl_agent_claim(dl_agent_scheduler *,dl_agent_job *);
dl_sched_status dl_agent_claim_id(dl_agent_scheduler *,const char *id,dl_agent_job *);
dl_sched_status dl_agent_job_get(dl_agent_scheduler *,const char *id,dl_agent_job *);
void dl_agent_job_clear(dl_agent_job *);
/* Generation-bound completion accepts succeeded/failed. Cancellation is a
 * request while executing; settlement changes it to cancelled, never undoes
 * external effects. Cancel propagates to descendants; running resource leases
 * remain held until settlement or explicit exclusive recovery. */
dl_sched_status dl_agent_finish(dl_agent_scheduler *,const char *id,int64_t generation,const char *outcome);
/* Host may park only at a quiescent boundary: no model/tool operation in flight.
 * The existing worker keeps its continuation; this does NOT enqueue/replay it.
 * Wake reacquires capacity/resource/chat admission for the same generation.
 * A waiting parent can then allow a child to use its workspace without deadlock. */
dl_sched_status dl_agent_park(dl_agent_scheduler *,const char *id,int64_t generation);
dl_sched_status dl_agent_wake(dl_agent_scheduler *,const char *id,int64_t generation);
dl_sched_status dl_agent_cancel(dl_agent_scheduler *,const char *id);
/* Retract an unclaimed input only. The queued-state comparison and family
 * cancellation are atomic; running/waiting/settled roots return STALE without
 * changing that job or its descendants. */
dl_sched_status dl_agent_cancel_queued(dl_agent_scheduler *,const char *id);
dl_sched_status dl_agent_scheduler_recover(dl_agent_scheduler *);
#endif
