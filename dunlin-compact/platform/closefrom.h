#ifndef DUNLIN_CLOSEFROM_H
#define DUNLIN_CLOSEFROM_H
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
static void close_from(int first) {
#if defined(__linux__) && defined(__aarch64__) && !defined(SYS_close_range)
    /* Linux arm64 syscall, also usable with the Bionic userspace headers. */
    #define SYS_close_range 436
#endif
#ifdef SYS_close_range
    if (syscall(SYS_close_range,(unsigned)first,~0u,0u)==0) return;
#endif
    long top=sysconf(_SC_OPEN_MAX);
    for(long i=first;i<top;i++) close((int)i);
}
#endif
