#ifndef DUNLIN_MEDIA_RECORDING_H
#define DUNLIN_MEDIA_RECORDING_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional desktop recording codec. No devices, filesystem, credentials or
 * threads; callers own storage and explicit recording consent. PCM tracks are
 * signed little-endian 16-bit mono at 24 kHz; video packets are encoded JPEG. */
typedef enum {DL_RECORD_INPUT=1,DL_RECORD_OUTPUT=2,DL_RECORD_VIDEO=3} dl_record_track;
typedef enum {DL_RECORD_OK=0,DL_RECORD_INVALID=1,DL_RECORD_IO=2,DL_RECORD_LIMIT=3,DL_RECORD_CORRUPT=4,DL_RECORD_TRUNCATED=5,DL_RECORD_FINISHED=6} dl_record_result;
typedef size_t (*dl_record_write_fn)(void *,const void *,size_t);
typedef size_t (*dl_record_read_fn)(void *,void *,size_t);
typedef struct dl_record_writer dl_record_writer;
dl_record_writer *dl_record_open(dl_record_write_fn,void *,uint64_t byte_limit);
dl_record_result dl_record_append(dl_record_writer *,dl_record_track,uint64_t milliseconds,const void *,size_t);
dl_record_result dl_record_finish(dl_record_writer *);
void dl_record_close(dl_record_writer *);
/* Visitor payload is borrowed for the duration of the callback. A stream only
 * succeeds when the explicit end marker and physical EOF are both observed. */
typedef int (*dl_record_visit_fn)(void *,dl_record_track,uint64_t,const void *,size_t);
dl_record_result dl_record_scan(dl_record_read_fn,void *,uint64_t byte_limit,dl_record_visit_fn,void *);
#ifdef __cplusplus
}
#endif
#endif
