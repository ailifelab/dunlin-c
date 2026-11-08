#ifndef DUNLIN_IMAGE_RESOURCE_H
#define DUNLIN_IMAGE_RESOURCE_H
#include <stddef.h>
#include <stdint.h>
/* Independent C11 image resources. No agent, model, UI or language host.
 * SQLite connections are borrowed; callers serialize writes. Free returned
 * buffers with dl_image_free(). The store never reads a path or runs code. */
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sqlite3 sqlite3;
typedef struct dl_image_store dl_image_store;
typedef enum {
    DL_IMAGE_OK = 0, DL_IMAGE_INVALID, DL_IMAGE_LIMIT, DL_IMAGE_NOT_FOUND,
    DL_IMAGE_STALE, DL_IMAGE_STORAGE, DL_IMAGE_TRANSACTION, DL_IMAGE_NOMEM
} dl_image_status;
enum { DL_IMAGE_MAX_BYTES = 1048576, DL_IMAGE_MAX_DIMENSION = 1024 };
typedef struct {
    char sha256[65];
    unsigned width, height;
    size_t bytes;
} dl_image_info;
typedef struct {
    char id[33];                 /* fresh random ID even for identical bytes */
    char name[257];              /* display name only, never a path */
    char kind[16];               /* file, clipboard, screenshot */
    dl_image_info image;
} dl_image_draft;
/* Checks the PNG signature, chunks/CRC, dimensions, critical ordering, zlib
 * stream and scanline/filter layout (including Adam7). Only a single static
 * PNG; APNG is rejected. No color conversion/rendering or metadata execution.
 * Outputs are cleared on failure. No mutation of caller bytes. */
dl_image_status dl_png_inspect(const void *, size_t, dl_image_info *);
dl_image_status dl_png_data_url(const void *, size_t, char **owned_url);
void dl_image_free(void *);
/* Must open outside a transaction. quota_bytes bounds stored PNG payloads,
 * not SQLite pages, WAL, native decoders or model memory. Positive, <=INT64_MAX.
 * Schema failure returns NULL; does not take ownership of db. */
dl_image_store *dl_images_open(sqlite3 *db, uint64_t quota_bytes);
void dl_images_close(dl_image_store *);
/* Owner IDs are opaque host-authorized ASCII [A-Za-z0-9_-], 1..64 bytes.
 * Operations below do not resolve an OS user or authorize identity sharing.
 * stage/remove manage their own transactions and reject a nested transaction.
 * expected_draft_id=NULL requires no existing draft; a replacement requires
 * its exact prior ID. Rejected replacement leaves the old durable draft intact. */
dl_image_status dl_images_stage(dl_image_store *, const char *owner,
    const char *expected_draft_id, const char *name, const char *kind,
    const void *, size_t, dl_image_draft *);
dl_image_status dl_images_draft(dl_image_store *, const char *owner, dl_image_draft *);
dl_image_status dl_images_remove_draft(dl_image_store *, const char *owner,
    const char *expected_draft_id);
/* Reads only the specified owner's exact current draft or committed sequence.
 * Nonnegative sequence selects a committed record and requires the current
 * canonical message source_hash; -1 requires exact draft_id and ignores hash.
 * Metadata and bytes are checked against the immutable stored SHA-256.
 * On failure *owned_bytes=NULL and *size=0. */
dl_image_status dl_images_read(dl_image_store *, const char *owner,
    int64_t sequence, const char *draft_id, const char *source_hash,
    void **owned_bytes, size_t *size,
    dl_image_draft *);
/* MUST be inside the caller's write transaction alongside canonical-source
 * commit. No BEGIN/COMMIT/ROLLBACK here. A stale/duplicate bind changes nothing;
 * success consumes only expected_draft_id. Caller must roll back on failure.
 * source_hash is the SHA-256 of the canonical message representation. */
dl_image_status dl_images_bind(dl_image_store *, const char *owner,
    const char *expected_draft_id, int64_t sequence, const char *source_hash);
/* Same transaction contract as bind. Deletes only this owner's associations,
 * then collects blobs no other draft/committed record references. */
dl_image_status dl_images_delete_owner(dl_image_store *, const char *owner);
/* Read-only payload usage. Returns STORAGE for malformed SQL/blob accounting. */
dl_image_status dl_images_usage(dl_image_store *, uint64_t *bytes);
#ifdef __cplusplus
}
#endif
#endif
