#ifndef DUNLIN_LIVE_STREAM_H
#define DUNLIN_LIVE_STREAM_H
#include <stddef.h>
#include <json-c/json.h>
typedef struct dl_live_stream dl_live_stream;
/* Process-local, visible-text-only derived snapshots. Never a journal or an
 * executor. Each store owns one reference; readers never acquire host/DB locks.
 * At most 32 simultaneous chats, 64 KiB per model attempt. Overflow omits the
 * entire live preview, never a misleading truncated tail. Settlement is still
 * read from canonical history. Caller retains its reference through a call. */
dl_live_stream *dl_live_stream_open(void);
dl_live_stream *dl_live_stream_retain(dl_live_stream *);
void dl_live_stream_close(dl_live_stream *);
void dl_live_stream_begin(dl_live_stream *,const char *chat,const char *run,long long through);
void dl_live_stream_append(dl_live_stream *,const char *chat,const char *run,const char *,size_t);
void dl_live_stream_end(dl_live_stream *,const char *chat,const char *run);
/* Owned {available,active,text?,attemptId?,time?,omitted?}. No cross-process
 * recovery. UTF-8 split across callback boundaries is hidden until complete. */
struct json_object *dl_live_stream_snapshot(dl_live_stream *,const char *chat);
#endif
