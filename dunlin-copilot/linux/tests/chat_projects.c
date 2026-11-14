/* Native GTK integration driver. Only run inside the disposable data tree created by the peer. */
#include "activity.h"
#include "chat.h"
#include "settings.h"
#include "ui.h"
#include <string.h>

static int approvals;
void copilot_status(LinuxClient *client, const gchar *message) {
    (void)client;
    g_print("status %s\n", message);
}
void copilot_show(LinuxClient *client, const gchar *page) {
    gtk_stack_set_visible_child_name(client->pages, page);
}
int copilot_chat_approval(LinuxClient *client, const char *tool, const char *args) {
    (void)client;
    g_assert_nonnull(strstr(tool, "write_file"));
    g_assert_nonnull(strstr(args, "disallowed.txt"));
    g_atomic_int_inc(&approvals);
    return 0;
}
static GtkWidget *locate(GtkWidget *node, const char *key) {
    if ((GTK_IS_BUTTON(node) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(node)), key)) ||
        (GTK_IS_ENTRY(node) && !g_strcmp0(gtk_entry_get_placeholder_text(GTK_ENTRY(node)), key)) ||
        !g_strcmp0(gtk_widget_get_name(node), key))
        return node;
    if (!GTK_IS_CONTAINER(node))
        return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(node));
    GtkWidget *found = NULL;
    for (GList *p = children; p && !found; p = p->next)
        found = locate(p->data, key);
    g_list_free(children);
    return found;
}
static void click(GtkWidget *page, const char *caption) {
    GtkWidget *w = locate(page, caption);
    if (!w)
        g_error("missing button %s", caption);
    g_assert_true(gtk_widget_is_sensitive(w));
    gtk_button_clicked(GTK_BUTTON(w));
}
static char *contents(GtkWidget *view) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(buf, &a, &z);
    return gtk_text_buffer_get_text(buf, &a, &z, FALSE);
}
static gboolean inspect_dialog(gpointer caption) {
    GList *wins = gtk_window_list_toplevels();
    gboolean found = FALSE;
    for (GList *p = wins; p; p = p->next) {
        if (!GTK_IS_DIALOG(p->data))
            continue;
        GtkWidget *w = locate(p->data, caption);
        if (!w)
            continue;
        g_assert_true(gtk_widget_is_sensitive(w));
        if (GTK_IS_TOGGLE_BUTTON(w))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), TRUE);
        found = TRUE;
        gtk_dialog_response(GTK_DIALOG(p->data), GTK_RESPONSE_ACCEPT);
        break;
    }
    g_list_free(wins);
    g_assert_true(found);
    return G_SOURCE_REMOVE;
}
static void rich_snapshot(GtkWidget *node, GString *markup, GString *text, guint *grids) {
    if (GTK_IS_LABEL(node)) {
        g_string_append(markup, gtk_label_get_label(GTK_LABEL(node)));
        g_string_append_c(markup, '\n');
        g_string_append(text, gtk_label_get_text(GTK_LABEL(node)));
        g_string_append_c(text, '\n');
        return;
    }
    if (GTK_IS_GRID(node))
        (*grids)++;
    if (!GTK_IS_CONTAINER(node))
        return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(node));
    for (GList *p = children; p; p = p->next)
        rich_snapshot(p->data, markup, text, grids);
    g_list_free(children);
}
typedef struct {
    const gchar *path;
    gboolean set;
} FileSelection;
static gboolean choose_fixture(gpointer data) {
    FileSelection *selection = data;
    GList *wins = gtk_window_list_toplevels();
    GtkWidget *dialog = NULL;
    for (GList *i = wins; i; i = i->next)
        if (GTK_IS_FILE_CHOOSER_DIALOG(i->data))
            dialog = i->data;
    g_list_free(wins);
    g_assert_nonnull(dialog);
    if (!selection->set) {
        selection->set = TRUE;
        g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), selection->path));
        return G_SOURCE_CONTINUE;
    }
    gchar *current = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    gboolean ready = !g_strcmp0(current, selection->path);
    g_free(current);
    if (!ready)
        return G_SOURCE_CONTINUE;
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    return G_SOURCE_REMOVE;
}
static gboolean accept_attachment(gpointer data) {
    GList *wins = gtk_window_list_toplevels();
    GtkWidget *preview = NULL;
    for (GList *p = wins; p; p = p->next)
        if (GTK_IS_DIALOG(p->data) && locate(p->data, "添加到本轮"))
            preview = p->data;
    g_list_free(wins);
    if (!preview)
        return G_SOURCE_CONTINUE;
    GtkWidget *v = locate(preview, "GtkTextView");
    g_assert_nonnull(v);
    char *text = contents(v);
    g_assert_cmpstr(text, ==, (char *)data);
    g_free(text);
    gtk_dialog_response(GTK_DIALOG(preview), GTK_RESPONSE_APPLY);
    return G_SOURCE_REMOVE;
}
static gboolean image_dialog(gpointer selection) {
    gboolean editing = GPOINTER_TO_INT(selection);
    GList *wins = gtk_window_list_toplevels();
    GtkWidget *preview = NULL;
    for (GList *p = wins; p; p = p->next)
        if (GTK_IS_DIALOG(p->data) && locate(p->data, "GtkImage") &&
            !g_strcmp0(gtk_window_get_title(GTK_WINDOW(p->data)), "剪贴板图片.png"))
            preview = p->data;
    g_list_free(wins);
    if (!preview)
        return G_SOURCE_CONTINUE;
    GtkWidget *image = locate(preview, "GtkImage");
    g_assert_nonnull(gtk_image_get_pixbuf(GTK_IMAGE(image)));
    gtk_dialog_response(GTK_DIALOG(preview), editing ? GTK_RESPONSE_APPLY : GTK_RESPONSE_CLOSE);
    return G_SOURCE_REMOVE;
}
static void inspect_historical_receipt(GtkWidget *activity, const char *work) {
    click(activity, "查看修改");
    GList *wins = gtk_window_list_toplevels();
    gboolean shown = FALSE;
    for (GList *p = wins; p; p = p->next) {
        if (!GTK_IS_DIALOG(p->data) || g_strcmp0(gtk_window_get_title(GTK_WINDOW(p->data)), "文件修改记录"))
            continue;
        GtkWidget *v = locate(p->data, "GtkTextView");
        char *value = contents(v);
        g_assert_nonnull(strstr(value, "current revision"));
        g_assert_nonnull(strstr(value, "restored original"));
        g_free(value);
        shown = TRUE;
        gtk_dialog_response(GTK_DIALOG(p->data), GTK_RESPONSE_CLOSE);
        break;
    }
    g_list_free(wins);
    g_assert_true(shown);
    g_idle_add(inspect_dialog, "确认回退");
    click(activity, "检查回退…");
    char *path = g_build_filename(work, "receipt-fixture.txt", NULL), *disk = NULL;
    g_assert_true(g_file_get_contents(path, &disk, NULL, NULL));
    g_assert_cmpstr(disk, ==, "restored original");
    g_free(disk);
    g_free(path);
}
static gboolean inspect_preview(gpointer data) {
    GList *wins = gtk_window_list_toplevels();
    gboolean found = FALSE;
    for (GList *p = wins; p; p = p->next) {
        if (!GTK_IS_DIALOG(p->data))
            continue;
        GtkWidget *v = locate(p->data, "GtkTextView");
        if (!v)
            continue;
        char *text = contents(v);
        g_assert_cmpstr(text, ==, (char *)data);
        g_free(text);
        found = TRUE;
        g_assert_nonnull(locate(p->data, "固定到项目上下文"));
        gtk_dialog_response(GTK_DIALOG(p->data), GTK_RESPONSE_APPLY);
        break;
    }
    g_list_free(wins);
    g_assert_true(found);
    return G_SOURCE_REMOVE;
}
static void wait_run(LinuxClient *client) {
    g_assert_true(client->busy);
    gint64 deadline = g_get_monotonic_time() + 25 * G_TIME_SPAN_SECOND;
    while (client->busy && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {
        }
        g_usleep(5000);
    }
    g_assert_false(client->busy);
}
static void expect_reply(LinuxClient *c, const char *id, const char *expected) {
    char *raw = dc_messages(c->store, id);
    g_assert_nonnull(raw);
    UiJson *messages = json_tokener_parse(raw);
    dc_free(raw);
    g_assert_cmpuint(json_object_array_length(messages), >=, 2);
    UiJson *last = json_object_array_get_idx(messages, json_object_array_length(messages) - 1);
    g_assert_cmpstr(ui_text(last, "role"), ==, "assistant");
    g_assert_cmpstr(ui_text(last, "content"), ==, expected);
    json_object_put(messages);
}
static void send(LinuxClient *c, GtkWidget *page, const char *text) {
    GtkWidget *entry = locate(page, "chat-input");
    g_assert_nonnull(entry);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry)), text, -1);
    click(page, "发送");
    wait_run(c);
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    g_assert_cmpint(argc, ==, 4);
    LinuxClient c = {0};
    c.data_dir = argv[1];
    c.store = dc_open(c.data_dir);
    g_assert_nonnull(c.store);
    g_mutex_init(&c.lock);
    c.window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    c.pages = GTK_STACK(gtk_stack_new());
    gtk_container_add(GTK_CONTAINER(c.window), GTK_WIDGET(c.pages));
    UiJson *cfg = json_tokener_parse(
        "{\"op\":\"config.save\",\"revision\":0,\"models\":[{\"id\":\"m-fixture\",\"name\":\"Peer\","
        "\"model\":\"fixture\",\"protocolName\":\"chat\",\"maxOutput\":256,\"inputModalities\":[\"text\","
        "\"image\"],\"authentication\":\"none\"}],\"providers\":[]}");
    ui_string(json_object_array_get_idx(ui_get(cfg, "models"), 0), "endpoint", argv[3]);
    UiJson *saved = ui_call(&c, UI_WORKSPACE, cfg);
    g_assert_nonnull(saved);
    json_object_put(saved);
    g_assert_true(copilot_pref_save(&c, "default-model", "m-fixture"));
    UiJson *project =
        json_tokener_parse("{\"op\":\"projects.create\",\"id\":\"p\",\"name\":\"验收项目\",\"model\":\"m-"
                           "fixture\",\"instructions\":\"\",\"policy\":\"ask\",\"context\":[]}");
    ui_string(project, "path", argv[2]);
    saved = ui_call(&c, UI_WORKSPACE, project);
    g_assert_nonnull(saved);
    json_object_put(saved);
    GtkWidget *chat = copilot_chat_new(&c), *projects = copilot_projects_new(&c),
              *activity = copilot_activity_new(&c);
    gtk_stack_add_named(c.pages, chat, "chat");
    gtk_stack_add_named(c.pages, projects, "projects");
    gtk_stack_add_named(c.pages, activity, "activity");
    gtk_widget_show_all(GTK_WIDGET(c.window));
    copilot_chat_refresh(&c);
    copilot_show(&c, "chat");
    click(chat, "新聊天");
    g_assert_cmpstr(c.model_id, ==, "m-fixture");
    g_assert_cmpstr(c.project_id, ==, "");
    send(&c, chat, "ordinary-fixture");
    expect_reply(&c, c.chat_id, "Reply ordinary-fixture");
    send(&c, chat, "retry-fixture");
    g_assert_true(gtk_widget_get_visible(locate(chat, "重试未答复的回合")));
    click(chat, "重试未答复的回合");
    wait_run(&c);
    expect_reply(&c, c.chat_id, "Reply retry-fixture");
    char *history = dc_messages(c.store, c.chat_id);
    UiJson *h = json_tokener_parse(history);
    dc_free(history);
    unsigned matches = 0;
    for (size_t i = 0; i < (size_t)json_object_array_length(h); i++) {
        UiJson *row = json_object_array_get_idx(h, i);
        if (!strcmp(ui_text(row, "role"), "user") && !strcmp(ui_text(row, "content"), "retry-fixture"))
            matches++;
    }
    g_assert_cmpuint(matches, ==, 1);
    json_object_put(h);
    g_assert_false(gtk_widget_get_visible(locate(chat, "重试未答复的回合")));
    send(&c, chat, "rich-fixture");
    send(&c, chat, "html-fixture");
    GString *styled = g_string_new(NULL), *text = g_string_new(NULL);
    guint tables = 0;
    rich_snapshot(chat, styled, text, &tables);
    g_assert_nonnull(strstr(styled->str, "18432"));
    g_assert_nonnull(strstr(styled->str, "<b>safe</b>"));
    g_assert_nonnull(strstr(styled->str, "<tt>code</tt>"));
    g_assert_cmpuint(tables, ==, 1);
    g_assert_nonnull(strstr(text->str, "<tag>"));
    g_assert_nonnull(strstr(text->str, "Native HTML"));
    g_assert_nonnull(strstr(text->str, "safe alternative"));
    g_assert_null(strstr(text->str, "never-load"));
    g_string_free(styled, TRUE);
    g_string_free(text, TRUE);
    copilot_show(&c, "projects");
    click(projects, "验收项目");
    g_assert_cmpstr(c.project_id, ==, "p");
    click(projects, "▸ src");
    g_idle_add(inspect_preview, "source-file-content");
    click(projects, "fixture.txt");
    click(projects, "新建项目对话");
    g_assert_cmpstr(c.model_id, ==, "m-fixture");
    g_assert_cmpstr(c.project_id, ==, "p");
    g_idle_add(inspect_dialog, "src/fixture.txt");
    click(chat, "本轮项目资料…");
    send(&c, chat, "project-reference");
    expect_reply(&c, c.chat_id, "Reference received");
    send(&c, chat, "project-fixture");
    expect_reply(&c, c.chat_id, "Reply project-fixture");
    gchar *attachment = g_build_filename(argv[2], "attach.txt", NULL);
    FileSelection file = {attachment, FALSE};
    g_timeout_add(35, choose_fixture, &file);
    g_timeout_add(35, accept_attachment, "attachment-body");
    click(chat, "添加附件…");
    g_assert_nonnull(locate(chat, "移除附件"));
    send(&c, chat, "attachment-message");
    expect_reply(&c, c.chat_id, "Attachment received");
    g_assert_null(locate(chat, "移除附件"));
    g_assert_nonnull(locate(chat, "附件 · attach.txt"));
    g_free(attachment);
    GdkPixbuf *pixels = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 4, 3);
    gdk_pixbuf_fill(pixels, 0x123f44ff);
    gtk_clipboard_set_image(gtk_widget_get_clipboard(chat, GDK_SELECTION_CLIPBOARD), pixels);
    g_object_unref(pixels);
    g_timeout_add(30, image_dialog, GINT_TO_POINTER(1));
    click(chat, "粘贴图片");
    click(chat, "移除附件");
    g_assert_null(locate(chat, "移除附件"));
    g_assert_null(locate(chat, "附件 · 剪贴板图片.png"));
    g_timeout_add(30, image_dialog, GINT_TO_POINTER(1));
    click(chat, "粘贴图片");
    send(&c, chat, "image-prompt");
    expect_reply(&c, c.chat_id, "Image received");
    g_assert_null(locate(chat, "移除附件"));
    g_timeout_add(30, image_dialog, GINT_TO_POINTER(0));
    click(chat, "附件 · 剪贴板图片.png");
    send(&c, chat, "deny-write");
    g_assert_cmpint(g_atomic_int_get(&approvals), ==, 1);
    char *denied = g_build_filename(argv[2], "disallowed.txt", NULL);
    g_assert_false(g_file_test(denied, G_FILE_TEST_EXISTS));
    g_free(denied);
    char *records = dc_messages(c.store, c.chat_id);
    g_assert_nonnull(strstr(records, "permission denied"));
    dc_free(records);
    GtkWidget *input = locate(chat, "chat-input");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(input)), "pending draft", -1);
    copilot_chat_flush(&c);
    copilot_chat_refresh(&c);
    char *draft = contents(input);
    g_assert_cmpstr(draft, ==, "pending draft");
    g_free(draft);
    copilot_show(&c, "activity");
    copilot_activity_refresh(&c);
    inspect_historical_receipt(activity, argv[2]);
    GtkWidget *task = locate(activity, "分配给子任务的目标");
    g_assert_nonnull(task);
    for (int i = 0; i < 2; i++) {
        gtk_entry_set_text(GTK_ENTRY(task), i ? "agent-two" : "agent-one");
        click(activity, "创建任务");
    }
    click(activity, "将当前项目聊天加入协作");
    UiJson *queued = ui_call(&c, UI_READ, ui_op("agents.list"));
    g_assert_cmpuint(json_object_array_length(queued), ==, 2);
    UiJson *target = json_object_array_get_idx(queued, 0);
    if (strcmp(ui_text(target, "task"), "agent-two"))
        target = json_object_array_get_idx(queued, 1);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(locate(activity, "agent-sender")), c.chat_id));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(locate(activity, "agent-receiver")),
                                              ui_text(target, "chat")));
    json_object_put(queued);
    gtk_entry_set_text(GTK_ENTRY(locate(activity, "作为资料发送给另一段对话")), "collaboration-example");
    click(activity, "发送协作消息");
    click(activity, "运行项目队列");
    wait_run(&c);
    UiJson *mr = ui_op("agents.messages");
    ui_string(mr, "project", "p");
    UiJson *mail = ui_call(&c, UI_READ, mr);
    g_assert_cmpuint(json_object_array_length(mail), ==, 1);
    g_assert_true(json_object_get_boolean(ui_get(json_object_array_get_idx(mail, 0), "delivered")));
    json_object_put(mail);
    UiJson *jobs = ui_call(&c, UI_READ, ui_op("agents.list"));
    g_assert_cmpuint(json_object_array_length(jobs), ==, 2);
    for (size_t i = 0; i < 2; i++) {
        UiJson *job = json_object_array_get_idx(jobs, i);
        g_assert_cmpstr(ui_text(job, "state"), ==, "succeeded");
        char *expected = g_strconcat("Reply ", ui_text(job, "task"), NULL);
        expect_reply(&c, ui_text(job, "chat"), expected);
        g_free(expected);
    }
    json_object_put(jobs);
    gtk_widget_destroy(GTK_WIDGET(c.window));
    dc_close(c.store);
    g_mutex_clear(&c.lock);
    g_free(c.chat_id);
    g_free(c.project_id);
    g_free(c.model_id);
    g_print(
        "PASS native GTK chat/retry, Markdown/HTML, project sources, text/image/clipboard attachments, "
        "denied writes, historical file restore, drafts, scheduled workers and delivered collaboration\n");
    return 0;
}
