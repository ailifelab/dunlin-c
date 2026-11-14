#include "chat.h"
#include "dunlin/image_resource.h"
#include "rich_message.h"
#include "settings.h"
#include "ui.h"
#include <json-c/json.h>
#include <string.h>

typedef struct json_object Json;
typedef struct {
    LinuxClient *client;
    GtkWidget *chat, *project_page, *tree, *title, *transcript, *messages, *entry;
    GtkWidget *send, *stop, *recover, *end_turn, *retry, *busy_label, *manage, *actions;
    GtkWidget *project_list, *project_name, *project_path, *project_actions;
    GtkWidget *project_chats, *project_files, *file_path;
    GtkTreeStore *rows;
    GtkWidget *project_references, *turn_refs, *pending_box, *add_attachment, *paste;
    char *pending_attachment;
    GHashTable *references;
    char *reference_chat, *reference_domain, *retry_run, *retry_chat, *retry_directory;
    Json *projects, *chats;
    char *draft_for, *file_dir;
    guint draft_timer;
    gint cancelled;
    guint64 generation;
    gboolean syncing, writing_input, follow_tail;
} ChatUI;

enum { KIND, ID, TITLE, COLS };
enum { REGULAR, PROJECT, ARCHIVE, CHAT };
static const char state_key[] = "dunlin-chat-ui";
static void render_project(ChatUI *ui);
static void render_chat(ChatUI *ui);
static Json *reference_selection(ChatUI *ui);
static void create_chat(ChatUI *ui, const char *project_id);

static Json *get(Json *obj, const char *key) {
    Json *v = NULL;
    if (obj)
        json_object_object_get_ex(obj, key, &v);
    return v;
}
static const char *str(Json *obj, const char *key) {
    Json *v = get(obj, key);
    return json_object_is_type(v, json_type_string) ? json_object_get_string(v) : "";
}
static gboolean flag(Json *obj, const char *key) {
    return json_object_get_boolean(get(obj, key));
}
static void put(Json *obj, const char *key, const char *v) {
    json_object_object_add(obj, key, json_object_new_string(v ? v : ""));
}
static Json *request(const char *op) {
    Json *r = json_object_new_object();
    put(r, "op", op);
    return r;
}
static Json *lookup(Json *list, const char *id) {
    if (!id || !json_object_is_type(list, json_type_array))
        return NULL;
    for (size_t i = 0; i < (size_t)json_object_array_length(list); i++) {
        Json *row = json_object_array_get_idx(list, i);
        if (!strcmp(str(row, "id"), id))
            return row;
    }
    return NULL;
}
static Json *take_json(char *raw) {
    Json *j = raw ? json_tokener_parse(raw) : NULL;
    dc_free(raw);
    return j;
}
/* UI writes only while idle; worker hooks never reenter the store writer. */
static Json *call(ChatUI *ui, Json *r, gboolean *ok) {
    Json *reply = NULL, *data = NULL;
    if (!ui->client->busy) {
        g_mutex_lock(&ui->client->lock);
        reply = take_json(dc_workspace_request(ui->client->store, json_object_to_json_string(r)));
        g_mutex_unlock(&ui->client->lock);
    }
    gboolean good = flag(reply, "ok");
    if (good)
        data = json_object_get(get(reply, "data"));
    else
        copilot_status(ui->client, reply ? str(reply, "error") : "工作区正在运行或不可用。");
    if (ok)
        *ok = good;
    json_object_put(reply);
    json_object_put(r);
    return data;
}
static gboolean chat_prop(ChatUI *ui, const char *id, const char *field, Json *value) {
    Json *r = request("chats.update");
    put(r, "id", id);
    put(r, "field", field);
    json_object_object_add(r, "value", value);
    gboolean ok;
    json_object_put(call(ui, r, &ok));
    return ok;
}
static void replace(char **slot, const char *value) {
    char *next = g_strdup(value ? value : "");
    g_free(*slot);
    *slot = next;
}
static void set_margins(GtkWidget *w, int px) {
    gtk_widget_set_margin_start(w, px);
    gtk_widget_set_margin_end(w, px);
    gtk_widget_set_margin_top(w, px);
    gtk_widget_set_margin_bottom(w, px);
}
static GtkWidget *scroll(GtkWidget *child) {
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}
static char *buffer_text(GtkWidget *w) {
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(b, &a, &z);
    return gtk_text_buffer_get_text(b, &a, &z, FALSE);
}
static void set_input(ChatUI *ui, const char *s) {
    ui->writing_input = TRUE;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->entry)), s ? s : "", -1);
    ui->writing_input = FALSE;
}
static gboolean flush_draft(gpointer ptr) {
    ChatUI *ui = ptr;
    ui->draft_timer = 0;
    if (ui->entry && ui->draft_for && *ui->draft_for && !ui->client->busy) {
        char *s = buffer_text(ui->entry);
        chat_prop(ui, ui->draft_for, "draft", json_object_new_string(s));
        g_free(s);
    }
    return G_SOURCE_REMOVE;
}
void copilot_chat_flush(LinuxClient *client) {
    ChatUI *ui = g_object_get_data(G_OBJECT(client->window), state_key);
    if (ui && ui->draft_timer) {
        g_source_remove(ui->draft_timer);
        flush_draft(ui);
    }
}
static void draft_changed(GtkTextBuffer *b, ChatUI *ui) {
    (void)b;
    if (ui->writing_input || ui->client->busy || !ui->draft_for || !*ui->draft_for)
        return;
    if (ui->draft_timer)
        g_source_remove(ui->draft_timer);
    ui->draft_timer = g_timeout_add(250, flush_draft, ui);
}
static void release_ui(gpointer p) {
    ChatUI *ui = p;
    if (ui->draft_timer)
        g_source_remove(ui->draft_timer);
    json_object_put(ui->projects);
    json_object_put(ui->chats);
    g_free(ui->retry_chat);
    g_free(ui->retry_directory);
    g_free(ui->retry_run);
    g_free(ui->reference_chat);
    g_free(ui->reference_domain);
    g_hash_table_unref(ui->references);
    g_free(ui->pending_attachment);
    g_clear_object(&ui->rows);
    g_free(ui->draft_for);
    g_free(ui->file_dir);
    g_free(ui);
}
static ChatUI *state(LinuxClient *client) {
    ChatUI *ui = g_object_get_data(G_OBJECT(client->window), state_key);
    if (!ui) {
        ui = g_new0(ChatUI, 1);
        ui->client = client;
        ui->references = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_object_set_data_full(G_OBJECT(client->window), state_key, ui, release_ui);
    }
    return ui;
}
static gboolean preview_attachment(ChatUI *ui, Json *attachment, gboolean adding) {
    if (!attachment)
        return FALSE;
    GtkWidget *d = gtk_dialog_new_with_buttons(str(attachment, "name"), ui->client->window,
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "关闭",
                                               GTK_RESPONSE_CLOSE, NULL);
    if (adding)
        gtk_dialog_add_button(GTK_DIALOG(d), "添加到本轮", GTK_RESPONSE_APPLY);
    gtk_window_set_default_size(GTK_WINDOW(d), 560, 380);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    set_margins(area, 12);
    const char *data = str(attachment, "dataURL");
    if (!strcmp(str(attachment, "format"), "png") && g_str_has_prefix(data, "data:image/png;base64,")) {
        gsize size = 0;
        guchar *bytes = g_base64_decode(data + strlen("data:image/png;base64,"), &size);
        GInputStream *stream = g_memory_input_stream_new_from_data(bytes, size, g_free);
        GdkPixbuf *image = gdk_pixbuf_new_from_stream_at_scale(stream, 490, 330, TRUE, NULL, NULL);
        g_object_unref(stream);
        if (image) {
            gtk_box_pack_start(GTK_BOX(area), gtk_image_new_from_pixbuf(image), TRUE, TRUE, 0);
            g_object_unref(image);
        } else {
            gtk_widget_destroy(d);
            copilot_status(ui->client, "无法预览此 PNG 图片。");
            return FALSE;
        }
    } else {
        GtkWidget *text = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), str(attachment, "text"),
                                 -1);
        gtk_box_pack_start(GTK_BOX(area), scroll(text), TRUE, TRUE, 0);
    }
    gtk_widget_show_all(d);
    gboolean apply = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_APPLY;
    gtk_widget_destroy(d);
    return apply;
}
static void open_attachment(GtkButton *b, ChatUI *ui) {
    Json *attachment = g_object_get_data(G_OBJECT(b), "attachment");
    if (!attachment)
        return;
    Json *r = request("attachments.read");
    put(r, "chat", ui->client->chat_id);
    if (get(attachment, "sequence")) {
        json_object_object_add(r, "sequence", json_object_get(get(attachment, "sequence")));
        put(r, "canonicalHash", str(attachment, "canonicalHash"));
    } else
        put(r, "draftID", str(attachment, "id"));
    Json *data = ui_call(ui->client, UI_READ, r);
    preview_attachment(ui, data, FALSE);
    json_object_put(data);
}
static void release_json(gpointer ptr) {
    json_object_put(ptr);
}
static void attach_button(ChatUI *ui, GtkWidget *box, Json *attachment) {
    if (!attachment || !*str(attachment, "name"))
        return;
    char *title = g_strdup_printf("附件 · %s", str(attachment, "name"));
    GtkWidget *b = ui_button(box, title, G_CALLBACK(open_attachment), ui);
    g_free(title);
    g_object_set_data_full(G_OBJECT(b), "attachment", json_object_get(attachment), release_json);
}
static void remove_attachment(GtkButton *b, ChatUI *ui) {
    (void)b;
    if (ui->client->busy || !ui->pending_attachment)
        return;
    Json *r = request("attachments.remove");
    put(r, "chat", ui->client->chat_id);
    put(r, "expectedID", ui->pending_attachment);
    json_object_put(call(ui, r, NULL));
    copilot_chat_refresh(ui->client);
}
static void add_attachment(GtkButton *b, ChatUI *ui) {
    (void)b;
    LinuxClient *c = ui->client;
    if (c->busy || !c->chat_id)
        return;
    GtkWidget *choose =
        gtk_file_chooser_dialog_new("选择 PNG 图片或 UTF-8 文本", c->window, GTK_FILE_CHOOSER_ACTION_OPEN,
                                    "取消", GTK_RESPONSE_CANCEL, "预览", GTK_RESPONSE_ACCEPT, NULL);
    char *path = gtk_dialog_run(GTK_DIALOG(choose)) == GTK_RESPONSE_ACCEPT
                     ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(choose))
                     : NULL;
    gtk_widget_destroy(choose);
    if (!path)
        return;
    const char *extension = strrchr(path, '.');
    const char *format = extension && !g_ascii_strcasecmp(extension, ".png") ? "png" : "text";
    Json *r = request("attachments.previewFile");
    put(r, "chat", c->chat_id);
    put(r, "path", path);
    put(r, "format", format);
    Json *data = call(ui, r, NULL);
    if (preview_attachment(ui, data, TRUE)) {
        r = request("attachments.stageFile");
        put(r, "chat", c->chat_id);
        put(r, "path", path);
        put(r, "format", format);
        put(r, "expectedSHA", str(data, "sha256"));
        if (ui->pending_attachment)
            put(r, "expectedID", ui->pending_attachment);
        json_object_put(call(ui, r, NULL));
        copilot_chat_refresh(c);
    }
    json_object_put(data);
    g_free(path);
}
static gboolean clipboard_attachment(ChatUI *ui) {
    LinuxClient *c = ui->client;
    if (!c->chat_id || c->busy || !gtk_widget_is_sensitive(ui->entry))
        return FALSE;
    GtkClipboard *clipboard = gtk_widget_get_clipboard(ui->entry, GDK_SELECTION_CLIPBOARD);
    GdkAtom png = gdk_atom_intern_static_string("image/png");
    GtkSelectionData *selection = gtk_clipboard_wait_for_contents(clipboard, png);
    const guchar *bytes = NULL;
    gsize length = 0;
    char *converted = NULL;
    if (selection && gtk_selection_data_get_length(selection) > 0) {
        bytes = gtk_selection_data_get_data(selection);
        length = (gsize)gtk_selection_data_get_length(selection);
    } else {
        GdkPixbuf *bitmap = gtk_clipboard_wait_for_image(clipboard);
        if (!bitmap) {
            if (selection)
                gtk_selection_data_free(selection);
            return FALSE;
        }
        if (gdk_pixbuf_get_width(bitmap) <= DL_IMAGE_MAX_DIMENSION &&
            gdk_pixbuf_get_height(bitmap) <= DL_IMAGE_MAX_DIMENSION &&
            gdk_pixbuf_save_to_buffer(bitmap, &converted, &length, "png", NULL, NULL))
            bytes = (const guchar *)converted;
        g_object_unref(bitmap);
    }
    if (!bytes || length > DL_IMAGE_MAX_BYTES) {
        copilot_status(c, "图片超出附件大小或像素限制。");
    } else {
        const char *name = "剪贴板图片.png";
        Json *response = take_json(dc_attachment_preview(name, "clipboard", 1, bytes, length));
        if (!flag(response, "ok"))
            copilot_status(c, str(response, "error"));
        else if (preview_attachment(ui, get(response, "data"), TRUE)) {
            g_mutex_lock(&c->lock);
            Json *staged = take_json(dc_attachment_stage(c->store, c->chat_id, ui->pending_attachment, name,
                                                         "clipboard", 1, bytes, length));
            g_mutex_unlock(&c->lock);
            if (!flag(staged, "ok"))
                copilot_status(c, str(staged, "error"));
            json_object_put(staged);
            copilot_chat_refresh(c);
        }
        json_object_put(response);
    }
    if (selection)
        gtk_selection_data_free(selection);
    g_free(converted);
    return TRUE;
}
static void paste_attachment(GtkButton *b, ChatUI *ui) {
    (void)b;
    if (!clipboard_attachment(ui))
        copilot_status(ui->client, "剪贴板没有可添加的图片。");
}

