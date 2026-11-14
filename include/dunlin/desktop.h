#ifndef DUNLIN_DESKTOP_H
#define DUNLIN_DESKTOP_H
#include <stddef.h>
#include <stdint.h>
typedef struct dc_store dc_store;
/* Callbacks are synchronous, borrowed for a run; do not reenter its writer.
 * chat.commit notifies only a durable append: {chat,run,through,settlesModel}.
 * through is the last canonical sequence (including hidden messages), usable
 * with a separate dc_read_view chat.messages prefix read. Never replay effects. */
typedef struct {void *user;void (*event)(void *,const char *,const char *);int (*approve)(void *,const char *,const char *);int (*cancelled)(void *);} dc_hooks;
/* One serialized host queue per store. The store holds an exclusive process lock.
 * All returned strings are owned; dc_free releases them. No secret in inventory. */
dc_store *dc_open(const char *directory);
/* Create on the owner's serialized queue before dispatch. A worker owns a
 * separate connection and a duplicate of the process lease, without startup
 * recovery. Use dc_agent_execute or dc_helper_execute on its serial queue,
 * then dc_close.
 * Host must freeze configuration/destructive edits until workers have joined.
 * Never use this to access a second identity or run two workers on one store. */
dc_store *dc_open_agent_worker(dc_store *owner);
/* Execute one text-only optional-plugin helper on its own worker connection.
 * revision must be the C pending DTO revision used to resolve the borrowed key.
 * No tools, chat mutation, environment keys or optional-host reentry. */
char *dc_helper_execute(dc_store *,const char *job,long long revision,
    const char *secret,const dc_hooks *);
int dc_helper_cancel(dc_store *,const char *job,long long revision);
void dc_close(dc_store *);
/* Trusted native host binds its own bundle for OS permission reset adapters. */
int dc_configure_platform_grants(dc_store *,const char *bundle_id);
/* Native account connection only. Endpoint, account label and opaque lease id
 * are persisted. The bearer token is borrowed only during explicit revocation. */
int dc_remote_account_record(dc_store *,const char *id,const char *chat,const char *label,
    const char *endpoint,int64_t expires);
char *dc_remote_account_revoke(dc_store *,const char *id,const char *expected_endpoint,const char *token);
void dc_free(void *);
int dc_add_chat(dc_store *,const char *id,const char *title,const char *workspace);
int dc_delete_chat(dc_store *,const char *id);
/* Keep eligible project/global memories in their existing scope when explicitly chosen. */
int dc_delete_chat_preserving_memories(dc_store *,const char *id,int preserve);
int dc_rename_chat(dc_store *,const char *id,const char *title);
char *dc_chats(dc_store *);
char *dc_messages(dc_store *,const char *id);
char *dc_run(dc_store *,const char *id,const char *settings_json,const char *input,const dc_hooks *);
/* Host-resolved model key, borrowed for this synchronous call. NULL/empty is
 * no authentication; does not inspect or mutate process environment. Never
 * stored in settings/checkpoints/run snapshots/events. */
char *dc_run_with_secret(dc_store *,const char *,const char *,const char *,const dc_hooks *,const char *secret);
/* Execute an admitted collaboration task on the serialized desktop owner.
 * Uses the same model loop, source history and approval callbacks as chat.
 * Never resumes/replays interrupted jobs automatically. Settings are host-mapped
 * model settings; borrowed secret is not stored in the task queue. */
char *dc_agent_execute(dc_store *,const char *job,const char *settings,
    const char *secret,const dc_hooks *);
/* Explicit optional attachment send. expected_draft=NULL requires no pending
 * attachment. NULL input resumes without consuming pending resources.
 * input_committed reports the durable user-append boundary on every return,
 * including transport/permission failures after the input was accepted. */
/* Retry a settled unanswered user message, identified by its original run.
 * Revalidates the checkpoint at admission; never reopens pending tool work. */
char *dc_retry_chat_turn(dc_store *,const char *chat,const char *settings,const char *prior_run,
                         const dc_hooks *,const char *secret);
