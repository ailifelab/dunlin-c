#ifndef DUNLIN_REALTIME_H
#define DUNLIN_REALTIME_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct dl_realtime dl_realtime;
typedef struct {
 void *user;
 int (*cancelled)(void *);
 /* Borrowed event payload: ready/error/interrupted/final/gap are UTF8 JSON;
  * audio is PCM16 LE mono 24kHz. Callbacks cannot reenter the adapter. */
 void (*event)(void *,const char *kind,const void *payload,size_t bytes);
} dl_realtime_hooks;
/* settings: {url,model,voice,instructions?,transcriptionModel?,images?,local?}.
 * Explicit OpenAI Realtime event protocol, separate from GPT-Live.
 * No camera capture, recording, tool approval or UI inside this C adapter. */
dl_realtime *dl_realtime_open(const char *settings,const char *secret,const dl_realtime_hooks *);
int dl_realtime_poll(dl_realtime *);
int dl_realtime_audio(dl_realtime *,const void *pcm,size_t bytes);
int dl_realtime_image(dl_realtime *,const void *jpeg,size_t bytes);
/* Host reports actually played duration when interrupting an output item. */
int dl_realtime_interrupt(dl_realtime *,const char *item,unsigned played_ms);
int dl_realtime_ready(const dl_realtime *);
void dl_realtime_close(dl_realtime *);
#ifdef __cplusplus
}
#endif
#endif
