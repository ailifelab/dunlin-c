#include "settings.h"
#include "dunlin/context_budget.h"
#include <libsecret/secret.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct json_object Json;
typedef struct {
    LinuxClient *client;
    GtkWidget *page, *providers, *models, *selected, *theme, *text_size, *notice;
    gboolean syncing;
    Json *theme_rows;
    GtkCssProvider *palette;
} Settings;

static const SecretSchema key_schema = {
    .name = "org.dunlin.Copilot.Provider",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {{"workspace", SECRET_SCHEMA_ATTRIBUTE_STRING},
                   {"credential", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}}
};

static Json *field(Json *obj, const char *key)
{
    Json *result = NULL;
    if (obj) json_object_object_get_ex(obj, key, &result);
    return result;
}
static const char *strfield(Json *obj, const char *key)
{
    Json *value = field(obj,key);
    return value && json_object_is_type(value,json_type_string) ? json_object_get_string(value) : "";
}
static gint64 number(Json *obj, const char *key) { return json_object_get_int64(field(obj,key)); }
static void putstr(Json *obj,const char *key,const char *s) { json_object_object_add(obj,key,json_object_new_string(s?s:"")); }
static void putint(Json *obj,const char *key,gint64 n) { json_object_object_add(obj,key,json_object_new_int64(n)); }
static Json *operation(const char *op) { Json *r=json_object_new_object();putstr(r,"op",op);return r; }
static Json *lookup(Json *rows,const char *id)
{
    if (!id || !json_object_is_type(rows,json_type_array)) return NULL;
    for (gsize i=0;i<(gsize)json_object_array_length(rows);i++) {
        Json *row=json_object_array_get_idx(rows,i);
        if (g_strcmp0(id,strfield(row,"id"))==0) return row;
    }
    return NULL;
}
static void copy_field(Json *to,const char *k,Json *from,const char *source)
{
    Json *v=field(from,source);
    if (v) json_object_object_add(to,k,json_object_get(v));
    else json_object_object_del(to,k);
}
/* All incoming requests are owned; only data is returned from the envelope. */
static Json *request(LinuxClient *client, Json *req, gchar **error)
{
    Json *value=NULL;
    g_mutex_lock(&client->lock);
    char *raw=dc_workspace_request(client->store,json_object_to_json_string_ext(req,JSON_C_TO_STRING_PLAIN));
    g_mutex_unlock(&client->lock);
    Json *reply=raw?json_tokener_parse(raw):NULL;
    if (reply && json_object_get_boolean(field(reply,"ok"))) value=json_object_get(field(reply,"data"));
    else if (error) *error=g_strdup(reply?strfield(reply,"error"):"configuration_storage_error");
    json_object_put(reply);
    dc_free(raw);
    json_object_put(req);
    return value;
}
static void notify(Settings *s,const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(s->notice),message?message:"");
    copilot_status(s->client,message?message:"");
}
static void failed(Settings *s,const gchar *title,gchar *error)
{
    gchar *message=g_strdup_printf("%s · %s",title,error?error:"本地服务不可用");
    notify(s,message);g_free(error);g_free(message);
}
static gboolean editable(Settings *s)
{
    if (!s->client->busy) return TRUE;
    notify(s,"当前任务仍在运行，请在回合结束后修改配置。");return FALSE;
}
static gchar *workspace_key(LinuxClient *c)
{
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256,c->data_dir,-1);
}
static gchar *secret_get(LinuxClient *c,const gchar *id,GError **err)
{
    gchar *scope=workspace_key(c);
    gchar *value=secret_password_lookup_sync(&key_schema,NULL,err,"workspace",scope,"credential",id,NULL);
    g_free(scope);return value;
}
static gboolean secret_write(LinuxClient *c,const gchar *id,const gchar *key,GError **err)
{
    gchar *scope=workspace_key(c);
    gboolean ok=secret_password_store_sync(&key_schema,SECRET_COLLECTION_DEFAULT,"Dunlin Copilot 服务商凭据",
                                           key,NULL,err,"workspace",scope,"credential",id,NULL);
    g_free(scope);return ok;
}
static gboolean secret_remove(LinuxClient *c,const gchar *id)
{
    if (!id || !*id) return TRUE;
    gchar *scope=workspace_key(c);GError *error=NULL;
    secret_password_clear_sync(&key_schema,NULL,&error,"workspace",scope,"credential",id,NULL);
    g_free(scope);gboolean ok=error==NULL;g_clear_error(&error);return ok;
}
static gchar *pref_path(LinuxClient *c) {return g_build_filename(c->data_dir,"linux-settings.ini",NULL);}
gchar *copilot_pref_read(LinuxClient *c,const gchar *kind)
{
    GKeyFile *kf=g_key_file_new();gchar *p=pref_path(c);
    g_key_file_load_from_file(kf,p,G_KEY_FILE_NONE,NULL);
    gchar *v=g_key_file_get_string(kf,"interface",kind,NULL);
    g_key_file_unref(kf);g_free(p);return v;
}
gboolean copilot_pref_save(LinuxClient *c,const gchar *kind,const gchar *val)
{
    gchar *p=pref_path(c);GKeyFile *kf=g_key_file_new();
    g_key_file_load_from_file(kf,p,G_KEY_FILE_NONE,NULL);
    g_key_file_set_string(kf,"interface",kind,val);
    gsize n=0;gchar *data=g_key_file_to_data(kf,&n,NULL);
    gboolean ok=g_file_set_contents(p,data,(gssize)n,NULL);
    g_free(data);g_key_file_unref(kf);g_free(p);return ok;
}
static gchar *source_key(LinuxClient *c,const gchar *chat)
{
    return g_strdup_printf("%s\n%s",c->data_dir,chat?chat:"");
}
void copilot_settings_set_sources(LinuxClient *c,const gchar *chat,Json *selection)
{
    if(!chat||!*chat||!c->window)return;
    GHashTable *choices=g_object_get_data(G_OBJECT(c->window),"dunlin-source-selections");
    if(!choices){
        choices=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
        g_object_set_data_full(G_OBJECT(c->window),"dunlin-source-selections",choices,(GDestroyNotify)g_hash_table_unref);
    }
    gchar *key=source_key(c,chat);
    if(selection)g_hash_table_replace(choices,key,g_strdup(json_object_to_json_string_ext(selection,JSON_C_TO_STRING_PLAIN)));
    else {g_hash_table_remove(choices,key);g_free(key);}
}
void copilot_settings_consume_sources(LinuxClient *c,const gchar *chat) {copilot_settings_set_sources(c,chat,NULL);}
static Json *source_selection(LinuxClient *c,const gchar *chat)
{
    if(!c->window || !chat)return NULL;
    GHashTable *choices=g_object_get_data(G_OBJECT(c->window),"dunlin-source-selections");
    if(!choices)return NULL;
    gchar *key=source_key(c,chat);const char *raw=g_hash_table_lookup(choices,key);
    Json *value=raw?json_tokener_parse(raw):NULL;g_free(key);return value;
}
static gboolean uses_secret(Json *cfg)
{
    return g_strcmp0(strfield(cfg,"desktop_auth"),"none")!=0
        && (g_strcmp0(strfield(cfg,"desktop_auth"),"bearer")==0 || number(cfg,"desktop_credential_revision")>0);
}
static void runtime_budget(LinuxClient *client,Json *cfg,Json *model)
{
    guint64 output=(guint64)number(model,"maxOutput");
    dc_context_capabilities caps={.model_limit=(guint64)number(model,"contextLimit"),.deployment_limit=(guint64)number(model,"deploymentLimit"),
        .selected_limit=(guint64)number(model,"contextBudget"),.client_limit=4194304,.output_limit=(guint64)number(model,"outputLimit"),.requested_output=output,
        .overhead_reserve=field(model,"contextReserve")?(guint64)number(model,"contextReserve"):512};
    guint64 advertised=(guint64)number(model,"advertisedOutputLimit");
    if (advertised && (!caps.output_limit || caps.output_limit>advertised)) caps.output_limit=advertised;
    if (!caps.output_limit) caps.output_limit=65536;
    dc_context_range range;
    if (dc_context_budget(&caps,&range)!=DC_CONTEXT_VALID) return;
    Json *policy=json_object_new_object();
    putint(policy,"modelLimit",caps.model_limit);putint(policy,"deploymentLimit",caps.deployment_limit);
    putint(policy,"selectedLimit",range.selected);putint(policy,"outputLimit",caps.output_limit);
    putint(policy,"requestedOutput",output);putint(policy,"reserve",caps.overhead_reserve);
    gchar *preference=copilot_pref_read(client,"compression"),*automatic=copilot_pref_read(client,"automatic-compression");
    const char *strategy=strfield(model,"compactionStrategy");
    if (!*strategy && preference)strategy=preference;
    if (!g_strcmp0(strategy,"external")){
        gchar *ref=copilot_pref_read(client,"compression-extension");Json *external=ref?json_tokener_parse(ref):NULL;
        if (external)json_object_object_add(policy,"external",external);
        g_free(ref);
    }
    putstr(policy,"strategy",*strategy?strategy:"semantic");json_object_object_add(policy,"automatic",json_object_new_boolean(g_strcmp0(automatic,"false")!=0));g_free(preference);g_free(automatic);
    json_object_object_add(policy,"threshold",json_object_new_double(0.8));putint(policy,"retainTurns",2);putint(policy,"summaryOutput",MIN(range.output,1024));
    putint(cfg,"max_tokens",range.output);json_object_object_add(cfg,"desktop_context",policy);
}

