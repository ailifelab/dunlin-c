#ifndef AE_RESPONSES_H
#define AE_RESPONSES_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
 char previous_id[128];
 char pending_call[81];
 int sent_messages,pending_messages;
 unsigned calls,usage_known;
 uint64_t input_tokens,output_tokens,reasoning_tokens,network_ms;
} ae_responses;
void ae_responses_reset(ae_responses *);
/* Request memory belongs to cJSON_free. No network/credentials/kernel state. */
char *ae_responses_request(ae_responses *,const char *,const char *,unsigned);
int ae_responses_reply(ae_responses *,const char *,char *,size_t);
#endif
