#ifndef DUNLIN_WINDOWS_PROCESS_H
#define DUNLIN_WINDOWS_PROCESS_H

#include <stddef.h>

enum { DWP_OK, DWP_EXITED, DWP_TIMEOUT, DWP_OUTPUT_LIMIT, DWP_CANCELLED, DWP_FAILED };

typedef struct {
    int status;
    unsigned long exit_code;
    char *output;
    size_t output_size;
} dwp_result;

int dwp_run(const char *executable, const char *const *argv, size_t argc,
            const char *workspace, unsigned timeout_ms, size_t output_limit,
            dwp_result *result);
int dwp_run_ex(const char *executable, const char *const *argv, size_t argc,
               const char *workspace, unsigned timeout_ms, size_t output_limit,
               int (*cancelled)(void *), void *cancel_user, dwp_result *result);
int dwp_run_shell(const char *cmd_executable, const char *command,
                  const char *workspace, unsigned timeout_ms, size_t output_limit,
                  int (*cancelled)(void *), void *cancel_user, dwp_result *result);
void dwp_result_free(dwp_result *result);

#endif
