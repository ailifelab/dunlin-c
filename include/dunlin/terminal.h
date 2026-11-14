#ifndef DUNLIN_TERMINAL_H
#define DUNLIN_TERMINAL_H
#include <stddef.h>
#include <stdint.h>
/* Desktop host primitive. No model, UI, configuration or plugin dependency. */
typedef struct dl_terminal_pool dl_terminal_pool;
typedef struct {
 char id[65],cwd[4096],shell[4096],error[128];
 int cols,rows,running,exit_code;
 size_t bytes;
} dl_terminal_info;
dl_terminal_pool *dl_terminal_pool_open(void);
void dl_terminal_pool_close(dl_terminal_pool *);
int dl_terminal_open(dl_terminal_pool *,const char *owner,const char *chat,const char *content,const char *cwd,const char *shell,int cols,int rows,dl_terminal_info *);
int dl_terminal_list(dl_terminal_pool *,const char *owner,const char *chat,size_t index,dl_terminal_info *);
int dl_terminal_read(dl_terminal_pool *,const char *owner,const char *chat,const char *id,size_t offset,void *buffer,size_t capacity,size_t *length,dl_terminal_info *);
int dl_terminal_attach(dl_terminal_pool *,const char *owner,const char *chat,const char *id,const char *lease);
int dl_terminal_writable(dl_terminal_pool *,const char *owner,const char *chat,const char *id,const char *lease);
int dl_terminal_chat_busy(dl_terminal_pool *,const char *chat);
int dl_terminal_write(dl_terminal_pool *,const char *owner,const char *chat,const char *id,const char *lease,const void *data,size_t length,size_t *written);
int dl_terminal_resize(dl_terminal_pool *,const char *owner,const char *chat,const char *id,const char *lease,int cols,int rows);
int dl_terminal_close(dl_terminal_pool *,const char *owner,const char *chat,const char *id);
int dl_terminal_close_content(dl_terminal_pool *,const char *owner,const char *chat,const char *content);
int dl_terminal_find_content(dl_terminal_pool *,const char *owner,const char *chat,const char *content,dl_terminal_info *);
int dl_terminal_forget_chat(dl_terminal_pool *,const char *chat);
void dl_terminal_detach(dl_terminal_pool *,const char *owner,const char *chat,const char *id,const char *lease);
void dl_terminal_release_surface(dl_terminal_pool *,const char *owner,const char *lease);
void dl_terminal_close_owner(dl_terminal_pool *,const char *owner_prefix);
#endif
