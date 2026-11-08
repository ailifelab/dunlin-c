#include "random.h"
#include <limits.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

int ah_os_random(void *buffer, size_t size) {
    if (!buffer || size > ULONG_MAX) return 0;
    return BCryptGenRandom(NULL, (PUCHAR)buffer, (ULONG)size,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int ah_os_random(void *buffer, size_t size) {
    if (!buffer) return 0;
    unsigned char *out = buffer;
    size_t offset = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    while (offset < size) {
        ssize_t got = read(fd, out + offset, size - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        offset += (size_t)got;
    }
    int ok = offset == size;
    close(fd);
    return ok;
}
#endif