static void render_attachment(ChatUI *ui, Json *chat) {
    ui_clear(ui->pending_box);
    g_clear_pointer(&ui->pending_attachment, g_free);
    gtk_widget_set_sensitive(ui->add_attachment, chat && !flag(chat, "active"));
    gtk_widget_set_sensitive(ui->paste, chat && !flag(chat, "active"));
    if (!chat)
        return;
    Json *r = request("attachments.pending");
    put(r, "chat", str(chat, "id"));
    Json *pending = ui_call(ui->client, UI_READ, r);
    if (*str(pending, "id")) {
        ui->pending_attachment = g_strdup(str(pending, "id"));
        attach_button(ui, ui->pending_box, pending);
        GtkWidget *remove = ui_button(ui->pending_box, "移除附件", G_CALLBACK(remove_attachment), ui);
        gtk_widget_set_sensitive(remove, !flag(chat, "active"));
        gtk_widget_show_all(ui->pending_box);
    }
    json_object_put(pending);
}

static void add_bubble(ChatUI *ui, const char *role, const char *text, const char *model,
                       Json *attachment) {
    if ((!*text && !attachment) || !strcmp(role, "system"))
        return;
    gboolean outgoing = !strcmp(role, "user");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *frame = gtk_frame_new(NULL);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame),
                                outgoing ? "chat-outgoing" : "chat-incoming");
    gtk_widget_set_name(frame, outgoing ? "user-message" : "assistant-message");
    const char *who = outgoing ? "我" : !strcmp(role, "tool") ? "工具结果" : *model ? model : "助手";
    gboolean rich = !strcmp(role, "assistant");
    GtkWidget *by = ui_label(who), *body = rich ? copilot_rich_message_new(text) : ui_label(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(by), "chat-metadata");
    gtk_style_context_add_class(gtk_widget_get_style_context(body), "chat-body");
    gtk_style_context_add_class(gtk_widget_get_style_context(by), "dim-label");
    if (!rich) {
        gtk_label_set_selectable(GTK_LABEL(body), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(body), 75);
    }
    set_margins(col, 12);
    gtk_box_pack_start(GTK_BOX(col), by, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), body, FALSE, FALSE, 0);
    attach_button(ui, col, attachment);
    gtk_container_add(GTK_CONTAINER(frame), col);
    if (outgoing)
        gtk_box_pack_end(GTK_BOX(row), frame, FALSE, FALSE, 0);
    else
        gtk_box_pack_start(GTK_BOX(row), frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ui->messages), row, FALSE, FALSE, 0);
}
/* Keep following new messages until the user scrolls back to an older one. */
static void track_scroll(GtkAdjustment *adjustment, ChatUI *ui) {
    double end = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    ui->follow_tail = end - gtk_adjustment_get_value(adjustment) < 24;
}
static void scroll_new_messages(GtkAdjustment *adjustment, ChatUI *ui) {
    if (ui->follow_tail)
        gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment) -
                                                 gtk_adjustment_get_page_size(adjustment));
}
static void render_messages(ChatUI *ui, int64_t through) {
    Json *r = request("chat.messages");
    put(r, "chat", ui->client->chat_id);
    if (through >= 0)
        json_object_object_add(r, "through", json_object_new_int64(through));
    Json *reply = take_json(dc_read_view(ui->client->data_dir, json_object_to_json_string(r)));
    json_object_put(r);
    if (!flag(reply, "ok")) {
        json_object_put(reply);
        return;
    }
    ui_clear(ui->messages);
    Json *a = get(reply, "data");
    if (json_object_is_type(a, json_type_array))
        for (size_t i = 0; i < (size_t)json_object_array_length(a); i++) {
            Json *m = json_object_array_get_idx(a, i);
            add_bubble(ui, str(m, "role"), str(m, "content"), str(m, "modelName"), get(m, "attachment"));
        }
    if (!json_object_is_type(a, json_type_array) || !json_object_array_length(a))
        gtk_box_pack_start(GTK_BOX(ui->messages), ui_label("今天想一起做什么？"), FALSE, FALSE, 0);
    gtk_widget_show_all(ui->messages);
    json_object_put(reply);
}
static void select_chat(ChatUI *ui, const char *id) {
    if (ui->client->busy)
        return;
    Json *row = lookup(ui->chats, id);
    if (!row)
        return;
    copilot_chat_flush(ui->client);
    ui->follow_tail = TRUE;
    replace(&ui->client->chat_id, id);
    replace(&ui->client->project_id, str(row, "project"));
    replace(&ui->file_dir, "");
    replace(&ui->client->model_id, str(row, "model"));
    copilot_show(ui->client, "chat");
    render_chat(ui);
}
static void row_pressed(GtkTreeSelection *sel, ChatUI *ui) {
    GtkTreeModel *model;
    GtkTreeIter iter;
    gint kind;
    gchar *id = NULL;
    if (ui->syncing || ui->client->busy || !gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    gtk_tree_model_get(model, &iter, KIND, &kind, ID, &id, -1);
    if (kind == CHAT)
        select_chat(ui, id);
    else if (kind == PROJECT) {
        replace(&ui->client->project_id, id);
        replace(&ui->file_dir, "");
        copilot_show(ui->client, "projects");
        render_project(ui);
    }
    g_free(id);
}
static void expanded(GtkTreeView *tree, GtkTreeIter *iter, GtkTreePath *path, ChatUI *ui) {
    if (ui->syncing || ui->client->busy)
        return;
    gint kind;
    gchar *id = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->rows), iter, KIND, &kind, ID, &id, -1);
    if (kind == PROJECT) {
        Json *r = request("projects.expanded");
        put(r, "id", id);
        json_object_object_add(r, "value", json_object_new_boolean(gtk_tree_view_row_expanded(tree, path)));
        json_object_put(call(ui, r, NULL));
    }
    g_free(id);
}
static GtkTreeIter tree_row(ChatUI *ui, GtkTreeIter *parent, int kind, const char *id, const char *title) {
    GtkTreeIter row;
    gtk_tree_store_append(ui->rows, &row, parent);
    gtk_tree_store_set(ui->rows, &row, KIND, kind, ID, id, TITLE, title, -1);
    return row;
}
static void tree_chats(ChatUI *ui, GtkTreeIter *parent, const char *project, gboolean archived) {
    for (size_t i = 0; i < (size_t)json_object_array_length(ui->chats); i++) {
        Json *c = json_object_array_get_idx(ui->chats, i);
        if (flag(c, "archived") != archived || (!archived && strcmp(str(c, "project"), project)))
            continue;
        char *name = g_strdup_printf("%s%s%s", flag(c, "pinned") ? "★ " : "", str(c, "title"),
                                     flag(c, "active") ? " · 等待恢复" : "");
        tree_row(ui, parent, CHAT, str(c, "id"), name);
        g_free(name);
    }
}
static void expand_row(ChatUI *ui, GtkTreeIter *row) {
    GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(ui->rows), row);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(ui->tree), p, FALSE);
    gtk_tree_path_free(p);
}
static void render_chat(ChatUI *ui) {
    if (!ui->chat || ui->client->busy)
        return;
    Json *c = lookup(ui->chats, ui->client->chat_id);
    if (g_strcmp0(ui->reference_chat, ui->client->chat_id) ||
        g_strcmp0(ui->reference_domain, ui->client->data_dir)) {
        g_hash_table_remove_all(ui->references);
        g_free(ui->reference_chat);
        g_free(ui->reference_domain);
        ui->reference_chat = g_strdup(ui->client->chat_id);
        ui->reference_domain = g_strdup(ui->client->data_dir);
    }
    gtk_widget_set_visible(ui->turn_refs, c && *str(c, "project"));
    gtk_widget_set_sensitive(ui->turn_refs, c && !flag(c, "active"));
    gtk_widget_set_sensitive(ui->pending_box, TRUE);
    render_attachment(ui, c);
    gtk_label_set_text(GTK_LABEL(ui->title), c ? str(c, "title") : "普通聊天");
    ui_clear(ui->messages);
    set_input(ui, "");
    replace(&ui->draft_for, c ? str(c, "id") : "");
    if (c) {
        Json *r = request("chats.metadata");
        put(r, "id", str(c, "id"));
        Json *meta = call(ui, r, NULL);
        set_input(ui, str(meta, "draft"));
        json_object_put(meta);
        render_messages(ui, -1);
    } else {
        gtk_box_pack_start(GTK_BOX(ui->messages), ui_label("新建聊天，或从左侧打开一段对话。"), FALSE,
                           FALSE, 0);
    }
    gboolean active = c && flag(c, "active"), available = c && flag(c, "workspaceAvailable");
    gtk_widget_set_sensitive(ui->entry, c && !active);
    gtk_widget_set_sensitive(ui->send, c && available && !active);
    gtk_widget_set_sensitive(ui->manage, c != NULL);
    gtk_widget_set_visible(ui->retry, c && !active && ui->retry_run &&
                                          !g_strcmp0(ui->retry_chat, ui->client->chat_id) &&
                                          !g_strcmp0(ui->retry_directory, ui->client->data_dir));
    gtk_widget_set_visible(ui->recover, active);
    gtk_widget_set_visible(ui->end_turn, active);
    gtk_widget_set_visible(ui->stop, FALSE);
    gtk_label_set_text(GTK_LABEL(ui->busy_label), active            ? "上次回合未结束，请先检查恢复状态。"
                                                  : c && !available ? "对话工作目录已不可用。"
                                                                    : "");
    gtk_widget_show_all(ui->messages);
}
void copilot_chat_refresh(LinuxClient *client) {
    ChatUI *ui = state(client);
    if (client->busy)
        return;
    copilot_chat_flush(client);
    gboolean ok = FALSE;
    Json *projects = call(ui, request("projects.list"), &ok);
    if (!ok || !json_object_is_type(projects, json_type_array)) {
        json_object_put(projects);
        return;
    }
    g_mutex_lock(&client->lock);
    Json *chats = take_json(dc_chats(client->store));
    g_mutex_unlock(&client->lock);
    if (!json_object_is_type(chats, json_type_array)) {
        json_object_put(chats);
        json_object_put(projects);
        return;
    }
    json_object_put(ui->projects);
    ui->projects = projects;
    json_object_put(ui->chats);
    ui->chats = chats;
    if (ui->tree) {
        ui->syncing = TRUE;
        gtk_tree_store_clear(ui->rows);
        GtkTreeIter ordinary = tree_row(ui, NULL, REGULAR, "", "普通聊天");
        tree_chats(ui, &ordinary, "", FALSE);
        expand_row(ui, &ordinary);
        for (size_t i = 0; i < (size_t)json_object_array_length(projects); i++) {
            Json *p = json_object_array_get_idx(projects, i);
            GtkTreeIter group = tree_row(ui, NULL, PROJECT, str(p, "id"), str(p, "name"));
            tree_chats(ui, &group, str(p, "id"), FALSE);
            if (flag(p, "expanded"))
                expand_row(ui, &group);
        }
        GtkTreeIter archive = tree_row(ui, NULL, ARCHIVE, "", "已归档");
        tree_chats(ui, &archive, "", TRUE);
        ui->syncing = FALSE;
    }
    render_chat(ui);
    render_project(ui);
}
typedef struct {
    ChatUI *ui;
    char *id, *model_config, *credential, *input, *draft_attachment, *retry_run;
    GMutex progress_lock;
    GString *stream;
    gint64 through, rendered;
    gboolean changed;
    guint timer;
    int committed;
    Json *result;
} ChatRun;
static void event(void *context, const char *name, const char *text) {
    ChatRun *run = context;
    g_mutex_lock(&run->progress_lock);
    if (!strcmp(name, "delta")) {
        g_string_append(run->stream, text);
        run->changed = TRUE;
    } else if (!strcmp(name, "model.request")) {
        g_string_truncate(run->stream, 0);
        run->changed = TRUE;
    } else if (!strcmp(name, "chat.commit")) {
        Json *j = json_tokener_parse(text);
        if (j && !strcmp(str(j, "chat"), run->id)) {
            run->through = MAX(run->through, json_object_get_int64(get(j, "through")));
            if (flag(j, "settlesModel"))
                g_string_truncate(run->stream, 0);
            run->changed = TRUE;
        }
        json_object_put(j);
    }
    g_mutex_unlock(&run->progress_lock);
}
static gboolean poll_run(gpointer context) {
    ChatRun *r = context;
    g_mutex_lock(&r->progress_lock);
    gint64 through = r->through;
    gboolean changed = r->changed;
    char *stream = changed ? g_strdup(r->stream->str) : NULL;
    r->changed = FALSE;
    g_mutex_unlock(&r->progress_lock);
    if (changed) {
        if (through > r->rendered) {
            render_messages(r->ui, through);
            r->rendered = through;
        }
        gtk_label_set_text(GTK_LABEL(r->ui->busy_label), *stream ? stream : "正在运行…");
        g_free(stream);
    }
    return G_SOURCE_CONTINUE;
}
static int cancel_run(void *context) {
    return g_atomic_int_get(&((ChatRun *)context)->ui->cancelled);
}
static int approve(void *context, const char *tool, const char *args) {
    ChatRun *r = context;
    if (cancel_run(r))
        return 0;
    return copilot_chat_approval(r->ui->client, tool, args);
}
static gboolean finish_run(gpointer context) {
    ChatRun *r = context;
    ChatUI *ui = r->ui;
    LinuxClient *c = ui->client;
    g_source_remove(r->timer);
    c->busy = FALSE;
    if (r->committed || !strcmp(str(r->result, "status"), "completed")) {
        copilot_settings_consume_sources(c, r->id);
        g_hash_table_remove_all(ui->references);
    }
    if (r->committed)
        chat_prop(ui, r->id, "draft", json_object_new_string(""));
    if (!g_strcmp0(ui->retry_chat, r->id) || *str(r->result, "retryRun")) {
        g_clear_pointer(&ui->retry_run, g_free);
        g_clear_pointer(&ui->retry_chat, g_free);
        g_clear_pointer(&ui->retry_directory, g_free);
        if (*str(r->result, "retryRun")) {
            ui->retry_run = g_strdup(str(r->result, "retryRun"));
            ui->retry_chat = g_strdup(r->id);
            ui->retry_directory = g_strdup(c->data_dir);
        }
    }
    copilot_chat_refresh(c);
    gtk_widget_set_sensitive(ui->tree, TRUE);
    gtk_widget_set_sensitive(ui->actions, TRUE);
    if (ui->project_page)
        gtk_widget_set_sensitive(ui->project_page, TRUE);
    if (!strcmp(str(r->result, "status"), "completed"))
        copilot_status(c, "本回合已完成。");
    else
        copilot_status(c, *str(r->result, "text") ? str(r->result, "text") : "回合没有完成。");
    json_object_put(r->result);
    g_free(r->id);
    g_free(r->input);
    g_free(r->draft_attachment);
    g_free(r->retry_run);
    g_free(r->model_config);
    g_string_free(r->stream, TRUE);
    g_mutex_clear(&r->progress_lock);
    g_free(r);
    return G_SOURCE_REMOVE;
}
static gpointer work_run(gpointer context) {
    ChatRun *r = context;
    dc_hooks hooks = {r, event, approve, cancel_run};
    LinuxClient *c = r->ui->client;
    g_mutex_lock(&c->lock);
    if (r->retry_run)
        r->result = take_json(
            dc_retry_chat_turn(c->store, r->id, r->model_config, r->retry_run, &hooks, r->credential));
    else
        r->result =
            take_json(dc_run_message_with_secret(c->store, r->id, r->model_config, r->input, &hooks,
                                                 r->credential, r->draft_attachment, &r->committed));
    if (r->credential) {
        volatile char *p = r->credential;
        size_t n = strlen(r->credential);
        while (n--)
            *p++ = 0;
        g_free(r->credential);
    }
    (void)dc_index_pending(c->store, 16);
    g_mutex_unlock(&c->lock);
    g_idle_add(finish_run, r);
    return NULL;
}
static void start_run(ChatUI *ui, gboolean resume, const char *retry) {
    LinuxClient *c = ui->client;
    Json *row = lookup(ui->chats, c->chat_id);
    if (c->busy || !row || resume != flag(row, "active"))
        return;
    if (!*str(row, "model") || g_strcmp0(c->model_id, str(row, "model"))) {
        copilot_status(c, "请先在设置中为当前聊天选择模型。");
        copilot_show(c, "settings");
        return;
    }
    if (!flag(row, "workspaceAvailable")) {
        copilot_status(c, "工作目录不可用，请先重新关联。");
        return;
    }
    char *message = resume || retry ? NULL : buffer_text(ui->entry);
    if (message) {
        char *check = g_strdup(message);
        g_strstrip(check);
        gboolean empty = !*check;
        g_free(check);
        if (empty && !ui->pending_attachment) {
            g_free(message);
            return;
        }
    }
    char *secret = NULL, *config = copilot_model_config(c, &secret);
    Json *settings = config ? json_tokener_parse(config) : NULL;
    g_free(config);
    if (!settings) {
        g_free(secret);
        g_free(message);
        copilot_status(c, "请在设置中检查当前模型及凭据。");
        return;
    }
    Json *project = lookup(ui->projects, str(row, "project"));
    put(settings, "desktop_tools", project ? str(project, "policy") : "noTools");
    if (project && !resume)
        json_object_object_add(settings, "desktop_project_context", reference_selection(ui));
    ChatRun *r = g_new0(ChatRun, 1);
    r->ui = ui;
    r->id = g_strdup(c->chat_id);
    r->input = message;
    r->credential = secret;
    r->retry_run = g_strdup(retry);
    r->draft_attachment = resume || retry ? NULL : g_strdup(ui->pending_attachment);
    r->model_config = g_strdup(json_object_to_json_string_ext(settings, JSON_C_TO_STRING_PLAIN));
    json_object_put(settings);
    r->through = r->rendered = -1;
    r->stream = g_string_new(NULL);
    g_mutex_init(&r->progress_lock);
    copilot_chat_flush(c);
    c->busy = TRUE;
    g_atomic_int_set(&ui->cancelled, 0);
    gtk_widget_set_sensitive(ui->tree, FALSE);
    gtk_widget_set_sensitive(ui->actions, FALSE);
    gtk_widget_set_sensitive(ui->paste, FALSE);
    gtk_widget_set_sensitive(ui->add_attachment, FALSE);
    gtk_widget_set_sensitive(ui->pending_box, FALSE);
    gtk_widget_set_sensitive(ui->turn_refs, FALSE);
    gtk_widget_set_sensitive(ui->send, FALSE);
    gtk_widget_set_sensitive(ui->entry, FALSE);
    gtk_widget_set_sensitive(ui->manage, FALSE);
    gtk_widget_set_visible(ui->retry, FALSE);
    gtk_widget_set_visible(ui->recover, FALSE);
    gtk_widget_set_visible(ui->end_turn, FALSE);
    gtk_widget_set_visible(ui->stop, TRUE);
    gtk_widget_set_sensitive(ui->stop, TRUE);
    if (ui->project_page)
        gtk_widget_set_sensitive(ui->project_page, FALSE);
    gtk_label_set_text(GTK_LABEL(ui->busy_label), "正在运行…");
    r->timer = g_timeout_add(70, poll_run, r);
    g_thread_unref(g_thread_new("chat-model", work_run, r));
}
static void retry_clicked(GtkButton *b, ChatUI *ui) {
    (void)b;
    if (ui->retry_run && !g_strcmp0(ui->client->chat_id, ui->retry_chat) &&
        !g_strcmp0(ui->client->data_dir, ui->retry_directory))
        start_run(ui, FALSE, ui->retry_run);
}
static void send_clicked(GtkButton *button, ChatUI *ui) {
    (void)button;
    start_run(ui, FALSE, NULL);
}
void copilot_chat_stop(LinuxClient *c) {
    ChatUI *ui = state(c);
    if (c->busy) {
        g_atomic_int_set(&ui->cancelled, 1);
        if (ui->stop)
            gtk_widget_set_sensitive(ui->stop, FALSE);
        copilot_status(c, "正在停止回合…");
    }
}
static void stop_clicked(GtkButton *b, ChatUI *ui) {
    (void)b;
    copilot_chat_stop(ui->client);
}
static void recover_clicked(GtkButton *b, ChatUI *ui) {
    if (ui->client->busy || !ui->client->chat_id)
        return;
    Json *r = request("recovery.inspect");
    put(r, "chat", ui->client->chat_id);
    Json *view = call(ui, r, NULL);
    if (!view)
        return;
    gboolean unknown = *str(view, "pending") != 0;
    if (GTK_WIDGET(b) == ui->end_turn) {
        if (ui_confirm(ui->client, "结束未完成的回合？",
                       unknown ? "已经发出的工具可能产生了效果。结束将保留未知状态和原始聊天。"
                               : "保留原始聊天，结束这次回合。",
                       "结束回合")) {
            Json *op = request("recovery.abandon");
            put(op, "chat", ui->client->chat_id);
            put(op, "stateHash", str(view, "stateHash"));
            json_object_object_add(op, "acknowledgeUnknown", json_object_new_boolean(unknown));
            json_object_put(call(ui, op, NULL));
            copilot_chat_refresh(ui->client);
        }
    } else if (unknown)
        copilot_status(ui->client, "存在结果未确认的工具，请检查工作目录和工具记录后结束旧回合。");
    else if (ui_confirm(ui->client, "继续当前回合？", "将使用原会话和模型继续待处理的请求。", "继续回合"))
        start_run(ui, TRUE, NULL);
    json_object_put(view);
}
static gboolean entry_key(GtkWidget *w, GdkEventKey *e, ChatUI *ui) {
    (void)w;
    if (e->keyval == GDK_KEY_v && (e->state & gtk_accelerator_get_default_mod_mask()) == GDK_CONTROL_MASK &&
        clipboard_attachment(ui))
        return TRUE;
    if ((e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_KP_Enter) && (e->state & GDK_CONTROL_MASK)) {
        start_run(ui, FALSE, NULL);
        return TRUE;
    }
    return FALSE;
}
static void create_chat(ChatUI *ui, const char *project_id) {
    LinuxClient *c = ui->client;
    if (c->busy)
        return;
    Json *p = lookup(ui->projects, project_id);
    if (project_id && *project_id && (!p || !flag(p, "available"))) {
        copilot_status(c, "项目路径不可用。");
        return;
    }
    copilot_chat_flush(c);
    char *dir = p ? g_strdup(str(p, "path")) : g_build_filename(c->data_dir, "Chat Workspace", NULL);
    if (!p && g_mkdir_with_parents(dir, 0700) < 0) {
        copilot_status(c, "无法创建本地聊天目录。");
        g_free(dir);
        return;
    }
    char *id = g_uuid_string_random();
    g_mutex_lock(&c->lock);
    gboolean ok = dc_add_chat(c->store, id, p ? "项目聊天" : "新聊天", dir);
    g_mutex_unlock(&c->lock);
    if (ok) {
        gchar *preferred = copilot_settings_default_model(c);
        const char *model = p && *str(p, "model") ? str(p, "model") : preferred;
        chat_prop(ui, id, "model", json_object_new_string(model ? model : ""));
        g_free(preferred);
        copilot_chat_refresh(c);
        select_chat(ui, id);
    } else
        copilot_status(c, "创建聊天失败，请确认目录仍可用。");
    g_free(id);
    g_free(dir);
}
static void ordinary_new(GtkButton *b, ChatUI *ui) {
    (void)b;
    create_chat(ui, NULL);
}
static void project_new_chat(GtkButton *b, ChatUI *ui) {
    (void)b;
    create_chat(ui, ui->client->project_id);
}
static void go_settings(GtkButton *b, ChatUI *ui) {
    (void)b;
    if (!ui->client->busy)
        copilot_show(ui->client, "settings");
}
static void go_tools(GtkButton *b, ChatUI *ui) {
    (void)b;
    copilot_show(ui->client, "tools");
}
static char *choose_dir(ChatUI *ui, const char *purpose, const char *current) {
    GtkWidget *d =
        gtk_file_chooser_dialog_new(purpose, ui->client->window, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                    "取消", GTK_RESPONSE_CANCEL, "选择目录", GTK_RESPONSE_ACCEPT, NULL);
    if (current && *current)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), current);
    char *path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
    gtk_widget_destroy(d);
    return path;
}
static void rebind(ChatUI *ui, Json *row) {
    char *path = choose_dir(ui, "选择本对话的工作目录", str(row, "workspace"));
    if (!path)
        return;
    Json *q = request("chats.workspace.preview");
    put(q, "chat", str(row, "id"));
    put(q, "path", path);
    g_free(path);
    Json *plan = call(ui, q, NULL);
    if (!plan)
        return;
    if (!flag(plan, "canApply"))
        copilot_status(ui->client, "当前目录绑定不能变更。请检查是否仍有待恢复回合。");
    else if (ui_confirm(ui->client, "重新关联这段对话的工作目录？", str(plan, "path"), "确认关联")) {
        const char *keys[] = {
            "chat",        "path", "device", "inode", "project", "bindingRevision", "projectRevision",
            "historyHash", NULL};
        Json *apply = request("chats.workspace.apply");
        for (int i = 0; keys[i]; i++)
            json_object_object_add(apply, keys[i], json_object_get(get(plan, keys[i])));
        json_object_put(call(ui, apply, NULL));
    }
    json_object_put(plan);
}
static void chat_manage(GtkButton *b, ChatUI *ui) {
    (void)b;
    LinuxClient *c = ui->client;
    if (c->busy)
        return;
    Json *row = lookup(ui->chats, c->chat_id);
    if (!row)
        return;
    GtkWidget *d = gtk_dialog_new_with_buttons("对话设置", c->window,
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "关闭",
                                               GTK_RESPONSE_CANCEL, "保存名称", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d)), *name = gtk_entry_new();
    set_margins(area, 16);
    gtk_entry_set_text(GTK_ENTRY(name), str(row, "title"));
    gtk_box_pack_start(GTK_BOX(area), name, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(area), ui_label(str(row, "workspace")), FALSE, FALSE, 5);
    gtk_dialog_add_button(GTK_DIALOG(d), flag(row, "archived") ? "取消归档" : "归档", 1);
    gtk_dialog_add_button(GTK_DIALOG(d), flag(row, "pinned") ? "取消置顶" : "置顶", 2);
    gtk_dialog_add_button(GTK_DIALOG(d), "重新关联目录…", 3);
    gtk_dialog_add_button(GTK_DIALOG(d), "删除…", 4);
    gtk_widget_show_all(d);
    int answer = gtk_dialog_run(GTK_DIALOG(d));
    char *title = g_strdup(gtk_entry_get_text(GTK_ENTRY(name)));
    gtk_widget_destroy(d);
    if (answer == GTK_RESPONSE_ACCEPT) {
        g_mutex_lock(&c->lock);
        int ok = dc_rename_chat(c->store, c->chat_id, title);
        g_mutex_unlock(&c->lock);
        if (!ok)
            copilot_status(c, "名称无效或保存失败。");
    }
    if (answer == 1 || answer == 2) {
        const char *f = answer == 1 ? "archived" : "pinned";
        chat_prop(ui, c->chat_id, f, json_object_new_boolean(!flag(row, f)));
    }
    if (answer == 3)
        rebind(ui, row);
    if (answer == 4 && ui_confirm(ui->client, "删除这段聊天及其缓存？",
                                  "对话、工具记录与检索缓存将清理。项目文件保留。", "删除并清理")) {
        copilot_chat_flush(c);
        g_mutex_lock(&c->lock);
        int ok = dc_delete_chat(c->store, c->chat_id);
        g_mutex_unlock(&c->lock);
        if (ok) {
            replace(&c->chat_id, "");
            replace(&ui->draft_for, "");
        } else
            copilot_status(c, "聊天无法删除。请先处理未完成回合。");
    }
    g_free(title);
    copilot_chat_refresh(c);
}
static void choose_project(GtkButton *b, ChatUI *ui) {
    const char *id = g_object_get_data(G_OBJECT(b), "item-id");
    replace(&ui->client->project_id, id);
    replace(&ui->file_dir, "");
    render_project(ui);
}
static void choose_project_chat(GtkButton *b, ChatUI *ui) {
    const char *id = g_object_get_data(G_OBJECT(b), "item-id");
    select_chat(ui, id);
}
static void named_item(GtkWidget *list, const char *title, const char *id, GCallback cb, ChatUI *ui) {
    GtkWidget *b = ui_button(list, title, cb, ui);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(b)), GTK_ALIGN_START);
    g_object_set_data_full(G_OBJECT(b), "item-id", g_strdup(id), g_free);
}
static void file_clicked(GtkButton *b, ChatUI *ui);
static gboolean pinned(Json *project, const char *path) {
    Json *items = get(project, "context");
    if (!json_object_is_type(items, json_type_array))
        return FALSE;
    for (size_t i = 0; i < (size_t)json_object_array_length(items); i++)
        if (!g_strcmp0(json_object_get_string(json_object_array_get_idx(items, i)), path))
            return TRUE;
    return FALSE;
}
static void save_reference(ChatUI *ui, Json *project, const char *path, const char *hash) {
    Json *r = request("projects.update"), *paths = json_object_new_array(),
         *existing = get(project, "context");
    const char *fields[] = {"id", "name", "path", "revision", "instructions", "model", "policy", NULL};
    for (guint i = 0; fields[i]; i++)
        json_object_object_add(r, fields[i], json_object_get(get(project, fields[i])));
    for (size_t i = 0; i < (size_t)json_object_array_length(existing); i++) {
        Json *v = json_object_array_get_idx(existing, i);
        if (hash || strcmp(path, json_object_get_string(v)))
            json_object_array_add(paths, json_object_get(v));
    }
    if (hash) {
        if (!pinned(project, path))
            json_object_array_add(paths, json_object_new_string(path));
        Json *versions = json_object_new_object();
        put(versions, path, hash);
        json_object_object_add(r, "contextVersions", versions);
    }
    json_object_object_add(r, "context", paths);
    gboolean ok;
    json_object_put(call(ui, r, &ok));
    if (ok) {
        g_hash_table_remove(ui->references, path);
        copilot_chat_refresh(ui->client);
    }
}
static void reference_remove(GtkButton *b, ChatUI *ui) {
    const char *path = g_object_get_data(G_OBJECT(b), "item-id");
    Json *p = lookup(ui->projects, ui->client->project_id);
    if (p && !ui->client->busy)
        save_reference(ui, p, path, NULL);
}
static void refresh_project(GtkButton *b, ChatUI *ui) {
    (void)b;
    render_project(ui);
}
static void list_references(ChatUI *ui, Json *p) {
    ui_clear(ui->project_references);
    if (!p)
        return;
    Json *req = request("files.references");
    put(req, "project", str(p, "id"));
    Json *items = call(ui, req, NULL);
    for (size_t i = 0;
         json_object_is_type(items, json_type_array) && i < (size_t)json_object_array_length(items); i++) {
        Json *f = json_object_array_get_idx(items, i);
        const char *path = str(f, "path"), *status = str(f, "state");
        const char *hint = !strcmp(status, "current")       ? "已确认版本"
                           : !strcmp(status, "changed")     ? "文件已变更，预览后确认新版本"
                           : !strcmp(status, "unavailable") ? "文件不可用"
                                                            : "尚未确认当前版本";
        GtkWidget *row = ui_row(ui->project_references, path, hint);
        GtkWidget *open = ui_button(row, "预览", G_CALLBACK(file_clicked), ui);
        g_object_set_data_full(G_OBJECT(open), "item-id", g_strdup(path), g_free);
        gtk_widget_set_sensitive(open, *str(f, "currentSHA") != 0);
        GtkWidget *remove = ui_button(row, "移除固定", G_CALLBACK(reference_remove), ui);
        g_object_set_data_full(G_OBJECT(remove), "item-id", g_strdup(path), g_free);
    }
    if (json_object_is_type(items, json_type_array) && !json_object_array_length(items))
        gtk_box_pack_start(GTK_BOX(ui->project_references),
                           ui_label("还没有固定资料。可以从项目文件的预览中固定资料。"), FALSE, FALSE, 0);
    json_object_put(items);
    gtk_widget_show_all(ui->project_references);
}
static void turn_references(GtkButton *b, ChatUI *ui) {
    (void)b;
    LinuxClient *c = ui->client;
    if (c->busy)
        return;
    Json *p = lookup(ui->projects, c->project_id);
    if (!p)
        return;
    Json *r = request("files.references");
    put(r, "project", c->project_id);
    put(r, "chat", c->chat_id);
    Json *items = call(ui, r, NULL);
    if (!items)
        return;
    GtkWidget *d = gtk_dialog_new_with_buttons("本轮项目资料", c->window,
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "取消",
                                               GTK_RESPONSE_CANCEL, "应用选择", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d)),
              *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_pack_start(GTK_BOX(area), ui_label("选择本轮使用的文件。发送时将检查文件版本。"), FALSE, FALSE,
                       4);
    GtkWidget *view = scroll(rows);
    gtk_widget_set_size_request(view, 350, 180);
    gtk_box_pack_start(GTK_BOX(area), view, TRUE, TRUE, 0);
    for (size_t i = 0; i < (size_t)json_object_array_length(items); i++) {
        Json *f = json_object_array_get_idx(items, i);
        const char *path = str(f, "path"), *sha = str(f, "currentSHA");
        GtkWidget *toggle = gtk_check_button_new_with_label(path);
        gboolean available = !strcmp(str(f, "state"), "current") && *sha;
        gtk_widget_set_sensitive(toggle, available);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle),
                                     available &&
                                         !g_strcmp0(g_hash_table_lookup(ui->references, path), sha));
        gtk_widget_set_tooltip_text(toggle, available ? NULL : "请在项目的上下文页预览并确认文件版本。");
        gtk_box_pack_start(GTK_BOX(rows), toggle, FALSE, FALSE, 0);
    }
    if (!json_object_array_length(items))
        gtk_box_pack_start(GTK_BOX(rows), ui_label("请先在项目文件预览中固定资料。"), FALSE, FALSE, 0);
    gtk_widget_show_all(d);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(rows)), *node = children;
        g_hash_table_remove_all(ui->references);
        for (size_t i = 0; i < (size_t)json_object_array_length(items); i++, node = node->next) {
            Json *f = json_object_array_get_idx(items, i);
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(node->data)))
                g_hash_table_insert(ui->references, g_strdup(str(f, "path")),
                                    g_strdup(str(f, "currentSHA")));
        }
        g_list_free(children);
    }
    gtk_widget_destroy(d);
    json_object_put(items);
}
static Json *reference_selection(ChatUI *ui) {
    Json *selection = json_object_new_array();
    GList *names = g_hash_table_get_keys(ui->references);
    names = g_list_sort(names, (GCompareFunc)g_strcmp0);
    for (GList *it = names; it; it = it->next) {
        Json *item = json_object_new_object();
        put(item, "path", it->data);
        put(item, "sha256", g_hash_table_lookup(ui->references, it->data));
        json_object_array_add(selection, item);
    }
    g_list_free(names);
    return selection;
}