static gchar *resolve_model(LinuxClient *c,const gchar *explicit_model,gchar **credential)
{
    if (!credential) return NULL;
    *credential=NULL;
    Json *registry=request(c,operation("config.list"),NULL);
    if (!registry) return NULL;
    gchar *id=g_strdup(explicit_model?explicit_model:c->model_id);
    if (!explicit_model && c->chat_id && *c->chat_id) {
        Json *q=operation("chats.metadata");putstr(q,"id",c->chat_id);
        Json *meta=request(c,q,NULL);
        if (!meta) {g_free(id);json_object_put(registry);return NULL;}
        const gchar *bound=strfield(meta,"model");
        g_free(id);id=*bound?g_strdup(bound):NULL;
        json_object_put(meta);
    }
    if (!explicit_model && (!c->chat_id || !*c->chat_id) && (!id || !*id)) {g_free(id);id=copilot_pref_read(c,"default-model");}
    Json *models=field(registry,"models");
    Json *model=lookup(models,id);
    if (!model) {g_free(id);json_object_put(registry);return NULL;}
    Json *q=operation("config.model");putstr(q,"id",id);putint(q,"revision",number(registry,"revision"));
    Json *cfg=request(c,q,NULL);g_free(id);
    if (!cfg) {json_object_put(registry);return NULL;}
    gboolean needs=uses_secret(cfg);const char *credential_id=strfield(cfg,"desktop_credential_id");
    if (!*credential_id) credential_id=strfield(model,"id");
    char *key=NULL;GError *err=NULL;
    if (needs) key=secret_get(c,credential_id,&err);
    if ((needs&&(!key||!*key))||err) {g_clear_error(&err);if(key)secret_password_free(key);json_object_put(cfg);json_object_put(registry);return NULL;}
    runtime_budget(c,cfg,model);
    Json *selection=explicit_model?NULL:source_selection(c,c->chat_id);
    if(selection&&!field(cfg,"desktop_context")){
        json_object_put(selection);if(key)secret_password_free(key);json_object_put(cfg);json_object_put(registry);return NULL;
    }
    if(selection)json_object_object_add(field(cfg,"desktop_context"),"reinclude",selection);
    *credential=g_strdup(key?key:"");if(key)secret_password_free(key);
    gchar *mem=copilot_pref_read(c,"memory"),*rag=copilot_pref_read(c,"rag");
    if (!g_strcmp0(mem,"true") || !g_strcmp0(rag,"true")) {
        Json *knowledge=json_object_new_object();json_object_object_add(knowledge,"memory",json_object_new_boolean(!g_strcmp0(mem,"true")));
        json_object_object_add(knowledge,"rag",json_object_new_boolean(!g_strcmp0(rag,"true")));json_object_object_add(cfg,"desktop_knowledge",knowledge);
    }
    g_free(mem);g_free(rag);
    char *result=g_strdup(json_object_to_json_string_ext(cfg,JSON_C_TO_STRING_PLAIN));
    json_object_put(cfg);json_object_put(registry);return result;
}

