#ifndef DUNLIN_LINUX_SETTINGS_H
#define DUNLIN_LINUX_SETTINGS_H
#include "client.h"

GtkWidget *copilot_settings_new(LinuxClient *client);
void copilot_settings_refresh(LinuxClient *client);
/* Take no client lock before calling. Both results belong to the caller. */
gchar *copilot_model_config(LinuxClient *client, gchar **credential);
gchar *copilot_settings_default_model(LinuxClient *client);
gchar *copilot_read_local_utf8(const gchar *path,gsize limit);
gchar *copilot_pref_read(LinuxClient *client,const gchar *name);
gboolean copilot_pref_save(LinuxClient *client,const gchar *name,const gchar *value);
gchar *copilot_model_config_for_id(LinuxClient *client,const gchar *model_id,gchar **credential);
void copilot_settings_set_sources(LinuxClient *client,const gchar *chat_id,struct json_object *selection);
void copilot_settings_consume_sources(LinuxClient *client,const gchar *chat_id);
gboolean copilot_settings_apply_theme(LinuxClient *client,const gchar *preference);
void copilot_settings_refresh_themes(LinuxClient *client);
#endif
