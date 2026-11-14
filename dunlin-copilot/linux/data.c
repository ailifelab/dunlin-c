#include "pages.h"
#include "ui.h"
#include "chat.h"
#include "settings.h"
#include "dunlin/transcript.h"
#include <string.h>

typedef UiJson J;
typedef struct { LinuxClient *c; GtkWidget *body,*chat; } DataPage;

static gboolean lock_store(LinuxClient *c) {
    if(!c->closing && !c->pending_identity && !c->busy && g_mutex_trylock(&c->lock))return TRUE;
    copilot_status(c,"请在当前任务结束后重试。");return FALSE;
}
static J *list_chats(LinuxClient *c) {
    if(!lock_store(c))return NULL;
    char *raw=dc_chats(c->store);g_mutex_unlock(&c->lock);J *rows=raw?json_tokener_parse(raw):NULL;dc_free(raw);return rows;
}
static gint64 number(J *row,const char *field){return json_object_get_int64(ui_get(row,field));}
static GtkWidget *actions(GtkWidget *body) {
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);gtk_box_pack_start(GTK_BOX(body),box,FALSE,FALSE,0);return box;
}
static gboolean preview_text(LinuxClient *c,const char *title,const char *detail,const char *text,const char *confirm) {
    GtkWidget *d=gtk_dialog_new_with_buttons(title,c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消",GTK_RESPONSE_CANCEL,confirm,GTK_RESPONSE_ACCEPT,NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d),GTK_RESPONSE_CANCEL);gtk_window_set_default_size(GTK_WINDOW(d),620,430);
    GtkWidget *body=gtk_dialog_get_content_area(GTK_DIALOG(d)),*scroll=gtk_scrolled_window_new(NULL,NULL),*view=gtk_text_view_new();
    gtk_container_set_border_width(GTK_CONTAINER(body),16);gtk_box_set_spacing(GTK_BOX(body),10);
    gtk_box_pack_start(GTK_BOX(body),ui_label(detail),FALSE,FALSE,0);gtk_text_view_set_editable(GTK_TEXT_VIEW(view),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),GTK_WRAP_WORD_CHAR);gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),text,-1);
    gtk_container_add(GTK_CONTAINER(scroll),view);gtk_box_pack_start(GTK_BOX(body),scroll,TRUE,TRUE,0);gtk_widget_show_all(d);
    gboolean accepted=gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT;gtk_widget_destroy(d);return accepted;
}
static gchar *choose_file(LinuxClient *c,gboolean save,const char *suggestion) {
    GtkWidget *d=gtk_file_chooser_dialog_new(save?"保存聊天正文":"从 JSON 恢复聊天正文",c->window,save?GTK_FILE_CHOOSER_ACTION_SAVE:GTK_FILE_CHOOSER_ACTION_OPEN,
        "取消",GTK_RESPONSE_CANCEL,save?"保存":"预览",GTK_RESPONSE_ACCEPT,NULL);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(d),TRUE);
    if(save){gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d),suggestion);gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d),TRUE);}
    gchar *path=gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT?gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)):NULL;
    gtk_widget_destroy(d);return path;
}
static gchar *markdown(J *doc) {
    GString *result=g_string_new("# ");g_string_append(result,ui_text(doc,"title"));J *rows=ui_get(doc,"messages");
    for(gsize i=0;i<(gsize)json_object_array_length(rows);i++){J *row=json_object_array_get_idx(rows,i);
        g_string_append_printf(result,"\n\n## %s\n\n%s",!strcmp(ui_text(row,"role"),"user")?"用户":"助手",ui_text(row,"content"));}
    g_string_append_c(result,'\n');return g_string_free(result,FALSE);
}
static void export_chat(GtkButton *b,DataPage *p) {
    LinuxClient *c=p->c;const char *id=gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->chat));if(!id || !lock_store(c))return;
    gchar *identity=g_strdup(c->identity_id);char *raw=dc_transcript_export(c->store,id);g_mutex_unlock(&c->lock);
    if(!raw){g_free(identity);copilot_status(c,"聊天不可读或超出单份正文备份的 8 MiB 上限。");return;}
    J *doc=json_tokener_parse(raw);dc_free(raw);gboolean md=g_object_get_data(G_OBJECT(b),"markdown")!=NULL;
    gchar *wire=md?markdown(doc):g_strdup(json_object_to_json_string_ext(doc,JSON_C_TO_STRING_PLAIN));json_object_put(doc);
    gchar *path=NULL;if(preview_text(c,"预览导出正文","仅包含用户与助手的可见文本。",wire,"选择保存位置"))
        path=choose_file(c,TRUE,md?"Dunlin Chat.md":"Dunlin Chat.json");
    if(path && !g_strcmp0(identity,c->identity_id)){
        GFile *file=g_file_new_for_path(path);GError *error=NULL;
        if(g_file_replace_contents(file,wire,strlen(wire),NULL,FALSE,G_FILE_CREATE_PRIVATE|G_FILE_CREATE_REPLACE_DESTINATION,NULL,NULL,&error))copilot_status(c,"聊天正文已保存。");
        else copilot_status(c,error?error->message:"保存失败。");
        g_clear_error(&error);g_object_unref(file);
    }
    g_free(identity);g_free(path);g_free(wire);
}
static void import_chat(GtkButton *unused,DataPage *p) {
    (void)unused;LinuxClient *c=p->c;gchar *identity=g_strdup(c->identity_id),*path=choose_file(c,FALSE,NULL);
    if(!path){g_free(identity);return;}
    gchar *raw=copilot_read_local_utf8(path,8*1024*1024);g_free(path);
    if(!raw){copilot_status(c,"请选择不超过 8 MiB 的 UTF-8 JSON 正文备份。");g_free(identity);return;}
    char *checked=dc_transcript_preview(raw);
    if(!checked){copilot_status(c,"文件格式不受支持；仅可恢复用户与助手正文。");g_free(identity);g_free(raw);return;}
    J *summary=json_tokener_parse(checked);dc_free(checked);
    gchar *hint=g_strdup_printf("将在当前身份创建新聊天「%s」，共 %"G_GINT64_FORMAT" 段正文。",ui_text(summary,"title"),number(summary,"messages"));
    json_object_put(summary);J *doc=json_tokener_parse(raw);gchar *view=markdown(doc);json_object_put(doc);
    if(preview_text(c,"确认恢复聊天正文",hint,view,"恢复为新的普通聊天") && !g_strcmp0(identity,c->identity_id) && lock_store(c)){
        gchar *id=g_uuid_string_random();int ok=dc_transcript_restore(c->store,id,identity,raw);g_free(id);g_mutex_unlock(&c->lock);
        copilot_status(c,ok?"聊天正文已恢复到新的普通聊天。":"恢复失败，原有聊天保留。");if(ok){copilot_chat_refresh(c);copilot_data_refresh(c);}
    }
    g_free(view);g_free(hint);g_free(raw);g_free(identity);
}
static void clear_chats(GtkButton *unused,DataPage *p) {
    (void)unused;LinuxClient *c=p->c;copilot_chat_flush(c);J *rows=list_chats(c);if(!rows)return;
    gsize n=(gsize)json_object_array_length(rows);if(!n){json_object_put(rows);return;}
    GString *review=g_string_new(NULL);gchar *identity=g_strdup(c->identity_id);gboolean complete=TRUE;
    for(gsize i=0;i<n;i++){J *r=json_object_array_get_idx(rows,i),*q=ui_op("chats.cleanup.preview");ui_string(q,"id",ui_text(r,"id"));
        J *v=ui_call(c,UI_WORKSPACE,q);if(!v){complete=FALSE;break;}
        gchar *size=g_format_size(number(v,"exclusiveBytes"));g_string_append_printf(review,"%s%s · %"G_GINT64_FORMAT" 段 · %s · %"G_GINT64_FORMAT" 条关联记忆\n",
            ui_text(r,"title"),number(r,"archived")?"（已归档）":"",number(v,"messages"),size,number(v,"memories"));g_free(size);json_object_put(v);}
    gchar *label=g_strdup_printf("确认删除 %"G_GSIZE_FORMAT" 个聊天",n);
    if(complete && preview_text(c,"删除全部本机聊天？","请核对当前身份的聊天清单。正文、草稿及关联记录将被清理，项目文件保留。",review->str,label) &&
       !g_strcmp0(identity,c->identity_id) && lock_store(c)){
        gsize settled=0;for(gsize i=0;i<n;i++){const char *id=ui_text(json_object_array_get_idx(rows,i),"id");
            if(dc_delete_chat(c->store,id)){settled++;if(!g_strcmp0(c->chat_id,id)){g_clear_pointer(&c->chat_id,g_free);g_clear_pointer(&c->model_id,g_free);}}}
        g_mutex_unlock(&c->lock);gchar *result=g_strdup_printf("已清理 %"G_GSIZE_FORMAT" 个聊天，保留 %"G_GSIZE_FORMAT" 个。",settled,n-settled);
        copilot_chat_refresh(c);copilot_data_refresh(c);copilot_status(c,result);g_free(result);
    }
    g_free(identity);g_free(label);g_string_free(review,TRUE);json_object_put(rows);
}
static void maintenance(GtkButton *b,DataPage *p) {
    const char *op=g_object_get_data(G_OBJECT(b),"op");const char *message=!strcmp(op,"index.rebuild")?"重新建立当前身份的本地检索索引？":"重试已登记的暂存文件清理？";
    gchar *identity=g_strdup(p->c->identity_id);
    if(ui_confirm(p->c,message,"项目文件及聊天原文保留。","确认") && !g_strcmp0(identity,p->c->identity_id)){
        J *r=ui_call(p->c,UI_WORKSPACE,ui_op(op));if(r){json_object_put(r);copilot_data_refresh(p->c);copilot_status(p->c,"维护请求已完成。");}}
    g_free(identity);
}
static void go_permissions(GtkButton *unused,DataPage *p){(void)unused;copilot_show(p->c,"permissions");}
static void open_backup(GtkButton *b,DataPage *p){
    const char *local=g_object_get_data(G_OBJECT(b),"path");gchar *path=g_build_filename(p->c->data_dir,local,NULL);
    gchar *dir=g_path_get_dirname(path),*uri=g_filename_to_uri(dir,NULL,NULL);GError *error=NULL;
    if(uri && !gtk_show_uri_on_window(p->c->window,uri,GDK_CURRENT_TIME,&error))copilot_status(p->c,error?error->message:"无法显示目录。");
    g_clear_error(&error);g_free(uri);g_free(dir);g_free(path);
}
void copilot_data_refresh(LinuxClient *c){
    DataPage *p=g_object_get_data(G_OBJECT(c->window),"copilot-data");if(!p)return;ui_clear(p->body);p->chat=NULL;
    GtkWidget *row=ui_row(p->body,"文件访问","项目命令及文件修改需要授权。已授予的权限可在授权记录中撤销。");
    ui_button(row,"授权记录",G_CALLBACK(go_permissions),p);
    ui_row(p->body,"本机存储","聊天和检索索引保存在当前身份目录。模型密钥由系统 Secret Service 保管。");
    if(c->busy){ui_row(p->body,"当前回合正在进行","回合结束后可预览、导出与清理。");gtk_widget_show_all(p->body);return;}
    J *status=ui_call(c,UI_READ,ui_op("receipts.status")),*idx=ui_call(c,UI_WORKSPACE,ui_op("index.status"));
    if(status && idx){
        gchar *size=g_format_size(number(status,"blobBytes"));gchar *info=g_strdup_printf("文件历史 %"G_GINT64_FORMAT" 项 · 快照 %s · 待清理 %"G_GINT64_FORMAT" 项 · 索引原文 %"G_GINT64_FORMAT" 段",
            number(status,"operations"),size,number(status,"pendingStages"),number(idx,"documents"));ui_row(p->body,"数据维护",info);g_free(size);g_free(info);}
    json_object_put(status);json_object_put(idx);
    J *pending=ui_call(c,UI_READ,ui_op("receipts.pending"));
    if(pending)for(gsize n=0;n<(gsize)json_object_array_length(pending);n++){
        J *r=json_object_array_get_idx(pending,n);ui_row(p->body,ui_text(r,"path"),ui_text(r,"root"));}
    json_object_put(pending);
    row=actions(p->body);GtkWidget *clean=ui_button(row,"重试暂存清理",G_CALLBACK(maintenance),p),*index=ui_button(row,"重建聊天索引",G_CALLBACK(maintenance),p);
    g_object_set_data(G_OBJECT(clean),"op","receipts.cleanup");g_object_set_data(G_OBJECT(index),"op","index.rebuild");
    J *migration=ui_call(c,UI_READ,ui_op("chats.workspace.migration"));
    if(migration){const char *rel=ui_text(migration,"backup");
        if(*rel){gchar *size=g_format_size(number(migration,"backupBytes"));row=ui_row(p->body,"升级前数据库副本",size);g_free(size);
            if(number(migration,"backupExists")){GtkWidget *b=ui_button(row,"显示副本位置",G_CALLBACK(open_backup),p);g_object_set_data_full(G_OBJECT(b),"path",g_strdup(rel),g_free);}}
        else ui_row(p->body,"数据库格式","当前身份没有可用的升级前数据库副本。");
        json_object_put(migration);}
    ui_row(p->body,"聊天正文备份","JSON 可恢复为普通聊天，Markdown 可直接阅读。附件、凭据与工具授权保留在原身份。");
    J *rows=list_chats(c);gsize n=rows?(gsize)json_object_array_length(rows):0;
    p->chat=gtk_combo_box_text_new();for(gsize i=0;i<n;i++){
        J *r=json_object_array_get_idx(rows,i);gchar *label=g_strdup_printf("%s%s",ui_text(r,"title"),number(r,"archived")?"（已归档）":"");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->chat),ui_text(r,"id"),label);g_free(label);}
    if(n && (!c->chat_id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->chat),c->chat_id)))gtk_combo_box_set_active(GTK_COMBO_BOX(p->chat),0);
    json_object_put(rows);gtk_box_pack_start(GTK_BOX(p->body),p->chat,FALSE,FALSE,0);row=actions(p->body);
    GtkWidget *json=ui_button(row,"导出 JSON",G_CALLBACK(export_chat),p),*md=ui_button(row,"导出 Markdown",G_CALLBACK(export_chat),p);
    g_object_set_data(G_OBJECT(md),"markdown",GINT_TO_POINTER(1));gtk_widget_set_sensitive(md,n>0);gtk_widget_set_sensitive(json,n>0);
    ui_button(row,"从备份恢复…",G_CALLBACK(import_chat),p);
    GtkWidget *erase=ui_button(p->body,"删除全部本机聊天…",G_CALLBACK(clear_chats),p);gtk_widget_set_sensitive(erase,n>0);
    gtk_style_context_add_class(gtk_widget_get_style_context(erase),"destructive-action");gtk_widget_show_all(p->body);
}
GtkWidget *copilot_data_new(LinuxClient *c){
    DataPage *p=g_new0(DataPage,1);p->c=c;GtkWidget *page=ui_page("权限与数据",&p->body);
    g_object_set_data_full(G_OBJECT(c->window),"copilot-data",p,g_free);copilot_data_refresh(c);return page;
}
