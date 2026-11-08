#ifndef DUNLIN_AGENT_MAILBOX_H
#define DUNLIN_AGENT_MAILBOX_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sqlite3 sqlite3;
typedef struct dl_agent_mailbox dl_agent_mailbox;
typedef enum {
    DL_MAIL_OK=0, DL_MAIL_INVALID, DL_MAIL_DENIED, DL_MAIL_LIMIT,
    DL_MAIL_STALE, DL_MAIL_NOT_FOUND, DL_MAIL_STORAGE, DL_MAIL_TRANSACTION,
    DL_MAIL_NOMEM
} dl_mail_status;
typedef struct {
    int64_t sequence;
    char project[129],sender[129],receiver[129],message_id[129],reply_to[129];
    char payload_sha256[65];
    char envelope_sha256[65];
    char *text;
    size_t bytes;
} dl_agent_message;
/* Independent C11/SQLite inbox. No agent executor, UI, filesystem, thread,
 * network, Node or Python. No incoming message automatically starts a turn.
 * The host authenticates the principal/sender/receiver and authorizes explicit
 * project membership; model-supplied identifiers are not authorization.
 * Connection is borrowed. Caller serializes each connection's operations.
 * IDs are nonempty ASCII [A-Za-z0-9_.-], at most128 bytes.
 * open creates v2 schema outside transactions; reader never writes or does DDL.
 * principal is permanently bound to this mailbox database namespace.
 * max_payload1…1MiB, max_pending1…4096 per project/receiver, count not disk quota.
 */
dl_agent_mailbox *dl_agent_mailbox_open(sqlite3 *,const char *principal,
    size_t max_payload,unsigned max_pending);
dl_agent_mailbox *dl_agent_mailbox_reader(sqlite3 *,const char *principal,
    size_t max_payload,unsigned max_pending);
void dl_agent_mailbox_close(dl_agent_mailbox *);
/* Explicit membership gates new sends and pending reads. Disabling does not
 * erase earlier shared messages or acknowledge them. These mutations and send
 * manage their own immediate transaction and reject nested transactions. */
dl_mail_status dl_agent_mailbox_member(dl_agent_mailbox *,const char *project,
    const char *agent,int enabled);
dl_mail_status dl_agent_mailbox_send(dl_agent_mailbox *,const char *project,
    const char *sender,const char *receiver,const char *message_id,
    const char *reply_to,const char *text,size_t bytes,int64_t *sequence);
/* Exact UTF8 bytes, nonempty/no NUL. A sender message ID is unique within a
 * project; retry of the exact envelope is idempotent, a different envelope or
 * body is stale. No reading by a bare message ID or a different receiver.
 * Pending reads are ordered/paged by immutable sequence. Returned text remains
 * low-trust shared content and cannot change system/tool permissions.
 * limit1…200; all outputs are cleared on failure. Reset cursor after restart
 * to reconsider any older unacknowledged messages. */
dl_mail_status dl_agent_mailbox_pending(dl_agent_mailbox *,const char *project,
    const char *receiver,int64_t after,unsigned limit,dl_agent_message **messages,
    size_t *count,int *more,int64_t *cursor);
void dl_agent_messages_free(dl_agent_message *,size_t count);
/* Caller MUST already hold a WRITE transaction shared with canonical source
 * append. Host verifies the exact committed canonical sequence/hash; this
 * independent module does not interpret its host's transcript schema.
 * Roll back the entire transaction on any failure. This function never begins,
 * commits or rolls back, so source append and ack are indivisible. Repeating the
 * exact acknowledgement is idempotent; another source receipt is stale.
 */
dl_mail_status dl_agent_mailbox_ack(dl_agent_mailbox *,const char *project,
    const char *receiver,int64_t message_sequence,int64_t canonical_sequence,
    const char *canonical_sha256);
#ifdef __cplusplus
}
#endif
#endif
