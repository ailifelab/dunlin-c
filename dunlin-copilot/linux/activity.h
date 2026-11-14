#ifndef DUNLIN_COPILOT_ACTIVITY_H
#define DUNLIN_COPILOT_ACTIVITY_H
#include "client.h"
GtkWidget *copilot_activity_new(LinuxClient *client);
void copilot_activity_dispatch_plugins(LinuxClient *);
void copilot_activity_refresh(LinuxClient *client);
void copilot_activity_stop(LinuxClient *client);
gboolean copilot_activity_stop_chat(LinuxClient *client, const char *chat);
#endif
