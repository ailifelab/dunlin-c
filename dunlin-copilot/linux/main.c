#include "ui.h"
#include "pages.h"
#include "chat.h"
#include "surface.h"
#include "dunlin/knowledge.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void build_window(LinuxClient *c);
void copilot_status(LinuxClient *c,const gchar *message) {
    if(c->status)gtk_label_set_text(GTK_LABEL(c->status),message?message:"");
}
void copilot_show(LinuxClient *c,const gchar *page) {
    if(!c->pages || !gtk_stack_get_child_by_name(c->pages,page))return;
    gtk_stack_set_visible_child_name(c->pages,page);
    if(!strcmp(page,"tools") || !strcmp(page,"permissions"))copilot_records_refresh(c,!strcmp(page,"permissions"));
    else if(!strcmp(page,"extensions"))copilot_extensions_refresh(c);
    else if(!strcmp(page,"settings") && !c->busy)copilot_settings_refresh(c);
    else if(!strcmp(page,"data"))copilot_data_refresh(c);
    else if(!strcmp(page,"library"))copilot_library_refresh(c);
    else if(!strcmp(page,"activity"))copilot_activity_refresh(c);
}
static void navigate(GtkButton *button,LinuxClient *c) {copilot_show(c,g_object_get_data(G_OBJECT(button),"page"));}
typedef struct {
    LinuxClient *client;
    const char *tool,*arguments;
    GMutex mutex;
    GCond condition;
    gboolean finished,allowed;
} Approval;
static gboolean prompt_approval(gpointer data) {
    Approval *a=data;LinuxClient *c=a->client;gboolean allowed=FALSE;
    if(c->approval && !c->closing && !c->pending_identity){g_timeout_add(100,prompt_approval,a);return G_SOURCE_REMOVE;}
    if(c->window && !c->closing && !c->pending_identity){
        GtkWidget *d=gtk_dialog_new_with_buttons("工具需要授权",c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
            "拒绝",GTK_RESPONSE_REJECT,"仅允许这一次",GTK_RESPONSE_ACCEPT,NULL);
        gtk_window_set_default_size(GTK_WINDOW(d),540,290);gtk_dialog_set_default_response(GTK_DIALOG(d),GTK_RESPONSE_REJECT);
        GtkWidget *body=gtk_dialog_get_content_area(GTK_DIALOG(d));gtk_container_set_border_width(GTK_CONTAINER(body),16);
        gtk_box_pack_start(GTK_BOX(body),ui_label(a->tool),FALSE,FALSE,6);
        GtkWidget *sc=gtk_scrolled_window_new(NULL,NULL),*text=gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(text),FALSE);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text),GTK_WRAP_WORD_CHAR);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)),a->arguments?a->arguments:"",-1);
        gtk_container_add(GTK_CONTAINER(sc),text);gtk_box_pack_start(GTK_BOX(body),sc,TRUE,TRUE,6);
        c->approval=d;gtk_widget_show_all(d);
        allowed=gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT && !c->closing && !c->pending_identity;
        c->approval=NULL;gtk_widget_destroy(d);
    }
    g_mutex_lock(&a->mutex);a->allowed=allowed;a->finished=TRUE;g_cond_signal(&a->condition);g_mutex_unlock(&a->mutex);
    return G_SOURCE_REMOVE;
}
int copilot_chat_approval(LinuxClient *c,const char *tool,const char *arguments) {
    Approval a={.client=c,.tool=tool,.arguments=arguments};g_mutex_init(&a.mutex);g_cond_init(&a.condition);
    g_mutex_lock(&a.mutex);g_idle_add(prompt_approval,&a);
    while(!a.finished)g_cond_wait(&a.condition,&a.mutex);
    g_mutex_unlock(&a.mutex);g_cond_clear(&a.condition);g_mutex_clear(&a.mutex);return a.allowed;
}
static UiJson *identity_call(LinuxClient *c,UiJson *r) {
    char *raw=dc_identity_request(c->identities,json_object_to_json_string(r));
    UiJson *reply=raw?json_tokener_parse(raw):NULL;dc_free(raw);json_object_put(r);
    UiJson *rows=NULL;
    if(json_object_get_boolean(ui_get(reply,"ok")))rows=json_object_get(ui_get(reply,"data"));
    else copilot_status(c,reply?ui_text(reply,"error"):"无法打开身份目录。");
    json_object_put(reply);return rows;
}
static gboolean select_domain(LinuxClient *c,const char *target) {
    UiJson *rows=identity_call(c,ui_op("list")),*row=NULL;gint64 epoch=0;
    for(gsize i=0;rows && i<(gsize)json_object_array_length(rows);i++){
        UiJson *v=json_object_array_get_idx(rows,i);
        gboolean active=json_object_get_boolean(ui_get(v,"selected"));
        if(active)epoch=json_object_get_int64(ui_get(v,"epoch"));
        if(!json_object_get_boolean(ui_get(v,"hidden")) && (target?!strcmp(ui_text(v,"id"),target):active))row=v;
    }
    if(!row){json_object_put(rows);return FALSE;}
    const char *directory=ui_text(row,"directory"),*id=ui_text(row,"id");
    if(c->store && !g_strcmp0(id,c->identity_id)){json_object_put(rows);return TRUE;}
    dc_store *next=dc_open(directory);
    if(!next || !dc_bind_identity(next,id)){if(next)dc_close(next);json_object_put(rows);return FALSE;}
    if(target){
        UiJson *r=ui_op("select");ui_string(r,"id",id);json_object_object_add(r,"epoch",json_object_new_int64(epoch));
        UiJson *confirmed=identity_call(c,r);
        if(!confirmed){dc_close(next);json_object_put(rows);return FALSE;}
        json_object_put(confirmed);
    }
    for(gsize i=0;i<(gsize)json_object_array_length(rows);i++){
        UiJson *v=json_object_array_get_idx(rows,i);
        if(json_object_get_boolean(ui_get(v,"hidden")))continue;
        UiJson *subject=ui_op("memory.subject.add");ui_string(subject,"id",ui_text(v,"id"));
        ui_string(subject,"name",ui_text(v,"name"));ui_string(subject,"kind",ui_text(v,"kind"));
        dc_free(dc_knowledge_call(next,json_object_to_json_string(subject)));json_object_put(subject);
    }
    if(c->store)dc_close(c->store);
    c->store=next;g_free(c->identity_id);c->identity_id=g_strdup(id);g_free(c->data_dir);c->data_dir=g_strdup(directory);
    g_clear_pointer(&c->chat_id,g_free);g_clear_pointer(&c->project_id,g_free);g_clear_pointer(&c->model_id,g_free);
    json_object_put(rows);return TRUE;
}
static gpointer index_worker(gpointer data) {
    LinuxClient *c=data;
    if(g_mutex_trylock(&c->lock)){dc_index_pending(c->store,32);g_mutex_unlock(&c->lock);}
    g_atomic_int_set(&c->indexing,0);return NULL;
}
static gboolean index_tick(gpointer data) {
    LinuxClient *c=data;
    if(!c->busy && !c->closing && !c->pending_identity && g_atomic_int_compare_and_exchange(&c->indexing,0,1))
        g_thread_unref(g_thread_new("history-index",index_worker,c));
    return G_SOURCE_CONTINUE;
}
static gboolean finish_transition(gpointer data) {
    LinuxClient *c=data;if(c->busy || g_atomic_int_get(&c->indexing))return G_SOURCE_CONTINUE;
    if(!copilot_surface_close_all(c))return G_SOURCE_CONTINUE;
    c->transition=0;copilot_chat_flush(c);
    char *raw=dc_extensions_call(c->store,"{\"op\":\"extensions.quiesce\"}",NULL);
    UiJson *quiesced=raw?json_tokener_parse(raw):NULL;dc_free(raw);
    if(!json_object_get_boolean(ui_get(quiesced,"ok"))){
        copilot_status(c,"扩展尚未完成停止，请查看插件日志后重试。");c->closing=FALSE;g_clear_pointer(&c->pending_identity,g_free);json_object_put(quiesced);return G_SOURCE_REMOVE;
    }
    json_object_put(quiesced);
    if(c->closing){gtk_widget_destroy(GTK_WIDGET(c->window));c->window=NULL;c->status=NULL;return G_SOURCE_REMOVE;}
    if(c->pending_identity && select_domain(c,c->pending_identity)){
        GtkWindow *old=c->window;g_application_hold(G_APPLICATION(c->application));
        gtk_widget_destroy(GTK_WIDGET(old));c->window=NULL;c->pages=NULL;c->status=NULL;
        build_window(c);g_application_release(G_APPLICATION(c->application));
    }else copilot_status(c,"身份切换失败，当前身份仍可继续使用。");
    g_clear_pointer(&c->pending_identity,g_free);return G_SOURCE_REMOVE;
}
static void transition(LinuxClient *c) {
    g_atomic_int_set(&c->service_cancel,1);
    if(c->approval)gtk_dialog_response(GTK_DIALOG(c->approval),GTK_RESPONSE_REJECT);
    if(c->busy){copilot_chat_stop(c);copilot_activity_stop(c);}
    if(!c->transition)c->transition=g_timeout_add(75,finish_transition,c);
}
static gboolean close_window(GtkWidget *w,GdkEvent *e,LinuxClient *c) {
    (void)w;(void)e;
    if(c->closing)return TRUE;
    if(c->busy && !ui_confirm(c,"停止当前任务并关闭？","已写入的对话与工具记录会保留。","停止并关闭"))return TRUE;
    c->closing=TRUE;transition(c);return TRUE;
}
static char *new_identity(LinuxClient *c,UiJson *rows,gboolean family){
    GtkWidget *d=gtk_dialog_new_with_buttons(family?"创建家庭共有身份":"创建个人身份",c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消",GTK_RESPONSE_CANCEL,"创建",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *body=gtk_dialog_get_content_area(GTK_DIALOG(d)),*name=gtk_entry_new();gtk_container_set_border_width(GTK_CONTAINER(body),16);
    gtk_box_pack_start(GTK_BOX(body),ui_label("身份名称"),FALSE,FALSE,4);gtk_box_pack_start(GTK_BOX(body),name,FALSE,FALSE,4);
    GPtrArray *members=g_ptr_array_new();
    if(family){
        gtk_box_pack_start(GTK_BOX(body),ui_label("家庭成员"),FALSE,FALSE,4);
        for(gsize i=0;i<(gsize)json_object_array_length(rows);i++){
            UiJson *v=json_object_array_get_idx(rows,i);
            if(json_object_get_boolean(ui_get(v,"hidden"))||strcmp(ui_text(v,"kind"),"person"))continue;
            GtkWidget *selected=gtk_check_button_new_with_label(ui_text(v,"name"));
            g_object_set_data(G_OBJECT(selected),"person",(gpointer)ui_text(v,"id"));g_ptr_array_add(members,selected);
            gtk_box_pack_start(GTK_BOX(body),selected,FALSE,FALSE,0);
        }
    }
    gtk_widget_show_all(d);char *id=NULL;
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT){
        id=g_uuid_string_random();UiJson *r=ui_op("create"),*list=json_object_new_array();
        ui_string(r,"id",id);ui_string(r,"name",gtk_entry_get_text(GTK_ENTRY(name)));ui_string(r,"kind",family?"shared":"person");
        for(guint i=0;i<members->len;i++){GtkWidget *toggle=g_ptr_array_index(members,i);if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)))json_object_array_add(list,json_object_new_string(g_object_get_data(G_OBJECT(toggle),"person")));}
        json_object_object_add(r,"members",list);UiJson *reply=identity_call(c,r);if(!reply)g_clear_pointer(&id,g_free);json_object_put(reply);
    }
    gtk_widget_destroy(d);g_ptr_array_unref(members);return id;
}
static void choose_identity(GtkButton *sender,LinuxClient *c) {
    (void)sender;if(c->closing || c->pending_identity)return;
    UiJson *rows=identity_call(c,ui_op("list"));if(!rows)return;
    GtkWidget *d=gtk_dialog_new_with_buttons("身份与家庭",c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消",GTK_RESPONSE_CANCEL,"创建个人身份",1,"创建家庭身份",2,"切换",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *body=gtk_dialog_get_content_area(GTK_DIALOG(d)),*combo=gtk_combo_box_text_new();
    gtk_container_set_border_width(GTK_CONTAINER(body),16);gtk_box_pack_start(GTK_BOX(body),combo,FALSE,FALSE,5);
    for(gsize i=0;i<(gsize)json_object_array_length(rows);i++){
        UiJson *v=json_object_array_get_idx(rows,i);
        if(!json_object_get_boolean(ui_get(v,"hidden")))gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),ui_text(v,"id"),ui_text(v,"name"));
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo),c->identity_id);gtk_widget_show_all(d);
    int result=gtk_dialog_run(GTK_DIALOG(d));gchar *id=g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
    gtk_widget_destroy(d);
    if(result==1||result==2){g_free(id);id=new_identity(c,rows,result==2);result=id?GTK_RESPONSE_ACCEPT:GTK_RESPONSE_CANCEL;}
    json_object_put(rows);
    if(result==GTK_RESPONSE_ACCEPT && id && strcmp(id,c->identity_id) &&
       (!c->busy || ui_confirm(c,"停止任务并切换身份？","将先等待当前任务结束，再打开选中身份的数据。","停止并切换"))){
        c->pending_identity=g_strdup(id);transition(c);
    }
    g_free(id);
}
static void build_window(LinuxClient *c) {
    c->window=GTK_WINDOW(gtk_application_window_new(c->application));gtk_window_set_title(c->window,"Dunlin Copilot");
    gtk_window_set_default_size(c->window,1100,720);gtk_widget_set_size_request(GTK_WIDGET(c->window),720,480);
    GtkWidget *shell=gtk_box_new(GTK_ORIENTATION_VERTICAL,0),*split=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
    GtkWidget *nav=gtk_box_new(GTK_ORIENTATION_VERTICAL,5),*nav_scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nav_scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(nav_scroll,122,-1);gtk_container_set_border_width(GTK_CONTAINER(nav),8);gtk_container_add(GTK_CONTAINER(nav_scroll),nav);
    gtk_container_add(GTK_CONTAINER(c->window),shell);gtk_box_pack_start(GTK_BOX(shell),split,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(split),nav_scroll,FALSE,FALSE,0);c->pages=GTK_STACK(gtk_stack_new());
    gtk_stack_set_homogeneous(c->pages,FALSE);gtk_box_pack_start(GTK_BOX(split),GTK_WIDGET(c->pages),TRUE,TRUE,0);
    c->status=ui_label("");gtk_widget_set_margin_start(c->status,12);gtk_label_set_ellipsize(GTK_LABEL(c->status),PANGO_ELLIPSIZE_END);
    gtk_box_pack_end(GTK_BOX(shell),c->status,FALSE,FALSE,3);
    GtkWidget *chat=copilot_chat_new(c),*projects=copilot_projects_new(c),*settings=copilot_settings_new(c);
    c->tools=copilot_records_new(c,FALSE);c->permissions=copilot_records_new(c,TRUE);c->extensions=copilot_extensions_new(c);
    GtkWidget *library=copilot_library_new(c),*activity=copilot_activity_new(c);
    struct {const char *id,*title;GtkWidget *view;} pages[]={{"chat","聊天",chat},{"projects","项目",projects},{"tools","工具记录",c->tools},{"activity","协作与轨迹",activity},{"permissions","权限",c->permissions},{"data","权限与数据",copilot_data_new(c)},{"library","记忆与知识库",library},{"extensions","插件",c->extensions},{"settings","设置",settings}};
    for(guint i=0;i<G_N_ELEMENTS(pages);i++){
        gtk_stack_add_named(c->pages,pages[i].view,pages[i].id);GtkWidget *b=ui_button(nav,pages[i].title,G_CALLBACK(navigate),c);
        gtk_button_set_relief(GTK_BUTTON(b),GTK_RELIEF_NONE);g_object_set_data(G_OBJECT(b),"page",(gpointer)pages[i].id);
    }
    ui_button(nav,"切换身份",G_CALLBACK(choose_identity),c);
    g_signal_connect(c->window,"delete-event",G_CALLBACK(close_window),c);
    copilot_chat_refresh(c);gtk_widget_show_all(GTK_WIDGET(c->window));copilot_show(c,"chat");copilot_extensions_start(c);
}
static void activate(GtkApplication *app,LinuxClient *c) {
    if(c->window){gtk_window_present(c->window);return;}
    c->application=app;
    GtkCssProvider *css=gtk_css_provider_new();gtk_css_provider_load_from_data(css,".page-title { font-size: 19px; font-weight:bold; }", -1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(css);
    build_window(c);c->index_timer=g_timeout_add_seconds(3,index_tick,c);
}
int main(int argc,char **argv) {
    LinuxClient c={0};
    GOptionEntry options[]={{"data-dir",0,0,G_OPTION_ARG_FILENAME,&c.root_dir,"身份和本地数据目录","DIR"},{NULL}};
    GOptionContext *options_context=g_option_context_new(NULL);g_option_context_add_main_entries(options_context,options,NULL);
    GError *error=NULL;
    if(!g_option_context_parse(options_context,&argc,&argv,&error)){g_printerr("%s\n",error->message);g_error_free(error);g_option_context_free(options_context);return 2;}
    g_option_context_free(options_context);if(!c.root_dir)c.root_dir=g_build_filename(g_get_user_data_dir(),"dunlin-copilot",NULL);
    struct stat st;
    if(!g_path_is_absolute(c.root_dir) || g_mkdir_with_parents(c.root_dir,0700) || lstat(c.root_dir,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&0077)){
        g_printerr("需要当前用户专属的绝对数据目录（权限 0700）。\n");return 1;}
    c.identities=dc_identity_open(c.root_dir);g_mutex_init(&c.lock);
    if(!c.identities || !select_domain(&c,NULL)){g_printerr("身份数据无法打开或已在使用。\n");if(c.identities)dc_identity_close(c.identities);return 1;}
    GtkApplication *app=gtk_application_new("org.dunlin.copilot",G_APPLICATION_NON_UNIQUE);g_signal_connect(app,"activate",G_CALLBACK(activate),&c);
    int result=g_application_run(G_APPLICATION(app),argc,argv);
    if(c.index_timer)g_source_remove(c.index_timer);
    while(g_atomic_int_get(&c.indexing))g_usleep(1000);
    dc_close(c.store);dc_identity_close(c.identities);g_object_unref(app);g_mutex_clear(&c.lock);
    g_free(c.root_dir);g_free(c.data_dir);g_free(c.identity_id);g_free(c.project_id);g_free(c.chat_id);g_free(c.model_id);g_free(c.pending_identity);return result;
}
