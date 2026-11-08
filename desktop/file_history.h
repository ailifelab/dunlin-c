#ifndef DUNLIN_DESKTOP_FILE_HISTORY_H
#define DUNLIN_DESKTOP_FILE_HISTORY_H
#include "internal.h"
#include "dunlin/file_journal.h"
typedef struct {
    dc_store *store;const char *chat,*run;
    char call[257];long long proposal,grant;
    const char *parent,*plan;
    const char *authorized_arguments;
    char last_operation_id[65];
} dc_file_history_run;
int dc_file_history_schema(dc_store *);
dc_file_journal_v1 dc_file_history_journal(dc_file_history_run *);
int dc_file_history_drop_chat(dc_store *,const char *);
int dc_file_history_cleanup(dc_store *,unsigned limit);
struct json_object *dc_file_history_request(dc_store *,struct json_object *);
#endif