static GtkWidget *label(const gchar *text)
{
    GtkWidget *w=gtk_label_new(text);gtk_label_set_xalign(GTK_LABEL(w),0);
    gtk_label_set_line_wrap(GTK_LABEL(w),TRUE);return w;
}
static GtkWidget *form(GtkWidget *dialog)
{
    GtkWidget *grid=gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid),10);gtk_grid_set_column_spacing(GTK_GRID(grid),12);
    gtk_container_set_border_width(GTK_CONTAINER(grid),16);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),grid,TRUE,TRUE,0);
    return grid;
}
static void form_row(GtkWidget *grid,int row,const char *title,GtkWidget *control)
{
    GtkWidget *l=label(title);gtk_widget_set_halign(l,GTK_ALIGN_START);gtk_grid_attach(GTK_GRID(grid),l,0,row,1,1);
    gtk_widget_set_hexpand(control,TRUE);gtk_grid_attach(GTK_GRID(grid),control,1,row,1,1);
}
static GtkWidget *editor(Settings *s,const char *title)
{
    GtkWidget *d=gtk_dialog_new_with_buttons(title,s->client->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
                                              "取消",GTK_RESPONSE_CANCEL,"保存",GTK_RESPONSE_ACCEPT,NULL);
    gtk_window_set_default_size(GTK_WINDOW(d),470,-1);return d;
}
static GtkWidget *entry(const char *text) {GtkWidget *e=gtk_entry_new();gtk_entry_set_text(GTK_ENTRY(e),text?text:"");return e;}
static const char *typed(GtkWidget *e) {return gtk_entry_get_text(GTK_ENTRY(e));}
static void item(GtkWidget *combo,const char *id,const char *title) {gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),id,title);}
static const char *chosen(GtkWidget *combo) {const char *v=gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));return v?v:"";}
static Json *save_registry(Settings *s,Json *r,gchar **err)
{
    Json *q=operation("config.save");
    copy_field(q,"revision",r,"revision");copy_field(q,"providers",r,"providers");copy_field(q,"models",r,"models");
    return request(s->client,q,err);
}
static void link_provider(Json *m,Json *p)
{
    gboolean route=g_strcmp0(strfield(m,"endpoint"),strfield(p,"endpoint")) ||
                   g_strcmp0(strfield(m,"protocolName"),strfield(p,"protocolName"));
    if (route) {
        const char *limits[]={"contextLimit","contextSource","advertisedOutputLimit","contextBudget","deploymentLimit","outputLimit",
                              "reasoningSupported","reasoningCapabilities","reasoningEffort","inputModalities","temperature","enableThinking","chatTemplate",NULL};
        for (size_t i=0;limits[i];i++) json_object_object_del(m,limits[i]);
    }
    const char *fields[]={"endpoint","protocolName","authentication","credentialID","credentialRevision",NULL};
    for (size_t i=0;fields[i];i++) copy_field(m,fields[i],p,fields[i]);
    json_object_object_del(m,"keyEnvironment");putstr(m,"providerID",strfield(p,"id"));
}
static Json *clone_row(Json *row)
{
    return row?json_tokener_parse(json_object_to_json_string_ext(row,JSON_C_TO_STRING_PLAIN)):json_object_new_object();
}
static void upsert(Json *rows,Json *row)
{
    for (gsize i=0;i<(gsize)json_object_array_length(rows);i++) {
        if (g_strcmp0(strfield(json_object_array_get_idx(rows,i),"id"),strfield(row,"id"))==0) {
            json_object_array_put_idx(rows,i,row);return;
        }
    }
    json_object_array_add(rows,row);
}
static void provider_editor(Settings *s,const char *id)
{
    if (!editable(s)) return;
    gchar *err=NULL;Json *base=request(s->client,operation("config.list"),&err);
    if (!base) {failed(s,"无法读取服务商",err);return;}
    Json *old=id?lookup(field(base,"providers"),id):NULL;
    if (id&&!old) {json_object_put(base);notify(s,"服务商已发生变化，请刷新。");return;}
    GtkWidget *d=editor(s,old?"编辑服务商":"添加服务商"),*grid=form(d);
    GtkWidget *name=entry(strfield(old,"name")),*url=entry(old?strfield(old,"endpoint"):"http://127.0.0.1:8080/v1");
    GtkWidget *proto=gtk_combo_box_text_new(),*auth=gtk_combo_box_text_new(),*key=entry("");
    const char *pr[][2]={{"chat","Chat Completions"},{"responses","OpenAI Responses"},{"anthropic","Anthropic Messages"},{"gemini","Gemini"},{"ollama","Ollama"}};
    for (size_t i=0;i<G_N_ELEMENTS(pr);i++) item(proto,pr[i][0],pr[i][1]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(proto),old?strfield(old,"protocolName"):"chat");
    item(auth,"none","无凭据");item(auth,"bearer","系统钥匙串中的 API 密钥");item(auth,"auto","按协议自动认证");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(auth),old&&*strfield(old,"authentication")?strfield(old,"authentication"):old?"auto":"none");
    gtk_entry_set_visibility(GTK_ENTRY(key),FALSE);gtk_entry_set_input_purpose(GTK_ENTRY(key),GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_placeholder_text(GTK_ENTRY(key),old?"留空以保留当前凭据":"输入服务商 API 密钥");
    form_row(grid,0,"名称",name);form_row(grid,1,"连接地址",url);form_row(grid,2,"协议",proto);
    form_row(grid,3,"认证方式",auth);form_row(grid,4,"API 密钥",key);
    GtkWidget *feedback=label("");gtk_grid_attach(GTK_GRID(grid),feedback,0,5,2,1);
    gtk_widget_show_all(d);
    while (gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT && editable(s)) {
        const char *token=typed(key),*mode=chosen(auth);
        const char *prior=strfield(old,"credentialID");
        if (!*typed(name)||!*typed(url)||(!strcmp(mode,"none")&&*token) ||
            (strlen(token)>16384 || strpbrk(token,"\r\n")) ||
            (!strcmp(mode,"bearer")&&!*token&&!*prior)) {
            gtk_label_set_text(GTK_LABEL(feedback),"请填写名称和地址，并为所选认证方式配置有效的密钥。");continue;
        }
        gboolean changed_route=old && (g_strcmp0(strfield(old,"endpoint"),typed(url))||g_strcmp0(strfield(old,"protocolName"),chosen(proto)));
        if (changed_route && *prior && !*token && strcmp(mode,"none")!=0) {
            gtk_label_set_text(GTK_LABEL(feedback),"连接地址或协议已更改，请为新连接重新输入密钥。");continue;
        }
        Json *draft=clone_row(base),*p=clone_row(old);
        gchar *fresh=g_uuid_string_random(),*new_key=NULL;
        putstr(p,"id",old?id:fresh);g_free(fresh);putstr(p,"name",typed(name));putstr(p,"endpoint",typed(url));putstr(p,"protocolName",chosen(proto));putstr(p,"authentication",mode);
        json_object_object_del(p,"keyEnvironment");
        if (*token) {
            GError *key_error=NULL;new_key=g_uuid_string_random();
            if (!secret_write(s->client,new_key,token,&key_error)) {
                gtk_label_set_text(GTK_LABEL(feedback),"无法写入系统钥匙串。请解锁当前登录会话的 Secret Service 后重试。");
                g_clear_error(&key_error);g_free(new_key);json_object_put(p);json_object_put(draft);continue;
            }
            putstr(p,"credentialID",new_key);putint(p,"credentialRevision",1+number(old,"credentialRevision"));
        } else if (!strcmp(mode,"none")) {
            json_object_object_del(p,"credentialID");json_object_object_del(p,"credentialRevision");
        }
        Json *models=field(draft,"models");
        for (gsize i=0;i<(gsize)json_object_array_length(models);i++) {
            Json *m=json_object_array_get_idx(models,i);
            if (g_strcmp0(strfield(m,"providerID"),strfield(p,"id"))==0) link_provider(m,p);
        }
        upsert(field(draft,"providers"),p);
        Json *saved=save_registry(s,draft,&err);json_object_put(draft);
        if (saved) {
            gboolean cleanup=TRUE;
            if (*prior && (new_key || !strcmp(mode,"none"))) {
                gboolean referenced=FALSE;
                const char *groups[]={"models","providers"};
                for (size_t k=0;k<G_N_ELEMENTS(groups);k++) {
                    Json *list=field(saved,groups[k]);
                    for (gsize i=0;i<(gsize)json_object_array_length(list);i++)
                        if (g_strcmp0(strfield(json_object_array_get_idx(list,i),"credentialID"),prior)==0) referenced=TRUE;
                }
                if (!referenced) cleanup=secret_remove(s->client,prior);
            }
            json_object_put(saved);g_free(new_key);
            notify(s,cleanup?"服务商已保存。":"配置已保存，旧凭据未能从系统钥匙串删除。");break;
        }
        gboolean cleanup=!new_key||secret_remove(s->client,new_key);g_free(new_key);
        gtk_label_set_text(GTK_LABEL(feedback),cleanup?(err?err:"保存失败，请重新打开设置。"):"保存失败，新凭据仍需从系统钥匙串清理。");
        g_clear_pointer(&err,g_free);
    }
    gtk_entry_set_text(GTK_ENTRY(key),"");gtk_widget_destroy(d);json_object_put(base);copilot_settings_refresh(s->client);
}

