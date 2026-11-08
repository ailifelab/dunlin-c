#ifndef DUNLIN_PROFILE_HASH_H
#define DUNLIN_PROFILE_HASH_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void dunlin_profile_sha256(const void *,size_t,char out[65]);
int dunlin_profile_session_id(const char *conversation,const char *alias,const char *protocol,
    const char *model,const char *endpoint,char out[65]);
#ifdef __cplusplus
}
#endif
#endif