static void render_files(ChatUI *ui, Json *p) {
    ui_clear(ui->project_files);
    gtk_label_set_text(GTK_LABEL(ui->file_path),
                       ui->file_dir && *ui->file_dir ? ui->file_dir : "项目根目录");
    if (!p || !flag(p, "available"))
        return;
    if (ui->file_dir && *ui->file_dir) {
        char *parent = g_path_get_dirname(ui->file_dir);
        named_item(ui->project_files, "↑ 上一级", !strcmp(parent, ".") ? "" : parent,
                   G_CALLBACK(file_clicked), ui);
        GList *children = gtk_container_get_children(GTK_CONTAINER(ui->project_files));
        g_object_set_data(G_OBJECT(children->data), "is-directory", GINT_TO_POINTER(1));
        g_list_free(children);
        g_free(parent);
    }
    Json *r = request("files.list");
    put(r, "project", str(p, "id"));
    put(r, "path", ui->file_dir);
    Json *entries = call(ui, r, NULL);
    if (!json_object_is_type(entries, json_type_array)) {
        json_object_put(entries);
        return;
    }
    for (size_t i = 0; i < (size_t)json_object_array_length(entries); i++) {
        Json *f = json_object_array_get_idx(entries, i);
        gboolean directory = flag(f, "directory");
        char *name = g_strdup_printf("%s%s", directory ? "▸ " : "", str(f, "name"));
        GtkWidget *b = ui_button(ui->project_files, name, G_CALLBACK(file_clicked), ui);
        g_free(name);
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(b)), GTK_ALIGN_START);
        g_object_set_data_full(G_OBJECT(b), "item-id", g_strdup(str(f, "path")), g_free);
        g_object_set_data(G_OBJECT(b), "is-directory", GINT_TO_POINTER(directory));
    }
    json_object_put(entries);
    gtk_widget_show_all(ui->project_files);
}
static void file_clicked(GtkButton *b, ChatUI *ui) {
    if (ui->client->busy)
        return;
    const char *path = g_object_get_data(G_OBJECT(b), "item-id");
    Json *p = lookup(ui->projects, ui->client->project_id);
    if (!p)
        return;
    if (g_object_get_data(G_OBJECT(b), "is-directory")) {
        replace(&ui->file_dir, path);
        render_files(ui, p);
        return;
    }
    Json *r = request("files.read");
    put(r, "project", str(p, "id"));
    put(r, "path", path);
    Json *doc = call(ui, r, NULL);
    if (!doc)
        return;
    GtkWidget *d = gtk_dialog_new_with_buttons(path, ui->client->window,
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "关闭",
                                               GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 680, 500);
    GtkWidget *v = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(v), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(v), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(v), TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(v)), str(doc, "text"), -1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), scroll(v), TRUE, TRUE, 0);
    gtk_dialog_add_button(GTK_DIALOG(d), pinned(p, path) ? "确认当前版本" : "固定到项目上下文",
                          GTK_RESPONSE_APPLY);
    gtk_widget_show_all(d);
    int selected = gtk_dialog_run(GTK_DIALOG(d));
    if (selected == GTK_RESPONSE_APPLY) {
        char *chosen = g_strdup(path);
        save_reference(ui, p, chosen, str(doc, "sha256"));
        g_free(chosen);
    }
    gtk_widget_destroy(d);
    json_object_put(doc);
}
static void render_project(ChatUI *ui) {
    if (!ui->project_page || ui->client->busy)
        return;
    ui_clear(ui->project_list);
    ui_clear(ui->project_chats);
    Json *p = lookup(ui->projects, ui->client->project_id);
    if (json_object_is_type(ui->projects, json_type_array))
        for (size_t i = 0; i < (size_t)json_object_array_length(ui->projects); i++) {
            Json *v = json_object_array_get_idx(ui->projects, i);
            named_item(ui->project_list, str(v, "name"), str(v, "id"), G_CALLBACK(choose_project), ui);
        }
    gtk_label_set_text(GTK_LABEL(ui->project_name), p ? str(p, "name") : "项目");
    gtk_label_set_text(GTK_LABEL(ui->project_path),
                       p ? str(p, "path") : "选择项目或添加一个已有的本地目录。");
    gtk_widget_set_sensitive(ui->project_actions, p != NULL);
    int count = 0;
    if (p && json_object_is_type(ui->chats, json_type_array))
        for (size_t i = 0; i < (size_t)json_object_array_length(ui->chats); i++) {
            Json *c = json_object_array_get_idx(ui->chats, i);
            if (!strcmp(str(c, "project"), str(p, "id")) && !flag(c, "archived")) {
                named_item(ui->project_chats, str(c, "title"), str(c, "id"),
                           G_CALLBACK(choose_project_chat), ui);
                count++;
            }
        }
    if (!count)
        gtk_box_pack_start(GTK_BOX(ui->project_chats), ui_label("这个项目还没有聊天。"), FALSE, FALSE, 8);
    render_files(ui, p);
    list_references(ui, p);
    gtk_widget_show_all(ui->project_list);
    gtk_widget_show_all(ui->project_chats);
}
static void project_browse(GtkButton *b, ChatUI *ui) {
    GtkWidget *entry = g_object_get_data(G_OBJECT(b), "path-entry");
    char *path = choose_dir(ui, "选择项目文件夹", gtk_entry_get_text(GTK_ENTRY(entry)));
    if (path) {
        gtk_entry_set_text(GTK_ENTRY(entry), path);
        g_free(path);
    }
}
static GtkWidget *edit_field(GtkWidget *area, const char *caption, const char *value) {
    gtk_box_pack_start(GTK_BOX(area), ui_label(caption), FALSE, FALSE, 3);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), value ? value : "");
    gtk_box_pack_start(GTK_BOX(area), e, FALSE, FALSE, 2);
    return e;
}
static void project_editor(GtkButton *source, ChatUI *ui) {
    LinuxClient *c = ui->client;
    if (c->busy)
        return;
    Json *p = g_object_get_data(G_OBJECT(source), "existing") ? lookup(ui->projects, c->project_id) : NULL;
    char *new_dir = p ? NULL : choose_dir(ui, "添加本地项目", NULL);
    if (!p && !new_dir)
        return;
    char *name = p ? g_strdup(str(p, "name")) : g_path_get_basename(new_dir);
    GtkWidget *d = gtk_dialog_new_with_buttons(p ? "项目属性" : "添加项目", c->window,
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "取消",
                                               GTK_RESPONSE_CANCEL, "保存", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 520, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    set_margins(area, 16);
    GtkWidget *title = edit_field(area, "项目名称", name),
              *dir = edit_field(area, "工作目录", p ? str(p, "path") : new_dir);
    GtkWidget *browse = ui_button(area, "选择目录…", G_CALLBACK(project_browse), ui);
    g_object_set_data(G_OBJECT(browse), "path-entry", dir);
    gtk_box_pack_start(GTK_BOX(area), ui_label("项目指令"), FALSE, FALSE, 4);
    GtkWidget *ins = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ins), GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ins)), str(p, "instructions"), -1);
    GtkWidget *sw = scroll(ins);
    gtk_widget_set_size_request(sw, -1, 72);
    gtk_box_pack_start(GTK_BOX(area), sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), ui_label("工具权限"), FALSE, FALSE, 4);
    GtkWidget *policy = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(policy), "ask", "修改文件和执行命令时请求授权");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(policy), "readOnly", "只读工具");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(policy), "noTools", "关闭项目工具");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(policy), p ? str(p, "policy") : "ask");
    gtk_box_pack_start(GTK_BOX(area), policy, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(area), ui_label("新建项目聊天的默认模型"), FALSE, FALSE, 4);
    GtkWidget *models = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(models), "", "使用默认模型");
    Json *registry = call(ui, request("config.list"), NULL), *list = get(registry, "models");
    if (json_object_is_type(list, json_type_array))
        for (size_t i = 0; i < (size_t)json_object_array_length(list); i++) {
            Json *m = json_object_array_get_idx(list, i);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(models), str(m, "id"), str(m, "name"));
        }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(models), str(p, "model"));
    gtk_box_pack_start(GTK_BOX(area), models, FALSE, FALSE, 4);
    json_object_put(registry);
    gtk_widget_show_all(d);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *id = p ? g_strdup(str(p, "id")) : g_uuid_string_random(), *instructions = buffer_text(ins);
        const char *path = gtk_entry_get_text(GTK_ENTRY(dir));
        gboolean moved = p && (strcmp(path, str(p, "path")) || !flag(p, "available"));
        if (!moved || ui_confirm(ui->client, "确认更新项目目录绑定？", path, "关联目录")) {
            Json *r = request(p ? "projects.update" : "projects.create");
            put(r, "id", id);
            put(r, "name", gtk_entry_get_text(GTK_ENTRY(title)));
            put(r, "instructions", instructions);
            put(r, "path", path);
            put(r, "model", gtk_combo_box_get_active_id(GTK_COMBO_BOX(models)));
            put(r, "policy", gtk_combo_box_get_active_id(GTK_COMBO_BOX(policy)));
            json_object_object_add(r, "context",
                                   p ? json_object_get(get(p, "context")) : json_object_new_array());
            if (p)
                json_object_object_add(r, "revision", json_object_get(get(p, "revision")));
            if (moved)
                json_object_object_add(r, "relink", json_object_new_boolean(TRUE));
            gboolean ok;
            json_object_put(call(ui, r, &ok));
            if (ok) {
                replace(&c->project_id, id);
                replace(&ui->file_dir, "");
            }
        }
        g_free(id);
        g_free(instructions);
    }
    gtk_widget_destroy(d);
    g_free(new_dir);
    g_free(name);
    copilot_chat_refresh(c);
}
static void project_remove(GtkButton *b, ChatUI *ui) {
    (void)b;
    Json *p = lookup(ui->projects, ui->client->project_id);
    if (!p || ui->client->busy)
        return;
    if (!ui_confirm(ui->client, "从项目列表中移除？", "已有对话保留在普通聊天。磁盘中的项目文件不变。",
                    "移除项目"))
        return;
    Json *r = request("projects.remove");
    put(r, "id", str(p, "id"));
    put(r, "mode", "keep");
    json_object_object_add(r, "revision", json_object_get(get(p, "revision")));
    gboolean ok;
    json_object_put(call(ui, r, &ok));
    if (ok) {
        replace(&ui->client->project_id, "");
        replace(&ui->file_dir, "");
    }
    copilot_chat_refresh(ui->client);
}
static void go_projects(GtkButton *b, ChatUI *ui) {
    (void)b;
    if (!ui->client->busy)
        copilot_show(ui->client, "projects");
}

