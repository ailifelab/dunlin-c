#include "surface.h"
#include "pages.h"
#include "chat.h"
#include "settings.h"
#include <webkit2/webkit2.h>
#include <string.h>

/* Each surface owns a separate ephemeral WebKit context and a fixed native
 * owner. The page can request a method, but cannot choose a different package,
 * chat, surface lease, cookie jar or host filesystem path. */
typedef struct {
    LinuxClient *c;
    UiJson *owner;
    GtkWidget *window;
    WebKitWebView *web;
    gchar *identifier,*chat,*slot,*assets,*script,*html;
    GHashTable *static_files;
    gboolean closing,failed,pressed,dragging,in_call;
    GQueue requests;
    gsize queued_bytes;
    guint request_timer;
    gdouble press_x,press_y;
    guint fault_timer;
    gchar *fault_reason,*download_id,*download_name;
    gsize download_size;
    GByteArray *download;
} Surface;
static GList *open_views;
static UiJson *owner_request(Surface *s,const char *op){
    UiJson *r=ui_op(op);const char *keys[]={"source","package","version","instance"};
    for(guint i=0;i<G_N_ELEMENTS(keys);i++)if(ui_get(s->owner,keys[i]))ui_string(r,keys[i],ui_text(s->owner,keys[i]));
    if(s->chat && *s->chat)ui_string(r,"chat",s->chat);
    return r;
}
static int approve_surface(void *data,const char *tool,const char *arguments){
    Surface *s=data;
    return s && !s->closing && ui_confirm(s->c,tool,arguments,"仅允许这一次");
}
static UiJson *execute(Surface *s,UiJson *r,gchar **error){
    LinuxClient *c=s->c;
    if(c->busy || !g_mutex_trylock(&c->lock)){json_object_put(r);*error=g_strdup("宿主任务正在运行");return NULL;}
    /* The owner runs on GTK's main context. Modal confirmation may spin a
     * nested event loop; hold the busy gate until the writer has returned. */
    c->busy=TRUE;dc_hooks hooks={.user=s,.approve=approve_surface};
    char *raw=dc_extensions_call(c->store,json_object_to_json_string(r),&hooks);
    g_mutex_unlock(&c->lock);c->busy=FALSE;
    UiJson *reply=raw?json_tokener_parse(raw):NULL,*data=NULL;dc_free(raw);json_object_put(r);
    if(reply && json_object_get_boolean(ui_get(reply,"ok")))data=json_object_get(ui_get(reply,"data"));
    else *error=g_strdup(reply?ui_text(reply,"error"):"扩展服务未返回结果");
    json_object_put(reply);return data;
}
static gchar *asset_bytes(Surface *,const char *,gsize *);
static void reset_download(Surface *s){
    g_clear_pointer(&s->download_id,g_free);g_clear_pointer(&s->download_name,g_free);g_clear_pointer(&s->download,g_byte_array_unref);s->download_size=0;
}
static UiJson *download(Surface *s,const char *method,UiJson *args,gchar **error){
    if(!strcmp(method,"download.error")){copilot_status(s->c,"插件导出未完成。");return json_object_new_boolean(FALSE);}
    if(!strcmp(method,"download.begin")){
        gint64 size=json_object_get_int64(ui_get(args,"bytes"));
        if(s->download || !json_object_is_type(ui_get(args,"bytes"),json_type_int)||size<0||size>16777216){*error=g_strdup("文件大小或导出状态无效");return NULL;}
        UiJson *owner=execute(s,owner_request(s,"extensions.clientStorage.read"),error);json_object_put(owner);if(*error)return NULL;
        s->download_size=size;s->download=g_byte_array_new();s->download_id=g_uuid_string_random();
        s->download_name=g_path_get_basename(ui_text(args,"name"));
        if(!*s->download_name || !strcmp(s->download_name,".")||!strcmp(s->download_name,"..")||strlen(s->download_name)>160){g_free(s->download_name);s->download_name=g_strdup("download");}
        UiJson *value=json_object_new_object();ui_string(value,"id",s->download_id);return value;
    }
    if(!s->download || g_strcmp0(s->download_id,ui_text(args,"id"))){*error=g_strdup("导出会话已过期");return NULL;}
    if(!strcmp(method,"download.cancel")){reset_download(s);return json_object_new_boolean(TRUE);}
    if(!strcmp(method,"download.append")){
        const char *raw=ui_text(args,"base64");gsize length=0;
        if(!json_object_is_type(ui_get(args,"offset"),json_type_int)||json_object_get_int64(ui_get(args,"offset"))!=s->download->len || strlen(raw)>65536 || strlen(raw)%4){*error=g_strdup("导出片段无效");return NULL;}
        guchar *bytes=g_base64_decode(raw,&length);
        if(!length || length>s->download_size-s->download->len){g_free(bytes);*error=g_strdup("导出片段超出范围");return NULL;}
        g_byte_array_append(s->download,bytes,length);g_free(bytes);return json_object_new_boolean(TRUE);
    }
    if(strcmp(method,"download.finish")||s->download->len!=s->download_size){*error=g_strdup("文件尚未接收完整");return NULL;}
    GtkWidget *d=gtk_file_chooser_dialog_new("导出插件文件",GTK_WINDOW(s->window),GTK_FILE_CHOOSER_ACTION_SAVE,"取消",GTK_RESPONSE_CANCEL,"保存",GTK_RESPONSE_ACCEPT,NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d),s->download_name);gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d),TRUE);
    gchar *file=gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT?gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)):NULL;
    gtk_widget_destroy(d);gboolean ok=file && g_file_set_contents(file,s->download->len?(const gchar*)s->download->data:"",s->download->len,NULL);
    if(file&&!ok)*error=g_strdup("文件保存失败");
    g_free(file);reset_download(s);return json_object_new_boolean(ok);
}
static UiJson *invoke(Surface *s,const char *method,UiJson *args,gchar **error){
    UiJson *r=NULL;gboolean unwrap=FALSE;
    if(!json_object_is_type(args,json_type_object)){*error=g_strdup("无效参数");return NULL;}
    ui_string(args,"_surface",s->identifier);
    if(!strcmp(method,"theme.select")){
        UiJson *owner=execute(s,owner_request(s,"extensions.clientStorage.read"),error);json_object_put(owner);
        if(*error)return NULL;
        return json_object_new_boolean(copilot_settings_apply_theme(s->c,ui_text(args,"preference")));
    }
    if(g_str_has_prefix(method,"download."))return download(s,method,args,error);
    if(!strcmp(method,"native.asset.read")){
        const char *path=ui_text(args,"path");if(!g_str_has_prefix(path,"/dunlin-assets/tldraw/")){*error=g_strdup("未知资源");return NULL;}
        UiJson *guard=execute(s,owner_request(s,"extensions.clientStorage.read"),error);json_object_put(guard);if(*error)return NULL;
        char *relative=g_strconcat("web/",path+strlen("/dunlin-assets/"),NULL);gsize len=0;char *bytes=asset_bytes(s,relative,&len);g_free(relative);
        if(!bytes){*error=g_strdup("资源校验失败");return NULL;}
        char *encoded=g_base64_encode((guchar*)bytes,len);g_free(bytes);UiJson *out=json_object_new_object();ui_string(out,"base64",encoded);g_free(encoded);
        ui_string(out,"type",g_str_has_suffix(path,".svg")?"image/svg+xml":g_str_has_suffix(path,".woff2")?"font/woff2":g_str_has_suffix(path,".json")?"application/json":"application/octet-stream");return out;
    }
    if(!strcmp(method,"storage.read")||!strcmp(method,"storage.write")){
        r=owner_request(s,!strcmp(method,"storage.read")?"extensions.clientStorage.read":"extensions.clientStorage.write");
        if(ui_get(args,"value"))json_object_object_add(r,"value",json_object_get(ui_get(args,"value")));
    }else if(!strcmp(method,"theme.presentation")){
        r=owner_request(s,"extensions.presentation.write");json_object_object_add(r,"value",json_object_get(ui_get(args,"value")));
    }else if(!strcmp(method,"config.set")){
        const char *k=ui_text(args,"field");
        if(!*k || strlen(k)>128 || !strcmp(k,"__proto__") || !strcmp(k,"constructor") || !strcmp(k,"prototype")){*error=g_strdup("无效的配置字段");return NULL;}
        UiJson *current=execute(s,owner_request(s,"extensions.config.get"),error);if(!current)return NULL;
        UiJson *cfg=ui_get(current,"config");if(!json_object_is_type(cfg,json_type_object)){json_object_put(current);*error=g_strdup("配置格式不正确");return NULL;}
        json_object_object_add(cfg,k,json_object_get(ui_get(args,"value")));r=owner_request(s,"extensions.settings.apply");
        ui_string(r,"expectedHash",ui_text(current,"hash"));json_object_object_add(r,"config",json_object_get(cfg));json_object_put(current);
    }else if(!strcmp(method,"native.snapshot"))r=owner_request(s,"extensions.clientContext");
    else if(g_str_has_prefix(method,"native.")){
        UiJson *context=execute(s,owner_request(s,"extensions.clientContext"),error);if(!context)return NULL;
        if(!strcmp(method,"native.clipboard.write")){
            const char *v=ui_text(args,"text");if(strlen(v)>65536){*error=g_strdup("文本超过上限");json_object_put(context);return NULL;}
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),v,-1);json_object_put(context);return json_tokener_parse("{\"copied\":true}");
        }
        if(!strcmp(method,"native.directory.pick")||(!strcmp(method,"native.workspace.create")&&!ui_get(args,"cwd")&&!ui_get(args,"path"))){
            GtkWidget *d=gtk_file_chooser_dialog_new("选择项目目录",GTK_WINDOW(s->window),GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,"取消",GTK_RESPONSE_CANCEL,"选择",GTK_RESPONSE_ACCEPT,NULL);
            char *path=gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT?gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)):NULL;gtk_widget_destroy(d);
            if(!path || !strcmp(method,"native.directory.pick")){UiJson *out=path?json_object_new_string(path):NULL;g_free(path);json_object_put(context);return out;}
            ui_string(args,"cwd",path);g_free(path);
        }
        if(!strcmp(method,"native.session.open")){
            const char *id=ui_text(args,"id");UiJson *sessions=ui_get(ui_get(context,"sessions"),"byId");
            if(!ui_get(sessions,id)){json_object_put(context);*error=g_strdup("会话不存在");return NULL;}
            copilot_chat_flush(s->c);g_free(s->c->chat_id);s->c->chat_id=g_strdup(id);copilot_chat_refresh(s->c);copilot_show(s->c,"chat");
            UiJson *o=json_object_new_object();json_object_object_add(o,"value",json_object_new_boolean(TRUE));json_object_object_add(o,"snapshot",context);return o;
        }
        const char *action=method+7;gchar *remapped=NULL;UiJson *arguments=args;
        if(!strcmp(action,"remoteSession")){
            const char *remote=ui_text(args,"method");UiJson *o=json_object_new_object();
            if(!strcmp(remote,"list") || !strcmp(remote,"canOpenWorkspacePath")){
                json_object_object_add(o,"ok",json_object_new_boolean(TRUE));json_object_object_add(o,"value",!strcmp(remote,"list")?json_object_get(ui_get(context,"sessions")):json_object_new_boolean(FALSE));json_object_put(context);return o;
            }
            json_object_put(o);remapped=g_strconcat("session.",remote,NULL);action=remapped;
            UiJson *items=ui_get(args,"args");arguments=json_object_is_type(items,json_type_array)?json_object_array_get_idx(items,0):NULL;
        }
        r=owner_request(s,"extensions.clientAction");ui_string(r,"method",action);json_object_object_add(r,"arguments",arguments?json_object_get(arguments):json_object_new_object());g_free(remapped);json_object_put(context);
    }else{
        const char *permitted[]={"http","http.pending","http.body","http.events","rpc","reasoning.service","ui.close"};
        gboolean supported=g_str_has_prefix(method,"ws:")||g_str_has_prefix(method,"http.upload.");
        for(guint i=0;i<G_N_ELEMENTS(permitted);i++)supported|=!strcmp(method,permitted[i]);
        if(!supported){*error=g_strdup("宿主不支持此调用");return NULL;}
        r=owner_request(s,"extensions.command");ui_string(r,"command",method);json_object_object_add(r,"arguments",json_object_get(args));unwrap=TRUE;
    }
    UiJson *result=execute(s,r,error);
    if(unwrap && result){UiJson *value=json_object_get(ui_get(result,"data"));json_object_put(result);return value;}
    return result;
}
static UiJson *message_value(WebKitJavascriptResult *result){
    JSCValue *value=webkit_javascript_result_get_js_value(result);if(!jsc_value_is_object(value))return NULL;
    gchar *text=jsc_value_to_json(value,0);UiJson *msg=text&&strlen(text)<=1572864?json_tokener_parse(text):NULL;g_free(text);return msg;
}
static void javascript(WebKitWebView *web,const gchar *script){
#if WEBKIT_CHECK_VERSION(2,40,0)
    webkit_web_view_evaluate_javascript(web,script,-1,NULL,NULL,NULL,NULL,NULL);
#else
    webkit_web_view_run_javascript(web,script,NULL,NULL,NULL);
#endif
}
static void send_reply(Surface *s,gint64 id,UiJson *value,const gchar *error){
    UiJson *payload=json_object_new_array();
    json_object_array_add(payload,json_object_new_int64(id));json_object_array_add(payload,value);
    json_object_array_add(payload,error?json_object_new_string(error):NULL);
    gchar *script=g_strdup_printf("globalThis.dunlinReply && dunlinReply.apply(null,%s)",json_object_to_json_string_ext(payload,JSON_C_TO_STRING_PLAIN));
    javascript(s->web,script);g_free(script);json_object_put(payload);
}
static void dispatch(Surface *s,UiJson *m){
    s->in_call=TRUE;gchar *error=NULL;
    UiJson *value=invoke(s,ui_text(m,"command"),ui_get(m,"args"),&error);
    send_reply(s,json_object_get_int64(ui_get(m,"id")),value,error);
    g_free(error);s->in_call=FALSE;
}
static gboolean host_occupied(Surface *s){
    return s->in_call || s->c->busy || g_atomic_int_get(&s->c->indexing);
}
static gboolean drain_requests(gpointer data){
    Surface *s=data;
    if(!s->failed && !g_queue_is_empty(&s->requests)){
        if(host_occupied(s))return G_SOURCE_CONTINUE;
        UiJson *m=g_queue_pop_head(&s->requests);
        s->queued_bytes-=strlen(json_object_to_json_string_ext(m,JSON_C_TO_STRING_PLAIN));
        dispatch(s,m);json_object_put(m);
        if(!g_queue_is_empty(&s->requests))return G_SOURCE_CONTINUE;
    }
    s->request_timer=0;return G_SOURCE_REMOVE;
}
static void message(WebKitUserContentManager *manager,WebKitJavascriptResult *result,Surface *s){
    (void)manager;if(s->closing || s->failed)return;
    UiJson *m=message_value(result);if(!m)return;
    const char *method=ui_text(m,"command");gint64 id=json_object_get_int64(ui_get(m,"id"));
    if(id<=0 || id>G_MAXINT32 || !*method || strlen(method)>256){json_object_put(m);return;}
    if(host_occupied(s) || s->request_timer){
        gsize bytes=strlen(json_object_to_json_string_ext(m,JSON_C_TO_STRING_PLAIN));
        if(s->requests.length<64 && bytes<=2097152-s->queued_bytes){
            s->queued_bytes+=bytes;g_queue_push_tail(&s->requests,m);
            if(!s->request_timer)s->request_timer=g_timeout_add(50,drain_requests,s);
            return;
        }
        send_reply(s,id,NULL,"插件待处理请求已达到上限");
    }else dispatch(s,m);
    json_object_put(m);
}
static gboolean persist_fault(gpointer data){
    Surface *s=data;if(host_occupied(s))return G_SOURCE_CONTINUE;
    gchar *err=NULL;UiJson *r=owner_request(s,"extensions.clientFault");ui_string(r,"diagnostic",s->fault_reason);
    UiJson *receipt=execute(s,r,&err);json_object_put(receipt);
    if(err){copilot_status(s->c,err);g_free(err);}else {copilot_status(s->c,"插件出现错误，已停用并保留日志。");copilot_settings_refresh_themes(s->c);}
    s->fault_timer=0;return G_SOURCE_REMOVE;
}
static void report_fault(Surface *s,const char *diagnostic){
    if(s->closing || s->failed)return;
    s->failed=TRUE;s->fault_reason=g_strdup(diagnostic);gtk_widget_set_sensitive(GTK_WIDGET(s->web),FALSE);
    if(persist_fault(s)==G_SOURCE_CONTINUE)s->fault_timer=g_timeout_add(100,persist_fault,s);
}
static void process_lost(WebKitWebView *web,WebKitWebProcessTerminationReason reason,Surface *s){
    (void)web;(void)reason;report_fault(s,"Error line 0");
}
static void fault(WebKitUserContentManager *manager,WebKitJavascriptResult *result,Surface *s){
    (void)manager;if(s->closing||s->failed)return;
    JSCValue *value=webkit_javascript_result_get_js_value(result);if(!jsc_value_is_string(value))return;
    gchar *reason=jsc_value_to_string(value);
    GRegex *pattern=g_regex_new("^(TypeError|RangeError|ReferenceError|SyntaxError|Error) line [0-9]{1,9}$",0,0,NULL);
    report_fault(s,g_regex_match(pattern,reason,0,NULL)?reason:"Error line 0");g_regex_unref(pattern);g_free(reason);
}
/* Transform third-party source as text. No evaluation, bundling or package
 * scripts run in the compiler. The installed asset manifest pins its bytes. */
