#ifndef DUNLIN_LINUX_PAGES_H
#define DUNLIN_LINUX_PAGES_H
#include "client.h"
GtkWidget *copilot_data_new(LinuxClient *);
void copilot_data_refresh(LinuxClient *);
GtkWidget *copilot_records_new(LinuxClient *,gboolean permissions);
void copilot_records_refresh(LinuxClient *,gboolean permissions);
GtkWidget *copilot_extensions_new(LinuxClient *);
void copilot_extensions_refresh(LinuxClient *);
void copilot_extensions_start(LinuxClient *);
GtkWidget *copilot_library_new(LinuxClient *);
void copilot_library_refresh(LinuxClient *);
GtkWidget *copilot_activity_new(LinuxClient *);
void copilot_activity_stop(LinuxClient *);
gboolean copilot_activity_stop_chat(LinuxClient *,const char *chat);
void copilot_activity_refresh(LinuxClient *);
#endif
