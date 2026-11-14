#ifndef DUNLIN_LINUX_SURFACE_H
#define DUNLIN_LINUX_SURFACE_H
#include "ui.h"
void copilot_surface_open(LinuxClient *client,UiJson *descriptor,const char *slot,const char *assets);
void copilot_surface_check(LinuxClient *client,UiJson *inventory);
gboolean copilot_surface_close_all(LinuxClient *client);
#endif