char *dc_run_message_with_secret(dc_store *,const char *,const char *,const char *,
    const dc_hooks *,const char *secret,const char *expected_draft,int *input_committed);
/* Native clipboard/capture bytes enter only this bounded C staging API.
 * format: 1=PNG, 2=UTF8 text. Serialized host queue; owned {ok,data?,error?}.
 * expected_draft follows the independent store's exact replacement CAS. */
/* Optional call keyframe: exact PNG, C admission, quota and durable owner binding. */
/* Original-media capture is opt-in in media.start. Append belongs to one
 * recording worker; open/finish/scan belong to the serialized store owner. */
typedef struct dc_media_capture dc_media_capture;
dc_media_capture *dc_media_recording_open(dc_store *,const char *call,unsigned long long byte_limit);
int dc_media_recording_append(dc_media_capture *,int track,unsigned long long milliseconds,const void *,size_t);
char *dc_media_recording_finish(dc_store *,dc_media_capture *);
typedef int (*dc_media_recording_visitor)(void *,int,unsigned long long,const void *,size_t);
typedef struct dc_media_replay dc_media_replay;
dc_media_replay *dc_media_recording_read_open(dc_store *,const char *call);
int dc_media_recording_scan(dc_media_replay *,dc_media_recording_visitor,void *);
void dc_media_recording_read_close(dc_media_replay *);
char *dc_media_frame_stage(dc_store *,const char *call,const void *,size_t,long long milliseconds);
char *dc_attachment_stage(dc_store *,const char *chat,const char *expected_draft,
    const char *name,const char *origin,int format,const void *bytes,size_t size);
/* Bounded local preview only; no store/owner/data mutation or model call. */
char *dc_attachment_preview(const char *name,const char *origin,int format,
    const void *bytes,size_t size);
/* Host runs this at startup/after committed turns on a low-priority queue.
 * Durable cache deletions retry before text indexing, even when text indexing
 * is paused. Failure retains cleanup intent and never deletes chat source.
 * Returns1 to schedule another bounded batch,0 when drained/paused,-1 on error. */
int dc_index_pending(dc_store *,unsigned limit);
char *dc_search(dc_store *,const char *query);
char *dc_search_filtered(dc_store *,const char *query,const char *project,int64_t since_ms);
/* Versioned C-owned project/chat metadata service. Serialized like dc_run.
 * JSON request {op,...}; owned reply {ok,error?,data?}. Revisions are required
 * on project updates. Project removal never removes workspace files. */
/* Catalog network calls use bounded inputs and a separate cancel callback. */
char *dc_extensions_call(dc_store *,const char *,const dc_hooks *);
char *dc_catalog_call(dc_store *,const char *,const dc_hooks *);
char *dc_workspace_request(dc_store *,const char *request_json);
/* Read-only source/trajectory WAL snapshot, safe alongside a synchronous run.
 * Uses no store pointer and never marks active runs interrupted. */
char *dc_read_trajectory(const char *private_directory,const char *request_json);
/* Read-only live local DTOs: trace status/segments/events/export, receipts,
 * grants, tool records, configuration inventory/mapping and chat.messages.
 * chat.messages accepts an optional nonnegative integer through cursor.
 * All effects/configuration mutations rejected. */
char *dc_read_view(const char *private_directory,const char *request_json);
/* Bounded project-relative UTF-8 preview; traverses no symlinks. No database
 * or runtime dependency. Owned JSON {path,sha256,text}, NULL on unsafe input. */
char *dc_preview_file(const char *root,const char *relative_path);
/* Bounded provider inventory probe; TLS verified, redirects not followed,
 * key is borrowed and never returned. Capability zero means unknown. */
char *dc_probe_model(const char *settings_json,const char *secret,const dc_hooks *);
/* Explicit minimal text generation, separate from inventory, chat and tools.
 * Public provider DTO includes testModel and versioned credential reference;
 * borrowed key is never persisted. Same serialized-store rule as dc_run.
 * Reply {ok,data:{status,...}} distinguishes test outcome from storage failure. */
char *dc_test_connection(dc_store *,const char *provider_json,const char *secret,const dc_hooks *);
#endif