static gchar *client_source(Surface *s,const char *source,GError **error){
    if(webkit_get_major_version()>2 || webkit_get_minor_version()>=40)return g_strdup(source);
#if defined(__aarch64__)
    const char *relative="toolchain/node_modules/@esbuild/linux-arm64/bin/esbuild";
#elif defined(__x86_64__)
    const char *relative="toolchain/node_modules/@esbuild/linux-x64/bin/esbuild";
#else
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"此处理器尚无兼容编译器");return NULL;
#endif
    gsize length=0;char *bytes=asset_bytes(s,relative,&length);
    if(!bytes){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"兼容编译器摘要校验失败");return NULL;}
    g_free(bytes);char *tool=g_build_filename(s->assets,relative,NULL);
    GSubprocessLauncher *launch=g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE|G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    char *env[]={"LANG=C","PATH=/usr/bin:/bin",NULL};g_subprocess_launcher_set_environ(launch,env);
    GSubprocess *process=g_subprocess_launcher_spawn(launch,error,tool,"--loader=js","--target=safari14","--supported:destructuring=true","--log-level=error",NULL);
    g_free(tool);g_object_unref(launch);if(!process)return NULL;
    char *out=NULL;gboolean success=g_subprocess_communicate_utf8(process,source,NULL,&out,NULL,error);
    if(success && !g_subprocess_get_successful(process)){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"插件脚本无法转换为当前 WebKit 支持的语法");g_clear_pointer(&out,g_free);}
    g_object_unref(process);return out;
}
static void send_bytes(WebKitURISchemeRequest *request,gpointer bytes,gsize len,const char *type){
    GInputStream *stream=g_memory_input_stream_new_from_data(bytes,len,g_free);
    webkit_uri_scheme_request_finish(request,stream,len,type);g_object_unref(stream);
}
static void deny(WebKitURISchemeRequest *request){
    GError *e=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_NOT_FOUND,"插件资源不可用");webkit_uri_scheme_request_finish_error(request,e);g_error_free(e);
}
static gchar *asset_bytes(Surface *s,const char *relative,gsize *len){
    const char *hash=g_hash_table_lookup(s->static_files,relative);if(!hash)return NULL;
    char *path=g_build_filename(s->assets,relative,NULL),*bytes=NULL;
    gboolean ok=g_file_get_contents(path,&bytes,len,NULL);g_free(path);if(!ok||*len>33554432){g_free(bytes);return NULL;}
    char *actual=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(guchar*)bytes,*len);ok=!strcmp(hash,actual);g_free(actual);
    if(!ok){g_free(bytes);return NULL;}return bytes;
}
typedef struct {
    WebKitURISchemeRequest *request;
    gchar *pending;
    gint64 deadline;
} Resource;
static UiJson *handle_request(Surface *s,const char *method,const char *handle,gssize offset,gboolean release,gchar **err){
    UiJson *a=json_object_new_object();ui_string(a,"id",handle);
    if(offset>=0)json_object_object_add(a,"offset",json_object_new_int64(offset));
    if(release)json_object_object_add(a,"release",json_object_new_boolean(TRUE));
    UiJson *v=invoke(s,method,a,err);json_object_put(a);return v;
}
static void release_handle(Surface *s,const char *method,const char *handle){
    if(!handle||!*handle)return;
    gchar *error=NULL;json_object_put(handle_request(s,method,handle,-1,TRUE,&error));g_free(error);
}
static const char *asset_mime(const char *name){
    if(g_str_has_suffix(name,".svg"))return "image/svg+xml";
    if(g_str_has_suffix(name,".woff2"))return "font/woff2";
    if(g_str_has_suffix(name,".json"))return "application/json";
    if(g_str_has_suffix(name,".js"))return "text/javascript";
    if(g_str_has_suffix(name,".css"))return "text/css";
    if(g_str_has_suffix(name,".png"))return "image/png";
    return "application/octet-stream";
}
static void resource_free(Resource *r){g_free(r->pending);g_object_unref(r->request);g_free(r);}
static gboolean poll_resource(gpointer data);
static void finish_http(Surface *s,Resource *r,UiJson *v){
    const char *pending=ui_text(v,"pendingHandle");
    if(*pending && g_get_monotonic_time()<r->deadline){g_free(r->pending);r->pending=g_strdup(pending);g_timeout_add(150,poll_resource,r);return;}
    gint status=json_object_get_int(ui_get(v,"status"));const char *handle=ui_text(v,"bodyHandle");
    GByteArray *out=NULL;gchar *error=NULL;
    if(status>=200 && status<300 && *handle){
        gint64 total=json_object_get_int64(ui_get(v,"bytes"));
        if(total>=0 && total<=16777216){out=g_byte_array_new();
            while(out->len<total){
                UiJson *chunk=handle_request(s,"http.body",handle,out->len,FALSE,&error);const char *b64=ui_text(chunk,"base64");
                gsize n=0;guchar *bytes=strlen(b64)<=524288?g_base64_decode(b64,&n):NULL;
                gboolean ok=!error&&n&&n<=(gsize)total-out->len;
                if(ok)g_byte_array_append(out,bytes,n);
                g_free(bytes);json_object_put(chunk);
                if(!ok){g_byte_array_unref(out);out=NULL;break;}
            }
        }
        release_handle(s,"http.body",handle);
    }else if(status>=200 && status<300){
        const char *base64=ui_text(v,"bodyBase64");gsize n=0;
        if(ui_get(v,"bodyBase64") && strlen(base64)<=22369624){guchar *bytes=g_base64_decode(base64,&n);out=g_byte_array_new_take(bytes,n);}
        else if(ui_get(v,"body")){const char *body=json_object_is_type(ui_get(v,"body"),json_type_string)?ui_text(v,"body"):json_object_to_json_string(ui_get(v,"body"));
            if(strlen(body)<=16777216)out=g_byte_array_new_take((guchar*)g_strdup(body),strlen(body));}
    }
    g_free(error);const char *mime=ui_text(ui_get(v,"headers"),"content-type");
    if(out){gsize n=out->len;send_bytes(r->request,g_byte_array_free(out,FALSE),n,*mime?mime:asset_mime(webkit_uri_scheme_request_get_uri(r->request)));}
    else deny(r->request);
    resource_free(r);
}
static gboolean poll_resource(gpointer data){
    Resource *r=data;WebKitWebView *view=webkit_uri_scheme_request_get_web_view(r->request);
    Surface *s=view?g_object_get_data(G_OBJECT(view),"surface"):NULL;
    if(!s||s->closing||s->failed||g_get_monotonic_time()>=r->deadline){
        if(s&&!s->closing&&!s->failed)release_handle(s,"http.pending",r->pending);
        deny(r->request);resource_free(r);return G_SOURCE_REMOVE;
    }
    if(s->c->busy)return G_SOURCE_CONTINUE;
    gchar *err=NULL;UiJson *v=handle_request(s,"http.pending",r->pending,-1,FALSE,&err);
    if(!err && !json_object_get_boolean(ui_get(v,"ready"))){json_object_put(v);return G_SOURCE_CONTINUE;}
    release_handle(s,"http.pending",r->pending);g_clear_pointer(&r->pending,g_free);
    if(err){deny(r->request);resource_free(r);}else finish_http(s,r,ui_get(v,"response"));
    g_free(err);json_object_put(v);return G_SOURCE_REMOVE;
}
static void scheme(WebKitURISchemeRequest *request,gpointer unused){
    (void)unused;WebKitWebView *view=webkit_uri_scheme_request_get_web_view(request);
    Surface *s=view?g_object_get_data(G_OBJECT(view),"surface"):NULL;
    const char *uri=webkit_uri_scheme_request_get_uri(request);
    if(!s||s->closing||s->failed||!g_str_has_prefix(uri,"dunlin-plugin://surface/")){deny(request);return;}
    const char *path=uri+strlen("dunlin-plugin://surface");gsize len;
    if(!strcmp(path,"/index.html")){send_bytes(request,g_strdup(s->html),strlen(s->html),"text/html");return;}
    if(!strcmp(path,"/client.js")){send_bytes(request,g_strdup(s->script),strlen(s->script),"text/javascript");return;}
    if(!strcmp(path,"/runtime.js")||!strcmp(path,"/runtime.css")||g_str_has_prefix(path,"/dunlin-assets/tldraw/")){
        char *name=g_str_has_prefix(path,"/dunlin-assets/")?g_strconcat("web/",path+strlen("/dunlin-assets/"),NULL):g_strconcat("web",path,NULL);
        char *bytes=asset_bytes(s,name,&len);if(bytes)send_bytes(request,bytes,len,asset_mime(name));else deny(request);g_free(name);return;
    }
    UiJson *args=json_object_new_object();ui_string(args,"url",path);ui_string(args,"method","GET");
    gchar *error=NULL;UiJson *v=invoke(s,"http",args,&error);json_object_put(args);
    if(error)deny(request);else{Resource *r=g_new0(Resource,1);r->request=g_object_ref(request);r->deadline=g_get_monotonic_time()+90*G_TIME_SPAN_SECOND;finish_http(s,r,v);}
    json_object_put(v);g_free(error);
}
/* GDK hands window motion to the display server. Coordinates are logical on
 * both backends; Wayland compositors own placement, scale and monitor edges. */
