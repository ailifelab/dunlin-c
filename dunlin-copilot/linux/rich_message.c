#include "rich_message.h"
#include "ui.h"
#include <libxml/HTMLparser.h>
#include <string.h>

typedef struct {
    GtkWidget *box;
    GString *markup;
    guint tables;
} Document;

static void literal(GString *out, const char *text, gsize length) {
    char *safe = g_markup_escape_text(text, length);
    g_string_append(out, safe);
    g_free(safe);
}
static void inline_text(GString *out, const char *s, gsize length, guint depth) {
    const char *end = s + length, *plain = s;
    if (depth >= 8) {
        literal(out, s, length);
        return;
    }
    while (s < end) {
        gboolean matched = FALSE;
        if (*s == '\\' && end - s >= 2 && strchr("*`[]_|\\", s[1])) {
            literal(out, plain, s - plain);
            literal(out, s + 1, 1);
            s += 2;
            plain = s;
            continue;
        }
        const char *open = NULL, *close = NULL, *marker = s;
        gsize size = 1;
        if (*s == '`') {
            while (s + size < end && s[size] == '`')
                size++;
            open = "<tt>";
            close = "</tt>";
        } else if (*s == '*' || (*s == '_' && (s == plain || g_ascii_isspace(s[-1])))) {
            if (end - s >= 2 && s[1] == *s) {
                size = 2;
                open = "<b>";
                close = "</b>";
            } else {
                open = "<i>";
                close = "</i>";
            }
        } else if (*s == '[') {
            const char *label_end = g_strstr_len(s + 1, MIN(end - s - 1, 4096), "](");
            const char *address_end = label_end ? memchr(label_end + 2, ')', end - label_end - 2) : NULL;
            if (address_end && label_end > s + 1) {
                literal(out, plain, s - plain);
                g_string_append(out, "<u>");
                inline_text(out, s + 1, label_end - s - 1, depth + 1);
                g_string_append(out, "</u>");
                s = address_end + 1;
                plain = s;
                continue;
            }
        }
        if (open && end - s > (gssize)size) {
            gchar *token = g_strndup(marker, size);
            const char *stop = g_strstr_len(s + size, MIN((gsize)(end - s) - size, 4096), token);
            g_free(token);
            if (stop && stop > s + (gssize)size) {
                literal(out, plain, s - plain);
                g_string_append(out, open);
                if (*s == '`')
                    literal(out, s + size, stop - s - size);
                else
                    inline_text(out, s + size, stop - s - size, depth + 1);
                g_string_append(out, close);
                s = stop + size;
                plain = s;
                matched = TRUE;
            }
        }
        if (!matched)
            s += size;
    }
    literal(out, plain, end - plain);
}
static GtkWidget *rich_label(void) {
    GtkWidget *label = ui_label("");
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 72);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    return label;
}
static void flush(Document *doc) {
    if (!doc->markup->len)
        return;
    while (doc->markup->len && doc->markup->str[doc->markup->len - 1] == '\n')
        g_string_truncate(doc->markup, doc->markup->len - 1);
    if (doc->markup->len) {
        GtkWidget *label = rich_label();
        gtk_label_set_markup(GTK_LABEL(label), doc->markup->str);
        gtk_box_pack_start(GTK_BOX(doc->box), label, FALSE, FALSE, 0);
    }
    g_string_truncate(doc->markup, 0);
}
static void cell(GtkWidget *grid, gint row, gint col, const gchar *text, gboolean heading,
                 gboolean markdown) {
    GtkWidget *label = rich_label();
    GString *markup = g_string_new(heading ? "<b>" : "");
    if (markdown)
        inline_text(markup, text, strlen(text), 0);
    else
        literal(markup, text, strlen(text));
    if (heading)
        g_string_append(markup, "</b>");
    gtk_label_set_markup(GTK_LABEL(label), markup->str);
    g_string_free(markup, TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, col, row, 1, 1);
}
static GtkWidget *table(Document *d) {
    flush(d);
    d->tables++;
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "reply-table");
    gtk_box_pack_start(GTK_BOX(d->box), grid, FALSE, FALSE, 0);
    return grid;
}
static GPtrArray *cells(const char *line) {
    if (!strchr(line, '|'))
        return NULL;
    GPtrArray *cols = g_ptr_array_new_with_free_func(g_free);
    GString *buf = g_string_new(NULL);
    const char *p = line;
    while (*p == ' ')
        p++;
    if (*p == '|')
        p++;
    gboolean code = FALSE, ended = FALSE;
    while (*p) {
        if (*p == '\\' && p[1] == '|') {
            g_string_append_c(buf, '|');
            p += 2;
            ended = FALSE;
            continue;
        }
        if (*p == '`')
            code = !code;
        if (*p == '|' && !code) {
            g_ptr_array_add(cols, g_strdup(g_strstrip(buf->str)));
            g_string_truncate(buf, 0);
            ended = TRUE;
        } else {
            g_string_append_c(buf, *p);
            if (!g_ascii_isspace(*p))
                ended = FALSE;
        }
        p++;
    }
    if (!ended)
        g_ptr_array_add(cols, g_strdup(g_strstrip(buf->str)));
    g_string_free(buf, TRUE);
    if (code || cols->len < 2 || cols->len > 16) {
        g_ptr_array_unref(cols);
        return NULL;
    }
    return cols;
}
static gboolean delimiter(GPtrArray *cols) {
    if (!cols)
        return FALSE;
    for (guint i = 0; i < cols->len; i++) {
        const char *v = cols->pdata[i];
        if (*v == ':')
            v++;
        guint n = 0;
        while (*v == '-') {
            n++;
            v++;
        }
        if (*v == ':')
            v++;
        if (n < 3 || *v)
            return FALSE;
    }
    return TRUE;
}
static void add_cells(GtkWidget *grid, gint row, GPtrArray *cols) {
    for (guint i = 0; i < cols->len; i++)
        cell(grid, row, i, cols->pdata[i], row == 0, TRUE);
}
static void code(Document *d, const char *language, const char *text) {
    flush(d);
    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_style_context_add_class(gtk_widget_get_style_context(wrap), "reply-code");
    if (language && *language) {
        GtkWidget *lang = ui_label(language);
        gtk_style_context_add_class(gtk_widget_get_style_context(lang), "chat-metadata");
        gtk_box_pack_start(GTK_BOX(wrap), lang, FALSE, FALSE, 0);
    }
    GtkWidget *v = rich_label();
    gtk_label_set_text(GTK_LABEL(v), text);
    gtk_style_context_add_class(gtk_widget_get_style_context(v), "reply-monospace");
    gtk_box_pack_start(GTK_BOX(wrap), v, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(d->box), wrap, FALSE, FALSE, 0);
}
static guint repeated(const char *s, char c) {
    guint n = 0;
    while (s[n] == c)
        n++;
    return n;
}
static void markdown(Document *d, const char *source) {
    gchar **lines = g_strsplit(source, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        const char *s = lines[i];
        gsize n = strlen(s);
        if (n && s[n - 1] == '\r')
            lines[i][--n] = 0;
        for (guint j = 0; j < 3 && *s == ' '; j++)
            s++;
        guint fence = (*s == '`' || *s == '~') ? repeated(s, *s) : 0;
        if (fence >= 3) {
            const char marker = *s;
            const char *lang = s + fence;
            GString *body = g_string_new(NULL);
            while (lines[i + 1]) {
                const char *next = lines[++i], *marker_start = next;
                for (guint j = 0; j < 3 && *marker_start == ' '; j++)
                    marker_start++;
                guint run = repeated(marker_start, marker);
                if (run >= fence && strspn(marker_start + run, " \t\r") == strlen(marker_start + run))
                    break;
                if (body->len)
                    g_string_append_c(body, '\n');
                g_string_append(body, next);
            }
            code(d, lang, body->str);
            g_string_free(body, TRUE);
            continue;
        }
        if (d->tables < 40 && lines[i + 1] && strchr(s, '|')) {
            GPtrArray *head = cells(s), *dash = cells(lines[i + 1]);
            gboolean ok = head && delimiter(dash) && dash->len == head->len;
            if (dash)
                g_ptr_array_unref(dash);
            if (ok) {
                GtkWidget *grid = table(d);
                add_cells(grid, 0, head);
                guint col_count = head->len, row = 1;
                i++;
                while (lines[i + 1] && row < 200) {
                    GPtrArray *v = cells(lines[i + 1]);
                    if (!v)
                        break;
                    if (v->len != col_count) {
                        g_ptr_array_unref(v);
                        break;
                    }
                    add_cells(grid, row++, v);
                    g_ptr_array_unref(v);
                    i++;
                }
            }
            if (head)
                g_ptr_array_unref(head);
            if (ok)
                continue;
        }
        guint heading = repeated(s, '#');
        const char *close = "";
        if (heading >= 1 && heading <= 6 && s[heading] == ' ') {
            g_string_append(d->markup, heading == 1 ? "<span size=\"x-large\" weight=\"bold\">"
                                                    : "<span size=\"large\" weight=\"bold\">");
            close = "</span>";
            s += heading + 1;
        } else if (*s == '>' && (s[1] == ' ' || !s[1])) {
            g_string_append(d->markup, "<i>│  ");
            s += s[1] ? 2 : 1;
            close = "</i>";
        } else if ((*s == '*' || *s == '-' || *s == '+') && s[1] == ' ') {
            g_string_append(d->markup, "•  ");
            s += 2;
        } else
            s = lines[i];
        inline_text(d->markup, s, strlen(s), 0);
        g_string_append(d->markup, close);
        if (lines[i + 1])
            g_string_append_c(d->markup, '\n');
    }
    g_strfreev(lines);
}

