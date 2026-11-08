#ifndef DUNLIN_CONTENT_HASH_H
#define DUNLIN_CONTENT_HASH_H
#include <stddef.h>
/* SHA-256 of arbitrary bytes; out requires 65 bytes. No allocation. */
void dc_sha256(const void *,size_t,char out[65]);
#endif