typedef struct {
    GtkWidget *limit,*output,*budget,*caption;
    Json *old;
    GtkWidget *provider,*model;
} BudgetEditor;
static void bounds_changed(GtkSpinButton *unused,gpointer data)
{
    (void)unused;BudgetEditor *e=data;
    dc_context_capabilities c={.deployment_limit=(guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->limit)),.client_limit=4194304,
        .requested_output=(guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->output)),.output_limit=65536,.overhead_reserve=512};
    gboolean original=e->old && !g_strcmp0(chosen(e->provider),strfield(e->old,"providerID")) && !g_strcmp0(typed(e->model),strfield(e->old,"model"));
    if(original){
        c.model_limit=(guint64)number(e->old,"contextLimit");
        guint64 configured=(guint64)number(e->old,"outputLimit"), advertised=(guint64)number(e->old,"advertisedOutputLimit");
        if(configured)c.output_limit=MIN(c.output_limit,configured);
        if(advertised)c.output_limit=MIN(c.output_limit,advertised);
        if(field(e->old,"contextReserve"))c.overhead_reserve=(guint64)number(e->old,"contextReserve");
    }
    dc_context_range r;dc_context_status st=dc_context_editor_budget(&c,0,0,&r);gtk_widget_set_sensitive(e->budget,st==DC_CONTEXT_VALID);
    if (st!=DC_CONTEXT_VALID) {gtk_label_set_text(GTK_LABEL(e->caption),c.deployment_limit?"上下文上限不足以容纳输出和保留空间。":"上限未知。输入已确认的模型或部署限制后可选择预算。");return;}
    /* GtkRange requires distinct endpoints even when only one value is possible. */
    gtk_range_set_range(GTK_RANGE(e->budget),r.minimum,r.maximum>r.minimum?r.maximum:r.minimum+1);
    gtk_widget_set_sensitive(e->budget,r.maximum>r.minimum);
    gtk_range_set_value(GTK_RANGE(e->budget),r.recommended);
    gchar *t=g_strdup_printf("可选范围 %"G_GUINT64_FORMAT" – %"G_GUINT64_FORMAT"；建议 %"G_GUINT64_FORMAT"。",r.minimum,r.maximum,r.recommended);
    gtk_label_set_text(GTK_LABEL(e->caption),t);g_free(t);
}
static void route_edited(GtkWidget *control,gpointer data)
{
    (void)control;BudgetEditor *e=data;
    gboolean original=e->old&&!g_strcmp0(chosen(e->provider),strfield(e->old,"providerID"))&&!g_strcmp0(typed(e->model),strfield(e->old,"model"));
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(e->limit),0,original&&number(e->old,"contextLimit")>0?number(e->old,"contextLimit"):4194304);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->limit),original?MAX(number(e->old,"deploymentLimit"),0):0);
    gint64 output=65536;
    if(original && number(e->old,"outputLimit")>0)output=MIN(output,number(e->old,"outputLimit"));
    if(original && number(e->old,"advertisedOutputLimit")>0)output=MIN(output,number(e->old,"advertisedOutputLimit"));
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(e->output),1,output);
    bounds_changed(NULL,e);
}
static void model_editor(Settings *s,const char *id)
{
    if (!editable(s)) return;
    gchar *err=NULL;Json *base=request(s->client,operation("config.list"),&err);
    if (!base) {failed(s,"无法读取模型",err);return;}
    Json *providers=field(base,"providers"),*old=id?lookup(field(base,"models"),id):NULL;
    if (!providers||!json_object_array_length(providers)) {json_object_put(base);notify(s,"请先添加一个服务商。");return;}
    GtkWidget *d=editor(s,old?"编辑模型":"添加模型"),*g=form(d),*provider=gtk_combo_box_text_new();
    GtkWidget *name=entry(strfield(old,"name")),*model=entry(strfield(old,"model"));
    for (gsize i=0;i<(gsize)json_object_array_length(providers);i++) {Json *p=json_object_array_get_idx(providers,i);item(provider,strfield(p,"id"),strfield(p,"name"));}
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(provider),strfield(old,"providerID"))) gtk_combo_box_set_active(GTK_COMBO_BOX(provider),0);
    BudgetEditor e={.old=old,.provider=provider,.model=model};e.output=gtk_spin_button_new_with_range(1,65536,1);e.limit=gtk_spin_button_new_with_range(0,4194304,1);
    e.budget=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,1,1);gtk_scale_set_digits(GTK_SCALE(e.budget),0);e.caption=label("");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e.output),old?MAX(1,number(old,"maxOutput")):4096);
    guint64 model_limit=(guint64)number(old,"contextLimit"),deployment=(guint64)number(old,"deploymentLimit");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e.limit),deployment?deployment:model_limit);
    form_row(g,0,"服务商",provider);form_row(g,1,"显示名称",name);form_row(g,2,"模型 ID",model);
    form_row(g,3,"最大输出",e.output);form_row(g,4,"已确认的上下文上限",e.limit);form_row(g,5,"上下文预算",e.budget);
    gtk_grid_attach(GTK_GRID(g),e.caption,0,6,2,1);
    g_signal_connect(e.limit,"value-changed",G_CALLBACK(bounds_changed),&e);g_signal_connect(e.output,"value-changed",G_CALLBACK(bounds_changed),&e);
    g_signal_connect(model,"changed",G_CALLBACK(route_edited),&e);g_signal_connect(provider,"changed",G_CALLBACK(route_edited),&e);
    route_edited(NULL,&e);if (old&&number(old,"contextBudget")>0)gtk_range_set_value(GTK_RANGE(e.budget),number(old,"contextBudget"));
    GtkWidget *feedback=label("");gtk_grid_attach(GTK_GRID(g),feedback,0,7,2,1);gtk_widget_show_all(d);
    while (gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT && editable(s)) {
        if (!*typed(name)||!*typed(model)) {gtk_label_set_text(GTK_LABEL(feedback),"请填写显示名称和服务端模型 ID。");continue;}
        Json *draft=clone_row(base),*m=clone_row(old),*p=lookup(providers,chosen(provider));
        gboolean changed_model=old&&g_strcmp0(typed(model),strfield(old,"model"))!=0;
        if (changed_model) {json_object_object_del(m,"contextLimit");json_object_object_del(m,"advertisedOutputLimit");json_object_object_del(m,"inputModalities");json_object_object_del(m,"reasoningCapabilities");json_object_object_del(m,"reasoningSupported");json_object_object_del(m,"reasoningEffort");}
        gchar *uuid=g_uuid_string_random();putstr(m,"id",old?id:uuid);g_free(uuid);
        link_provider(m,p);putstr(m,"name",typed(name));putstr(m,"model",typed(model));
        putint(m,"maxOutput",gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e.output)));
        gint limit=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e.limit));
        if (limit>0 || number(m,"contextLimit")>0) {
            if(limit>0)putint(m,"deploymentLimit",limit);else json_object_object_del(m,"deploymentLimit");
            putint(m,"contextBudget",(gint64)gtk_range_get_value(GTK_RANGE(e.budget)));
            putstr(m,"contextSource","explicit-deployment");
        } else {json_object_object_del(m,"deploymentLimit");json_object_object_del(m,"contextBudget");}
        upsert(field(draft,"models"),m);
        Json *saved=save_registry(s,draft,&err);json_object_put(draft);
        if (saved) {json_object_put(saved);notify(s,"模型已保存。");break;}
        gtk_label_set_text(GTK_LABEL(feedback),err?err:"无法保存模型。");g_clear_pointer(&err,g_free);
    }
    gtk_widget_destroy(d);json_object_put(base);copilot_settings_refresh(s->client);
}
static void switch_model(GtkButton *unused,gpointer data)
{
    (void)unused;Settings *s=data;LinuxClient *c=s->client;
    if (!editable(s)) return;
    gchar *id=g_strdup(chosen(s->selected)),*chat=g_strdup(c->chat_id),*err=NULL;
    if (!*id) {notify(s,"请先配置并选择一个模型。");goto end;}
    if (!chat || !*chat) {notify(s,"请打开要切换的对话。新聊天的模型可通过“设为默认”选择。");goto end;}
    Json *registry=request(c,operation("config.list"),&err);
    if (!registry) {failed(s,"无法读取模型",err);goto end;}
    gint64 rev=number(registry,"revision");json_object_put(registry);
    const char *policy=NULL;
    for (;;) {
        Json *q=operation("chats.model.preview");putstr(q,"chat",chat);putstr(q,"target",id);putint(q,"revision",rev);if (policy) putstr(q,"imagePolicy",policy);
        Json *plan=request(c,q,&err);
        if (!plan) {failed(s,"模型切换预览失败",err);break;}
        gboolean can=json_object_get_boolean(field(plan,"canSwitch"));
        const char *fit=strfield(plan,"contextFit");
        gchar *budget=json_object_get_boolean(field(plan,"contextKnown"))?
            g_strdup_printf("已选窗口 %"G_GINT64_FORMAT"，可用输入 %"G_GINT64_FORMAT"。",number(plan,"contextSelected"),number(plan,"inputBudget")):
            g_strdup("目标模型的上下文上限尚未确认。");
        gchar *details=g_strdup_printf("%s → %s（%s）\n\n原对话保留 %"G_GINT64_FORMAT" 条消息、%"G_GINT64_FORMAT" 项工具结果和 %"G_GINT64_FORMAT" 项文件回执。\n历史图片 %"G_GINT64_FORMAT" 张，待发送图片 %"G_GINT64_FORMAT" 张。\n%s%s%s%s",
            strfield(plan,"currentName"),strfield(plan,"targetName"),strfield(plan,"protocol"),number(plan,"messageCount"),number(plan,"toolResults"),number(plan,"fileReceipts"),
            number(plan,"imageCount"),number(plan,"pendingImageCount"),budget,!strcmp(fit,"requires_compaction")?" 历史可能超出输入预算，下一轮需要压缩。":"",
            !g_strcmp0(strfield(plan,"imagePolicy"),"textOnly")?" 下一轮仅发送文字，原图片保留。":"",can?"":" 当前图片或协议与目标模型不兼容。");
        g_free(budget);
        GtkWidget *d=gtk_message_dialog_new(c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
            can?GTK_MESSAGE_QUESTION:GTK_MESSAGE_WARNING,GTK_BUTTONS_NONE,"确认下一轮模型");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),"%s",details);g_free(details);
        gtk_dialog_add_button(GTK_DIALOG(d),"取消",GTK_RESPONSE_CANCEL);
        if (json_object_get_boolean(field(plan,"canTextOnly")) && g_strcmp0(strfield(plan,"imagePolicy"),"textOnly"))
            gtk_dialog_add_button(GTK_DIALOG(d),"仅文字预览",2);
        if (can) gtk_dialog_add_button(GTK_DIALOG(d),"确认切换",GTK_RESPONSE_ACCEPT);
        int response=gtk_dialog_run(GTK_DIALOG(d));gtk_widget_destroy(d);
        if (response==2) {json_object_put(plan);policy="textOnly";continue;}
        if (can && response==GTK_RESPONSE_ACCEPT && editable(s) && !g_strcmp0(chat,c->chat_id)) {
            q=operation("chats.model.apply");
            copy_field(q,"chat",plan,"chat");copy_field(q,"target",plan,"targetID");copy_field(q,"revision",plan,"configRevision");
            const char *keys[]={"historyHash","currentID","currentImagePolicy","imagePolicy","pendingID",NULL};
            for (int i=0;keys[i];i++) copy_field(q,keys[i],plan,keys[i]);
            Json *applied=request(c,q,&err);
            if (applied) {g_free(c->model_id);c->model_id=g_strdup(id);json_object_put(applied);copilot_chat_refresh(c);notify(s,"已切换下一轮模型，原始对话与工具记录保留。");}
            else failed(s,"模型切换未完成，请重新预览",err);
        }
        json_object_put(plan);break;
    }
