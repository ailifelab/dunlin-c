#ifndef DUNLIN_LINUX_UI_H
#define DUNLIN_LINUX_UI_H
#include "client.h"

typedef struct json_object UiJson;
typedef enum { UI_WORKSPACE, UI_READ, UI_KNOWLEDGE, UI_EXTENSIONS, UI_CATALOG } UiRoute;
UiJson *ui_get(UiJson *object,const char *field);
const char *ui_text(UiJson *object,const char *field);
void ui_string(UiJson *object,const char *field,const char *value);
UiJson *ui_op(const char *operation);
UiJson *ui_call(LinuxClient *client,UiRoute route,UiJson *request);
GtkWidget *ui_label(const char *text);
GtkWidget *ui_page(const char *heading,GtkWidget **body);
GtkWidget *ui_row(GtkWidget *box,const char *title,const char *detail);
GtkWidget *ui_button(GtkWidget *box,const char *caption,GCallback callback,gpointer data);
void ui_json_release(gpointer value);
void ui_clear(GtkWidget *box);
gboolean ui_confirm(LinuxClient *client,const char *title,const char *detail,const char *action);
gchar *ui_prompt(LinuxClient *client,const char *title,const char *initial,gboolean secret);
void ui_detail(LinuxClient *client,const char *title,const char *value);
#endif
