#ifndef DUNLIN_DESKTOP_INTERNAL_H
#define DUNLIN_DESKTOP_INTERNAL_H
#include "dunlin/desktop.h"
#include "dunlin/history_index.h"
#include "dunlin/trajectory.h"
#include "dunlin/attachment_store.h"
#include <sqlite3.h>
#include <json-c/json.h>
#include <stdint.h>
struct dc_store {
    sqlite3 *db;
    dl_index *index;
    char *directory;
    int lock;
    int index_fault;
    void *extensions;
    char *platform_bundle;
    void *agent_delivery;
    dl_trajectory *trajectory;
    dl_attachment_store *attachments;
    char file_cleanup_cursor[65];
};
int dc_workspace_schema(dc_store *);
char *dc_messages_through(dc_store *,const char *,int64_t through);
/* Owned project snapshot for a chat; NULL means no project. */
struct json_object *dc_chat_project(dc_store *,const char *);
#endif
