#ifndef DUNLIN_COPILOT_LINUX_CLIENT_H
#define DUNLIN_COPILOT_LINUX_CLIENT_H

#include <gtk/gtk.h>
#include <json-c/json.h>
#include "dunlin/desktop.h"
#include "dunlin/identity.h"

typedef struct {
    dc_store *store;
    dc_identity_catalog *identities;
    GtkApplication *application;
    gchar *root_dir;
    gchar *pending_identity;
    GtkWidget *approval;
    gboolean closing;
    guint transition;
    guint index_timer;
    gint indexing;
    gint service_cancel;
    GtkWindow *window;
    GtkStack *pages;
    GtkWidget *status;
    GtkWidget *permissions;
    GtkWidget *tools;
    GtkWidget *extensions;
    gchar *data_dir;
    gchar *identity_id;
    gchar *chat_id;
    gchar *project_id;
    gchar *model_id;
    gboolean busy;
    GMutex lock;
} LinuxClient;

void copilot_status(LinuxClient *client, const gchar *message);
void copilot_show(LinuxClient *client, const gchar *page);
int copilot_chat_approval(LinuxClient *client, const char *tool, const char *arguments);
GtkWidget *copilot_chat_new(LinuxClient *client);
GtkWidget *copilot_projects_new(LinuxClient *client);
void copilot_chat_refresh(LinuxClient *client);
void copilot_chat_stop(LinuxClient *client);
GtkWidget *copilot_settings_new(LinuxClient *client);
void copilot_settings_refresh(LinuxClient *client);
gchar *copilot_model_config(LinuxClient *client, gchar **credential);

#endif