static gboolean tag(xmlNode *node, const char *name) {
    return !xmlStrcasecmp(node->name, BAD_CAST name);
}
static void html_nodes(Document *d, xmlNode *node, guint depth);
static guint html_rows(GtkWidget *grid, xmlNode *node, guint row, guint depth) {
    if (depth > 4)
        return row;
    for (; node && row < 200; node = node->next) {
        if (node->type != XML_ELEMENT_NODE)
            continue;
        if (tag(node, "thead") || tag(node, "tbody") || tag(node, "tfoot")) {
            row = html_rows(grid, node->children, row, depth + 1);
            continue;
        }
        if (!tag(node, "tr"))
            continue;
        guint col = 0;
        for (xmlNode *t = node->children; t && col < 16; t = t->next) {
            if (!tag(t, "th") && !tag(t, "td"))
                continue;
            xmlChar *text = xmlNodeGetContent(t);
            if (text) {
                cell(grid, row, col++, (const char *)text, tag(t, "th"), FALSE);
                xmlFree(text);
            }
        }
        row++;
    }
    return row;
}
static void html_nodes(Document *d, xmlNode *node, guint depth) {
    if (depth > 64)
        return;
    for (; node; node = node->next) {
        if (node->type == XML_TEXT_NODE) {
            if (node->content)
                literal(d->markup, (const char *)node->content, xmlStrlen(node->content));
            continue;
        }
        if (node->type != XML_ELEMENT_NODE)
            continue;
        if (tag(node, "script") || tag(node, "style") || tag(node, "head") || tag(node, "iframe") ||
            tag(node, "object") || tag(node, "svg"))
            continue;
        if (tag(node, "table") && d->tables < 40) {
            GtkWidget *grid = table(d);
            html_rows(grid, node->children, 0, 0);
            continue;
        }
        if (tag(node, "pre")) {
            xmlChar *text = xmlNodeGetContent(node);
            code(d, "", (const char *)(text ? text : BAD_CAST ""));
            xmlFree(text);
            continue;
        }
        if (tag(node, "br")) {
            g_string_append_c(d->markup, '\n');
            continue;
        }
        if (tag(node, "img")) {
            xmlChar *alt = xmlGetProp(node, BAD_CAST "alt");
            if (alt) {
                literal(d->markup, (const char *)alt, xmlStrlen(alt));
                xmlFree(alt);
            }
            continue;
        }
        const char *open = "", *close = "";
        gboolean line = FALSE;
        if (tag(node, "p") || tag(node, "div") || tag(node, "li") || tag(node, "blockquote"))
            line = TRUE;
        if (tag(node, "li"))
            open = "•  ";
        if (tag(node, "strong") || tag(node, "b")) {
            open = "<b>";
            close = "</b>";
        } else if (tag(node, "em") || tag(node, "i") || tag(node, "blockquote")) {
            open = "<i>";
            close = "</i>";
        } else if (tag(node, "code")) {
            open = "<tt>";
            close = "</tt>";
        } else if (tag(node, "a") || tag(node, "u")) {
            open = "<u>";
            close = "</u>";
        } else if (tag(node, "del") || tag(node, "s")) {
            open = "<s>";
            close = "</s>";
        } else if (strlen((const char *)node->name) == 2 && node->name[0] == 'h' && node->name[1] >= '1' &&
                   node->name[1] <= '6') {
            open = node->name[1] == '1' ? "<span size=\"x-large\" weight=\"bold\">"
                                        : "<span size=\"large\" weight=\"bold\">";
            close = "</span>";
            line = TRUE;
        }
        /* Keep nested cells/code blocks outside markup runs in malformed rich HTML. */
        if (*open && *close) {
            Document sub = {d->box, g_string_new(NULL), d->tables};
            html_nodes(&sub, node->children, depth + 1);
            d->tables = sub.tables;
            g_string_append(d->markup, open);
            g_string_append(d->markup, sub.markup->str);
            g_string_append(d->markup, close);
            g_string_free(sub.markup, TRUE);
        } else {
            g_string_append(d->markup, open);
            html_nodes(d, node->children, depth + 1);
        }
        if (line)
            g_string_append_c(d->markup, '\n');
    }
}
static gboolean html_source(const char *s) {
    while (g_ascii_isspace(*s))
        s++;
    if (*s++ != '<')
        return FALSE;
    const char *names[] = {"!doctype", "html",       "body",   "article", "section", "div", "p",    "h1",
                           "h2",       "h3",         "h4",     "h5",      "h6",      "pre", "ul",   "ol",
                           "table",    "blockquote", "strong", "b",       "em",      "i",   "span", NULL};
    for (guint i = 0; names[i]; i++) {
        gsize n = strlen(names[i]);
        if (!g_ascii_strncasecmp(s, names[i], n) && (s[n] == '>' || g_ascii_isspace(s[n])))
            return TRUE;
    }
    return FALSE;
}
GtkWidget *copilot_rich_message_new(const gchar *source) {
    Document d = {gtk_box_new(GTK_ORIENTATION_VERTICAL, 7), g_string_new(NULL), 0};
    gtk_widget_set_name(d.box, "rich-reply");
    gtk_style_context_add_class(gtk_widget_get_style_context(d.box), "chat-body");
    gchar *valid = g_utf8_make_valid(source ? source : "", -1);
    gchar *out = valid;
    for (gchar *p = valid; *p; p++) {
        if ((guchar)*p >= 0x20 || *p == '\n' || *p == '\t' || *p == '\r')
            *out++ = *p;
    }
    *out = 0;
    gsize len = out - valid;
    htmlDocPtr html = NULL;
    if (len <= 1024 * 1024 && html_source(valid))
        html = htmlReadMemory(valid, (int)len, NULL, "UTF-8",
                              HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING |
                                  HTML_PARSE_COMPACT);
    if (html) {
        html_nodes(&d, xmlDocGetRootElement(html), 0);
        xmlFreeDoc(html);
    } else if (len <= 1024 * 1024)
        markdown(&d, valid);
    else
        literal(d.markup, valid, len);
    flush(&d);
    g_string_free(d.markup, TRUE);
    g_free(valid);
    return d.box;
}
void copilot_chat_style(GtkWidget *root) {
    GdkScreen *screen = gtk_widget_get_screen(root);
    if (g_object_get_data(G_OBJECT(screen), "dunlin-chat-style"))
        return;
    GtkCssProvider *provider = gtk_css_provider_new();
    const char css[] =
        ".chat-title { font-size:18pt; font-weight:bold; } "
        ".chat-body, #chat-input { font-size:14pt; } .chat-metadata { font-size:13pt; } "
        ".chat-text-small .chat-body, .chat-text-small #chat-input { font-size:13pt; } "
        ".chat-text-small .chat-metadata { font-size:12pt; } "
        ".chat-text-large .chat-body, .chat-text-large #chat-input { font-size:18pt; } "
        ".chat-text-large .chat-metadata { font-size:16pt; } .reply-table { font-size:93%; } "
        ".chat-incoming, .chat-outgoing { border-radius:10px; } .chat-incoming > border, .chat-outgoing > "
        "border { border-color:alpha(@theme_fg_color,.18); border-radius:10px; } "
        ".chat-incoming { background-color:alpha(@theme_fg_color,.045); } .chat-outgoing { "
        "background-color:alpha(@theme_selected_bg_color,.12); } "
        ".reply-code,.reply-table { padding:6px; border-radius:5px; "
        "background-color:alpha(@theme_fg_color,.07); } "
        ".reply-monospace { font-family:monospace; font-size:93%; } .composer { box-shadow:0 2px 4px alpha(black,.13); "
        "border-radius:6px; }";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set_data_full(G_OBJECT(screen), "dunlin-chat-style", provider, g_object_unref);
}
