#ifndef DUNLIN_COPILOT_LINUX_CHAT_H
#define DUNLIN_COPILOT_LINUX_CHAT_H

#include "client.h"

GtkWidget *copilot_chat_new(LinuxClient *client);
GtkWidget *copilot_projects_new(LinuxClient *client);
void copilot_chat_refresh(LinuxClient *client);
void copilot_chat_stop(LinuxClient *client);
void copilot_chat_flush(LinuxClient *client);

#endif
