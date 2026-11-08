#ifndef DUNLIN_FILE_JOURNAL_H
#define DUNLIN_FILE_JOURNAL_H
struct json_object;
/* Borrowed synchronous sink. prepare must durably retain complete before/after
 * images before returning an operation ID. NULL after means intended deletion;
 * an absent before is distinguished by existed=0. Never authorizes execution. */
typedef struct {
    void *user;
    int (*prepare)(void *,const char *operation,const char *path,struct json_object *arguments,
        int existed,const char *before,const char *after,const char *root_device,
        const char *root_inode,char operation_id[65]);
    int (*settle)(void *,const char *operation_id,const char *state,const char *detail);
} dc_file_journal_v1;
#endif
