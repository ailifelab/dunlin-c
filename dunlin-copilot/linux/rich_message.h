#ifndef DUNLIN_LINUX_RICH_MESSAGE_H
#define DUNLIN_LINUX_RICH_MESSAGE_H

#include <gtk/gtk.h>

/* Presentation only. History, copy-source and search retain the original message. */
GtkWidget *copilot_rich_message_new(const gchar *source);
void copilot_chat_style(GtkWidget *root);

#endif