end: g_free(chat);g_free(id);
}
gchar *copilot_read_local_utf8(const gchar *path,gsize limit)
{
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return NULL;
    struct stat st;
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<1 || st.st_size>(off_t)limit) {close(fd);return NULL;}
    char *data=g_malloc(limit+2);gsize count=0;
    while(count<=limit) {
        ssize_t got=read(fd,data+count,limit+1-count);
        if (got<0 && errno==EINTR)continue;
        if (got<0){count=limit+1;break;}
        if (!got)break;
        count+=(gsize)got;
    }
    close(fd);
    if (!count||count>limit||memchr(data,0,count)||!g_utf8_validate(data,(gssize)count,NULL)) {g_free(data);return NULL;}
    data[count]=0;return data;
}
static void import_file(GtkButton *unused,gpointer data)
{
    (void)unused;Settings *s=data;
    if (!editable(s))return;
    GtkWidget *file=gtk_file_chooser_dialog_new("导入本地模型配置",s->client->window,GTK_FILE_CHOOSER_ACTION_OPEN,"取消",GTK_RESPONSE_CANCEL,"检查",GTK_RESPONSE_ACCEPT,NULL);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(file),TRUE);
    int response=gtk_dialog_run(GTK_DIALOG(file));gchar *path=response==GTK_RESPONSE_ACCEPT?gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(file)):NULL;
    gtk_widget_destroy(file);if(!path)return;
    gchar *text=copilot_read_local_utf8(path,1048576);g_free(path);
    if(!text){notify(s,"请选择 1 MiB 以内的 UTF-8 配置文件。");return;}
    Json *q=operation("config.preview");putstr(q,"source",text);g_free(text);
    gchar *error=NULL;Json *plan=request(s->client,q,&error);
    if (!plan){failed(s,"配置预检失败",error);return;}
    GString *details=g_string_new("新增配置将与现有聊天分开保存。凭据需要另行关联到系统钥匙串。\n");
    const char *groups[]={"providers","models","warnings"};
    for (int k=0;k<3;k++) {
        Json *list=field(plan,groups[k]);if (!json_object_is_type(list,json_type_array))continue;
        for (gsize i=0;i<(gsize)json_object_array_length(list);i++) {
            Json *v=json_object_array_get_idx(list,i);
            if (k==0) g_string_append_printf(details,"\n服务商  %s · %s · %s",strfield(v,"name"),strfield(v,"protocolName"),strfield(v,"endpoint"));
            else if (k==1) g_string_append_printf(details,"\n模型  %s · %s",strfield(v,"name"),strfield(v,"model"));
            else g_string_append_printf(details,"\n%s",json_object_get_string(v));
        }
    }
    GtkWidget *d=gtk_dialog_new_with_buttons("确认导入配置",s->client->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消",GTK_RESPONSE_CANCEL,"确认导入",GTK_RESPONSE_ACCEPT,NULL);gtk_window_set_default_size(GTK_WINDOW(d),560,400);
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL),*content=label(details->str);gtk_container_set_border_width(GTK_CONTAINER(scroll),14);
    gtk_container_add(GTK_CONTAINER(scroll),content);gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),scroll,TRUE,TRUE,0);
    g_string_free(details,TRUE);gtk_widget_show_all(d);response=gtk_dialog_run(GTK_DIALOG(d));gtk_widget_destroy(d);
    if (response==GTK_RESPONSE_ACCEPT && editable(s)) {
        q=operation("config.apply");copy_field(q,"plan",plan,"plan");json_object_object_add(q,"confirmed",json_object_new_boolean(TRUE));
        Json *result=request(s->client,q,&error);
        if (result){json_object_put(result);copilot_settings_refresh(s->client);notify(s,"配置已导入。");}
        else failed(s,"导入未完成，请重新检查文件",error);
    }
    json_object_put(plan);
}
static void default_model(GtkButton *unused,gpointer data)
{
    (void)unused;Settings *s=data;const char *id=chosen(s->selected);
    if (!editable(s) || !*id) return;
    if (!copilot_pref_save(s->client,"default-model",id)) {notify(s,"默认模型写入失败。");return;}
    if (!s->client->chat_id || !*s->client->chat_id) {g_free(s->client->model_id);s->client->model_id=g_strdup(id);}
    notify(s,"已设为新聊天的默认模型。");
}
static Json *theme_row(Json *rows,const gchar *value,gboolean *ambiguous)
{
    Json *match=lookup(rows,value);*ambiguous=FALSE;if(match)return match;
    for(gsize i=0;rows && i<(gsize)json_object_array_length(rows);i++){
        Json *r=json_object_array_get_idx(rows,i);
        if(!g_strcmp0(strfield(r,"preference"),value)){
            if(match){*ambiguous=TRUE;return NULL;}match=r;
        }
    }
    return match;
}
static const char *color_token(Json *row,const char *name,const char *fallback)
{
    const char *hex=strfield(row,name);if(strlen(hex)!=7 || hex[0]!='#')return fallback;
    for(int i=1;i<7;i++)if(!g_ascii_isxdigit(hex[i]))return fallback;
    return hex;
}
static void draw_theme(Settings *s,const char *val,Json *row)
{
    GtkSettings *gtk=gtk_widget_get_settings(s->page);
    if(s->palette){
        gtk_style_context_remove_provider_for_screen(gtk_widget_get_screen(s->page),GTK_STYLE_PROVIDER(s->palette));
        g_clear_object(&s->palette);
    }
    if(!strcmp(val,"system"))gtk_settings_reset_property(gtk,"gtk-application-prefer-dark-theme");
    else g_object_set(gtk,"gtk-application-prefer-dark-theme",row?!strcmp(strfield(row,"scheme"),"dark"):!strcmp(val,"dark"),NULL);
    if(!row)return;
    gboolean dark=!strcmp(strfield(row,"scheme"),"dark");
    Json *tokens=field(row,"tokens");const char *background=color_token(tokens,"background",dark?"#1b1d21":"#f7f8fa");
    const char *surface=color_token(tokens,"surface",dark?"#2a2e35":"#ffffff"),*text=color_token(tokens,"text",dark?"#eeeeee":"#202124");
    const char *accent=color_token(row,"accent",dark?"#92baff":"#2671bd"),*secondary=color_token(tokens,"secondary",dark?"#b5b9bd":"#636970");
    gchar *css=g_strdup_printf("@define-color theme_bg_color %s; @define-color theme_fg_color %s; @define-color theme_base_color %s; @define-color theme_text_color %s; @define-color theme_selected_bg_color %s; window.background, stack { background-color:%s; color:%s; } list, textview text, entry { background-color:%s; color:%s; } .dim-label { color:%s; } selection, row:selected { background-color:%s; }",background,text,surface,text,accent,background,text,surface,text,secondary,accent);
    s->palette=gtk_css_provider_new();GError *err=NULL;
    if(gtk_css_provider_load_from_data(s->palette,css,-1,&err))gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(s->page),GTK_STYLE_PROVIDER(s->palette),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION+1);
    g_clear_error(&err);g_free(css);
}
static void refill_themes(Settings *s)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->theme));
    item(s->theme,"system","跟随系统");item(s->theme,"light","科技白");item(s->theme,"dark","科技黑");
    for(gsize i=0;s->theme_rows&&i<(gsize)json_object_array_length(s->theme_rows);i++){Json *r=json_object_array_get_idx(s->theme_rows,i);item(s->theme,strfield(r,"id"),strfield(r,"name"));}
}
static gboolean refresh_theme_inventory(LinuxClient *c)
{
    Settings *s=g_object_get_data(G_OBJECT(c->window),"dunlin-settings");if(!s||c->busy)return FALSE;
    if(!g_mutex_trylock(&c->lock))return FALSE;
    char *raw=dc_extensions_call(c->store,"{\"op\":\"extensions.themes\"}",NULL);g_mutex_unlock(&c->lock);
    Json *reply=raw?json_tokener_parse(raw):NULL;dc_free(raw);
    if(!reply||!json_object_get_boolean(field(reply,"ok"))||!json_object_is_type(field(reply,"data"),json_type_array)){json_object_put(reply);return FALSE;}
    json_object_put(s->theme_rows);s->theme_rows=json_object_get(field(reply,"data"));json_object_put(reply);
    gboolean previous=s->syncing,ambiguous=FALSE;s->syncing=TRUE;refill_themes(s);
    gchar *val=copilot_pref_read(c,"theme");const gchar *id=val?val:"system";Json *row=theme_row(s->theme_rows,id,&ambiguous);
    if(!row && strcmp(id,"system")&&strcmp(id,"dark")&&strcmp(id,"light")){
        id="system";copilot_pref_save(c,"theme",id);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->theme),row?strfield(row,"id"):id);
    draw_theme(s,id,row);g_free(val);s->syncing=previous;return TRUE;
}
void copilot_settings_refresh_themes(LinuxClient *c) { (void)refresh_theme_inventory(c); }
gboolean copilot_settings_apply_theme(LinuxClient *c,const gchar *preference)
{
    if(!preference||!*preference||!c->window || c->busy)return FALSE;
    Settings *s=g_object_get_data(G_OBJECT(c->window),"dunlin-settings");if(!s)return FALSE;
    gboolean builtin=!strcmp(preference,"system")||!strcmp(preference,"light")||!strcmp(preference,"dark"),ambiguous=FALSE;
    /* Recheck the live host inventory. A package can be disabled between a web event and its native delivery. */
    gchar *wanted=g_strdup(preference);
    if(!builtin && !refresh_theme_inventory(c)){g_free(wanted);return FALSE;}
    Json *row=theme_row(s->theme_rows,wanted,&ambiguous);
    if(!builtin && (!row||ambiguous)){g_free(wanted);return FALSE;}
    const char *id=builtin?wanted:strfield(row,"id");
    gboolean saved=copilot_pref_save(c,"theme",id);
    if(saved){gboolean was=s->syncing;s->syncing=TRUE;gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->theme),id);draw_theme(s,id,row);s->syncing=was;}
    g_free(wanted);return saved;
}
static void theme_changed(GtkComboBox *button,gpointer data)
{
    Settings *s=data;if(s->syncing)return;
    const char *val=gtk_combo_box_get_active_id(button);if(!val)return;
    if(!copilot_settings_apply_theme(s->client,val))notify(s,"主题已不可用或外观偏好未能写入。");
}
static void apply_text_size(Settings *s,const gchar *id)
{
    GtkStyleContext *style=gtk_widget_get_style_context(GTK_WIDGET(s->client->window));
    gtk_style_context_remove_class(style,"chat-text-small");
    gtk_style_context_remove_class(style,"chat-text-large");
    if(!g_strcmp0(id,"small"))gtk_style_context_add_class(style,"chat-text-small");
    else if(!g_strcmp0(id,"large"))gtk_style_context_add_class(style,"chat-text-large");
}
static void read_text_size(Settings *s)
{
    gchar *saved=copilot_pref_read(s->client,"text-size");
    gboolean previous=s->syncing;s->syncing=TRUE;
    if(!saved || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->text_size),saved))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->text_size),"standard");
    apply_text_size(s,gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->text_size)));
    s->syncing=previous;g_free(saved);
}
static void text_size_changed(GtkComboBox *combo,gpointer data)
{
    Settings *s=data;if(s->syncing)return;
    const gchar *id=gtk_combo_box_get_active_id(combo);if(!id)return;
    if(copilot_pref_save(s->client,"text-size",id))apply_text_size(s,id);
    else{read_text_size(s);notify(s,"文字大小未能保存。");}
}
static void free_settings(gpointer data)
{
    Settings *s=data;
    if(s->palette){
        GdkScreen *screen=gdk_screen_get_default();if(screen)gtk_style_context_remove_provider_for_screen(screen,GTK_STYLE_PROVIDER(s->palette));
        g_object_unref(s->palette);
    }
    json_object_put(s->theme_rows);g_free(s);
}
static GtkWidget *action(const char *title,GCallback cb,Settings *s)
{
    GtkWidget *b=gtk_button_new_with_label(title);
    g_signal_connect(b,"clicked",cb,s);return b;
}
static void add_provider(GtkButton *b,gpointer data){(void)b;provider_editor(data,NULL);}
static void add_model(GtkButton *b,gpointer data){(void)b;model_editor(data,NULL);}
static void edit_provider(GtkButton *b,gpointer data){provider_editor(data,g_object_get_data(G_OBJECT(b),"row-id"));}
static void edit_model(GtkButton *b,gpointer data){model_editor(data,g_object_get_data(G_OBJECT(b),"row-id"));}
static void clear_rows(GtkWidget *box)
{
    GList *list=gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *p=list;p;p=p->next)gtk_widget_destroy(GTK_WIDGET(p->data));
    g_list_free(list);
}
static void refresh_list(Settings *s,GtkWidget *box,Json *list,gboolean models)
{
    clear_rows(box);
    if (!list || !json_object_array_length(list)) {
        gtk_box_pack_start(GTK_BOX(box),label(models?"尚未配置模型。":"尚未配置服务商。"),FALSE,FALSE,0);
    } else for (gsize i=0;i<(gsize)json_object_array_length(list);i++) {
        Json *row=json_object_array_get_idx(list,i);const char *id=strfield(row,"id");
        GtkWidget *line=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
        gchar *desc=models?g_strdup_printf("%s · %s",strfield(row,"model"),strfield(row,"protocolName")):g_strdup(strfield(row,"endpoint"));
        GtkWidget *inner=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
        gtk_box_pack_start(GTK_BOX(inner),label(strfield(row,"name")),FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(inner),label(desc),FALSE,FALSE,0);g_free(desc);
        gtk_box_pack_start(GTK_BOX(line),inner,TRUE,TRUE,0);
        GtkWidget *edit=action("编辑",models?G_CALLBACK(edit_model):G_CALLBACK(edit_provider),s);
        g_object_set_data_full(G_OBJECT(edit),"row-id",g_strdup(id),g_free);
        gtk_box_pack_end(GTK_BOX(line),edit,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(box),line,FALSE,FALSE,0);
        if (models)item(s->selected,id,strfield(row,"name"));
    }
    gtk_widget_show_all(box);
}
void copilot_settings_refresh(LinuxClient *c)
{
    Settings *s=g_object_get_data(G_OBJECT(c->window),"dunlin-settings");
    if (!s) return;
    read_text_size(s);
    if(c->busy)return;
    gchar *err=NULL;Json *registry=request(c,operation("config.list"),&err);
    if (!registry){failed(s,"本地设置不可用",err);return;}
    s->syncing=TRUE;gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->selected));
    refresh_list(s,s->providers,field(registry,"providers"),FALSE);
    refresh_list(s,s->models,field(registry,"models"),TRUE);
    gchar *def=copilot_pref_read(c,"default-model");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->selected),c->model_id?c->model_id:"") && (!c->chat_id || !*c->chat_id) && def)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->selected),def);
    g_free(def);s->syncing=FALSE;json_object_put(registry);copilot_settings_refresh_themes(c);
}
static GtkWidget *section(GtkWidget *parent,const gchar *title,GtkWidget *button)
{
    GtkWidget *bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10),*heading=label(title);
    PangoAttrList *attrs=pango_attr_list_new();pango_attr_list_insert(attrs,pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(heading),attrs);pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(bar),heading,TRUE,TRUE,0);if(button)gtk_box_pack_end(GTK_BOX(bar),button,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(parent),bar,FALSE,FALSE,0);
    GtkWidget *content=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
    gtk_box_pack_start(GTK_BOX(parent),content,FALSE,FALSE,0);return content;
}
GtkWidget *copilot_settings_new(LinuxClient *client)
{
    Settings *s=g_new0(Settings,1);s->client=client;
    s->page=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s->page),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    g_object_set_data_full(G_OBJECT(client->window),"dunlin-settings",s,free_settings);
    GtkWidget *page=gtk_box_new(GTK_ORIENTATION_VERTICAL,16);gtk_container_set_border_width(GTK_CONTAINER(page),24);
    gtk_container_add(GTK_CONTAINER(s->page),page);
    s->providers=section(page,"服务商",action("添加服务商",G_CALLBACK(add_provider),s));
    s->models=section(page,"模型",action("添加模型",G_CALLBACK(add_model),s));
    GtkWidget *row=section(page,"模型选择",NULL),*line=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    s->selected=gtk_combo_box_text_new();gtk_box_pack_start(GTK_BOX(line),s->selected,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(line),action("切换当前聊天模型",G_CALLBACK(switch_model),s),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(line),action("设为默认",G_CALLBACK(default_model),s),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(row),line,FALSE,FALSE,0);
    row=section(page,"基础设置",NULL);s->theme=gtk_combo_box_text_new();
    item(s->theme,"system","跟随系统");item(s->theme,"light","科技白");item(s->theme,"dark","科技黑");
    g_signal_connect(s->theme,"changed",G_CALLBACK(theme_changed),s);line=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);
    gtk_box_pack_start(GTK_BOX(line),label("外观"),TRUE,TRUE,0);gtk_box_pack_end(GTK_BOX(line),s->theme,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(row),line,FALSE,FALSE,0);
    line=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);s->text_size=gtk_combo_box_text_new();
    item(s->text_size,"small","较小");item(s->text_size,"standard","标准");item(s->text_size,"large","较大");
    atk_object_set_name(gtk_widget_get_accessible(s->text_size),"文字大小");
    gtk_box_pack_start(GTK_BOX(line),label("文字大小"),TRUE,TRUE,0);gtk_box_pack_end(GTK_BOX(line),s->text_size,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(row),line,FALSE,FALSE,0);g_signal_connect(s->text_size,"changed",G_CALLBACK(text_size_changed),s);
    gtk_box_pack_start(GTK_BOX(row),action("导入本地配置…",G_CALLBACK(import_file),s),FALSE,FALSE,0);
    GtkWidget *media=section(page,"语音与视频",NULL);
    gtk_box_pack_start(GTK_BOX(media),label("TODO · 语音聊天与视频通话是尚未开发完成的试验性功能。"),FALSE,FALSE,0);
    s->notice=label("");gtk_box_pack_start(GTK_BOX(page),s->notice,FALSE,FALSE,0);
    copilot_settings_refresh(client);gtk_widget_show_all(s->page);return s->page;
}

gchar *copilot_settings_default_model(LinuxClient *c) { return copilot_pref_read(c,"default-model"); }

gchar *copilot_model_config(LinuxClient *c,gchar **credential) {return resolve_model(c,NULL,credential);}
gchar *copilot_model_config_for_id(LinuxClient *c,const gchar *id,gchar **credential) {return resolve_model(c,id?id:"",credential);}
