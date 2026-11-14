#include "pages.h"
#include "ui.h"
#include <libsecret/secret.h>
#include <string.h>
#include <time.h>

typedef struct {
    LinuxClient *client;
    gboolean grants;
    GtkWidget *rows,*query,*filter,*progress;
    gint64 cursor;
} Records;
static const SecretSchema oauth_schema={.name="org.dunlin.Copilot.OAuth",.flags=SECRET_SCHEMA_NONE,
    .attributes={{"domain",SECRET_SCHEMA_ATTRIBUTE_STRING},{"lease",SECRET_SCHEMA_ATTRIBUTE_STRING},{NULL,0}}};
static void render(Records *p,gboolean more);
static void refresh(GtkWidget *sender,Records *p){(void)sender;render(p,FALSE);}
static void next(GtkWidget *sender,Records *p){gtk_widget_destroy(sender);render(p,TRUE);}
static void set_row(GtkWidget *b,UiJson *row){
    g_object_set_data_full(G_OBJECT(b),"record",json_object_get(row),ui_json_release);
}
static UiJson *record(GtkButton *b){return g_object_get_data(G_OBJECT(b),"record");}
static gchar *when(UiJson *,const char *);
static void detail(GtkButton *b,Records *p){
    UiJson *row=record(b),*full=NULL;
    if(!p->grants){
        UiJson *r=ui_op("tools.detail");ui_string(r,"chat",ui_text(row,"chat"));
        json_object_object_add(r,"id",json_object_get(ui_get(row,"id")));full=ui_call(p->client,UI_READ,r);if(!full)return;
    }
    if(p->grants){
        UiJson *r=ui_op("grants.list");json_object_object_add(r,"recordID",json_object_get(ui_get(row,"id")));ui_string(r,"chat",ui_text(row,"chat"));
        UiJson *list=ui_call(p->client,UI_READ,r);
        if(list && json_object_array_length(list))full=json_object_get(json_object_array_get_idx(list,0));
        json_object_put(list);if(!full){copilot_status(p->client,"这项授权记录现已不可读。");return;}
    }
    UiJson *item=full?full:row;GString *text=g_string_new("");
    if(p->grants){
        gchar *start=when(item,"started"),*decided=when(item,"decided"),*ended=when(item,"ended");
        g_string_append_printf(text,"记录 #%"G_GINT64_FORMAT"　·　仅限这次调用\n开始时间　%s\n审批时间　%s\n结束时间　%s\n\n",json_object_get_int64(ui_get(item,"id")),start,decided,ended);
        g_free(start);g_free(decided);g_free(ended);
    }
    const char *fields[][2]={{"name","工具"},{"tool","工具"},{"stage","阶段"},{"status","状态"},{"outcome","结果状态"},{"chat","所属聊天"},{"executionMs","执行耗时（毫秒）"},{"waitMs","审批耗时（毫秒）"},{"arguments","调用参数"},{"result","执行结果"},{"fileReceiptID","文件变更记录"}};
    for(guint i=0;i<G_N_ELEMENTS(fields);i++)if(ui_get(item,fields[i][0]))g_string_append_printf(text,"%s\n%s\n\n",fields[i][1],ui_text(item,fields[i][0]));
    ui_detail(p->client,p->grants?"对话授权详情":"工具执行详情",text->str);
    g_string_free(text,TRUE);json_object_put(full);
}
static void revoke_tool(GtkButton *b,Records *p){
    LinuxClient *c=p->client;UiJson *v=record(b);const char *chat=ui_text(v,"chat");
    if(!ui_confirm(c,"撤销这次工具授权？","已执行的操作保留。仍在运行的所属任务会收到停止请求。","撤销"))return;
    if(c->busy){
        if(copilot_activity_stop_chat(c,chat))copilot_status(c,"已请求停止这项授权所属的协作任务。");
        else if(!g_strcmp0(chat,c->chat_id)){copilot_chat_stop(c);copilot_status(c,"已请求停止所属对话的工具运行。");}
        else copilot_status(c,"尚未找到这项授权所属的活动任务。");
    }else{
        UiJson *r=ui_op("grants.revoke");json_object_object_add(r,"id",json_object_get(ui_get(v,"id")));
        UiJson *result=ui_call(c,UI_WORKSPACE,r);if(result)copilot_status(c,ui_text(result,"state"));json_object_put(result);
    }
    render(p,FALSE);
}
static gchar *domain(Records *p){return g_compute_checksum_for_string(G_CHECKSUM_SHA256,p->client->data_dir,-1);}
static void link_account(GtkButton *b,Records *p){
    (void)b;LinuxClient *c=p->client;
    if(c->busy || !c->chat_id || !*c->chat_id){copilot_status(c,"请先选择一个空闲对话。");return;}
    GtkWidget *d=gtk_dialog_new_with_buttons("关联远端账号",c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消",GTK_RESPONSE_CANCEL,"保存授权",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *area=gtk_dialog_get_content_area(GTK_DIALOG(d)),*fields[3];const char *labels[]={"账号名称","服务商 HTTPS 令牌撤销地址","OAuth 令牌"};
    gtk_container_set_border_width(GTK_CONTAINER(area),16);gtk_window_set_default_size(GTK_WINDOW(d),440,-1);
    for(guint i=0;i<3;i++){gtk_box_pack_start(GTK_BOX(area),ui_label(labels[i]),FALSE,FALSE,4);fields[i]=gtk_entry_new();gtk_box_pack_start(GTK_BOX(area),fields[i],FALSE,FALSE,4);}
    gtk_entry_set_max_length(GTK_ENTRY(fields[0]),128);gtk_entry_set_max_length(GTK_ENTRY(fields[1]),2048);
    gtk_entry_set_max_length(GTK_ENTRY(fields[2]),16384);gtk_entry_set_visibility(GTK_ENTRY(fields[2]),FALSE);
    GtkWidget *duration=gtk_spin_button_new_with_range(1,720,1);gtk_spin_button_set_value(GTK_SPIN_BUTTON(duration),24);
    gtk_box_pack_start(GTK_BOX(area),ui_label("本地授权时长（小时）"),FALSE,FALSE,4);gtk_box_pack_start(GTK_BOX(area),duration,FALSE,FALSE,4);gtk_widget_show_all(d);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT){
        const char *name=gtk_entry_get_text(GTK_ENTRY(fields[0])),*endpoint=gtk_entry_get_text(GTK_ENTRY(fields[1])),*token=gtk_entry_get_text(GTK_ENTRY(fields[2]));
        gchar *id=g_uuid_string_random(),*scope=domain(p);UiJson *saved=json_object_new_object();ui_string(saved,"endpoint",endpoint);ui_string(saved,"token",token);
        gboolean stored=*name && strlen(name)<=128 && *token && strlen(token)<=16384 && g_str_has_prefix(endpoint,"https://") && secret_password_store_sync(&oauth_schema,SECRET_COLLECTION_DEFAULT,"Dunlin Copilot 账号授权",json_object_to_json_string(saved),NULL,NULL,"domain",scope,"lease",id,NULL);
        gboolean ok=FALSE;if(stored){g_mutex_lock(&c->lock);ok=dc_remote_account_record(c->store,id,c->chat_id,name,endpoint,time(NULL)+3600LL*gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(duration)));g_mutex_unlock(&c->lock);}
        if(stored&&!ok)secret_password_clear_sync(&oauth_schema,NULL,NULL,"domain",scope,"lease",id,NULL);
        copilot_status(c,ok?"账号授权已关联到本对话。":"保存失败，请检查地址、对话和系统密钥环。");
        json_object_put(saved);g_free(id);g_free(scope);
    }
    gtk_widget_destroy(d);render(p,FALSE);
}
static const char *remote_status(const char *state) {
    if(!strcmp(state,"revoked"))return "服务商已确认撤销";
    if(!strcmp(state,"retry"))return "服务暂不可用，可重试";
    if(!strcmp(state,"needs_user"))return "需解锁密钥环或检查服务商账号授权";
    if(!strcmp(state,"unknown"))return "上次撤销结果未知";
    if(!strcmp(state,"revoking"))return "正在提交撤销";
    if(!strcmp(state,"active"))return "尚未申请远端撤销";
    return "远端撤销尚未确认";
}
static gchar *when(UiJson *v,const char *field) {
    gint64 seconds=json_object_get_int64(ui_get(v,field));
    GDateTime *t=seconds>0?g_date_time_new_from_unix_local(seconds):NULL;
    gchar *label=t?g_date_time_format(t,"%Y-%m-%d %H:%M:%S"):g_strdup("—");if(t)g_date_time_unref(t);return label;
}
static void receipt_detail(GtkButton *b,Records *p) {
    UiJson *v=record(b),*request=ui_op("grants.platform.attempts");ui_string(request,"id",ui_text(v,"id"));
    UiJson *rows=ui_call(p->client,UI_READ,request);if(!rows)return;
    const char *state=ui_text(v,"state");gboolean active=!strcmp(state,"active") && json_object_get_int64(ui_get(v,"expires"))>time(NULL);
    gchar *start=when(v,"started"),*end=when(v,"expires"),*confirm=when(v,"revoked");
    GString *result=g_string_new(NULL);g_string_append_printf(result,"账号　%s\n所属聊天　%s\n协议　%s\n\n本地授权　%s\n授予时间　%s\n到期时间　%s\n\n远端结果　%s\n确认时间　%s\n",
        ui_text(v,"scope"),ui_text(v,"chat"),ui_text(v,"protocol"),active?"生效中":"本地已停用",start,end,remote_status(state),confirm);
    if(json_object_array_length(rows))g_string_append(result,"\n撤销尝试\n");
    for(gsize i=0;i<(gsize)json_object_array_length(rows);i++){UiJson *r=json_object_array_get_idx(rows,i);gchar *time=when(r,"started");
        g_string_append_printf(result,"#%"G_GINT64_FORMAT"　%s　%s\n",json_object_get_int64(ui_get(r,"generation")),time,remote_status(ui_text(r,"state")));g_free(time);}
    ui_detail(p->client,"账号授权与撤销详情",result->str);
    g_free(start);g_free(end);g_free(confirm);g_string_free(result,TRUE);json_object_put(rows);
}
static gboolean clear_key(const char *scope,const char *id) {
    GError *error=NULL;
    secret_password_clear_sync(&oauth_schema,NULL,&error,"domain",scope,"lease",id,NULL);
    gboolean ok=error==NULL;g_clear_error(&error);return ok;
}
static void cleanup_key(GtkButton *b,Records *p){
    if(p->client->busy)return;
    UiJson *r=record(b);if(strcmp(ui_text(r,"state"),"revoked"))return;
    gchar *scope=domain(p);gboolean cleared=clear_key(scope,ui_text(r,"id"));g_free(scope);
    copilot_status(p->client,cleared?"该账号的撤销凭据已从系统密钥环清除。":"系统密钥环清理未完成，解锁后可再次清理。");
}
typedef struct {Records *p;UiJson *leases,*results;gchar *scope;} RevokeJob;
static void revoke_accounts(GtkButton *,Records *);
static void stop_batch(GtkButton *b,Records *p){g_atomic_int_set(&p->client->service_cancel,1);gtk_widget_set_sensitive(GTK_WIDGET(b),FALSE);copilot_status(p->client,"当前账号结算后停止后续撤销。");}
static gboolean revoke_done(gpointer data){
    RevokeJob *job=data;Records *p=job->p;LinuxClient *c=p->client;c->busy=FALSE;
    if(!c->closing && !c->pending_identity){
        ui_clear(p->progress);render(p,FALSE);
        GtkWidget *d=gtk_dialog_new_with_buttons("账号撤销回执",c->window,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"返回授权记录",GTK_RESPONSE_CLOSE,NULL);
        GtkWidget *area=gtk_dialog_get_content_area(GTK_DIALOG(d)),*scroll=gtk_scrolled_window_new(NULL,NULL),*body=gtk_box_new(GTK_ORIENTATION_VERTICAL,9);
        gtk_container_set_border_width(GTK_CONTAINER(body),16);gtk_window_set_default_size(GTK_WINDOW(d),600,360);
        gtk_container_add(GTK_CONTAINER(scroll),body);gtk_box_pack_start(GTK_BOX(area),scroll,TRUE,TRUE,0);
        UiJson *retry=json_object_new_array();
        for(gsize i=0;i<(gsize)json_object_array_length(job->results);i++){
            UiJson *item=json_object_array_get_idx(job->results,i),*r=ui_get(item,"row");const char *state=ui_text(r,"state");
            GtkWidget *row=ui_row(body,ui_text(r,"scope"),ui_text(item,"message"));
            if(!strcmp(state,"revoked") && !json_object_get_boolean(ui_get(item,"cleared")))
                set_row(ui_button(row,"重试密钥清理",G_CALLBACK(cleanup_key),p),r);
            if(strcmp(state,"revoked") && !json_object_get_boolean(ui_get(item,"skipped")))json_object_array_add(retry,json_object_get(r));
        }
        if(json_object_array_length(retry))gtk_dialog_add_button(GTK_DIALOG(d),"重试未确认账号",GTK_RESPONSE_APPLY);
        GtkWindow *window=g_object_ref(c->window);
        gtk_widget_show_all(d);gint answer=gtk_dialog_run(GTK_DIALOG(d));gtk_widget_destroy(d);
        if(answer==GTK_RESPONSE_APPLY && c->window==window && !c->closing && !c->pending_identity){
            GtkWidget *button=g_object_ref_sink(gtk_button_new());set_row(button,retry);revoke_accounts(GTK_BUTTON(button),p);g_object_unref(button);}
        g_object_unref(window);json_object_put(retry);
    }
    json_object_put(job->leases);json_object_put(job->results);g_free(job->scope);g_free(job);return G_SOURCE_REMOVE;
}
static gpointer revoke_worker(gpointer data) {
    RevokeJob *job=data;LinuxClient *c=job->p->client;
    for(gsize i=0;i<(gsize)json_object_array_length(job->leases);i++){
        UiJson *v=json_object_array_get_idx(job->leases,i),*item=json_object_new_object();const char *id=ui_text(v,"id");
        json_object_array_add(job->results,item);
        if(g_atomic_int_get(&c->service_cancel)){json_object_object_add(item,"skipped",json_object_new_boolean(TRUE));ui_string(item,"message","已停止后续请求，此账号未处理。");json_object_object_add(item,"row",json_object_get(v));continue;}
        gchar *secret=secret_password_lookup_sync(&oauth_schema,NULL,NULL,"domain",job->scope,"lease",id,NULL);
        UiJson *lease=secret?json_tokener_parse(secret):NULL;
        g_mutex_lock(&c->lock);char *raw=dc_remote_account_revoke(c->store,id,lease?ui_text(lease,"endpoint"):NULL,lease?ui_text(lease,"token"):NULL);g_mutex_unlock(&c->lock);
        UiJson *list=raw?json_tokener_parse(raw):NULL;dc_free(raw);UiJson *updated=NULL;
        for(gsize j=0;list && j<(gsize)json_object_array_length(list);j++){
            UiJson *r=json_object_array_get_idx(list,j);if(!strcmp(id,ui_text(r,"id"))){updated=r;break;}}
        const char *state=ui_text(updated,"state");gboolean revoked=!strcmp(state,"revoked");
        gboolean cleared=revoked && clear_key(job->scope,id);
        gchar *message=updated?g_strdup_printf("本地授权已停用 · %s%s",remote_status(state),revoked?(cleared?" · 系统密钥环已清理":" · 系统密钥环待清理"):""):g_strdup("撤销结果未确认，请检查服务与密钥环后重试。");
        ui_string(item,"message",message);g_free(message);json_object_object_add(item,"cleared",json_object_new_boolean(cleared));
        json_object_object_add(item,"row",json_object_get(updated?updated:v));
        json_object_put(list);json_object_put(lease);if(secret)secret_password_free(secret);
    }
    g_idle_add(revoke_done,job);return NULL;
}
static void revoke_accounts(GtkButton *b,Records *p) {
    LinuxClient *c=p->client;if(c->busy || c->pending_identity || c->closing)return;
    UiJson *leases=json_object_get(record(b));GString *names=g_string_new("");
    for(gsize i=0;i<(gsize)json_object_array_length(leases);i++)g_string_append_printf(names,"%s\n",ui_text(json_object_array_get_idx(leases,i),"scope"));
    gboolean ok=ui_confirm(c,"向以下服务撤销授权？",names->str,"撤销授权");g_string_free(names,TRUE);if(!ok){json_object_put(leases);return;}
    RevokeJob *job=g_new0(RevokeJob,1);job->p=p;job->leases=leases;job->results=json_object_new_array();job->scope=domain(p);
    c->busy=TRUE;g_atomic_int_set(&c->service_cancel,0);GtkWidget *progress=ui_row(p->progress,"正在向服务商提交撤销请求…","");
    if(json_object_array_length(leases)>1)ui_button(progress,"停止后续请求",G_CALLBACK(stop_batch),p);
    gtk_widget_show_all(p->progress);g_thread_unref(g_thread_new("oauth-revoke",revoke_worker,job));
}
static void platform_list(Records *p,gboolean history) {
    UiJson *rows=ui_call(p->client,UI_READ,ui_op("grants.platform.list")), *batch=json_object_new_array();
    for(gsize i=0;rows && i<(gsize)json_object_array_length(rows);i++){
        UiJson *v=json_object_array_get_idx(rows,i);const char *state=ui_text(v,"state");
        gboolean active=!strcmp(state,"active") && json_object_get_int64(ui_get(v,"expires"))>time(NULL);
        if(active==history)continue;
        gboolean account=!strcmp(ui_text(v,"protocol"),"oauth-revocation");
        gchar *line=g_strdup_printf("%s · %s",active?"本地授权生效中":"本地授权已停用",account?remote_status(state):state);
        GtkWidget *row=ui_row(p->rows,ui_text(v,"scope"),line);g_free(line);
        set_row(ui_button(row,"授权与回执",G_CALLBACK(receipt_detail),p),v);
        if(account && !p->client->busy && strcmp(state,"revoked") && strcmp(state,"revoking")){
            UiJson *one=json_object_new_array();json_object_array_add(one,json_object_get(v));
            set_row(ui_button(row,active?"撤销账号":"重试撤销",G_CALLBACK(revoke_accounts),p),one);json_object_put(one);
            json_object_array_add(batch,json_object_get(v));
        }else if(account && history && !strcmp(state,"revoked") && !p->client->busy)
            set_row(ui_button(row,"清理本机凭据",G_CALLBACK(cleanup_key),p),v);
    }
    if(json_object_array_length(batch)>1)set_row(ui_button(p->rows,"批量撤销这些账号…",G_CALLBACK(revoke_accounts),p),batch);
    json_object_put(batch);json_object_put(rows);
}
static void render(Records *p,gboolean more){
    if(!more){ui_clear(p->rows);p->cursor=0;}
    const char *chat=p->client->chat_id;gboolean history=p->grants && gtk_combo_box_get_active(GTK_COMBO_BOX(p->filter))==1;
    if(!more && p->grants)platform_list(p,history);
    if(!p->grants && (!chat || !*chat)){ui_row(p->rows,"请先选择对话","工具调用会随该对话保存在这里。");gtk_widget_show_all(p->rows);return;}
    UiJson *r=ui_op(p->grants?"grants.list":"tools.list");json_object_object_add(r,"before",json_object_new_int64(p->cursor));
    if(p->grants){ui_string(r,"scope",history?"history":"active");json_object_object_add(r,"limit",json_object_new_int(50));}
    else{ui_string(r,"chat",chat);ui_string(r,"query",gtk_entry_get_text(GTK_ENTRY(p->query)));}
    UiJson *reply=ui_call(p->client,UI_READ,r),*rows=p->grants?reply:ui_get(reply,"items");
    gsize n=rows?json_object_array_length(rows):0;
    if(!n && !more)ui_row(p->rows,p->grants?(history?"暂无工具授权历史":"暂无生效中的工具授权"):"暂无匹配的工具执行记录",NULL);
    for(gsize i=0;i<n;i++){
        UiJson *v=json_object_array_get_idx(rows,i);const char *state=ui_text(v,p->grants?"status":"stage");
        gchar *line=p->grants?g_strdup_printf("%s · %s · 审批耗时 %s ms",state,ui_text(v,"chat"),ui_text(v,"waitMs")):
            g_strdup_printf("%s · %s",state,ui_text(v,"outcome"));
        GtkWidget *box=ui_row(p->rows,ui_text(v,p->grants?"tool":"name"),line);g_free(line);
        set_row(ui_button(box,"详情",G_CALLBACK(detail),p),v);
        if(p->grants && !history)set_row(ui_button(box,"撤销",G_CALLBACK(revoke_tool),p),v);
    }
    if((p->grants && n==50)||(!p->grants && json_object_get_boolean(ui_get(reply,"more")))){
        p->cursor=p->grants?json_object_get_int64(ui_get(json_object_array_get_idx(rows,n-1),"id")):json_object_get_int64(ui_get(reply,"nextCursor"));
        ui_button(p->rows,"加载更早记录",G_CALLBACK(next),p);
    }
    json_object_put(reply);gtk_widget_show_all(p->rows);
}
GtkWidget *copilot_records_new(LinuxClient *c,gboolean grants){
    Records *p=g_new0(Records,1);p->client=c;p->grants=grants;
    GtkWidget *body,*page=ui_page(grants?"权限与授权记录":"工具执行记录",&body),*bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_box_pack_start(GTK_BOX(body),bar,FALSE,FALSE,0);
    if(grants){p->filter=gtk_combo_box_text_new();gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->filter),"生效中");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->filter),"历史记录");gtk_combo_box_set_active(GTK_COMBO_BOX(p->filter),0);
        gtk_box_pack_start(GTK_BOX(bar),p->filter,FALSE,FALSE,0);g_signal_connect(p->filter,"changed",G_CALLBACK(refresh),p);
        ui_button(bar,"关联远端账号…",G_CALLBACK(link_account),p);
    }else{p->query=gtk_search_entry_new();gtk_entry_set_placeholder_text(GTK_ENTRY(p->query),"搜索工具、参数或结果");
        gtk_box_pack_start(GTK_BOX(bar),p->query,TRUE,TRUE,0);g_signal_connect(p->query,"search-changed",G_CALLBACK(refresh),p);}
    ui_button(bar,"刷新",G_CALLBACK(refresh),p);p->progress=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);gtk_box_pack_start(GTK_BOX(body),p->progress,FALSE,FALSE,0);p->rows=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_box_pack_start(GTK_BOX(body),p->rows,FALSE,FALSE,0);
    g_object_set_data_full(G_OBJECT(page),"records",p,g_free);return page;
}
void copilot_records_refresh(LinuxClient *c,gboolean grants){
    GtkWidget *page=grants?c->permissions:c->tools;if(page)render(g_object_get_data(G_OBJECT(page),"records"),FALSE);
}
