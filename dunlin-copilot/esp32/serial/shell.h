#ifndef AE_LOCAL_SHELL_H
#define AE_LOCAL_SHELL_H
#include <dunlin/compact/embedded.h>
#include "../agent/agent.h"
/* Slash commands only. Normalize for internal parsers; natural text unchanged. */
int ae_shell_normalize(const char *,char *,size_t);
/* 0 not help, 1 help printed, -1 invalid help topic printed. */
int ae_shell_help(const char *,ae_write_fn,void *);
/* Streaming safe model display: actual newlines, every logical line prefixed. */
void ae_shell_text(const char *,ae_write_fn,void *);
/* Classify ORIGINAL input before normalization: 0 model text, 1 slash command. */
int ae_shell_kind(const char *);
int ae_shell_led(const char *,ae_led_action *);
#endif
