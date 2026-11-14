#include "ui.h"
#include "pages.h"
#include "settings.h"
#include <string.h>

typedef UiJson J;
typedef enum { HISTORY, MEMORY, DOCS, SKILLS, COMPRESSION } Section;
typedef struct {
    LinuxClient *client;
    GtkWidget *tabs,*scope,*body,*query,*search_scope;
    Section active;
    gint64 after;
    gboolean rebuilding;
} Library;
typedef struct {Library *lib;J *node;gchar *op;} RowAction;
static gboolean yes(J *o,const char *k){return json_object_get_boolean(ui_get(o,k));}
static gint64 count(J *o,const char *k){return json_object_get_int64(ui_get(o,k));}
static gsize length(J *list){return json_object_is_type(list,json_type_array)?(gsize)json_object_array_length(list):0;}
static void integer(J *q,const char *k,gint64 n){json_object_object_add(q,k,json_object_new_int64(n));}
static void boolean(J *q,const char *k,gboolean v){json_object_object_add(q,k,json_object_new_boolean(v));}
static void copy(J *q,const char *k,J *o,const char *field){json_object_object_add(q,k,json_object_get(ui_get(o,field)));}
static void row_free(gpointer data,GClosure *unused)
{
    (void)unused;RowAction *a=data;json_object_put(a->node);g_free(a->op);g_free(a);
}
static void row_button(Library *lib,GtkWidget *bar,const char *text,const char *op,J *node,GCallback cb)
{
    RowAction *a=g_new0(RowAction,1);a->lib=lib;a->op=g_strdup(op);a->node=json_object_get(node);
    GtkWidget *b=gtk_button_new_with_label(text);gtk_box_pack_start(GTK_BOX(bar),b,FALSE,FALSE,0);
    g_signal_connect_data(b,"clicked",cb,a,row_free,0);
}
static const char *scope(Library *l)
{
    const char *s=gtk_combo_box_get_active_id(GTK_COMBO_BOX(l->scope));return s?s:"global";
}
static const char *scope_id(Library *l)
{
    if(!strcmp(scope(l),"chat"))return l->client->chat_id?l->client->chat_id:"";
    if(!strcmp(scope(l),"project"))return l->client->project_id?l->client->project_id:"";
    return "";
}
static J *scoped(Library *l,const char *op)
{
    J *q=ui_op(op);ui_string(q,"scope",scope(l));ui_string(q,"scopeID",scope_id(l));return q;
}
static gchar *picker(Library *l,const char *title,GtkFileChooserAction kind)
{
    GtkWidget *d=gtk_file_chooser_dialog_new(title,l->client->window,kind,"取消",GTK_RESPONSE_CANCEL,"选择",GTK_RESPONSE_ACCEPT,NULL);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(d),TRUE);
    gchar *path=gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT?gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)):NULL;
    gtk_widget_destroy(d);return path;
}
static GtkWidget *bar(GtkWidget *box)
{
    GtkWidget *b=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);gtk_box_pack_start(GTK_BOX(box),b,FALSE,FALSE,2);return b;
}
static void reload(Library *l);
static void tabs_changed(GtkComboBox *b,gpointer d){Library *l=d;if(l->rebuilding)return;l->active=gtk_combo_box_get_active(b);l->after=0;reload(l);}
static void scope_changed(GtkComboBox *b,gpointer d){(void)b;Library *l=d;if(!l->rebuilding){l->after=0;reload(l);}}
static void refresh_click(GtkButton *b,gpointer data){(void)b;reload(data);}
static void checkbox_pref(GtkToggleButton *b,gpointer data)
{
    Library *l=data;const char *key=g_object_get_data(G_OBJECT(b),"setting");
    if(!copilot_pref_save(l->client,key,gtk_toggle_button_get_active(b)?"true":"false"))copilot_status(l->client,"偏好写入失败。");
}
static void add_check(Library *l,const char *title,const char *key,gboolean fallback)
{
    gchar *v=copilot_pref_read(l->client,key);GtkWidget *b=gtk_check_button_new_with_label(title);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b),v?!strcmp(v,"true"):fallback);g_free(v);
    gtk_box_pack_start(GTK_BOX(l->body),b,FALSE,FALSE,0);g_object_set_data(G_OBJECT(b),"setting",(gpointer)key);
    g_signal_connect(b,"toggled",G_CALLBACK(checkbox_pref),l);
}
static void inspect_hit(GtkButton *b,gpointer data)
{
    (void)b;RowAction *a=data;J *q=ui_op("index.source");
    copy(q,"chat",a->node,"chat");copy(q,"sequence",a->node,"sequence");copy(q,"text",a->node,"text");
    J *result=ui_call(a->lib->client,UI_WORKSPACE,q);if(!result)return;
    GString *text=g_string_new(NULL);
    for(gsize i=0;i<length(result);i++){J *m=json_object_array_get_idx(result,i);g_string_append_printf(text,"%s\n%s\n\n",ui_text(m,"role"),ui_text(m,"text"));}
    ui_detail(a->lib->client,"来源对话原文",text->str);g_string_free(text,TRUE);json_object_put(result);
}
static void history_search(GtkWidget *b,gpointer data)
{
    (void)b;Library *l=data;const char *query=gtk_entry_get_text(GTK_ENTRY(l->query));
    GtkWidget *results=g_object_get_data(G_OBJECT(l->query),"results");ui_clear(results);
    if(!*query||l->client->busy)return;
    if(!g_mutex_trylock(&l->client->lock)){copilot_status(l->client,"请在当前任务完成后搜索。");return;}
    const char *project=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(l->search_scope))?l->client->project_id:NULL;
    char *raw=dc_search_filtered(l->client->store,query,project,0);g_mutex_unlock(&l->client->lock);
    J *hits=raw?json_tokener_parse(raw):NULL;dc_free(raw);
    if (!hits){copilot_status(l->client,"本地索引暂不可用。");return;}
    if (!length(hits))ui_row(results,"没有匹配的原文","可缩短关键词或清除项目过滤。");
    for(gsize i=0;i<length(hits);i++){
        J *hit=json_object_array_get_idx(hits,i);
        GtkWidget *r=ui_row(results,ui_text(hit,"role"),ui_text(hit,"text"));row_button(l,r,"查看原文","",hit,G_CALLBACK(inspect_hit));
    }
    json_object_put(hits);gtk_widget_show_all(results);
}
static void index_action(GtkButton *b,gpointer data)
{
    (void)b;RowAction *a=data;Library *l=a->lib;J *q=ui_op(a->op);
    if(!strcmp(a->op,"index.pause"))boolean(q,"value",!yes(a->node,"paused"));
    else if(!strcmp(a->op,"index.quota")){
        gchar *v=ui_prompt(l->client,"容量上限（MiB，0 为自动管理）","0",FALSE);if(!v){json_object_put(q);return;}
        gchar *end;gint64 n=g_ascii_strtoll(v,&end,10);
        if(!*v||*end||n<0||n>4096){g_free(v);json_object_put(q);copilot_status(l->client,"请输入 0 至 4096 MiB。");return;}
        g_free(v);integer(q,"bytes",n*1048576);
    }else if(!ui_confirm(l->client,!strcmp(a->op,"index.clear")?"清除索引？":"重新建立索引？","来源对话保留。重建后将在后台重新整理。","确认")){json_object_put(q);return;}
    J *result=ui_call(l->client,UI_WORKSPACE,q);if(result){json_object_put(result);reload(l);}
}
static void history_page(Library *l)
{
    J *state=ui_call(l->client,UI_WORKSPACE,ui_op("index.status"));if(!state)return;
    gchar *detail=g_strdup_printf("%s · 已整理 %"G_GINT64_FORMAT" 段 · 待处理 %"G_GINT64_FORMAT" 段 · %"G_GINT64_FORMAT" / %"G_GINT64_FORMAT" KiB",
        yes(state,"paused")?"暂停":count(state,"fault")?"整理未完成":"后台整理",count(state,"documents"),count(state,"pending"),count(state,"bytes")/1024,count(state,"quota")/1024);
    ui_row(l->body,"本地聊天索引",detail);g_free(detail);GtkWidget *actions=bar(l->body);
    row_button(l,actions,yes(state,"paused")?"继续整理":"暂停整理","index.pause",state,G_CALLBACK(index_action));
    row_button(l,actions,"设置配额","index.quota",state,G_CALLBACK(index_action));row_button(l,actions,"重建索引","index.rebuild",state,G_CALLBACK(index_action));row_button(l,actions,"清除索引","index.clear",state,G_CALLBACK(index_action));
    json_object_put(state);GtkWidget *controls=bar(l->body);l->query=gtk_search_entry_new();gtk_entry_set_placeholder_text(GTK_ENTRY(l->query),"搜索旧对话中的原文");
    gtk_box_pack_start(GTK_BOX(controls),l->query,TRUE,TRUE,0);ui_button(controls,"搜索",G_CALLBACK(history_search),l);
    l->search_scope=gtk_check_button_new_with_label("仅当前项目");gtk_widget_set_sensitive(l->search_scope,l->client->project_id&&*l->client->project_id);gtk_box_pack_start(GTK_BOX(l->body),l->search_scope,FALSE,FALSE,0);
    GtkWidget *results=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);gtk_box_pack_start(GTK_BOX(l->body),results,FALSE,FALSE,0);g_object_set_data(G_OBJECT(l->query),"results",results);
    g_signal_connect(l->query,"activate",G_CALLBACK(history_search),l);
}
static void node_act(GtkButton *b,gpointer data)
{
    (void)b;RowAction *a=data;Library *l=a->lib;J *row=a->node,*body=ui_get(row,"body");
    if(!strcmp(a->op,"detail")){ui_detail(l->client,ui_text(row,"name"),ui_text(body,"text"));return;}
    if(!strcmp(a->op,"evidence")) {
        J *q=ui_op("graph.neighbors");copy(q,"id",row,"id");ui_string(q,"type","derivedFrom");J *v=ui_call(l->client,UI_KNOWLEDGE,q);
        if(v){GString *r=g_string_new(NULL);for(gsize i=0;i<length(v);i++)g_string_append_printf(r,"%s\n",ui_text(ui_get(json_object_array_get_idx(v,i),"body"),"quote"));
            ui_detail(l->client,"来源原文",r->len?r->str:"手动录入。");g_string_free(r,TRUE);json_object_put(v);}return;
    }
    gboolean memory=l->active==MEMORY;const char *name=ui_text(row,"name");
    if(!strcmp(a->op,"move")){
        J *projects=ui_call(l->client,UI_WORKSPACE,ui_op("projects.list"));if(!projects)return;
        GtkWidget *d=gtk_dialog_new_with_buttons("更改资料范围",l->client->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
            "取消",GTK_RESPONSE_CANCEL,"保存范围",GTK_RESPONSE_ACCEPT,NULL);
        GtkWidget *combo=gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),"","当前身份");
        for(gsize i=0;i<length(projects);i++){J *p=json_object_array_get_idx(projects,i);gchar *label=g_strdup_printf("项目 · %s",ui_text(p,"name"));
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),ui_text(p,"id"),label);g_free(label);}
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo),ui_text(row,"scopeID"));json_object_put(projects);
        GtkWidget *box=gtk_dialog_get_content_area(GTK_DIALOG(d));gtk_container_set_border_width(GTK_CONTAINER(box),14);
        gtk_container_add(GTK_CONTAINER(box),ui_label(name));gtk_container_add(GTK_CONTAINER(box),combo);gtk_widget_show_all(d);
        int reply=gtk_dialog_run(GTK_DIALOG(d));gchar *id=g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));gtk_widget_destroy(d);
        if(reply==GTK_RESPONSE_ACCEPT && id){J *q=ui_op("knowledge.move");copy(q,"id",row,"id");copy(q,"revision",row,"revision");
            ui_string(q,"scope",*id?"project":"global");ui_string(q,"scopeID",id);J *v=ui_call(l->client,UI_KNOWLEDGE,q);if(v){json_object_put(v);reload(l);}}
        g_free(id);return;
    }
    if(!strcmp(a->op,"remove")&&!ui_confirm(l->client,"删除所选条目？",name,"删除"))return;
    J *q=ui_op(!strcmp(a->op,"remove")?(memory?"memory.remove":"knowledge.remove"):memory?"memory.update":"knowledge.reindex");
    copy(q,"id",row,"id");copy(q,"revision",row,"revision");
    if(!strcmp(a->op,"confirm"))ui_string(q,"status","confirmed");
    if(!strcmp(a->op,"pause"))ui_string(q,"status","paused");
    if(!strcmp(a->op,"correct")){
        gchar *v=ui_prompt(l->client,"更正记忆",ui_text(body,"text"),FALSE);
        if(!v||!*v){g_free(v);json_object_put(q);return;}ui_string(q,"text",v);ui_string(q,"status","confirmed");g_free(v);
    }
    J *v=ui_call(l->client,UI_KNOWLEDGE,q);if(v){json_object_put(v);reload(l);}
}
static void subject_add(GtkButton *button,gpointer data)
{
    (void)button;Library *l=data;
    GtkWidget *d=gtk_dialog_new_with_buttons("添加描述对象",l->client->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消",GTK_RESPONSE_CANCEL,"添加",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *box=gtk_dialog_get_content_area(GTK_DIALOG(d)),*type=gtk_combo_box_text_new(),*name=gtk_entry_new();
    gtk_container_set_border_width(GTK_CONTAINER(box),14);gtk_box_set_spacing(GTK_BOX(box),8);
    gtk_entry_set_placeholder_text(GTK_ENTRY(name),"对象名称");gtk_container_add(GTK_CONTAINER(box),name);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(type),"person","个人");gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(type),"shared","家庭共有");
    gtk_combo_box_set_active(GTK_COMBO_BOX(type),0);gtk_container_add(GTK_CONTAINER(box),type);gtk_widget_show_all(d);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT && *gtk_entry_get_text(GTK_ENTRY(name))){
        J *q=ui_op("memory.subject.add");gchar *id=g_uuid_string_random();ui_string(q,"id",id);g_free(id);
        ui_string(q,"name",gtk_entry_get_text(GTK_ENTRY(name)));ui_string(q,"kind",gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)));
        J *r=ui_call(l->client,UI_KNOWLEDGE,q);if(r){json_object_put(r);copilot_status(l->client,"已添加描述对象。");}
    }
    gtk_widget_destroy(d);
}
static void add_knowledge(GtkButton *button,gpointer data)
{
    Library *l=data;gboolean memory=l->active==MEMORY;
    J *subjects=memory?ui_call(l->client,UI_KNOWLEDGE,ui_op("memory.subjects")):NULL;
    if(memory && !subjects)return;
    gchar *file=NULL,*source=NULL,*title=NULL;
    if(g_object_get_data(G_OBJECT(button),"import")){
        file=picker(l,"选择 UTF-8 资料文件",GTK_FILE_CHOOSER_ACTION_OPEN);if(!file)return;
        source=copilot_read_local_utf8(file,262144);title=g_filename_display_basename(file);g_free(file);
        if(!source){g_free(title);copilot_status(l->client,"请选择 256 KiB 以内的非空 UTF-8 文本文件。");return;}
    }
    GtkWidget *d=gtk_dialog_new_with_buttons(memory?"保存记忆候选":"保存知识资料",l->client->window,
        GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"取消",GTK_RESPONSE_CANCEL,"保存",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *box=gtk_dialog_get_content_area(GTK_DIALOG(d)),*subject=NULL,*kind=NULL,*name=NULL;
    gtk_container_set_border_width(GTK_CONTAINER(box),14);gtk_box_set_spacing(GTK_BOX(box),8);
    if(memory){
        subject=gtk_combo_box_text_new();kind=gtk_combo_box_text_new();
        for(gsize i=0;i<length(subjects);i++){J *r=json_object_array_get_idx(subjects,i);gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(subject),ui_text(r,"id"),ui_text(r,"name"));}
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(subject),l->client->identity_id);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kind),"preference","偏好");gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kind),"fact","事实陈述");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kind),"inference","推断");gtk_combo_box_set_active(GTK_COMBO_BOX(kind),0);
        gtk_container_add(GTK_CONTAINER(box),ui_label("描述对象"));gtk_container_add(GTK_CONTAINER(box),subject);gtk_container_add(GTK_CONTAINER(box),kind);
    }else{name=gtk_entry_new();gtk_entry_set_placeholder_text(GTK_ENTRY(name),"资料名称");gtk_entry_set_text(GTK_ENTRY(name),title?title:"");gtk_container_add(GTK_CONTAINER(box),name);}
    GtkWidget *view=gtk_text_view_new(),*scroll=gtk_scrolled_window_new(NULL,NULL);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),GTK_WRAP_WORD_CHAR);
    GtkTextBuffer *buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));if(source)gtk_text_buffer_set_text(buffer,source,-1);
    gtk_container_add(GTK_CONTAINER(scroll),view);gtk_widget_set_size_request(scroll,380,180);gtk_box_pack_start(GTK_BOX(box),scroll,TRUE,TRUE,0);
    gtk_widget_show_all(d);
    while(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT){
        GtkTextIter start,end;gtk_text_buffer_get_bounds(buffer,&start,&end);gchar *text=gtk_text_buffer_get_text(buffer,&start,&end,FALSE);
        const char *heading=memory?gtk_combo_box_get_active_id(GTK_COMBO_BOX(subject)):gtk_entry_get_text(GTK_ENTRY(name));
        if(!heading||!*heading||!*text||strlen(text)>(memory?4096U:262144U)){g_free(text);copilot_status(l->client,"请补全名称或对象，并检查正文长度。");continue;}
        J *q=scoped(l,memory?"memory.create":"knowledge.add");gchar *id=g_uuid_string_random();ui_string(q,"id",id);g_free(id);
        ui_string(q,"text",text);g_free(text);
        if(memory){ui_string(q,"subject",heading);ui_string(q,"kind",gtk_combo_box_get_active_id(GTK_COMBO_BOX(kind)));boolean(q,"independent",TRUE);}
        else ui_string(q,"name",heading);
        J *result=ui_call(l->client,UI_KNOWLEDGE,q);if(result){json_object_put(result);l->after=0;break;}
    }
    gtk_widget_destroy(d);json_object_put(subjects);g_free(source);g_free(title);reload(l);
}
static void page_more(GtkButton *b,gpointer data)
{
    Library *l=data;l->after=GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(b),"cursor"));reload(l);
}
static void source_policy(GtkToggleButton *b,gpointer data)
{
    Library *l=data;if(!l->client->chat_id)return;
    J *q=ui_op("memory.source.policy");ui_string(q,"chat",l->client->chat_id);boolean(q,"enabled",gtk_toggle_button_get_active(b));
    J *v=ui_call(l->client,UI_KNOWLEDGE,q);if(!v)reload(l);else json_object_put(v);
}
static void knowledge_search(GtkWidget *b,gpointer data)
{
    (void)b;Library *l=data;const char *text=gtk_entry_get_text(GTK_ENTRY(l->query));if(!*text)return;
    J *q=scoped(l,"knowledge.search");ui_string(q,"query",text);J *v=ui_call(l->client,UI_KNOWLEDGE,q);if(!v)return;
    GString *s=g_string_new(NULL);for(gsize i=0;i<length(v);i++){J *row=json_object_array_get_idx(v,i);if(!strcmp(ui_text(row,"kind"),l->active==MEMORY?"memory":"chunk"))g_string_append_printf(s,"%s\n%s\n\n",ui_text(row,"name"),ui_text(ui_get(row,"body"),"text"));}
    ui_detail(l->client,"检索结果",s->len?s->str:"没有匹配的记忆或资料。");g_string_free(s,TRUE);json_object_put(v);
}
static void knowledge_page(Library *l)
{
    gboolean memory=l->active==MEMORY;
    add_check(l,memory?"允许本身份在对话中使用已确认记忆":"允许本身份在对话中检索知识库",memory?"memory":"rag",FALSE);
    if(memory && l->client->chat_id && *l->client->chat_id){
        J *q=ui_op("memory.source.policy");ui_string(q,"chat",l->client->chat_id);J *v=ui_call(l->client,UI_KNOWLEDGE,q);
        if(v){GtkWidget *b=gtk_check_button_new_with_label("允许从当前聊天记录记忆来源");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b),yes(v,"enabled"));gtk_box_pack_start(GTK_BOX(l->body),b,FALSE,FALSE,0);
            g_signal_connect(b,"toggled",G_CALLBACK(source_policy),l);json_object_put(v);}
    }
    GtkWidget *line=bar(l->body);l->query=gtk_search_entry_new();gtk_entry_set_placeholder_text(GTK_ENTRY(l->query),"检索记忆与资料");
    gtk_box_pack_start(GTK_BOX(line),l->query,TRUE,TRUE,0);ui_button(line,"搜索",G_CALLBACK(knowledge_search),l);g_signal_connect(l->query,"activate",G_CALLBACK(knowledge_search),l);
    ui_button(line,memory?"添加候选":"添加资料",G_CALLBACK(add_knowledge),l);
    if(memory)ui_button(line,"添加描述对象",G_CALLBACK(subject_add),l);
    else{GtkWidget *import=ui_button(line,"导入 UTF-8 文件",G_CALLBACK(add_knowledge),l);g_object_set_data(G_OBJECT(import),"import",GINT_TO_POINTER(1));}
    J *q=scoped(l,memory?"memory.list":"knowledge.list");integer(q,"after",l->after);integer(q,"limit",24);
    J *page=ui_call(l->client,UI_KNOWLEDGE,q);if(!page)return;J *rows=ui_get(page,"rows");
    if(!length(rows))ui_row(l->body,memory?"此范围暂无记忆":"此范围暂无资料","");
    for(gsize i=0;i<length(rows);i++){J *row=json_object_array_get_idx(rows,i),*v=ui_get(row,"body");
        const char *text=ui_text(v,"text");gchar *excerpt=g_utf8_substring(text,0,MIN(g_utf8_strlen(text,-1),110));
        GtkWidget *r=ui_row(l->body,memory?(!strcmp(ui_text(v,"status"),"confirmed")?"已确认":!strcmp(ui_text(v,"status"),"paused")?"已暂停":"待确认"):ui_text(row,"name"),excerpt);g_free(excerpt);
        row_button(l,r,"原文","detail",row,G_CALLBACK(node_act));
        if(memory){row_button(l,r,"来源","evidence",row,G_CALLBACK(node_act));gboolean confirmed=!g_strcmp0(ui_text(v,"status"),"confirmed")||!g_strcmp0(ui_text(row,"state"),"confirmed");
            row_button(l,r,confirmed?"暂停":"确认",confirmed?"pause":"confirm",row,G_CALLBACK(node_act));row_button(l,r,"更正","correct",row,G_CALLBACK(node_act));}
        else{row_button(l,r,"更改范围","move",row,G_CALLBACK(node_act));row_button(l,r,"重建索引","reindex",row,G_CALLBACK(node_act));}
        row_button(l,r,"删除","remove",row,G_CALLBACK(node_act));
    }
    GtkWidget *nav=bar(l->body);
    if(l->after)ui_button(nav,"第一页",G_CALLBACK(page_more),l);
    if(yes(page,"hasMore")){GtkWidget *more=ui_button(nav,"下一页",G_CALLBACK(page_more),l);g_object_set_data(G_OBJECT(more),"cursor",GSIZE_TO_POINTER((gsize)count(page,"next")));}
    json_object_put(page);
}
static void skill_add(GtkButton *button,gpointer data)
{
    (void)button;Library *l=data;gchar *p=picker(l,"选择包含 SKILL.md 的本地目录",GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    if(!p)return;
    J *q=ui_op("skills.import");ui_string(q,"path",p);g_free(p);
    J *imported=ui_call(l->client,UI_WORKSPACE,q);
    if(imported){json_object_put(imported);reload(l);}
}
static void skill_act(GtkButton *button,gpointer data)
{
    (void)button;RowAction *a=data;Library *l=a->lib;J *r=a->node;
    if(!strcmp(a->op,"view")){
        J *selected=ui_get(r,"selected");ui_detail(l->client,ui_text(selected,"name"),ui_text(selected,"body"));return;
    }
    J *q=scoped(l,!strcmp(a->op,"update")?"skills.update":"skills.bind");copy(q,"source",r,"source");copy(q,"package",r,"package");
    if(strcmp(a->op,"update")){
        const char *version=ui_text(r,"version"),*mode=a->op;gchar *selected=NULL;
        if(!strcmp(mode,"versions")){
            GtkWidget *d=gtk_dialog_new_with_buttons("选择已下载版本",l->client->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
                "取消",GTK_RESPONSE_CANCEL,"应用到此范围",GTK_RESPONSE_ACCEPT,NULL);
            GtkWidget *combo=gtk_combo_box_text_new();J *versions=ui_get(r,"versions");
            for(gsize i=0;i<length(versions);i++){const char *v=ui_text(json_object_array_get_idx(versions,i),"version");gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),v,v);}
            if(!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo),version))gtk_combo_box_set_active(GTK_COMBO_BOX(combo),0);
            gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),combo);gtk_widget_show_all(d);
            if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT)selected=g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
            gtk_widget_destroy(d);if(!selected){json_object_put(q);return;}mode="enable";version=selected;
        }
        ui_string(q,"mode",mode);ui_string(q,"version",version);g_free(selected);
    }
    J *result=ui_call(l->client,UI_WORKSPACE,q);if(result){json_object_put(result);reload(l);}
}
static void skills_page(Library *l)
{
    GtkWidget *controls=bar(l->body);ui_button(controls,"从本地目录安装",G_CALLBACK(skill_add),l);
    J *rows=ui_call(l->client,UI_WORKSPACE,scoped(l,"skills.list"));if(!rows)return;
    if(!length(rows))ui_row(l->body,"本地 Skills 库为空","安装后可在当前范围独立启用或固定版本。");
    for(gsize i=0;i<length(rows);i++){
        J *r=json_object_array_get_idx(rows,i),*v=ui_get(r,"selected");
        gchar *info=g_strdup_printf("%s · %s · %s",ui_text(r,"version"),ui_text(r,"origin"),yes(r,"enabled")?"已启用":"未启用");
        GtkWidget *row=ui_row(l->body,*ui_text(v,"name")?ui_text(v,"name"):ui_text(r,"package"),info);g_free(info);
        row_button(l,row,"详情","view",r,G_CALLBACK(skill_act));
        row_button(l,row,yes(r,"enabled")?"停用":"启用",yes(r,"enabled")?"disable":"enable",r,G_CALLBACK(skill_act));
        if(strcmp(scope(l),"global")&&g_strcmp0(ui_text(r,"scopeMode"),"inherit"))row_button(l,row,"恢复继承","inherit",r,G_CALLBACK(skill_act));
        GtkWidget *actions=bar(l->body);row_button(l,actions,"选择已下载版本","versions",r,G_CALLBACK(skill_act));
        row_button(l,actions,"更新","update",r,G_CALLBACK(skill_act));
    }
    json_object_put(rows);
}
static void strategy_changed(GtkComboBox *combo,gpointer data)
{
    Library *l=data;const char *id=gtk_combo_box_get_active_id(combo);if(!id)return;
    if(!copilot_pref_save(l->client,"compression",id))copilot_status(l->client,"保存压缩策略失败。");
}
static void strategy_add(GtkButton *button,gpointer data)
{
    Library *l=data;gchar *dir=NULL;gboolean bundled=g_object_get_data(G_OBJECT(button),"bundled")!=NULL;
    if(bundled){
        gchar *path=g_file_read_link("/proc/self/exe",NULL);
        if(path){gchar *parent=g_path_get_dirname(path);dir=g_build_filename(parent,"ContextStrategies","extractive",NULL);g_free(parent);g_free(path);}
        if(!dir || !g_file_test(dir,G_FILE_TEST_IS_DIR)){g_free(dir);dir=g_strdup("/usr/share/dunlin-copilot/ContextStrategies/extractive");}
    }else dir=picker(l,"选择本地压缩策略目录",GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    if(!dir)return;
    gchar *manifest=g_build_filename(dir,"dunlin.json",NULL);gchar *raw=copilot_read_local_utf8(manifest,65536);g_free(manifest);
    J *m=raw?json_tokener_parse(raw):NULL;g_free(raw);
    if(!m||strcmp(ui_text(m,"contextStrategy"),"extractive-v1")){copilot_status(l->client,"目录没有可识别的压缩策略清单。");json_object_put(m);g_free(dir);return;}
    gboolean approved=ui_confirm(l->client,"安装压缩策略？",ui_text(m,"displayName"),"安装并保持停用");json_object_put(m);
    if(approved){J *q=ui_op("extensions.import");ui_string(q,"path",dir);J *installed=ui_call(l->client,UI_EXTENSIONS,q);if(installed){json_object_put(installed);reload(l);}}
    g_free(dir);
}
static void strategy_use(GtkButton *button,gpointer data)
{
    (void)button;RowAction *a=data;Library *l=a->lib;J *r=a->node,*ref=json_object_new_object();
    copy(ref,"source",r,"source");copy(ref,"package",r,"package");copy(ref,"version",r,"version");
    if(!strcmp(a->op,"enable")){
        if(ui_confirm(l->client,"启用外部压缩策略？","此程序会接收当前身份中需要压缩的原文，并以桌面用户权限运行。","信任并启用")){
            J *q=ui_op("extensions.bind");copy(q,"source",r,"source");copy(q,"package",r,"package");copy(q,"version",r,"version");boolean(q,"enabled",TRUE);boolean(q,"acceptHostPermissions",TRUE);
            J *v=ui_call(l->client,UI_EXTENSIONS,q);if(v){json_object_put(v);reload(l);}
        }
    }else if(copilot_pref_save(l->client,"compression-extension",json_object_to_json_string_ext(ref,JSON_C_TO_STRING_PLAIN))&&copilot_pref_save(l->client,"compression","external")){reload(l);}
    json_object_put(ref);
}
static void python_setup(GtkButton *button,gpointer data)
{
    (void)button;Library *l=data;gchar *path=picker(l,"选择 Python 3 解释器",GTK_FILE_CHOOSER_ACTION_OPEN);
    if(!path)return;
    if(ui_confirm(l->client,"启用 Python 运行环境？",path,"允许使用此解释器")){
        J *q=ui_op("python.configure");ui_string(q,"executable",path);J *v=ui_call(l->client,UI_EXTENSIONS,q);
        if(v){json_object_put(v);copilot_status(l->client,"Python 环境已配置。");}
    }
    g_free(path);
}
static void context_records(GtkButton *button,gpointer data)
{
    (void)button;Library *l=data;if(!l->client->chat_id||!*l->client->chat_id)return;
    J *q=ui_op("context.records");ui_string(q,"chat",l->client->chat_id);J *rows=ui_call(l->client,UI_WORKSPACE,q);
    if(!rows)return;
    if(!length(rows)){json_object_put(rows);ui_detail(l->client,"当前对话压缩记录","此对话尚无压缩记录。");return;}
    GtkWidget *d=gtk_dialog_new_with_buttons("当前对话压缩记录",l->client->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "关闭",GTK_RESPONSE_CANCEL,"查看原文",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *combo=gtk_combo_box_text_new();
    for(gsize i=0;i<length(rows);i++){
        J *r=json_object_array_get_idx(rows,i);gchar *title=g_strdup_printf("#%"G_GINT64_FORMAT" · %s · %s · %"G_GINT64_FORMAT" 段",count(r,"id"),ui_text(r,"strategy"),ui_text(r,"status"),count(r,"sourceCount"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),title);g_free(title);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo),0);gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),combo);gtk_widget_show_all(d);
    int answer=gtk_dialog_run(GTK_DIALOG(d));gint choice=gtk_combo_box_get_active(GTK_COMBO_BOX(combo));gtk_widget_destroy(d);
    if(answer!=GTK_RESPONSE_ACCEPT || choice<0){json_object_put(rows);return;}
    gint64 rec=count(json_object_array_get_idx(rows,(gsize)choice),"id");json_object_put(rows);
    gchar *chat=g_strdup(l->client->chat_id);q=ui_op("context.sources");ui_string(q,"chat",chat);integer(q,"id",rec);
    J *sources=ui_call(l->client,UI_WORKSPACE,q);if(!sources){g_free(chat);return;}
    d=gtk_dialog_new_with_buttons("选择下一轮纳入的原文",l->client->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消",GTK_RESPONSE_CANCEL,"清空选择",2,"应用到下一轮",GTK_RESPONSE_ACCEPT,NULL);
    gtk_window_set_default_size(GTK_WINDOW(d),680,450);GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL),*list=gtk_box_new(GTK_ORIENTATION_VERTICAL,7);
    gtk_container_set_border_width(GTK_CONTAINER(list),12);gtk_container_add(GTK_CONTAINER(scroll),list);gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),scroll);
    GPtrArray *options=g_ptr_array_new();
    for(gsize i=0;i<length(sources);i++){J *r=json_object_array_get_idx(sources,i);
        GtkWidget *button=gtk_check_button_new();GtkWidget *excerpt=ui_label(ui_text(r,"text"));gtk_container_add(GTK_CONTAINER(button),excerpt);
        gtk_box_pack_start(GTK_BOX(list),button,FALSE,FALSE,0);g_ptr_array_add(options,button);}
    if(!options->len)gtk_box_pack_start(GTK_BOX(list),ui_label("没有仍可验证的来源原文。"),FALSE,FALSE,0);
    gtk_widget_show_all(d);answer=gtk_dialog_run(GTK_DIALOG(d));
    if(answer==2)copilot_settings_consume_sources(l->client,chat);
    else if(answer==GTK_RESPONSE_ACCEPT){
        J *digests=json_object_new_array();for(gsize i=0;i<options->len;i++)if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_ptr_array_index(options,i)))){
            J *r=json_object_array_get_idx(sources,i);gchar *tag=g_strdup_printf("%"G_GINT64_FORMAT":%s",count(r,"seq"),ui_text(r,"digest"));json_object_array_add(digests,json_object_new_string(tag));g_free(tag);}
        if(length(digests)>32){copilot_status(l->client,"最多可选择 32 段原文。");}
        else if(length(digests)){J *select=json_object_new_object();integer(select,"record",rec);json_object_object_add(select,"digests",json_object_get(digests));
            copilot_settings_set_sources(l->client,chat,select);json_object_put(select);copilot_status(l->client,"已为当前对话保留原文选择。");}
        else copilot_settings_consume_sources(l->client,chat);
        json_object_put(digests);
    }
    gtk_widget_destroy(d);g_ptr_array_free(options,TRUE);json_object_put(sources);g_free(chat);
}
static void compression_page(Library *l)
{
    add_check(l,"接近上下文上限时自动压缩","automatic-compression",TRUE);
    GtkWidget *combo=gtk_combo_box_text_new();gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),"semantic","摘要压缩 · 模型");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),"recent","保留最近轮次 · 本地");
    gchar *current=copilot_pref_read(l->client,"compression");gchar *saved=copilot_pref_read(l->client,"compression-extension");
    J *ref=saved?json_tokener_parse(saved):NULL;g_free(saved);
    if(ref)gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),"external",ui_text(ref,"package"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo),current?current:"semantic");g_free(current);json_object_put(ref);
    g_signal_connect(combo,"changed",G_CALLBACK(strategy_changed),l);gtk_box_pack_start(GTK_BOX(l->body),combo,FALSE,FALSE,0);
    GtkWidget *buttons=bar(l->body);ui_button(buttons,"配置 Python",G_CALLBACK(python_setup),l);ui_button(buttons,"安装本地策略",G_CALLBACK(strategy_add),l);
    GtkWidget *builtin=ui_button(buttons,"安装随附提取策略",G_CALLBACK(strategy_add),l);g_object_set_data(G_OBJECT(builtin),"bundled",GINT_TO_POINTER(1));
    gtk_box_pack_start(GTK_BOX(l->body),ui_label("随附提取策略使用本地 Python 3.8 或更高版本的 Python 3 解释器。"),FALSE,FALSE,0);
    if(l->client->chat_id && *l->client->chat_id)ui_button(buttons,"当前对话压缩记录",G_CALLBACK(context_records),l);
    J *rows=ui_call(l->client,UI_EXTENSIONS,ui_op("extensions.list"));if(!rows)return;
    for(gsize i=0;i<length(rows);i++){J *r=json_object_array_get_idx(rows,i),*versions=ui_get(r,"versions"),*m=NULL;
        for(gsize k=0;k<length(versions);k++){J *v=json_object_array_get_idx(versions,k);if(!g_strcmp0(ui_text(v,"version"),ui_text(r,"version")))m=ui_get(v,"manifest");}
        if(!m||g_strcmp0(ui_text(m,"contextStrategy"),"extractive-v1"))continue;
        GtkWidget *row=ui_row(l->body,ui_text(m,"displayName"),ui_text(r,"version"));
        row_button(l,row,yes(r,"active")?"使用此策略":"启用",yes(r,"active")?"select":"enable",r,G_CALLBACK(strategy_use));
    }
    json_object_put(rows);
}
static void reload(Library *l)
{
    if(l->rebuilding)return;
    l->rebuilding=TRUE;ui_clear(l->body);l->query=NULL;l->search_scope=NULL;
    gboolean scoped_page=l->active==MEMORY||l->active==DOCS||l->active==SKILLS;
    gtk_widget_set_visible(l->scope,scoped_page);
    if(l->client->busy){ui_row(l->body,"当前回合正在进行","完成后可刷新本页。");}
    else if(scoped_page && strcmp(scope(l),"global") && !*scope_id(l)){ui_row(l->body,"请先选择该范围的项目或聊天","");}
    else switch(l->active) {
        case HISTORY:history_page(l);break;
        case MEMORY:case DOCS:knowledge_page(l);break;
        case SKILLS:skills_page(l);break;
        case COMPRESSION:compression_page(l);break;
    }
    gtk_widget_show_all(l->body);l->rebuilding=FALSE;
}
void copilot_library_refresh(LinuxClient *client)
{
    Library *l=g_object_get_data(G_OBJECT(client->window),"dunlin-library");if(l)reload(l);
}
GtkWidget *copilot_library_new(LinuxClient *client)
{
    Library *l=g_new0(Library,1);l->client=client;
    g_object_set_data_full(G_OBJECT(client->window),"dunlin-library",l,g_free);
    GtkWidget *body,*page=ui_page("记忆、知识与检索",&body),*top=bar(body);l->tabs=gtk_combo_box_text_new();l->scope=gtk_combo_box_text_new();
    const char *tabs[]={"聊天检索","人物记忆","知识库","Skills","上下文压缩"};
    for(gsize i=0;i<G_N_ELEMENTS(tabs);i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(l->tabs),tabs[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(l->tabs),0);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(l->scope),"global","当前身份");gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(l->scope),"project","当前项目");gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(l->scope),"chat","当前聊天");gtk_combo_box_set_active(GTK_COMBO_BOX(l->scope),0);
    gtk_box_pack_start(GTK_BOX(top),l->tabs,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(top),l->scope,FALSE,FALSE,0);
    gtk_widget_set_no_show_all(l->scope,TRUE);ui_button(top,"刷新",G_CALLBACK(refresh_click),l);
    l->body=gtk_box_new(GTK_ORIENTATION_VERTICAL,7);gtk_box_pack_start(GTK_BOX(body),l->body,FALSE,FALSE,0);
    g_signal_connect(l->scope,"changed",G_CALLBACK(scope_changed),l);g_signal_connect(l->tabs,"changed",G_CALLBACK(tabs_changed),l);
    reload(l);gtk_widget_show_all(page);return page;
}
