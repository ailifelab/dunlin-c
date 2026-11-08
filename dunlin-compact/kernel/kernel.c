#include <dunlin/compact/kernel.h>
#include <string.h>

typedef enum { SERVICE, LISTENER, EFFECT } kind;
typedef struct entry {
    struct entry *next;
    ak_scope *owner;
    ak_handle id;
    kind type;
    const char *name;
    unsigned version;
    const void *iface;
    ak_listener listener;
    void (*dispose)(void *);
    void *user;
    int prepend;
} entry;

struct ak_scope {
    ak_context *ctx;
    ak_scope *next, *parent;
    const ak_plugin *plugin;
    void *config;
    ak_state state;
    int mark;
    int started;
};
struct ak_context {
    ak_platform host;
    ak_scope root, *plugins, *starting;
    entry *entries;
    ak_handle counter;
    int busy;
    size_t dispatches, count;
};

static void *alloc_zero(ak_context *c, size_t n) {
    void *p = c->host.alloc(c->host.user, n);
    if (p) memset(p, 0, n);
    return p;
}
static void release(ak_context *c, void *p) { c->host.free(c->host.user, p); }
ak_context *ak_create(const ak_platform *host) {
    if (!host || !host->alloc || !host->free) return NULL;
    ak_context *c = host->alloc(host->user, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->host = *host;
    c->root.ctx = c;
    c->root.state = AK_ACTIVE;
    return c;
}
ak_scope *ak_root(ak_context *c) { return c ? &c->root : NULL; }
ak_context *ak_context_of(ak_scope *s) { return s ? s->ctx : NULL; }
ak_scope *ak_parent(ak_scope *s) { return s ? s->parent : NULL; }
const ak_platform *ak_host(ak_scope *s) { return s ? &s->ctx->host : NULL; }
ak_state ak_status(const ak_scope *s) { return s ? s->state : AK_REMOVED; }

static int key_eq(const char *a, unsigned av, const char *b, unsigned bv) {
    return av == bv && strcmp(a, b) == 0;
}
static int declares(const ak_plugin *p, const char *name, unsigned version) {
    for (const ak_key *k = p->provides; k && k->name; k++)
        if (key_eq(k->name, k->version, name, version)) return 1;
    return 0;
}
static entry *service(ak_context *c, const char *name, unsigned version) {
    for (entry *e = c->entries; e; e = e->next)
        if (e->type == SERVICE && key_eq(name, version, e->name, e->version)) return e;
    return NULL;
}
const void *ak_get(ak_scope *s, const char *name, unsigned version) {
    if (!s || !name) return NULL;
    entry *e = service(s->ctx, name, version);
    return e && (e->owner->state == AK_ACTIVE || e->owner == s) ? e->iface : NULL;
}
static int ready(ak_scope *s) {
    if (s->parent->state != AK_ACTIVE) return 0;
    for (const ak_dep *d = s->plugin->deps; d && d->name; d++)
        if (!d->optional && !ak_get(s, d->name, d->version)) return 0;
    return 1;
}
static entry *push(ak_scope *s, kind type) {
    if (!s || (s->state != AK_ACTIVE && s->state != AK_STARTING)) return NULL;
    ak_context *c = s->ctx;
    if (c->counter == UINT64_MAX) return NULL;
    entry *e = alloc_zero(c, sizeof(*e));
    if (!e) return NULL;
    e->id = ++c->counter;
    e->owner = s;
    e->type = type;
    e->next = c->entries;
    c->entries = e;
    return e;
}
static void clean_entry(ak_context *c, entry **slot) {
    entry *e = *slot;
    *slot = e->next;
    if (e->dispose) e->dispose(e->user);
    release(c, e);
}
static void clean_scope(ak_scope *s) {
    entry **slot = &s->ctx->entries;
    while (*slot) {
        if ((*slot)->owner == s) clean_entry(s->ctx, slot);
        else slot = &(*slot)->next;
    }
}

static void deactivate(ak_scope *);
static void remove_scope(ak_scope *);
static void stop_consumers(ak_context *c, const char *name, unsigned version) {
    for (ak_scope *s = c->plugins; s; s = s->next) {
        if (s->state != AK_ACTIVE) continue;
        for (const ak_dep *d = s->plugin->deps; d && d->name; d++) {
            if (!d->optional && key_eq(name, version, d->name, d->version)) {
                deactivate(s);
                break;
            }
        }
    }
}
static void deactivate(ak_scope *s) {
    s->state = AK_STOPPING;
    for (;;) {
        ak_scope *child = s->ctx->plugins;
        while (child && child->parent != s) child = child->next;
        if (!child) break;
        remove_scope(child);
    }
    for (const ak_key *k = s->plugin->provides; k && k->name; k++)
        stop_consumers(s->ctx, k->name, k->version);
    if (s->started && s->plugin->stop) s->plugin->stop(s, s->config);
    clean_scope(s);
    s->started = 0;
    s->state = AK_WAITING;
}
/* Registration from start() only becomes visible after start() succeeds. */
static void reconcile(ak_context *c) {
    if (c->busy || c->dispatches) return;
    c->busy = 1;
    int progress;
    do {
        progress = 0;
        for (ak_scope *s = c->plugins; s; s = s->next) {
            if (s->state != AK_WAITING || !ready(s)) continue;
            s->state = AK_STARTING;
            s->started = 1;
            c->starting = s;
            int failed = s->plugin->start && s->plugin->start(s, s->config);
            c->starting = NULL;
            if (failed) {
                deactivate(s);
                s->state = AK_FAILED;
            } else {
                s->state = AK_ACTIVE;
            }
            progress = 1;
        }
    } while (progress);
    c->busy = 0;
}
ak_handle ak_provide(ak_scope *s, const char *name, unsigned version, const void *iface) {
    if (!s || !name || !*name || !version || !iface || service(s->ctx, name, version)) return 0;
    if (s->plugin && !declares(s->plugin, name, version)) return 0;
    if (!s->plugin)
        for (ak_scope *it = s->ctx->plugins; it; it = it->next)
            if (declares(it->plugin, name, version)) return 0;
    entry *e = push(s, SERVICE);
    if (!e) return 0;
    e->name = name;
    e->version = version;
    e->iface = iface;
    ak_handle id = e->id;
    reconcile(s->ctx);
    return id;
}
ak_handle ak_effect(ak_scope *s, void (*dispose)(void *), void *user) {
    if (!dispose) return 0;
    entry *e = push(s, EFFECT);
    if (!e) return 0;
    e->dispose = dispose;
    e->user = user;
    return e->id;
}
ak_handle ak_on(ak_scope *s, const char *event, ak_listener f, void *user, int prepend) {
    if (!event || !*event || !f) return 0;
    entry *e = push(s, LISTENER);
    if (!e) return 0;
    e->name = event;
    e->listener = f;
    e->user = user;
    e->prepend = prepend;
    return e->id;
}
int ak_revoke(ak_context *c, ak_handle id) {
    if (!c || c->busy || !id) return 0;
    entry **slot = &c->entries;
    while (*slot && (*slot)->id != id) slot = &(*slot)->next;
    if (!*slot || (c->dispatches && (*slot)->type != LISTENER)) return 0;
    entry *e = *slot;
    c->busy = 1;
    if (e->type == SERVICE) {
        /* Detach before cascade; the cascade may free the previous list node. */
        *slot = e->next;
        stop_consumers(c, e->name, e->version);
        release(c, e);
    } else {
        clean_entry(c, slot);
    }
    c->busy = 0;
    reconcile(c);
    return 1;
}
static int cycle(ak_scope *s) {
    if (s->mark) return s->mark == 1;
    s->mark = 1;
    if (s->parent->plugin && cycle(s->parent)) return 1;
    for (const ak_dep *d = s->plugin->deps; d && d->name; d++) {
        if (d->optional) continue;
        for (ak_scope *p = s->ctx->plugins; p; p = p->next)
            if (declares(p->plugin, d->name, d->version) && cycle(p)) return 1;
    }
    s->mark = 2;
    return 0;
}
int ak_install_in(ak_scope *parent, const ak_plugin *p, void *config, ak_scope **out) {
    ak_context *c = parent ? parent->ctx : NULL;
    if (out) *out = NULL;
    if (!c || !p || p->abi != AK_ABI || !p->name || !*p->name) return AK_INVALID;
    if (parent->state != AK_ACTIVE && parent != c->starting) return AK_INVALID;
    if ((c->busy && parent != c->starting) || c->dispatches || c->count == 256) return AK_BUSY;
    for (const ak_dep *d = p->deps; d && d->name; d++)
        if (!d->version || !*d->name) return AK_INVALID;
    for (const ak_key *k = p->provides; k && k->name; k++) {
        if (!*k->name || !k->version) return AK_INVALID;
        if (service(c, k->name, k->version)) return AK_CONFLICT;
        for (ak_scope *s = c->plugins; s; s = s->next)
            if (declares(s->plugin, k->name, k->version)) return AK_CONFLICT;
    }
    for (ak_scope *s = c->plugins; s; s = s->next) {
        s->mark = 0;
        if (!strcmp(s->plugin->name, p->name)) return AK_CONFLICT;
    }
    ak_scope *s = alloc_zero(c, sizeof(*s));
    if (!s) return AK_NOMEM;
    s->ctx = c;
    s->plugin = p;
    s->parent = parent;
    s->config = config;
    s->next = c->plugins;
    c->plugins = s;
    if (cycle(s)) {
        c->plugins = s->next;
        release(c, s);
        return AK_CYCLE;
    }
    /* init validates instance configuration once; start owns all effects. */
    ak_scope *starting = c->starting;
    c->starting = NULL;
    int was_busy = c->busy;
    c->busy = 1;
    if (p->init && p->init(s, config)) {
        if (p->unload) p->unload(s, config);
        c->plugins = s->next;
        release(c, s);
        c->busy = was_busy;
        c->starting = starting;
        return AK_CALLBACK;
    }
    c->busy = was_busy;
    c->starting = starting;
    c->count++;
    reconcile(c);
    if (out) *out = s;
    return s->state == AK_FAILED ? AK_CALLBACK : AK_OK;
}
int ak_install(ak_context *c, const ak_plugin *p, void *config, ak_scope **out) {
    return ak_install_in(ak_root(c), p, config, out);
}
static void remove_scope(ak_scope *s) {
    ak_context *c = s->ctx;
    if (s->state == AK_ACTIVE) deactivate(s);
    s->state = AK_REMOVED;
    if (s->plugin->unload) s->plugin->unload(s, s->config);
    ak_scope **slot = &c->plugins;
    while (*slot != s) slot = &(*slot)->next;
    *slot = s->next;
    release(c, s);
    c->count--;
}
int ak_remove(ak_scope *s) {
    if (!s || !s->plugin) return AK_INVALID;
    ak_context *c = s->ctx;
    if (c->busy || c->dispatches) return AK_BUSY;
    c->busy = 1;
    remove_scope(s);
    c->busy = 0;
    reconcile(c);
    return AK_OK;
}
int ak_destroy(ak_context *c) {
    if (!c) return AK_OK;
    if (c->dispatches || c->busy) return AK_BUSY;
    c->busy = 1;
    c->root.state = AK_STOPPING;
    while (c->plugins) remove_scope(c->plugins);
    clean_scope(&c->root);
    release(c, c);
    return AK_OK;
}

typedef struct {
    ak_listener callback;
    void *user;
    ak_value result;
} call;
struct ak_next { ak_event *event; };
struct ak_event {
    ak_context *ctx;
    ak_mode mode;
    const void *payload;
    ak_listener inner;
    void *inner_user;
    size_t count, cursor;
    ak_next next;
    ak_result result;
    int done;
    call calls[];
};

static ak_value error(const char *message) {
    return (ak_value){.tag = AK_ERROR, .as.string = message};
}
static int bailed(ak_value v) {
    return v.tag != AK_UNDEFINED && v.tag != AK_NULL && v.tag != AK_FALSE;
}
/* A host future progresses independently when scheduled; poll may also
 * implement the listener's continuation once its asynchronous input arrives. */
static int resolve(ak_value *v) {
    /* A resolved future may adopt another already-settled future. Only an
     * actual pending poll requires the platform scheduler. Bound bad cycles. */
    for (unsigned depth = 0; depth < 256; depth++) {
        if (v->tag != AK_FUTURE) return 1;
        ak_value out = {0};
        ak_future f = v->as.future;
        int rc = f.poll ? f.poll(f.user, &out) : -1;
        if (!rc) return 0;
        if (rc < 0 && out.tag != AK_ERROR) out = error("future rejected");
        *v = out;
    }
    if (v->tag == AK_FUTURE) *v = error("future resolution depth exceeded");
    return 1;
}
ak_value ak_await(ak_context *c, ak_value v) {
    while (!resolve(&v)) {
        if (!c || !c->host.pump || c->host.pump(c->host.user) < 0)
            return error("scheduler unavailable");
    }
    return v;
}
ak_value ak_next_call(ak_next *next) {
    if (!next || !next->event) return error("missing next");
    ak_event *op = next->event;
    if (op->cursor == op->count) return op->inner(op->inner_user, op->payload, next);
    call *cb = &op->calls[op->cursor++];
    return cb->callback(cb->user, op->payload, next);
}
static void finish(ak_event *op, ak_value v) {
    op->done = 1;
    op->result.value = v;
    if (v.tag == AK_ERROR && !op->result.errors) op->result.errors = 1;
}
static int matching(entry *e, const char *event) {
    return e->type == LISTENER && e->owner->state == AK_ACTIVE && !strcmp(e->name, event);
}
static void launch(ak_event *op) {
    if (op->mode == AK_WATERFALL) {
        op->result.value = ak_next_call(&op->next);
        if (op->result.value.tag != AK_FUTURE) finish(op, op->result.value);
        return;
    }
    while (op->cursor < op->count) {
        call *cb = &op->calls[op->cursor++];
        cb->result = cb->callback(cb->user, op->payload, NULL);
        if (op->mode == AK_PARALLEL) continue;
        if (op->mode == AK_SERIAL) return; /* await before launching another */
        if (cb->result.tag == AK_ERROR || (op->mode == AK_BAIL && bailed(cb->result))) {
            finish(op, cb->result);
            return;
        }
    }
    if (op->mode != AK_PARALLEL) finish(op, (ak_value){0});
}
ak_event *ak_begin(ak_context *c, ak_mode mode, const char *event, const void *payload,
                   ak_listener inner, void *inner_user) {
    if (!c || !event || mode < AK_EMIT || mode > AK_WATERFALL ||
        (mode == AK_WATERFALL && !inner)) return NULL;
    size_t n = 0;
    for (entry *e = c->entries; e; e = e->next) n += matching(e, event);
    if (n > (SIZE_MAX - sizeof(ak_event)) / sizeof(call)) return NULL;
    ak_event *op = alloc_zero(c, sizeof(*op) + n * sizeof(call));
    if (!op) return NULL;
    op->ctx = c;
    op->mode = mode;
    op->count = n;
    op->payload = payload;
    op->next.event = op;
    op->inner = inner;
    op->inner_user = inner_user;
    size_t front = 0, back = n;
    for (entry *e = c->entries; e; e = e->next) {
        if (!matching(e, event)) continue;
        call *cb = &op->calls[e->prepend ? front++ : --back];
        cb->callback = e->listener;
        cb->user = e->user;
    }
    c->dispatches++;
    launch(op);
    return op;
}
int ak_poll(ak_event *op, ak_result *out) {
    if (!op || !out) return -1;
    if (!op->done) {
        if (op->mode == AK_WATERFALL && resolve(&op->result.value))
            finish(op, op->result.value);
        if (op->mode == AK_SERIAL) {
            ak_value *v = &op->calls[op->cursor - 1].result;
            if (resolve(v)) {
                if (bailed(*v)) finish(op, *v);
                else launch(op);
            }
        }
        if (op->mode == AK_PARALLEL) {
            size_t pending = 0, errors = 0;
            for (size_t i = 0; i < op->count; i++) {
                ak_value *v = &op->calls[i].result;
                if (!resolve(v)) pending++;
                else if (v->tag == AK_ERROR) errors++;
            }
            if (!pending) {
                op->result.errors = errors;
                finish(op, errors ? error("parallel listener failures") : (ak_value){0});
            }
        }
    }
    if (op->done) *out = op->result;
    return op->done;
}
ak_value ak_event_error(const ak_event *op, size_t index) {
    if (op && op->done && index < op->result.errors) {
        if (op->mode != AK_PARALLEL) return op->result.value;
        for (size_t i = 0; i < op->count; i++)
            if (op->calls[i].result.tag == AK_ERROR && index-- == 0) return op->calls[i].result;
    }
    return (ak_value){0};
}
void ak_event_release(ak_event *op) {
    if (!op) return;
    ak_context *c = op->ctx;
    c->dispatches--;
    release(c, op);
    reconcile(c);
}
ak_result ak_dispatch(ak_context *c, ak_mode mode, const char *event, const void *payload,
                      ak_listener inner, void *inner_user) {
    ak_event *op = ak_begin(c, mode, event, payload, inner, inner_user);
    ak_result result = {error("dispatch unavailable"), 1};
    if (!op) return result;
    for (;;) {
        size_t previous_cursor = op->cursor;
        if (ak_poll(op, &result)) break;
        /* Starting the next serial callback counts as progress: poll its
         * return value before deciding whether to block in the scheduler. */
        if (mode == AK_SERIAL && op->cursor != previous_cursor) continue;
        if (!c->host.pump || c->host.pump(c->host.user) < 0) {
            result.value = error("scheduler unavailable");
            result.errors = 1;
            break;
        }
    }
    ak_event_release(op);
    return result;
}
