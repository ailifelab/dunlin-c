/* Linux benchmark driver. Small C parent avoids Python fork RSS contamination. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static double milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1000.0+t.tv_nsec*1e-6;
}
int main(int argc,char **argv) {
    if(argc<3) return 1;
    int samples=atoi(argv[1]);
    posix_spawn_file_actions_t io;
    if(samples<2 || samples>1000 || posix_spawn_file_actions_init(&io) ||
        posix_spawn_file_actions_addopen(&io,1,"/dev/null",O_WRONLY,0)) return 1;
    for(int i=-1;i<samples;i++) {
        double start=milliseconds();
        int status;
        pid_t pid;
        struct rusage usage;
        if(posix_spawn(&pid,argv[2],&io,NULL,argv+2,environ) ||
           wait4(pid,&status,0,&usage)!=pid || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
        if(i>=0) printf("%.3f %ld\n",milliseconds()-start,usage.ru_maxrss);
    }
    posix_spawn_file_actions_destroy(&io);
    return 0;
}
