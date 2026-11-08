#ifndef DUNLIN_ATTACHMENT_STORE_H
#define DUNLIN_ATTACHMENT_STORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sqlite3 sqlite3;
typedef struct dl_attachment_store dl_attachment_store;
typedef enum {
    DL_ATTACHMENT_OK=0, DL_ATTACHMENT_INVALID, DL_ATTACHMENT_LIMIT,
    DL_ATTACHMENT_NOT_FOUND, DL_ATTACHMENT_STALE, DL_ATTACHMENT_STORAGE,
    DL_ATTACHMENT_TRANSACTION, DL_ATTACHMENT_NOMEM
} dl_attachment_status;
typedef enum { DL_ATTACHMENT_PNG=1, DL_ATTACHMENT_TEXT=2 } dl_attachment_format;
typedef enum {
    DL_ATTACHMENT_CHAT=1, DL_ATTACHMENT_CALL=2,
    DL_ATTACHMENT_KNOWLEDGE=3, DL_ATTACHMENT_EXTENSION=4
} dl_attachment_owner_kind;
typedef struct {
    char sha256[65];
    dl_attachment_format format;
    unsigned width,height; /* PNG only; zero for UTF8 text */
    size_t bytes;
} dl_attachment_info;
typedef struct {
    char id[33],name[257],kind[16]; /* kind: file/clipboard/screenshot */
    dl_attachment_info content;
} dl_attachment_draft;
/* Independent C11/SQLite store. No agent, filesystem, UI or language host.
 * Caller authorizes owner kind/ID and serializes writes; these IDs do not
 * establish OS identity or permit an untrusted worker to choose an owner.
 * Connection is borrowed; all returned buffers use dl_attachment_free(). */
dl_attachment_status dl_attachment_inspect(dl_attachment_format,const void *,size_t,dl_attachment_info *);
const char *dl_attachment_mime(dl_attachment_format);
void dl_attachment_free(void *);
/* PNG <=1MiB/1024x1024; UTF8 text <=64KiB, no NUL, exact bytes including CRLF.
 * quota bounds payload bytes, not SQLite pages/WAL/decoder/model memory.
 * open creates schema outside transactions; reader performs no DDL/writes. */
dl_attachment_store *dl_attachments_open(sqlite3 *,uint64_t quota_bytes);
dl_attachment_store *dl_attachments_reader(sqlite3 *);
void dl_attachments_close(dl_attachment_store *);
/* One pending resource per owner. NULL expected_id requires no existing
 * pending resource. Replacement requires its exact ID, and gets a fresh ID.
 * These two writes manage their own immediate transactions, rejecting nesting. */
dl_attachment_status dl_attachments_stage(dl_attachment_store *,dl_attachment_owner_kind,
    const char *owner,const char *expected_id,const char *name,const char *kind,
    dl_attachment_format,const void *,size_t,dl_attachment_draft *);
dl_attachment_status dl_attachments_remove(dl_attachment_store *,dl_attachment_owner_kind,
    const char *owner,const char *expected_id);
dl_attachment_status dl_attachments_draft(dl_attachment_store *,dl_attachment_owner_kind,
    const char *owner,dl_attachment_draft *);
/* Cheap committed metadata/source-hash check; does not decode the payload. */
dl_attachment_status dl_attachments_info(dl_attachment_store *,dl_attachment_owner_kind,
    const char *owner,int64_t sequence,const char *source_hash,dl_attachment_draft *);
/* seq=-1 selects the exact current draft ID; nonnegative seq requires the
 * canonical message/event hash. Full reads verify payload/metadata/SHA again.
 * A zero-byte text returns an allocated buffer with size=0. Failure clears
 * all outputs. No read by a bare digest or another owner's draft ID. */
dl_attachment_status dl_attachments_read(dl_attachment_store *,dl_attachment_owner_kind,
    const char *owner,int64_t sequence,const char *draft_id,const char *source_hash,
    void **owned_bytes,size_t *size,dl_attachment_draft *);
/* Caller MUST hold a write transaction shared with canonical source commit;
 * roll it back on any failure. These functions do not begin/commit/rollback.
 * bind consumes only the exact draft; deletion collects only unreferenced
 * resources, including references held by different owner kinds. */
dl_attachment_status dl_attachments_bind(dl_attachment_store *,dl_attachment_owner_kind,
    const char *owner,const char *draft_id,int64_t sequence,const char *source_hash);
dl_attachment_status dl_attachments_delete_owner(dl_attachment_store *,dl_attachment_owner_kind,
    const char *owner);
dl_attachment_status dl_attachments_usage(dl_attachment_store *,uint64_t *bytes);
#ifdef __cplusplus
}
#endif
#endif
