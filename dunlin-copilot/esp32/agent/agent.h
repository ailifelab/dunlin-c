#ifndef AE_AGENT_H
#define AE_AGENT_H
#include <stddef.h>
/* Device host adapter, independent of kernel and desktop tools. Credentials belong
 * to the transport, never to model messages or evidence. No arbitrary commands. */
#define AE_MEMORY_TURNS 6u
typedef struct {char user[384],assistant[1024];} ae_memory_turn;
typedef struct {unsigned count;ae_memory_turn turns[AE_MEMORY_TURNS];} ae_agent_memory;
void ae_agent_memory_clear(ae_agent_memory *);
typedef struct {const char *mode;unsigned red,green,blue,period_ms;} ae_led_action;
typedef struct {
 void *user;
 const char *device_json;
 int disclose_identity;
 int (*exchange)(void *,const char *,char *,size_t);
 int (*led)(void *,const ae_led_action *,char *,size_t);
 void (*print)(void *,const char *);
 int (*python_exec)(void *,const char *,char *,size_t);
 const char *previous_prompt; /* Optional borrowed prior user turn, context only. */
 ae_agent_memory *memory; /* Optional bounded RAM conversation, no tool replay. */
 int (*current_time)(void *,char *,size_t); /* Fresh network observation or explicit failure. */
} ae_agent;
enum { AE_AGENT_OBSERVED=0, AE_AGENT_STOPPED=1, AE_AGENT_REPLIED=2, AE_AGENT_TRUNCATED=3 };
/* OBSERVED follows successful tool observations, not independent task proof.
 * REPLIED is text with no tool attempted; it never certifies device effects.
 * STOPPED reports a bounded failure. This is the private device adapter API. */
int ae_agent_turn(const ae_agent *,const char *model,const char *prompt);
#endif
