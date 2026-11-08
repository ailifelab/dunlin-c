#ifndef DUNLIN_PLATFORM_RANDOM_H
#define DUNLIN_PLATFORM_RANDOM_H

#include <stddef.h>

/* Fill the complete buffer from the operating system CSPRNG. Returns one on
 * success and zero without producing a usable partial result on failure. */
int ah_os_random(void *buffer, size_t size);

#endif
