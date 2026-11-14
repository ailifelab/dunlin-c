#include "ui.h"
#include "dunlin/knowledge.h"

UiJson *ui_get(UiJson *o,const char *k) {UiJson *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
const char *ui_text(UiJson *o,const char *k) {UiJson *v=ui_get(o,k);return v?json_object_get_string(v):"";}
void ui_string(UiJson *o,const char *k,const char *s) {json_object_object_add(o,k,json_object_new_string(s?s:""));}
UiJson *ui_op(const char *op) {UiJson *r=json_object_new_object();ui_string(r,"op",op);return r;}

UiJson *ui_call(LinuxClient *c,UiRoute route,UiJson *r) {
    const char *wire=json_object_to_json_string_ext(r,JSON_C_TO_STRING_PLAIN);
    char *raw=NULL;
    if(route==UI_READ)raw=dc_read_view(c->data_dir,wire);
    else if(!c->busy && g_mutex_trylock(&c->lock)) {
        switch(route){
        case UI_EXTENSIONS:raw=dc_extensions_call(c->store,wire,NULL);break;
        case UI_CATALOG:raw=dc_catalog_call(c->store,wire,NULL);break;
        case UI_KNOWLEDGE:raw=dc_knowledge_call(c->store,wire);break;
        case UI_WORKSPACE:raw=dc_workspace_request(c->store,wire);break;
        default:break;
        }
        g_mutex_unlock(&c->lock);
    } else {copilot_status(c,"当前任务运行中，请在结束后重试。");json_object_put(r);return NULL;}
    UiJson *reply=raw?json_tokener_parse(raw):NULL;dc_free(raw);json_object_put(r);
    UiJson *data=NULL;
    if(json_object_get_boolean(ui_get(reply,"ok"))){
        UiJson *value=ui_get(reply,"data");
        data=value?json_object_get(value):json_object_new_object();
    }
    else copilot_status(c,reply?ui_text(reply,"error"):"本地服务不可读。");
    json_object_put(reply);return data;
}
GtkWidget *ui_label(const char *s) {
    GtkWidget *l=gtk_label_new(s);gtk_label_set_xalign(GTK_LABEL(l),0);
    gtk_label_set_line_wrap(GTK_LABEL(l),TRUE);gtk_label_set_line_wrap_mode(GTK_LABEL(l),PANGO_WRAP_WORD_CHAR);return l;
}
GtkWidget *ui_page(const char *heading,GtkWidget **body) {
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    GtkWidget *page=gtk_box_new(GTK_ORIENTATION_VERTICAL,12);gtk_container_set_border_width(GTK_CONTAINER(page),20);
    GtkWidget *title=ui_label(heading);gtk_style_context_add_class(gtk_widget_get_style_context(title),"page-title");
    gtk_box_pack_start(GTK_BOX(page),title,FALSE,FALSE,0);
    *body=gtk_box_new(GTK_ORIENTATION_VERTICAL,9);gtk_box_pack_start(GTK_BOX(page),*body,FALSE,FALSE,0);
    gtk_container_add(GTK_CONTAINER(scroll),page);return scroll;
}
GtkWidget *ui_row(GtkWidget *box,const char *title,const char *detail) {
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10),*text=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);
    gtk_box_pack_start(GTK_BOX(text),ui_label(title),FALSE,FALSE,0);
    if(detail && *detail){GtkWidget *l=ui_label(detail);gtk_style_context_add_class(gtk_widget_get_style_context(l),"dim-label");gtk_box_pack_start(GTK_BOX(text),l,FALSE,FALSE,0);}
    gtk_box_pack_start(GTK_BOX(row),text,TRUE,TRUE,0);gtk_box_pack_start(GTK_BOX(box),row,FALSE,FALSE,3);return row;
}
GtkWidget *ui_button(GtkWidget *box,const char *name,GCallback cb,gpointer data) {
    GtkWidget *b=gtk_button_new_with_label(name);
    gtk_box_pack_start(GTK_BOX(box),b,FALSE,FALSE,0);g_signal_connect(b,"clicked",cb,data);return b;
}
void ui_json_release(gpointer value){json_object_put(value);}
void ui_clear(GtkWidget *box) {GList *children=gtk_container_get_children(GTK_CONTAINER(box));g_list_free_full(children,(GDestroyNotify)gtk_widget_destroy);}
gboolean ui_confirm(LinuxClient *c,const char *title,const char *detail,const char *action) {
    GtkWidget *d=gtk_message_dialog_new(c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_QUESTION,GTK_BUTTONS_NONE,"%s",title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),"%s",detail?detail:"");gtk_dialog_add_buttons(GTK_DIALOG(d),"取消",GTK_RESPONSE_CANCEL,action,GTK_RESPONSE_ACCEPT,NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d),GTK_RESPONSE_CANCEL);int answer=gtk_dialog_run(GTK_DIALOG(d));gtk_widget_destroy(d);return answer==GTK_RESPONSE_ACCEPT;
}
gchar *ui_prompt(LinuxClient *c,const char *title,const char *initial,gboolean secret) {
    GtkWidget *d=gtk_dialog_new_with_buttons(title,c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"取消",GTK_RESPONSE_CANCEL,"确定",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *entry=gtk_entry_new();gtk_entry_set_text(GTK_ENTRY(entry),initial?initial:"");gtk_entry_set_visibility(GTK_ENTRY(entry),!secret);
    gtk_entry_set_activates_default(GTK_ENTRY(entry),TRUE);gtk_dialog_set_default_response(GTK_DIALOG(d),GTK_RESPONSE_ACCEPT);
    gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),16);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),entry);gtk_widget_show_all(d);
    char *value=gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT?g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))):NULL;
    gtk_widget_destroy(d);return value;
}
void ui_detail(LinuxClient *c,const char *title,const char *value) {
    GtkWidget *d=gtk_dialog_new_with_buttons(title,c->window,GTK_DIALOG_DESTROY_WITH_PARENT,"关闭",GTK_RESPONSE_CLOSE,NULL);
    gtk_window_set_default_size(GTK_WINDOW(d),640,440);
    GtkWidget *view=gtk_text_view_new(),*scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view),FALSE);gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),GTK_WRAP_WORD_CHAR);gtk_text_view_set_monospace(GTK_TEXT_VIEW(view),TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),value?value:"",-1);
    gtk_container_add(GTK_CONTAINER(scroll),view);gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),scroll,TRUE,TRUE,0);
    g_signal_connect_swapped(d,"response",G_CALLBACK(gtk_widget_destroy),d);gtk_widget_show_all(d);
}
