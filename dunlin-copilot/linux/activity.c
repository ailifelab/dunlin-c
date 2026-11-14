#include "activity.h"
#include "chat.h"
#include "settings.h"
#include "ui.h"
#include <string.h>

typedef UiJson J;
typedef struct Activity Activity;
typedef struct {
    Activity *page;
    dc_store *worker;
    char *id, *chat, *workspace, *config, *secret, *status, *detail, *origin, *active_chat;
    GMutex origin_lock;
    gint stopped;
    gint64 helper_revision;
} Worker;
struct Activity {
    LinuxClient *c;
    GtkWidget *widget, *project, *model, *tasks, *members, *receivers, *mail, *traces, *files, *recovery;
    GtkWidget *task_input, *mail_input;
    GHashTable *workers;
    gboolean drain, rendering;
    guint timer;
    gint64 trace_limit, file_limit;
    char *filter_id;
};
static const char key[] = "dunlin-activity";
static gboolean start_job(Activity *a, J *job);
static void run_queue(Activity *a);
static size_t count(J *a) {
    return json_object_is_type(a, json_type_array) ? (size_t)json_object_array_length(a) : 0;
}
static const char *value(GtkWidget *combo) {
    const char *v = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    return v ? v : "";
}
static J *scoped(Activity *a, UiRoute rt, const char *op, const char *field, const char *v) {
    J *q = ui_op(op);
    ui_string(q, field, v);
    return ui_call(a->c, rt, q);
}
static void info(Activity *a, GtkWidget *area, const char *s) {
    (void)a;
    gtk_box_pack_start(GTK_BOX(area), ui_label(s), FALSE, FALSE, 0);
}
static void retain_combo(GtkWidget *combo, J *rows, const char *selection, const char *title_key) {
    char *old = g_strdup(selection);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    for (size_t i = 0; i < count(rows); i++) {
        J *item = json_object_array_get_idx(rows, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), ui_text(item, "id"), ui_text(item, title_key));
    }
    if (!old || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), old))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), count(rows) ? 0 : -1);
    g_free(old);
}
static GtkWidget *combo_row(GtkWidget *content, const char *title) {
    GtkWidget *row = ui_row(content, title, NULL), *c = gtk_combo_box_text_new();
    gtk_box_pack_end(GTK_BOX(row), c, FALSE, FALSE, 0);
    return c;
}
static void set_id(GtkWidget *b, const char *id) {
    g_object_set_data_full(G_OBJECT(b), "id", g_strdup(id), g_free);
}
static const char *button_id(GtkButton *b) {
    return g_object_get_data(G_OBJECT(b), "id");
}
static void stop_all(GtkButton *b, Activity *a) {
    (void)b;
    copilot_activity_stop(a->c);
}
void copilot_activity_stop(LinuxClient *c) {
    Activity *a = g_object_get_data(G_OBJECT(c->window), key);
    if (!a)
        return;
    a->drain = FALSE;
    GHashTableIter it;
    gpointer worker;
    g_hash_table_iter_init(&it, a->workers);
    while (g_hash_table_iter_next(&it, NULL, &worker))
        g_atomic_int_set(&((Worker *)worker)->stopped, 1);
}
static void project_changed(GtkComboBox *b, Activity *a) {
    (void)b;
    if (!a->rendering)
        copilot_activity_refresh(a->c);
}
static int is_cancelled(void *context) {
    return g_atomic_int_get(&((Worker *)context)->stopped);
}
static void agent_event(void *ptr, const char *name, const char *text) {
    Worker *w = ptr;
    if (!strcmp(name, "agent.approval")) {
        J *origin = json_tokener_parse(text);
        if (origin) {
            g_mutex_lock(&w->origin_lock);
            g_free(w->origin);
            g_free(w->active_chat);
            w->active_chat = g_strdup(ui_text(origin, "chat"));
            w->origin = g_strdup_printf("%s / %s", ui_text(origin, "chat"), ui_text(origin, "task"));
            g_mutex_unlock(&w->origin_lock);
        }
        json_object_put(origin);
    }
}
static int agent_approve(void *ptr, const char *tool, const char *arguments) {
    Worker *w = ptr;
    if (is_cancelled(w))
        return 0;
    g_mutex_lock(&w->origin_lock);
    gchar *origin = g_strdup_printf("%s · %s", tool, w->origin ? w->origin : w->chat);
    g_mutex_unlock(&w->origin_lock);
    int answer = copilot_chat_approval(w->page->c, origin, arguments);
    g_free(origin);
    return !is_cancelled(w) && answer;
}
static gboolean settled(gpointer ptr) {
    Worker *w = ptr;
    Activity *a = w->page;
    LinuxClient *c = a->c;
    g_hash_table_remove(a->workers, w->id);
    gboolean completed = !g_strcmp0(w->status, "completed");
    if (!completed)
        a->drain = FALSE;
    char *message = g_strdup_printf("协作任务 %s · %s", completed ? "已完成" : "已停止或失败",
                                    w->detail ? w->detail : "");
    copilot_status(c, message);
    g_free(message);
    g_free(w->id);
    g_free(w->workspace);
    g_free(w->config);
    g_free(w->chat);
    g_free(w->origin);
    g_free(w->active_chat);
    g_free(w->status);
    g_free(w->detail);
    g_mutex_clear(&w->origin_lock);
    g_free(w);
    if (!g_hash_table_size(a->workers))
        c->busy = FALSE;
    if (a->drain && !c->closing && !c->pending_identity)
        run_queue(a);
    copilot_activity_refresh(c);
    if (!c->busy)
        copilot_chat_refresh(c);
    return G_SOURCE_REMOVE;
}
static gpointer execute(gpointer ptr) {
    Worker *w = ptr;
    dc_hooks hooks = {w, agent_event, agent_approve, is_cancelled};
    char *raw;
    if (w->helper_revision > 0) {
        hooks.event = NULL;
        hooks.approve = NULL;
        raw = dc_helper_execute(w->worker, w->id, w->helper_revision, w->secret, &hooks);
    } else {
        raw = dc_agent_execute(w->worker, w->id, w->config, w->secret, &hooks);
    }
    J *res = raw ? json_tokener_parse(raw) : NULL;
    w->status = g_strdup(w->helper_revision > 0 ? (json_object_get_boolean(ui_get(res, "ok")) ? "completed" : "failed") : ui_text(res, "status"));
    w->detail = g_strdup(ui_text(res, "text"));
    dc_free(raw);
    json_object_put(res);
    dc_close(w->worker);
    if (w->secret) {
        volatile char *p = w->secret;
        size_t n = strlen(w->secret);
        while (n--)
            *p++ = 0;
        g_free(w->secret);
    }
    g_idle_add(settled, w);
    return NULL;
}
static gboolean start_job(Activity *a, J *job) {
    LinuxClient *c = a->c;
    const char *id = ui_text(job, "id"), *path = ui_text(job, "workspace");
    if ((c->busy && !g_hash_table_size(a->workers)) || g_hash_table_size(a->workers) >= 2 ||
        g_hash_table_contains(a->workers, id) || strcmp(ui_text(job, "state"), "queued"))
        return FALSE;
    GHashTableIter iter;
    gpointer data;
    g_hash_table_iter_init(&iter, a->workers);
    while (g_hash_table_iter_next(&iter, NULL, &data))
        if (!g_strcmp0(((Worker *)data)->workspace, path))
            return FALSE;
    char *secret = NULL, *config = copilot_model_config_for_id(c, ui_text(job, "model"), &secret);
    if (!config) {
        copilot_status(c, "任务绑定的模型或凭据不可用。");
        return FALSE;
    }
    g_mutex_lock(&c->lock);
    dc_store *worker = dc_open_agent_worker(c->store);
    g_mutex_unlock(&c->lock);
    if (!worker) {
        g_free(config);
        if (secret) {
            volatile char *q = secret;
            size_t n = strlen(secret);
            while (n--)
                *q++ = 0;
            g_free(secret);
        }
        copilot_status(c, "无法创建独立任务执行器。");
        return FALSE;
    }
    Worker *w = g_new0(Worker, 1);
    w->worker = worker;
    w->page = a;
    w->id = g_strdup(id);
    w->chat = g_strdup(ui_text(job, "chat"));
    w->workspace = g_strdup(path);
    w->secret = secret;
    J *settings = json_tokener_parse(config);
    g_free(config);
    ui_string(settings, "desktop_tools", *ui_text(job, "policy") ? ui_text(job, "policy") : "ask");
    json_object_object_add(settings, "desktop_project_context", json_object_new_array());
    w->config = g_strdup(json_object_to_json_string_ext(settings, JSON_C_TO_STRING_PLAIN));
    json_object_put(settings);
    g_mutex_init(&w->origin_lock);
    g_hash_table_insert(a->workers, w->id, w);
    c->busy = TRUE;
    g_thread_unref(g_thread_new("agent-model", execute, w));
    return TRUE;
}
static void run_queue(Activity *a) {
    J *queue = scoped(a, UI_READ, "agents.next", "project", value(a->project));
    gboolean started = FALSE;
    for (size_t i = 0; i < count(queue) && g_hash_table_size(a->workers) < 2; i++)
        if (start_job(a, json_object_array_get_idx(queue, i)))
            started = TRUE;
    json_object_put(queue);
    if (!started && !g_hash_table_size(a->workers))
        a->drain = FALSE;
}
static void begin_queue(GtkButton *b, Activity *a) {
    (void)b;
    if (!*value(a->project))
        return;
    a->drain = TRUE;
    run_queue(a);
    copilot_activity_refresh(a->c);
}
static void create_job(GtkButton *button, Activity *a) {
    (void)button;
    const char *model = value(a->model), *project = value(a->project),
               *text = gtk_entry_get_text(GTK_ENTRY(a->task_input));
    if (!*text || !*model || !*project || a->c->busy) {
        copilot_status(a->c, "请先选择项目、模型，填写任务。");
        return;
    }
    J *registry = ui_call(a->c, UI_READ, ui_op("config.list"));
    if (!registry)
        return;
    char *id = g_uuid_string_random();
    J *q = ui_op("agents.create");
    ui_string(q, "id", id);
    g_free(id);
    ui_string(q, "task", text);
    ui_string(q, "project", project);
    ui_string(q, "model", model);
    ui_string(q, "parent", "");
    ui_string(q, "chat", "");
    json_object_object_add(q, "revision", json_object_get(ui_get(registry, "revision")));
    json_object_put(registry);
    J *result = ui_call(a->c, UI_WORKSPACE, q);
    if (result) {
        gtk_entry_set_text(GTK_ENTRY(a->task_input), "");
        json_object_put(result);
        copilot_chat_refresh(a->c);
    }
    copilot_activity_refresh(a->c);
}
static void task_action(GtkButton *b, Activity *a) {
    const char *id = button_id(b);
    const char *action = g_object_get_data(G_OBJECT(b), "action");
    if (!strcmp(action, "cancel")) {
        Worker *w = g_hash_table_lookup(a->workers, id);
        if (w) {
            a->drain = FALSE;
            g_atomic_int_set(&w->stopped, 1);
        } else
            json_object_put(scoped(a, UI_WORKSPACE, "agents.cancel", "id", id));
    } else {
        J *job = g_object_get_data(G_OBJECT(b), "job");
        if (!strcmp(action, "inspect")) {
            J *r = scoped(a, UI_READ, "chat.messages", "chat", ui_text(job, "chat"));
            GString *text = g_string_new("");
            for (size_t i = 0; i < count(r); i++) {
                J *m = json_object_array_get_idx(r, i);
                g_string_append_printf(text, "%s\n%s\n\n", ui_text(m, "role"), ui_text(m, "content"));
            }
            ui_detail(a->c, "任务聊天原文", text->len ? text->str : "这项任务尚无已提交的消息。");
            g_string_free(text, TRUE);
            json_object_put(r);
        } else if (!start_job(a, job))
            copilot_status(a->c, "任务不可启动，或工作目录正被其他任务使用。");
    }
    copilot_activity_refresh(a->c);
}
static void drop_json(gpointer value) {
    json_object_put(value);
}
static void action_button(GtkWidget *row, const char *title, const char *action, Activity *a, J *job) {
    GtkWidget *b = ui_button(row, title, G_CALLBACK(task_action), a);
    set_id(b, ui_text(job, "id"));
    g_object_set_data(G_OBJECT(b), "action", (gpointer)action);
    g_object_set_data_full(G_OBJECT(b), "job", json_object_get(job), drop_json);
}
static void join_chat(GtkButton *b, Activity *a) {
    (void)b;
    if (!a->c->chat_id || !*a->c->chat_id) {
        copilot_status(a->c, "请先打开要加入协作的项目聊天。");
        return;
    }
    J *q = ui_op("agents.join");
    ui_string(q, "project", value(a->project));
    ui_string(q, "chat", a->c->chat_id);
    json_object_put(ui_call(a->c, UI_WORKSPACE, q));
    copilot_activity_refresh(a->c);
}
static void send_mail(GtkButton *b, Activity *a) {
    (void)b;
    const char *from = value(a->members), *to = value(a->receivers),
               *text = gtk_entry_get_text(GTK_ENTRY(a->mail_input));
    if (!*from || !*to || !*text || !strcmp(from, to)) {
        copilot_status(a->c, "请选择不同的发送和接收对话，并填写消息。");
        return;
    }
    J *q = ui_op("agents.send");
    ui_string(q, "project", value(a->project));
    ui_string(q, "sender", from);
    ui_string(q, "receiver", to);
    ui_string(q, "text", text);
    char *id = g_uuid_string_random();
    ui_string(q, "id", id);
    g_free(id);
    J *v = ui_call(a->c, UI_WORKSPACE, q);
    if (v) {
        gtk_entry_set_text(GTK_ENTRY(a->mail_input), "");
        copilot_status(a->c, "已放入接收方收件箱。");
        json_object_put(v);
    }
    copilot_activity_refresh(a->c);
}
gboolean copilot_activity_stop_chat(LinuxClient *c, const char *chat) {
    Activity *a = g_object_get_data(G_OBJECT(c->window), key);
    if (!a || !chat || !*chat)
        return FALSE;
    GHashTableIter it;
    gpointer data;
    g_hash_table_iter_init(&it, a->workers);
    while (g_hash_table_iter_next(&it, NULL, &data)) {
        Worker *w = data;
        g_mutex_lock(&w->origin_lock);
        gboolean owns = !g_strcmp0(chat, w->active_chat ? w->active_chat : w->chat);
        g_mutex_unlock(&w->origin_lock);
        if (owns) {
            a->drain = FALSE;
            g_atomic_int_set(&w->stopped, 1);
            return TRUE;
        }
    }
    return FALSE;
}
static J *chat_request(Activity *a, UiRoute rt, const char *op, const char *id) {
    J *q = ui_op(op);
    ui_string(q, "chat", a->c->chat_id);
    if (id)
        ui_string(q, "id", id);
    return ui_call(a->c, rt, q);
}
static void recovery_action(GtkButton *b, Activity *a) {
    (void)b;
    if (a->c->busy)
        return;
    J *v = chat_request(a, UI_WORKSPACE, "recovery.inspect", NULL);
    if (!v)
        return;
    if (!json_object_get_boolean(ui_get(v, "active"))) {
        ui_detail(a->c, "中断恢复", "当前聊天没有需要恢复的回合。");
        json_object_put(v);
        return;
    }
    gboolean unknown = *ui_text(v, "pending") != 0;
    gchar *msg = g_strdup_printf("%s\n%s",
                                 unknown ? "有一项外部效果尚未确认，结束后将保留为未知。"
                                         : "将结束未完成回合，保留聊天和现有结果。",
                                 ui_text(v, "detail"));
    if (ui_confirm(a->c, "结束这次中断的回合？", msg, "结束回合")) {
        J *q = ui_op("recovery.abandon");
        ui_string(q, "chat", a->c->chat_id);
        ui_string(q, "stateHash", ui_text(v, "stateHash"));
        json_object_object_add(q, "acknowledgeUnknown", json_object_new_boolean(unknown));
        json_object_put(ui_call(a->c, UI_WORKSPACE, q));
        copilot_chat_refresh(a->c);
    }
    g_free(msg);
    json_object_put(v);
    copilot_activity_refresh(a->c);
}
static void receipt_action(GtkButton *b, Activity *a) {
    const char *id = button_id(b);
    J *d = chat_request(a, UI_READ, "receipts.detail", id);
    if (!d)
        return;
    if (!g_object_get_data(G_OBJECT(b), "restore")) {
        char *text = g_strdup_printf("%s · %s\n\n修改前\n%s\n\n修改后\n%s\n\n当前磁盘状态 · %s",
                                     ui_text(d, "path"), ui_text(d, "operation"), ui_text(d, "before"),
                                     ui_text(d, "after"), ui_text(d, "currentState"));
        ui_detail(a->c, "文件修改记录", text);
        g_free(text);
    } else if (!a->c->busy) {
        gboolean unknown = json_object_get_boolean(ui_get(d, "unknownAcknowledgementRequired"));
        if (!unknown ||
            ui_confirm(a->c, "原工具效果没有结算，继续检查回退？", ui_text(d, "path"), "检查当前文件")) {
            J *q = ui_op("receipts.preview");
            ui_string(q, "chat", a->c->chat_id);
            ui_string(q, "id", id);
            json_object_object_add(q, "acknowledgeUnknown", json_object_new_boolean(unknown));
            J *plan = ui_call(a->c, UI_WORKSPACE, q);
            if (plan && *ui_text(plan, "plan") &&
                ui_confirm(a->c, "将文件恢复到记录的修改前状态？", ui_text(d, "path"), "确认回退")) {
                q = ui_op("receipts.restore");
                ui_string(q, "chat", a->c->chat_id);
                ui_string(q, "plan", ui_text(plan, "plan"));
                json_object_object_add(q, "confirmed", json_object_new_boolean(TRUE));
                J *res = ui_call(a->c, UI_WORKSPACE, q);
                if (res) {
                    const char *outcome = ui_text(res, "status");
                    if (!strcmp(outcome, "completed") || !strcmp(outcome, "ok"))
                        copilot_status(a->c, "文件已回退。");
                    else {
                        char *message =
                            g_strdup_printf("文件回退结果 · %s · %s", outcome, ui_text(res, "result"));
                        copilot_status(a->c, message);
                        g_free(message);
                    }
                }
                json_object_put(res);
            }
            json_object_put(plan);
        }
    }
    json_object_put(d);
    copilot_activity_refresh(a->c);
}
static void trace_detail(GtkButton *b, Activity *a) {
    const char *id = button_id(b);
    gchar *query = ui_prompt(a->c, "筛选事件（留空查看全部）", "", FALSE);
    if (!query)
        return;
    gint64 before = 0;
    GString *content = g_string_new("");
    gboolean more;
    do {
        J *q = ui_op("trace.events");
        ui_string(q, "chat", a->c->chat_id);
        ui_string(q, "segment", id);
        ui_string(q, "query", query);
        json_object_object_add(q, "before", json_object_new_int64(before));
        J *res = ui_call(a->c, UI_READ, q);
        J *items = ui_get(res, "items");
        for (size_t i = 0; i < count(items); i++) {
            J *e = json_object_array_get_idx(items, i), *body = ui_get(e, "body");
            g_string_append_printf(content, "+%" G_GINT64_FORMAT " ms   %s\n%s\n\n",
                                   (gint64)json_object_get_int64(ui_get(e, "ms")), ui_text(e, "kind"),
                                   body ? json_object_to_json_string_ext(body, JSON_C_TO_STRING_PRETTY)
                                        : "");
        }
        gint64 next = json_object_get_int64(ui_get(res, "nextCursor"));
        more = json_object_get_boolean(ui_get(res, "more")) && next != before &&
               content->len < 2 * 1024 * 1024;
        before = next;
        json_object_put(res);
    } while (more);
    ui_detail(a->c, "模型运行轨迹", content->len ? content->str : "没有匹配的事件。");
    g_string_free(content, TRUE);
    g_free(query);
}
static void more_history(GtkButton *b, Activity *a) {
    if (g_object_get_data(G_OBJECT(b), "files"))
        a->file_limit += 50;
    else
        a->trace_limit += 50;
    copilot_activity_refresh(a->c);
}
static void history(Activity *a, gboolean files) {
    GtkWidget *area = files ? a->files : a->traces;
    ui_clear(area);
    if (!a->c->chat_id || !*a->c->chat_id) {
        info(a, area, "请先打开一段聊天。");
        return;
    }
    gint64 cursor = 0, limit = files ? a->file_limit : a->trace_limit, shown = 0;
    gboolean more = FALSE;
    do {
        J *q = ui_op(files ? "receipts.list" : "trace.segments");
        ui_string(q, "chat", a->c->chat_id);
        if (cursor)
            json_object_object_add(q, "before", json_object_new_int64(cursor));
        J *page = ui_call(a->c, UI_READ, q), *items = ui_get(page, "items");
        for (size_t i = 0; i < count(items); i++) {
            J *item = json_object_array_get_idx(items, i);
            GtkWidget *row = ui_row(area, ui_text(item, files ? "path" : "model"),
                                    ui_text(item, files ? "state" : "status"));
            GtkWidget *b = ui_button(row, files ? "查看修改" : "事件…",
                                     files ? G_CALLBACK(receipt_action) : G_CALLBACK(trace_detail), a);
            set_id(b, ui_text(item, "id"));
            if (files) {
                b = ui_button(row, "检查回退…", G_CALLBACK(receipt_action), a);
                set_id(b, ui_text(item, "id"));
                g_object_set_data(G_OBJECT(b), "restore", GINT_TO_POINTER(1));
                gtk_widget_set_sensitive(b, !a->c->busy);
            }
            shown++;
        }
        gint64 next = json_object_get_int64(ui_get(page, "nextCursor"));
        more = json_object_get_boolean(ui_get(page, "more")) && count(items) > 0 && cursor != next;
        cursor = next;
        json_object_put(page);
    } while (more && shown < limit);
    if (more) {
        GtkWidget *b = ui_button(area, "加载更早记录", G_CALLBACK(more_history), a);
        g_object_set_data(G_OBJECT(b), "files", GINT_TO_POINTER(files));
    }
    if (!shown)
        info(a, area, files ? "当前聊天没有文件修改回执。" : "当前聊天没有模型运行记录。");
}
static void refresh_clicked(GtkButton *b, Activity *a) {
    (void)b;
    copilot_activity_refresh(a->c);
}
void copilot_activity_refresh(LinuxClient *c) {
    Activity *a = g_object_get_data(G_OBJECT(c->window), key);
    if (!a || a->rendering)
        return;
    a->rendering = TRUE;
    if (g_strcmp0(a->filter_id, c->chat_id)) {
        g_free(a->filter_id);
        a->filter_id = g_strdup(c->chat_id);
        a->trace_limit = a->file_limit = 50;
    }
    if (!c->busy) {
        J *ps = ui_call(c, UI_WORKSPACE, ui_op("projects.list"));
        if (ps)
            retain_combo(a->project, ps, *value(a->project) ? value(a->project) : c->project_id, "name");
        json_object_put(ps);
        J *reg = ui_call(c, UI_READ, ui_op("config.list"));
        if (reg)
            retain_combo(a->model, ui_get(reg, "models"), *value(a->model) ? value(a->model) : c->model_id,
                         "name");
        json_object_put(reg);
    }
    ui_clear(a->tasks);
    ui_clear(a->mail);
    ui_clear(a->recovery);
    const char *project = value(a->project);
    J *jobs = ui_call(c, UI_READ, ui_op("agents.list"));
    J *people = *project ? scoped(a, UI_READ, "agents.members", "project", project) : NULL;
    if (!people)
        people = json_object_new_array();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (size_t i = 0; i < count(people); i++)
        g_hash_table_add(seen, (gpointer)ui_text(json_object_array_get_idx(people, i), "id"));
    int visible = 0;
    for (size_t i = 0; i < count(jobs); i++) {
        J *job = json_object_array_get_idx(jobs, i);
        if (strcmp(project, ui_text(job, "project")))
            continue;
        const char *chat = ui_text(job, "chat"), *state = ui_text(job, "state");
        if (!g_hash_table_contains(seen, chat)) {
            g_hash_table_add(seen, (gpointer)chat);
            J *p = json_object_new_object();
            ui_string(p, "id", chat);
            ui_string(p, "title", ui_text(job, "task"));
            json_object_array_add(people, p);
        }
        char *subtitle = g_strdup_printf("%s · %s", state, ui_text(job, "model"));
        GtkWidget *row = ui_row(a->tasks, ui_text(job, "task"), subtitle);
        g_free(subtitle);
        if (!strcmp(state, "queued"))
            action_button(row, "运行", "run", a, job);
        if (!strcmp(state, "queued") || g_hash_table_contains(a->workers, ui_text(job, "id")))
            action_button(row, "停止", "cancel", a, job);
        action_button(row, "原文", "inspect", a, job);
        visible++;
    }
    if (!visible)
        info(a, a->tasks, "当前项目没有协作任务。");
    g_hash_table_destroy(seen);
    json_object_put(jobs);
    retain_combo(a->members, people, value(a->members), "title");
    retain_combo(a->receivers, people, value(a->receivers), "title");
    json_object_put(people);
    J *messages = *project ? scoped(a, UI_READ, "agents.messages", "project", project) : NULL;
    for (size_t i = 0; i < count(messages); i++) {
        J *m = json_object_array_get_idx(messages, i);
        ui_row(a->mail, ui_text(m, "text"),
               json_object_get_boolean(ui_get(m, "delivered")) ? "已送达" : "等待接收方运行");
    }
    json_object_put(messages);
    if (c->chat_id && *c->chat_id && !c->busy) {
        J *v = chat_request(a, UI_WORKSPACE, "recovery.inspect", NULL);
        if (json_object_get_boolean(ui_get(v, "active"))) {
            ui_row(a->recovery, "等待处理的回合", ui_text(v, "detail"));
            ui_button(a->recovery, "检查并结束…", G_CALLBACK(recovery_action), a);
        } else
            info(a, a->recovery, "当前聊天没有需要处理的回合。");
        json_object_put(v);
    }
    history(a, TRUE);
    history(a, FALSE);
    gtk_widget_show_all(a->widget);
    a->rendering = FALSE;
}
static void launch_helpers(Activity *a) {
    LinuxClient *c=a->c;
    J *rows=ui_call(c,UI_READ,ui_op("helpers.pending"));
    if(!count(rows)){json_object_put(rows);return;}
    J *registry=ui_call(c,UI_READ,ui_op("config.list"));
    gint64 revision=json_object_get_int64(ui_get(registry,"revision"));json_object_put(registry);
    for(size_t i=0;i<count(rows)&&g_hash_table_size(a->workers)<2;i++){
        J *job=json_object_array_get_idx(rows,i);const char *id=ui_text(job,"id");
        if(revision<=0||json_object_get_int64(ui_get(job,"revision"))!=revision||g_hash_table_contains(a->workers,id))continue;
        gchar *secret=NULL,*cfg=copilot_model_config_for_id(c,ui_text(job,"model"),&secret);
        g_mutex_lock(&c->lock);
        dc_store *worker=cfg?dc_open_agent_worker(c->store):NULL;
        if(!worker)dc_helper_cancel(c->store,id,revision);
        g_mutex_unlock(&c->lock);g_free(cfg);
        if(!worker){if(secret){volatile char *p=secret;for(gsize n=strlen(secret);n>0;n--)*p++=0;g_free(secret);}continue;}
        Worker *w=g_new0(Worker,1);w->page=a;w->worker=worker;w->id=g_strdup(id);w->secret=secret;w->helper_revision=revision;
        g_mutex_init(&w->origin_lock);g_hash_table_insert(a->workers,w->id,w);c->busy=TRUE;
        g_thread_unref(g_thread_new("plugin-model",execute,w));
    }
    json_object_put(rows);
}
void copilot_activity_dispatch_plugins(LinuxClient *c) {
    Activity *a=g_object_get_data(G_OBJECT(c->window),key);
    if(!a||c->closing||c->pending_identity||a->drain||(c->busy&&!g_hash_table_size(a->workers)))return;
    launch_helpers(a);
    if(g_hash_table_size(a->workers)>=2)return;
    J *rows=ui_call(c,UI_READ,ui_op("agents.nativePending"));
    for(size_t i=0;i<count(rows)&&g_hash_table_size(a->workers)<2;i++)start_job(a,json_object_array_get_idx(rows,i));
    json_object_put(rows);
}
static gboolean tick(gpointer ptr) {
    Activity *a = ptr;
    copilot_activity_dispatch_plugins(a->c);
    if (a->c->busy && gtk_widget_get_mapped(a->widget))
        copilot_activity_refresh(a->c);
    return G_SOURCE_CONTINUE;
}
static void release(gpointer ptr) {
    Activity *a = ptr;
    if (a->timer)
        g_source_remove(a->timer);
    g_hash_table_destroy(a->workers);
    g_free(a->filter_id);
    g_free(a);
}
GtkWidget *copilot_activity_new(LinuxClient *c) {
    Activity *a = g_new0(Activity, 1);
    a->c = c;
    a->workers = g_hash_table_new(g_str_hash, g_str_equal);
    a->trace_limit = a->file_limit = 50;
    a->widget = gtk_notebook_new();
    g_object_set_data_full(G_OBJECT(c->window), key, a, release);
    GtkWidget *body, *agents = ui_page("Agent 协作", &body);
    a->project = combo_row(body, "项目");
    a->model = combo_row(body, "模型");
    g_signal_connect(a->project, "changed", G_CALLBACK(project_changed), a);
    a->task_input = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->task_input), "分配给子任务的目标");
    gtk_box_pack_start(GTK_BOX(body), a->task_input, FALSE, FALSE, 0);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(body), bar, FALSE, FALSE, 0);
    ui_button(bar, "创建任务", G_CALLBACK(create_job), a);
    ui_button(bar, "运行项目队列", G_CALLBACK(begin_queue), a);
    ui_button(bar, "停止调度", G_CALLBACK(stop_all), a);
    ui_button(bar, "刷新", G_CALLBACK(refresh_clicked), a);
    a->tasks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(body), a->tasks, FALSE, FALSE, 0);
    ui_button(body, "将当前项目聊天加入协作", G_CALLBACK(join_chat), a);
    a->members = combo_row(body, "发送对话");
    a->receivers = combo_row(body, "接收对话");
    gtk_widget_set_name(a->members, "agent-sender");
    gtk_widget_set_name(a->receivers, "agent-receiver");
    a->mail_input = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->mail_input), "作为资料发送给另一段对话");
    gtk_box_pack_start(GTK_BOX(body), a->mail_input, FALSE, FALSE, 0);
    ui_button(body, "发送协作消息", G_CALLBACK(send_mail), a);
    a->mail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(body), a->mail, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->widget), agents, gtk_label_new("协作"));
    GtkWidget *p = ui_page("模型运行轨迹", &a->traces);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->widget), p, gtk_label_new("模型轨迹"));
    p = ui_page("文件修改历史", &a->files);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->widget), p, gtk_label_new("文件历史"));
    p = ui_page("中断恢复", &a->recovery);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->widget), p, gtk_label_new("中断恢复"));
    a->timer = g_timeout_add_seconds(2, tick, a);
    return a->widget;
}
