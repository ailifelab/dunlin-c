#include "pages.h"
#include "ui.h"
#include "surface.h"
#include "settings.h"
#include <string.h>

typedef struct {LinuxClient *c;GtkWidget *body,*section;gchar *assets;gboolean ready;guint timer;gint64 last_fault;} Extensions;
typedef struct {Extensions *ui;gboolean catalog;UiJson *req,*reply;} Work;
static void render(Extensions *ui);
static gboolean watch(gpointer data){
    Extensions *ui=data;LinuxClient *c=ui->c;
    if(c->busy||c->closing||c->pending_identity)return G_SOURCE_CONTINUE;
    UiJson *active=ui_call(c,UI_EXTENSIONS,ui_op("extensions.list"));
    if(!active)return G_SOURCE_CONTINUE;
    copilot_surface_check(c,active);json_object_put(active);
    UiJson *faults=ui_call(c,UI_EXTENSIONS,ui_op("extensions.faults"));
    if(faults && json_object_array_length(faults)){
        UiJson *v=json_object_array_get_idx(faults,0);gint64 latest=json_object_get_int64(ui_get(v,"id"));
        if(ui->last_fault>=0 && latest>ui->last_fault){
            gchar *msg=g_strdup_printf("插件 %s 已因异常停用。可在报错日志查看详情。",ui_text(v,"package"));copilot_status(c,msg);g_free(msg);
            copilot_settings_refresh_themes(c);if(!g_strcmp0(gtk_stack_get_visible_child_name(c->pages),"extensions"))render(ui);
        }
        ui->last_fault=latest;
    }else if(faults)ui->last_fault=0;
    json_object_put(faults);return G_SOURCE_CONTINUE;
}
static UiJson *param(GtkButton *b){return g_object_get_data(G_OBJECT(b),"args");}
static GtkWidget *action(GtkWidget *row,const char *title,GCallback cb,Extensions *ui,UiJson *r){
    GtkWidget *b=ui_button(row,title,cb,ui);g_object_set_data_full(G_OBJECT(b),"args",r,ui_json_release);return b;
}
static UiJson *reference(const char *op,UiJson *v){
    UiJson *r=ui_op(op);const char *keys[]={"package","source","version","instance"};
    for(guint i=0;i<G_N_ELEMENTS(keys);i++)if(ui_get(v,keys[i]))ui_string(r,keys[i],ui_text(v,keys[i]));
    return r;
}
static void release(gpointer data){Extensions *ui=data;if(ui->timer)g_source_remove(ui->timer);g_free(ui->assets);g_free(ui);}
static void job(Extensions *,gboolean,UiJson *);
static char *choose(Extensions *ui,const char *title,UiJson *versions){
    if(!json_object_is_type(versions,json_type_array)||!json_object_array_length(versions))return NULL;
    GtkWidget *d=gtk_dialog_new_with_buttons(title,ui->c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"取消",GTK_RESPONSE_CANCEL,"选择版本",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *combo=gtk_combo_box_text_new(),*area=gtk_dialog_get_content_area(GTK_DIALOG(d));gtk_container_set_border_width(GTK_CONTAINER(area),16);
    for(gsize i=0;i<(gsize)json_object_array_length(versions);i++){
        UiJson *v=json_object_array_get_idx(versions,i);const char *version=json_object_is_type(v,json_type_string)?json_object_get_string(v):ui_text(v,"version");
        if(*version)gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),version,version);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo),0);gtk_box_pack_start(GTK_BOX(area),combo,FALSE,FALSE,0);gtk_widget_show_all(d);
    char *s=gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT?g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo))):NULL;gtk_widget_destroy(d);return s;
}
static gboolean completed(gpointer data){
    Work *w=data;Extensions *ui=w->ui;LinuxClient *c=ui->c;c->busy=FALSE;
    if(!c->closing && !c->pending_identity){
        const char *op=ui_text(w->req,"op");UiJson *result=ui_get(w->reply,"data");
        if(!json_object_get_boolean(ui_get(w->reply,"ok")))copilot_status(c,w->reply?ui_text(w->reply,"error"):"扩展服务不可用。");
        else if(!strcmp(op,"compat.detect")){
            const char *disposition=ui_text(result,"action");gboolean upgrade=!strcmp(disposition,"outdated");
            const char *message=upgrade?"升级可能使已安装的插件不再兼容。是否继续？":!strcmp(disposition,"download")?"将下载并校验固定版本的兼容环境。":"将使用已检测到的兼容环境。";
            if(ui_confirm(c,"开启 DSH 兼容环境？",message,upgrade?"确认升级":"开启")){
                UiJson *r=ui_op("compat.enable");ui_string(r,"node",ui_text(w->req,"node"));json_object_object_add(r,"confirmUpgrade",json_object_new_boolean(upgrade));job(ui,FALSE,r);
            }
        }else if(!strcmp(op,"catalog.npm.versions")){
            char *v=choose(ui,"选择 npm 发布版本",result);
            if(v){UiJson *r=ui_op("catalog.npm.install");ui_string(r,"package",ui_text(w->req,"package"));ui_string(r,"repository",ui_text(w->req,"repository"));ui_string(r,"version",v);g_free(v);job(ui,TRUE,r);}
            else if(!result || !json_object_array_length(result))copilot_status(c,"没有可安装的版本。");
        }else if(strcmp(op,"extensions.restore"))copilot_status(c,"操作已完成。");
        if(!c->busy){
            copilot_settings_refresh_themes(c);
            if(!g_strcmp0(gtk_stack_get_visible_child_name(c->pages),"extensions"))render(ui);
        }
    }
    json_object_put(w->req);json_object_put(w->reply);g_free(w);return G_SOURCE_REMOVE;
}
static int approve_extension(void *data,const char *tool,const char *arguments){
    return copilot_chat_approval(data,tool,arguments);
}
static int cancelled(void *data){return g_atomic_int_get(&((LinuxClient *)data)->service_cancel);}
static gpointer worker(gpointer data){
    Work *w=data;LinuxClient *c=w->ui->c;dc_hooks hooks={.user=c,.cancelled=cancelled,.approve=approve_extension};
    const char *s=json_object_to_json_string(w->req);g_mutex_lock(&c->lock);
    char *raw=w->catalog?dc_catalog_call(c->store,s,&hooks):dc_extensions_call(c->store,s,&hooks);
    g_mutex_unlock(&c->lock);w->reply=raw?json_tokener_parse(raw):NULL;dc_free(raw);g_idle_add(completed,w);return NULL;
}
static void job(Extensions *ui,gboolean catalog,UiJson *r){
    if(ui->c->busy){copilot_status(ui->c,"请等待当前任务结束。");json_object_put(r);return;}
    Work *w=g_new0(Work,1);w->ui=ui;w->catalog=catalog;w->req=r;ui->c->busy=TRUE;g_atomic_int_set(&ui->c->service_cancel,0);
    copilot_status(ui->c,"正在处理…");g_thread_unref(g_thread_new("extensions",worker,w));
}
static void run(GtkButton *b,Extensions *ui){
    UiJson *r=param(b);const char *op=ui_text(r,"op");
    if(!strcmp(op,"extensions.uninstall")&&!ui_confirm(ui->c,"卸载这个插件？",ui_text(r,"package"),"卸载"))return;
    if(!strcmp(op,"extensions.bind")&&json_object_get_boolean(ui_get(r,"enabled"))&&!ui_confirm(ui->c,"允许此插件在兼容环境中运行？","第三方插件可能读取项目数据或使用宿主服务。仅启用你信任的版本。","启用"))return;
    if((!strcmp(op,"catalog.npm.versions")||!strcmp(op,"catalog.install")) &&
       !ui_confirm(ui->c,"安装选中的软件包？","请核对发布者与版本。未经验证的插件请谨慎安装。","继续"))return;
    if(!strcmp(op,"extensions.bind")||!strcmp(op,"extensions.uninstall")||!strcmp(op,"compat.disable"))copilot_surface_close_all(ui->c);
    job(ui,g_str_has_prefix(op,"catalog."),json_object_get(r));
}
static void enable(GtkButton *b,Extensions *ui){
    (void)b;char *system=g_find_program_in_path("node");
    char *path=ui_prompt(ui->c,"Node 可执行路径（留空使用托管环境）",system,FALSE);g_free(system);
    if(path){UiJson *r=ui_op("compat.detect");ui_string(r,"node",path);g_free(path);job(ui,FALSE,r);}
}
static void python(GtkButton *b,Extensions *ui){
    (void)b;char *found=g_find_program_in_path("python3"),*path=ui_prompt(ui->c,"Python 解释器",found,FALSE);g_free(found);
    if(path){UiJson *r=ui_op("python.configure");ui_string(r,"executable",path);g_free(path);job(ui,FALSE,r);}
}
static void package_import(GtkButton *b,Extensions *ui){
    (void)b;GtkWidget *d=gtk_file_chooser_dialog_new("选择本地扩展目录",ui->c->window,GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,"取消",GTK_RESPONSE_CANCEL,"导入",GTK_RESPONSE_ACCEPT,NULL);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT){char *path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));UiJson *r=ui_op("extensions.import");ui_string(r,"path",path);g_free(path);job(ui,FALSE,r);}
    gtk_widget_destroy(d);
}
static void bind_version(GtkButton *b,Extensions *ui){
    UiJson *v=param(b);char *version=choose(ui,"使用已下载的版本",ui_get(v,"versions"));
    if(!version)return;
    gboolean enabled=json_object_get_boolean(ui_get(v,"enabled"));
    if(enabled && !ui_confirm(ui->c,"切换已启用插件的版本？","选中的版本将替换当前运行版本，并获得该插件的宿主权限。","切换版本")){g_free(version);return;}
    UiJson *r=reference("extensions.bind",v);ui_string(r,"version",version);g_free(version);
    json_object_object_add(r,"enabled",json_object_new_boolean(enabled));json_object_object_add(r,"acceptHostPermissions",json_object_new_boolean(enabled));
    copilot_surface_close_all(ui->c);job(ui,FALSE,r);
}
static void open_surface(GtkButton *b,Extensions *ui){
    UiJson *v=param(b);copilot_surface_open(ui->c,ui_get(v,"client"),ui_text(v,"slot"),ui->assets);
}
static void add_source(GtkButton *b,Extensions *ui){
    gboolean signed_source=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),"signed"));
    char *title=ui_prompt(ui->c,"目录名称","",FALSE),*url=NULL,*key=NULL;
    if(title && *title)url=ui_prompt(ui->c,"目录 HTTPS 地址","https://",FALSE);
    if(signed_source && url && *url)key=ui_prompt(ui->c,"发布者 Ed25519 公钥（十六进制）","",FALSE);
    if(title && url && (!signed_source || key)){
        UiJson *r=ui_op(signed_source?"catalog.add":"catalog.community.add");char *id=g_uuid_string_random();
        ui_string(r,"id",id);g_free(id);ui_string(r,"title",title);ui_string(r,"url",url);if(key)ui_string(r,"publicKey",key);job(ui,TRUE,r);
    }
    g_free(title);g_free(url);g_free(key);
}
static void inventory(Extensions *ui){
    ui_button(ui->body,"从本地目录导入…",G_CALLBACK(package_import),ui);
    UiJson *rows=ui_call(ui->c,UI_EXTENSIONS,ui_op("extensions.list"));
    if(rows&&!json_object_array_length(rows))ui_row(ui->body,"尚未安装插件","可在商店选择软件包，或导入本地目录。");
    for(gsize i=0;rows && i<(gsize)json_object_array_length(rows);i++){
        UiJson *v=json_object_array_get_idx(rows,i),*versions=ui_get(v,"versions"),*selected=NULL;
        for(gsize k=0;versions && k<(gsize)json_object_array_length(versions);k++){UiJson *ver=json_object_array_get_idx(versions,k);if(!strcmp(ui_text(ver,"version"),ui_text(v,"version")))selected=ver;}
        gboolean enabled=json_object_get_boolean(ui_get(v,"enabled")),quarantine=json_object_get_boolean(ui_get(selected,"quarantined"));
        UiJson *manifest=ui_get(selected,"manifest");const char *name=ui_text(manifest,"displayName");
        gchar *line=g_strdup_printf("%s · %s%s%s",ui_text(v,"version"),enabled?"已启用":"已停用",quarantine?" · ":"",quarantine?ui_text(selected,"reason"):"");
        GtkWidget *row=ui_row(ui->body,*name?name:ui_text(v,"package"),line);g_free(line);
        UiJson *r=reference("extensions.bind",v);json_object_object_add(r,"enabled",json_object_new_boolean(!enabled));json_object_object_add(r,"acceptHostPermissions",json_object_new_boolean(!enabled));
        GtkWidget *toggle=action(row,enabled?"停用":"启用",G_CALLBACK(run),ui,r);
        if(!enabled && !strcmp(ui_text(ui_get(manifest,"engines"),"host"),"node-dsh") && !ui->ready)gtk_widget_set_sensitive(toggle,FALSE);
        action(row,"版本",G_CALLBACK(bind_version),ui,json_object_get(v));action(row,"卸载",G_CALLBACK(run),ui,reference("extensions.uninstall",v));
    }
    json_object_put(rows);
    UiJson *clients=ui_call(ui->c,UI_EXTENSIONS,ui_op("extensions.clients"));
    for(gsize i=0;clients && i<(gsize)json_object_array_length(clients);i++){
        UiJson *v=json_object_array_get_idx(clients,i),*slots=ui_get(v,"slots");
        for(gsize j=0;slots && j<(gsize)json_object_array_length(slots);j++){
            const char *slot=json_object_get_string(json_object_array_get_idx(slots,j));
            GtkWidget *row=ui_row(ui->body,ui_text(v,"package"),slot);UiJson *args=json_object_new_object();
            ui_string(args,"slot",slot);json_object_object_add(args,"client",json_object_get(v));action(row,"打开工作台",G_CALLBACK(open_surface),ui,args);
            if(!strcmp(slot,"dunlin.workbench")){
                UiJson *overlay=json_object_new_object();ui_string(overlay,"slot","shell.overlay");json_object_object_add(overlay,"client",json_object_get(v));
                GtkWidget *float_button=action(row,"桌面悬浮",G_CALLBACK(open_surface),ui,overlay);
                gtk_widget_set_tooltip_text(float_button,"显示插件注册的桌面悬浮内容");
            }
        }
    }
    json_object_put(clients);
}
static void source_list(Extensions *ui,gboolean signed_source){
    GtkWidget *add=ui_button(ui->body,signed_source?"添加签名商店…":"添加社区来源…",G_CALLBACK(add_source),ui);
    g_object_set_data(G_OBJECT(add),"signed",GINT_TO_POINTER(signed_source));
    UiJson *rows=ui_call(ui->c,UI_CATALOG,ui_op(signed_source?"catalog.list":"catalog.community.list"));
    for(gsize i=0;rows && i<(gsize)json_object_array_length(rows);i++){
        UiJson *v=json_object_array_get_idx(rows,i);GtkWidget *row=ui_row(ui->body,ui_text(v,"title"),ui_text(v,"url"));
        UiJson *sync=ui_op(signed_source?"catalog.refresh":"catalog.community.sync");ui_string(sync,"id",ui_text(v,"id"));action(row,"同步",G_CALLBACK(run),ui,sync);
        gboolean hidden=signed_source&&!json_object_get_boolean(ui_get(v,"enabled"));
        UiJson *remove=ui_op(signed_source?(hidden?"catalog.restore":"catalog.remove"):"catalog.community.remove");
        ui_string(remove,"id",ui_text(v,"id"));action(row,hidden?"恢复":"移除来源",G_CALLBACK(run),ui,remove);
        UiJson *entries=ui_get(v,"entries");
        for(gsize j=0;entries && j<(gsize)json_object_array_length(entries);j++){
            UiJson *e=json_object_array_get_idx(entries,j);
            const char *reason=ui_text(e,"compatibilityReason");GtkWidget *item=ui_row(ui->body,ui_text(e,signed_source?"displayName":"title"),*reason?reason:ui_text(e,"description"));
            if(signed_source){UiJson *versions=ui_get(e,"versions");
                for(gsize k=0;versions && k<(gsize)json_object_array_length(versions);k++){
                    UiJson *ver=json_object_array_get_idx(versions,k),*r=ui_op("catalog.install");ui_string(r,"source",ui_text(v,"id"));ui_string(r,"package",ui_text(e,"packageId"));ui_string(r,"version",ui_text(ver,"version"));
                    GtkWidget *bt=action(item,ui_text(ver,"version"),G_CALLBACK(run),ui,r);gtk_widget_set_sensitive(bt,json_object_get_boolean(ui_get(ver,"installable")));
                    gtk_widget_set_tooltip_text(bt,ui_text(ver,"reason"));
                }
            }else{UiJson *r=ui_op("catalog.npm.versions");ui_string(r,"package",ui_text(e,"name"));ui_string(r,"repository",ui_text(e,"url"));
                GtkWidget *bt=action(item,"选择版本…",G_CALLBACK(run),ui,r);gtk_widget_set_sensitive(bt,ui->ready);gtk_widget_set_tooltip_text(bt,ui->ready?"未经验证的插件请谨慎安装":"请先开启兼容环境");}
        }
    }
    json_object_put(rows);
}
static void faults(Extensions *ui){
    UiJson *rows=ui_call(ui->c,UI_EXTENSIONS,ui_op("extensions.faults"));
    if(rows && !json_object_array_length(rows))ui_row(ui->body,"暂无插件错误",NULL);
    for(gsize i=0;rows && i<(gsize)json_object_array_length(rows);i++){
        UiJson *v=json_object_array_get_idx(rows,i);gchar *text=g_strdup_printf("%s · %s\n%s\n%s",ui_text(v,"version"),ui_text(v,"phase"),ui_text(v,"error"),ui_text(v,"diagnostic"));
        ui_row(ui->body,ui_text(v,"package"),text);g_free(text);
    }
    json_object_put(rows);
}
static void render(Extensions *ui){
    if(ui->c->busy){copilot_status(ui->c,"任务运行中，结束后可刷新插件列表。");return;}
    ui_clear(ui->body);UiJson *status=ui_call(ui->c,UI_EXTENSIONS,ui_op("compat.status"));ui->ready=!strcmp(ui_text(status,"state"),"ready");
    switch(gtk_combo_box_get_active(GTK_COMBO_BOX(ui->section))){
    case 0:inventory(ui);break;
    case 1:source_list(ui,FALSE);break;
    case 2:source_list(ui,TRUE);break;
    case 3:{
        GtkWidget *row=ui_row(ui->body,"DSH 兼容环境",ui->ready?"已开启":ui_text(status,"state"));
        if(ui->ready)action(row,"停用",G_CALLBACK(run),ui,ui_op("compat.disable"));else ui_button(row,"检测并开启…",G_CALLBACK(enable),ui);
        if(*ui_text(status,"error"))ui_row(ui->body,"环境诊断",ui_text(status,"error"));
        UiJson *p=ui_call(ui->c,UI_EXTENSIONS,ui_op("python.status"));gboolean healthy=json_object_get_boolean(ui_get(p,"healthy"));
        row=ui_row(ui->body,"Python 协作引擎",healthy?ui_text(p,"executable"):"尚未开启");
        if(healthy)action(row,"停用",G_CALLBACK(run),ui,ui_op("python.disable"));else ui_button(row,"选择解释器…",G_CALLBACK(python),ui);
        json_object_put(p);break;}
    case 4:faults(ui);break;
    }
    json_object_put(status);gtk_widget_show_all(ui->body);
}
static void refresh(GtkWidget *b,Extensions *ui){(void)b;render(ui);}
static void cancel(GtkButton *b,Extensions *ui){(void)b;g_atomic_int_set(&ui->c->service_cancel,1);}
GtkWidget *copilot_extensions_new(LinuxClient *c){
    Extensions *ui=g_new0(Extensions,1);ui->c=c;GtkWidget *body,*page=ui_page("插件与商店",&body),*bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    char *exe=g_file_read_link("/proc/self/exe",NULL),*dir=exe?g_path_get_dirname(exe):NULL;
    if(dir){ui->assets=g_build_filename(dir,"ExtensionAssets",NULL);if(!g_file_test(ui->assets,G_FILE_TEST_IS_DIR))g_clear_pointer(&ui->assets,g_free);}
    if(!ui->assets)ui->assets=g_strdup("/usr/share/dunlin-copilot/ExtensionAssets");
    g_free(exe);g_free(dir);gtk_box_pack_start(GTK_BOX(body),bar,FALSE,FALSE,0);ui->section=gtk_combo_box_text_new();
    const char *names[]={"已安装","社区插件目录","签名商店与 Skills","兼容环境","报错日志"};
    for(guint i=0;i<G_N_ELEMENTS(names);i++)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->section),names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->section),0);gtk_box_pack_start(GTK_BOX(bar),ui->section,FALSE,FALSE,0);
    ui_button(bar,"刷新",G_CALLBACK(refresh),ui);ui_button(bar,"取消操作",G_CALLBACK(cancel),ui);
    ui->body=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_box_pack_start(GTK_BOX(body),ui->body,FALSE,FALSE,0);
    g_signal_connect(ui->section,"changed",G_CALLBACK(refresh),ui);g_object_set_data_full(G_OBJECT(page),"extensions",ui,release);return page;
}
void copilot_extensions_refresh(LinuxClient *c){if(c->extensions)render(g_object_get_data(G_OBJECT(c->extensions),"extensions"));}
void copilot_extensions_start(LinuxClient *c){
    Extensions *ui=g_object_get_data(G_OBJECT(c->extensions),"extensions");
    if(!g_file_test(ui->assets,G_FILE_TEST_IS_DIR))return;
    UiJson *r=ui_op("extensions.assets");ui_string(r,"path",ui->assets);UiJson *result=ui_call(c,UI_EXTENSIONS,r);json_object_put(result);
    ui->last_fault=-1;ui->timer=g_timeout_add_seconds(4,watch,ui);job(ui,FALSE,ui_op("extensions.restore"));
}
