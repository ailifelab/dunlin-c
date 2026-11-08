#ifndef DUNLIN_MEDIA_H
#define DUNLIN_MEDIA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct dl_media dl_media;
typedef enum {DL_MEDIA_NEW,DL_MEDIA_CONNECTING,DL_MEDIA_LIVE,DL_MEDIA_ENDED,DL_MEDIA_FAILED} dl_media_state;
enum {DL_MEDIA_AUDIO_INPUT=1,DL_MEDIA_AUDIO_OUTPUT=2,DL_MEDIA_IMAGE_INPUT=4,
      DL_MEDIA_VIDEO_OUTPUT=8,DL_MEDIA_TRANSCRIPT=16};
typedef enum {DL_MEDIA_OK,DL_MEDIA_INVALID,DL_MEDIA_DENIED,DL_MEDIA_UNSUPPORTED,
             DL_MEDIA_BACKPRESSURE,DL_MEDIA_CLOSED,DL_MEDIA_IO} dl_media_result;
/* Host owns platform capture permissions and identity routing. These consent
 * values describe this call only and never grant operating-system permissions.
 * A call shares audio only after both admission and service negotiation.
 * All calls are serialized by the host. Transport callbacks must not reenter.
 * Payloads are borrowed for the callback; never retained by this module. */
typedef struct {
    unsigned requested,available;
    int microphone_allowed,camera_allowed,upload_allowed;
    unsigned sample_rate;
    size_t max_queued_bytes;
} dl_media_options;
typedef struct {
    void *user;
    int (*connect)(void *,unsigned capabilities,unsigned sample_rate);
    /* 1 accepted, 0 retry later, -1 terminal failure. PCM is signed LE16 mono.
     * Frame payload is an encoded PNG/JPEG agreed with the service adapter. */
    int (*send)(void *,int frame,const void *,size_t,uint64_t timestamp_ms);
    void (*close)(void *);
} dl_media_transport;
dl_media *dl_media_new(const dl_media_options *,const dl_media_transport *);
void dl_media_free(dl_media *);
dl_media_result dl_media_start(dl_media *);
dl_media_result dl_media_negotiated(dl_media *,unsigned capabilities);
dl_media_result dl_media_push(dl_media *,int frame,const void *,size_t,uint64_t timestamp_ms);
dl_media_result dl_media_drain(dl_media *,unsigned packets);
/* Hangup, disconnect and identity changes stop transport and discard queued
 * raw media. Already committed external transcripts are not erased here. */
void dl_media_end(dl_media *,int failed);
dl_media_state dl_media_status(const dl_media *);
size_t dl_media_queued(const dl_media *);
unsigned dl_media_capabilities(const dl_media *);
#ifdef __cplusplus
}
#endif
#endif