GtkWidget *copilot_chat_new(LinuxClient *client) {
    ChatUI *ui = state(client);
    if (ui->chat)
        return ui->chat;
    ui->chat = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    copilot_chat_style(ui->chat);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(side, 8);
    ui->actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(side), ui->actions, FALSE, FALSE, 3);
    ui_button(ui->actions, "新聊天", G_CALLBACK(ordinary_new), ui);
    ui_button(ui->actions, "项目", G_CALLBACK(go_projects), ui);
    ui->rows = gtk_tree_store_new(COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    ui->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->rows));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ui->tree), FALSE);
    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(ui->tree), -1, "会话", cell, "text", TITLE,
                                                NULL);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), "changed",
                     G_CALLBACK(row_pressed), ui);
    g_signal_connect(ui->tree, "row-expanded", G_CALLBACK(expanded), ui);
    g_signal_connect(ui->tree, "row-collapsed", G_CALLBACK(expanded), ui);
    gtk_widget_set_size_request(side, 170, -1);
    gtk_box_pack_start(GTK_BOX(side), scroll(ui->tree), TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(ui->chat), side, FALSE, TRUE);
    gtk_paned_set_position(GTK_PANED(ui->chat), 208);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(body, 12);
    ui->title = ui_label("普通聊天");
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->title), "chat-title");
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(body), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), ui->title, TRUE, TRUE, 0);
    ui_button(bar, "切换模型…", G_CALLBACK(go_settings), ui);
    ui_button(bar, "工具记录", G_CALLBACK(go_tools), ui);
    ui->manage = ui_button(bar, "对话设置…", G_CALLBACK(chat_manage), ui);
    ui->messages = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    set_margins(ui->messages, 8);
    ui->transcript = scroll(ui->messages);
    ui->follow_tail = TRUE;
    GtkAdjustment *position = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->transcript));
    g_signal_connect(position, "value-changed", G_CALLBACK(track_scroll), ui);
    g_signal_connect(position, "changed", G_CALLBACK(scroll_new_messages), ui);
    gtk_box_pack_start(GTK_BOX(body), ui->transcript, TRUE, TRUE, 0);
    ui->busy_label = ui_label("");
    gtk_label_set_selectable(GTK_LABEL(ui->busy_label), TRUE);
    gtk_label_set_lines(GTK_LABEL(ui->busy_label), 6);
    gtk_label_set_ellipsize(GTK_LABEL(ui->busy_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(body), ui->busy_label, FALSE, FALSE, 0);
    ui->pending_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(body), ui->pending_box, FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "composer");
    ui->entry = gtk_text_view_new();
    gtk_widget_set_name(ui->entry, "chat-input");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ui->entry), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(ui->entry), 9);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(ui->entry), 9);
    GtkWidget *input = scroll(ui->entry);
    gtk_widget_set_size_request(input, -1, 66);
    gtk_container_add(GTK_CONTAINER(frame), input);
    gtk_box_pack_start(GTK_BOX(body), frame, FALSE, FALSE, 0);
    GtkWidget *sendbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(body), sendbar, FALSE, FALSE, 0);
    ui->add_attachment = ui_button(sendbar, "添加附件…", G_CALLBACK(add_attachment), ui);
    ui->paste = ui_button(sendbar, "粘贴图片", G_CALLBACK(paste_attachment), ui);
    ui->turn_refs = ui_button(sendbar, "本轮项目资料…", G_CALLBACK(turn_references), ui);
    gtk_widget_set_no_show_all(ui->turn_refs, TRUE);
    ui->retry = ui_button(sendbar, "重试未答复的回合", G_CALLBACK(retry_clicked), ui);
    gtk_widget_set_no_show_all(ui->retry, TRUE);
    ui->recover = ui_button(sendbar, "检查并恢复", G_CALLBACK(recover_clicked), ui);
    ui->end_turn = ui_button(sendbar, "结束旧回合…", G_CALLBACK(recover_clicked), ui);
    ui->stop = ui_button(sendbar, "停止", G_CALLBACK(stop_clicked), ui);
    ui->send = ui_button(sendbar, "发送", G_CALLBACK(send_clicked), ui);
    gtk_widget_set_tooltip_text(ui->send, "Ctrl+Enter 发送");
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->send), "suggested-action");
    gtk_widget_set_no_show_all(ui->stop, TRUE);
    gtk_widget_set_no_show_all(ui->recover, TRUE);
    gtk_widget_set_no_show_all(ui->end_turn, TRUE);
    g_signal_connect(ui->entry, "key-press-event", G_CALLBACK(entry_key), ui);
    g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->entry)), "changed",
                     G_CALLBACK(draft_changed), ui);
    gtk_paned_pack2(GTK_PANED(ui->chat), body, TRUE, TRUE);
    return ui->chat;
}
GtkWidget *copilot_projects_new(LinuxClient *client) {
    ChatUI *ui = state(client);
    if (ui->project_page)
        return ui->project_page;
    ui->project_page = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(side, 10);
    ui_button(side, "添加项目…", G_CALLBACK(project_editor), ui);
    ui->project_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(side), scroll(ui->project_list), TRUE, TRUE, 0);
    gtk_widget_set_size_request(side, 170, -1);
    gtk_paned_pack1(GTK_PANED(ui->project_page), side, FALSE, TRUE);
    gtk_paned_set_position(GTK_PANED(ui->project_page), 208);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(content, 15);
    ui->project_name = ui_label("项目");
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->project_name), "title");
    ui->project_path = ui_label("");
    gtk_label_set_selectable(GTK_LABEL(ui->project_path), TRUE);
    gtk_box_pack_start(GTK_BOX(content), ui->project_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), ui->project_path, FALSE, FALSE, 0);
    ui->project_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui_button(ui->project_actions, "新建项目对话", G_CALLBACK(project_new_chat), ui);
    GtkWidget *edit = ui_button(ui->project_actions, "项目属性…", G_CALLBACK(project_editor), ui);
    g_object_set_data(G_OBJECT(edit), "existing", GINT_TO_POINTER(1));
    ui_button(ui->project_actions, "移除项目…", G_CALLBACK(project_remove), ui);
    gtk_box_pack_start(GTK_BOX(content), ui->project_actions, FALSE, FALSE, 4);
    GtkWidget *tabs = gtk_notebook_new();
    ui->project_chats = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    set_margins(ui->project_chats, 8);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroll(ui->project_chats), gtk_label_new("对话"));
    GtkWidget *files = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    ui->file_path = ui_label("");
    ui->project_files = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_box_pack_start(GTK_BOX(files), ui->file_path, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(files), scroll(ui->project_files), TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), files, gtk_label_new("文件"));
    GtkWidget *refs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(refs, 8);
    ui_button(refs, "检查文件版本", G_CALLBACK(refresh_project), ui);
    ui->project_references = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(refs), ui->project_references, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroll(refs), gtk_label_new("上下文"));
    gtk_box_pack_start(GTK_BOX(content), tabs, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(ui->project_page), content, TRUE, TRUE);
    return ui->project_page;
}
