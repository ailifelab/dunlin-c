/* Native WebKit message bridge -> original installed Project Manager -> C receipts -> GTK dispatcher. */
#include "activity.h"
#include "dunlin/knowledge.h"
#include "surface.h"
#include "ui.h"
#include <string.h>
#include <webkit2/webkit2.h>

static LinuxClient client;
static unsigned approvals;
void copilot_status(LinuxClient *c, const char *text) {
    (void)c;
    g_print("status %s\n", text);
}
void copilot_show(LinuxClient *c, const char *page) {
    (void)c;
    (void)page;
}
void copilot_chat_flush(LinuxClient *c) {
    (void)c;
}
void copilot_chat_refresh(LinuxClient *c) {
    (void)c;
}
int copilot_chat_approval(LinuxClient *c, const char *name, const char *args) {
    (void)c;
    (void)name;
    (void)args;
    approvals++;
    return 0;
}
static UiJson *call(UiRoute route, UiJson *value) {
    UiJson *r = ui_call(&client, route, value);
    g_assert_nonnull(r);
    return r;
}
static void pump(gint64 ms) {
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(NULL, FALSE)) {
        }
        g_usleep(5000);
    }
}
static void action(WebKitWebView *web, const char *script) {
    webkit_web_view_run_javascript(web, script, NULL, NULL, NULL);
}
static void assert_manual_queued(void) {
    UiJson *jobs = call(UI_READ, ui_op("agents.list"));
    gboolean preserved = FALSE;
    for (size_t i = 0; i < json_object_array_length(jobs); i++) {
        UiJson *job = json_object_array_get_idx(jobs, i);
        if (!strcmp(ui_text(job, "id"), "manual")) {
            g_assert_cmpstr(ui_text(job, "state"), ==, "queued");
            preserved = TRUE;
        }
    }
    g_assert_true(preserved);
    json_object_put(jobs);
}
static void consume(WebKitWebView *web, const char *op, const char *script) {
    /* Establish receipt visibility before the timer can consume it. */
    client.pending_identity = g_strdup("defer-test-dispatch");
    action(web, script);
    gboolean reported = FALSE;
    for (unsigned i = 0; i < 180 && !reported; i++) {
        pump(100);
        UiJson *rows = call(UI_READ, ui_op(op));
        reported = json_object_array_length(rows) > 0;
        json_object_put(rows);
    }
    if (!reported) {
        UiJson *f = call(UI_EXTENSIONS, ui_op("extensions.faults"));
        g_error("Missing %s; faults %s", op, json_object_to_json_string(f));
    }
    g_print("reported %s\n", op);
    assert_manual_queued();
    g_clear_pointer(&client.pending_identity, g_free);
    copilot_activity_dispatch_plugins(&client);
    g_assert_true(client.busy);
    for (unsigned i = 0; i < 200 && client.busy; i++)
        pump(75);
    g_assert_false(client.busy);
    assert_manual_queued();
    UiJson *after = call(UI_READ, ui_op(op));
    g_assert_cmpuint(json_object_array_length(after), ==, 0);
    json_object_put(after);
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    g_assert_cmpint(argc, ==, 7);
    client.data_dir = argv[1];
    client.identity_id = g_strdup("local");
    client.chat_id = g_strdup("chat");
    client.project_id = g_strdup("p");
    client.model_id = g_strdup("m-fixture");
    client.window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    g_mutex_init(&client.lock);
    client.store = dc_open(client.data_dir);
    g_assert_nonnull(client.store);
    g_assert_true(dc_bind_identity(client.store, "local"));
    UiJson *cfg =
        json_tokener_parse("{\"op\":\"config.save\",\"revision\":0,\"models\":[{\"id\":\"m-fixture\","
                           "\"name\":\"Local\",\"model\":\"fixture\",\"protocolName\":\"chat\","
                           "\"maxOutput\":256,\"authentication\":\"none\"}],\"providers\":[]}");
    ui_string(json_object_array_get_idx(ui_get(cfg, "models"), 0), "endpoint", argv[3]);
    json_object_put(call(UI_WORKSPACE, cfg));
    UiJson *p =
        json_tokener_parse("{\"op\":\"projects.create\",\"id\":\"p\",\"name\":\"Example\",\"model\":\"m-"
                           "fixture\",\"policy\":\"ask\",\"instructions\":\"\",\"context\":[]}");
    ui_string(p, "path", argv[2]);
    json_object_put(call(UI_WORKSPACE, p));
    g_assert_true(dc_add_chat(client.store, "chat", "Native workbench", argv[2]));
    json_object_put(
        call(UI_WORKSPACE,
             json_tokener_parse(
                 "{\"op\":\"chats.update\",\"id\":\"chat\",\"field\":\"model\",\"value\":\"m-fixture\"}")));
    json_object_put(call(
        UI_WORKSPACE,
        json_tokener_parse("{\"op\":\"agents.create\",\"id\":\"manual\",\"model\":\"m-fixture\","
                           "\"project\":\"p\",\"chat\":\"\",\"task\":\"do-not-run\",\"revision\":1}")));
    UiJson *q = ui_op("extensions.assets");
    ui_string(q, "path", argv[4]);
    json_object_put(call(UI_EXTENSIONS, q));
    q = ui_op("compat.enable");
    ui_string(q, "node", argv[5]);
    json_object_put(call(UI_EXTENSIONS, q));
    q = ui_op("extensions.import");
    ui_string(q, "path", argv[6]);
    UiJson *installed = call(UI_EXTENSIONS, q);
    UiJson *row = json_object_array_get_idx(installed, 0);
    g_assert_nonnull(row);
    q = ui_op("extensions.bind");
    const char *keys[] = {"source", "package", "version"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++)
        ui_string(q, keys[i], ui_text(row, keys[i]));
    json_object_object_add(q, "enabled", json_object_new_boolean(TRUE));
    json_object_object_add(q, "acceptHostPermissions", json_object_new_boolean(TRUE));
    json_object_put(call(UI_EXTENSIONS, q));
    json_object_put(installed);
    GtkWidget *activity = copilot_activity_new(&client);
    gtk_container_add(GTK_CONTAINER(client.window), activity);
    gtk_widget_show_all(GTK_WIDGET(client.window));
    UiJson *clients = call(UI_EXTENSIONS, ui_op("extensions.clients"));
    g_assert_cmpuint(json_object_array_length(clients), ==, 1);
    copilot_surface_open(&client, json_object_array_get_idx(clients, 0), "dunlin.workbench", argv[4]);
    json_object_put(clients);
    WebKitWebView *web = NULL;
    GList *windows = gtk_window_list_toplevels();
    for (GList *it = windows; it; it = it->next) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(it->data));
        if (WEBKIT_IS_WEB_VIEW(child))
            web = WEBKIT_WEB_VIEW(child);
    }
    g_list_free(windows);
    g_assert_nonnull(web);
    pump(1500);
    while (webkit_web_view_is_loading(web))
        pump(50);
    consume(web, "agents.nativePending",
            "webkit.messageHandlers.rpc.postMessage({id:19881,command:'native.session.prompt',args:{id:'"
            "chat',mode:'queue',content:[{type:'text',text:'sdk-prompt'}]}})");
    consume(web, "helpers.pending",
            "webkit.messageHandlers.rpc.postMessage({id:19882,command:'http',args:{method:'POST',url:'/"
            "_dsh/dsh-project-manager/api/goal/"
            "title',body:{session:'chat',text:'helper-prompt',route:{provider:'dunlin-native',model:'m-"
            "fixture'}}}})");
    g_assert_cmpuint(approvals, ==, 0);
    copilot_surface_close_all(&client);
    gtk_widget_destroy(GTK_WIDGET(client.window));
    dc_close(client.store);
    g_mutex_clear(&client.lock);
    g_print("PASS reported workbench session prompt and text helper dispatched without draining unrelated "
            "manual tasks\n");
    return 0;
}
