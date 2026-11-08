#include "clock.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t ah_os_now_ms(void *unused) {
    (void)unused;
    return (uint64_t)GetTickCount64();
}
#else
#include <time.h>

uint64_t ah_os_now_ms(void *unused) {
    (void)unused;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)ts.tv_nsec / UINT64_C(1000000);
}
#endif

/* Compatibility symbol retained for existing hosts. */
uint64_t ah_linux_now_ms(void *unused) {
    return ah_os_now_ms(unused);
}