static gboolean overlay_press(GtkWidget *w,GdkEventButton *e,Surface *s){
    (void)w;if(e->button==1){s->pressed=TRUE;s->dragging=FALSE;s->press_x=e->x_root;s->press_y=e->y_root;}return FALSE;
}
static gboolean overlay_move(GtkWidget *w,GdkEventMotion *e,Surface *s){
    if(!s->pressed||s->dragging||!(e->state&GDK_BUTTON1_MASK))return FALSE;
    if(!gtk_drag_check_threshold(w,(gint)s->press_x,(gint)s->press_y,(gint)e->x_root,(gint)e->y_root))return FALSE;
    s->dragging=TRUE;s->pressed=FALSE;
    GdkDevice *device=gdk_event_get_device((GdkEvent*)e);
    if(device)gdk_window_begin_move_drag_for_device(gtk_widget_get_window(s->window),device,1,(gint)e->x_root,(gint)e->y_root,e->time);
    return FALSE;
}
static gboolean overlay_release(GtkWidget *w,GdkEventButton *e,Surface *s){
    (void)w;(void)e;s->pressed=FALSE;s->dragging=FALSE;return FALSE;
}
static gboolean navigation(WebKitWebView *v,WebKitPolicyDecision *decision,WebKitPolicyDecisionType kind,Surface *s){
    (void)v;(void)s;
    if(kind==WEBKIT_POLICY_DECISION_TYPE_RESPONSE)return FALSE;
    if(kind==WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION){
        WebKitNavigationAction *a=webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        const char *uri=webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
        if(!g_strcmp0(uri,"dunlin-plugin://surface/index.html"))return FALSE;
    }
    webkit_policy_decision_ignore(decision);return TRUE;
}
static gboolean defer_close(GtkWidget *w,GdkEvent *e,Surface *s){
    (void)w;(void)e;
    /* Preserve an in-flight native dialog and a fault receipt until the current
     * owner has released its writer. Identity transitions also wait for it. */
    return s->in_call || (s->fault_timer && host_occupied(s));
}
static void closed(GtkWidget *w,Surface *s){
    (void)w;open_views=g_list_remove(open_views,s);s->closing=TRUE;
    if(s->c->store && !s->c->busy){
        UiJson *args=json_object_new_object();gchar *error=NULL;json_object_put(invoke(s,"ui.close",args,&error));json_object_put(args);g_free(error);
    }
    if(s->fault_timer){g_source_remove(s->fault_timer);s->fault_timer=0;if(!s->c->busy)persist_fault(s);}
    if(s->request_timer)g_source_remove(s->request_timer);
    while(!g_queue_is_empty(&s->requests))json_object_put(g_queue_pop_head(&s->requests));
    reset_download(s);g_free(s->fault_reason);g_object_set_data(G_OBJECT(s->web),"surface",NULL);
    WebKitUserContentManager *manager=webkit_web_view_get_user_content_manager(s->web);
    g_signal_handlers_disconnect_by_data(manager,s);g_signal_handlers_disconnect_by_data(s->web,s);
    json_object_put(s->owner);g_free(s->identifier);g_free(s->chat);g_free(s->slot);g_free(s->assets);g_free(s->script);g_free(s->html);g_hash_table_destroy(s->static_files);g_free(s);
}
void copilot_surface_check(LinuxClient *c,UiJson *inventory){
    GList *copy=g_list_copy(open_views);
    for(GList *node=copy;node;node=node->next){
        Surface *s=node->data;if(s->c!=c)continue;gboolean alive=FALSE;
        for(gsize i=0;i<(gsize)json_object_array_length(inventory);i++){
            UiJson *v=json_object_array_get_idx(inventory,i);
            if(!strcmp(ui_text(s->owner,"source"),ui_text(v,"source"))&&!strcmp(ui_text(s->owner,"package"),ui_text(v,"package"))&&
               !strcmp(ui_text(s->owner,"version"),ui_text(v,"version"))&&!strcmp(ui_text(s->owner,"runtimeID"),ui_text(v,"runtimeID"))&&json_object_get_boolean(ui_get(v,"active"))){alive=TRUE;break;}
        }
        if(!alive && !defer_close(NULL,NULL,s))gtk_widget_destroy(s->window);
    }
    g_list_free(copy);
}
gboolean copilot_surface_close_all(LinuxClient *c){
    gboolean closed_all=TRUE;
    GList *copy=g_list_copy(open_views);
    for(GList *l=copy;l;l=l->next){
        Surface *s=l->data;if(s->c!=c)continue;
        if(defer_close(NULL,NULL,s))closed_all=FALSE;else gtk_widget_destroy(s->window);
    }
    g_list_free(copy);return closed_all;
}
void copilot_surface_open(LinuxClient *c,UiJson *descriptor,const char *slot,const char *assets){
    if(c->busy)return;
    Surface *s=g_new0(Surface,1);s->c=c;s->owner=json_object_get(descriptor);s->identifier=g_uuid_string_random();s->chat=g_strdup(c->chat_id);s->slot=g_strdup(slot);s->assets=g_strdup(assets);
    s->static_files=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
    gchar *index=g_build_filename(assets,"assets.json",NULL);UiJson *manifest=json_object_from_file(index),*files=ui_get(manifest,"files");g_free(index);
    for(gsize i=0;files && i<(gsize)json_object_array_length(files);i++){
        UiJson *v=json_object_array_get_idx(files,i);const char *name=ui_text(v,"path");
        if((g_str_has_prefix(name,"web/")||g_str_has_prefix(name,"toolchain/node_modules/@esbuild/"))&&!strstr(name,"..")&&strlen(ui_text(v,"sha256"))==64)g_hash_table_insert(s->static_files,g_strdup(name),g_strdup(ui_text(v,"sha256")));
    }
    json_object_put(manifest);UiJson *options=json_object_new_object();
    ui_string(options,"slot",slot);ui_string(options,"chat",s->chat);ui_string(options,"theme","system");ui_string(options,"package",ui_text(descriptor,"package"));
    json_object_object_add(options,"config",json_object_get(ui_get(descriptor,"config")));json_object_object_add(options,"presentations",json_object_new_array());
    json_object_object_add(options,"presentationPublisher",json_object_get(ui_get(descriptor,"presentationPublisher")));
    GError *compile_error=NULL;gchar *code=client_source(s,ui_text(descriptor,"code"),&compile_error);
    if(!code && g_error_matches(compile_error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT)){
        g_clear_error(&compile_error);code=g_strdup("throw new SyntaxError()");
    }
    if(!code){copilot_status(c,compile_error?compile_error->message:"无法转换插件代码");g_clear_error(&compile_error);json_object_put(s->owner);json_object_put(options);g_hash_table_destroy(s->static_files);g_free(s->identifier);g_free(s->chat);g_free(s->slot);g_free(s->assets);g_free(s);return;}
    s->script=g_strdup_printf("(()=>{let seq=0,registration;const pending=new Map();globalThis.dunlinReply=(id,val,err)=>{let p=pending.get(id);if(!p)return;pending.delete(id);if(err)p[1](Error(err));else p[0](val)};"
      "const rpc=(command,args)=>new Promise((ok,no)=>{const id=++seq;pending.set(id,[ok,no]);webkit.messageHandlers.rpc.postMessage({id,command,args:args||{}})});"
      "const fault=e=>webkit.messageHandlers.fault.postMessage(DunlinDSH.surfaceDiagnostic(e));DunlinDSH.installTransport(rpc);globalThis.__ModuleLoader__={load(v){if(registration)throw Error('duplicate client');registration=v}};"
      "try{%s\n}catch(e){fault(e);return}DunlinDSH.mountNativeSurface(DunlinDSH,registration,%s,rpc,fault,preference=>rpc('theme.select',{preference})).catch(fault)})();",code,json_object_to_json_string(options));g_free(code);json_object_put(options);
    const gboolean pixi=!strcmp(ui_text(descriptor,"package"),"dsh-pet-live2d")&&!strcmp(ui_text(descriptor,"digest"),"b6279c348e6e15ac3f29cf3430c1d106b9236660b835d1fd28acba05daa7a67b");
    s->html=g_strdup_printf("<!doctype html><html><head><meta charset='utf-8'><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src dunlin-plugin://surface%s; style-src dunlin-plugin://surface 'unsafe-inline'; img-src dunlin-plugin://surface data: blob:; font-src dunlin-plugin://surface data:; connect-src dunlin-plugin://surface; media-src dunlin-plugin://surface blob:; worker-src blob:; frame-src 'none'; form-action 'none'; base-uri 'none'\"><link rel='stylesheet' href='runtime.css'><style>html,body,#surface{margin:0;min-height:100vh;font:14px sans-serif}body{background:transparent}</style></head><body><div id='surface'></div><script src='runtime.js'></script><script src='client.js'></script></body></html>",pixi?" 'unsafe-eval'":"");
    WebKitWebContext *ctx=webkit_web_context_new_ephemeral();webkit_web_context_register_uri_scheme(ctx,"dunlin-plugin",scheme,NULL,NULL);
    WebKitSecurityManager *security=webkit_web_context_get_security_manager(ctx);webkit_security_manager_register_uri_scheme_as_secure(security,"dunlin-plugin");webkit_security_manager_register_uri_scheme_as_cors_enabled(security,"dunlin-plugin");
    WebKitUserContentManager *m=webkit_user_content_manager_new();g_signal_connect(m,"script-message-received::rpc",G_CALLBACK(message),s);g_signal_connect(m,"script-message-received::fault",G_CALLBACK(fault),s);
    webkit_user_content_manager_register_script_message_handler(m,"rpc");webkit_user_content_manager_register_script_message_handler(m,"fault");
    /* A syntax error in the client or its runtime occurs before either script
     * can install a listener. Seed the trusted document at its creation. */
    const char *startup=
        "(()=>{function report(e){const v=e.error||e.reason||e;"
        "const k=['TypeError','RangeError','ReferenceError','SyntaxError'].indexOf(v.name)>=0?v.name:'Error';"
        "webkit.messageHandlers.fault.postMessage(k+' line '+(Number.isSafeInteger(e.lineno)?e.lineno:0))}"
        "addEventListener('unhandledrejection',report);"
        "addEventListener('error',e=>{if(e.target===window || (e.target.tagName==='SCRIPT' && "
        "(e.target.getAttribute('src')==='client.js'||e.target.getAttribute('src')==='runtime.js')))report(e)},true)})();";
    WebKitUserScript *guard=webkit_user_script_new(startup,WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,NULL,NULL);
    webkit_user_content_manager_add_script(m,guard);webkit_user_script_unref(guard);
    s->web=g_object_new(WEBKIT_TYPE_WEB_VIEW,"web-context",ctx,"user-content-manager",m,NULL);g_object_unref(ctx);g_object_unref(m);
    gtk_widget_set_name(GTK_WIDGET(s->web),"plugin-surface");g_object_set_data(G_OBJECT(s->web),"surface",s);g_signal_connect(s->web,"decide-policy",G_CALLBACK(navigation),s);
    g_signal_connect(s->web,"web-process-terminated",G_CALLBACK(process_lost),s);
    s->window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_transient_for(GTK_WINDOW(s->window),c->window);gtk_window_set_destroy_with_parent(GTK_WINDOW(s->window),TRUE);
    gtk_window_set_title(GTK_WINDOW(s->window),ui_text(descriptor,"package"));gtk_window_set_default_size(GTK_WINDOW(s->window),880,640);
    if(!strcmp(slot,"shell.overlay")){
        GdkRGBA empty={0,0,0,0};GdkScreen *screen=gtk_widget_get_screen(s->window);GdkVisual *visual=gdk_screen_get_rgba_visual(screen);
        if(visual)gtk_widget_set_visual(s->window,visual);
        gtk_widget_set_app_paintable(s->window,TRUE);webkit_web_view_set_background_color(s->web,&empty);
        gtk_window_set_keep_above(GTK_WINDOW(s->window),TRUE);gtk_window_set_skip_taskbar_hint(GTK_WINDOW(s->window),TRUE);
        gtk_window_set_type_hint(GTK_WINDOW(s->window),GDK_WINDOW_TYPE_HINT_UTILITY);gtk_window_set_default_size(GTK_WINDOW(s->window),480,400);
        gtk_widget_add_events(GTK_WIDGET(s->web),GDK_BUTTON_PRESS_MASK|GDK_BUTTON_RELEASE_MASK|GDK_POINTER_MOTION_MASK);
        g_signal_connect(s->web,"button-press-event",G_CALLBACK(overlay_press),s);g_signal_connect(s->web,"motion-notify-event",G_CALLBACK(overlay_move),s);
        g_signal_connect(s->web,"button-release-event",G_CALLBACK(overlay_release),s);
    }
    gtk_container_add(GTK_CONTAINER(s->window),GTK_WIDGET(s->web));g_signal_connect(s->window,"delete-event",G_CALLBACK(defer_close),s);g_signal_connect(s->window,"destroy",G_CALLBACK(closed),s);
    open_views=g_list_prepend(open_views,s);gtk_widget_show_all(s->window);webkit_web_view_load_uri(s->web,"dunlin-plugin://surface/index.html");
}
