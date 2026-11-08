#include <dunlin/compact/kernel.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* The counters include all kernel storage. Force each allocation to fail in
 * turn and require all previously allocated storage to be released. */
typedef struct { size_t live, tries, fail_at; } memory;
static void *reserve(void *u, size_t size) {
    memory *m = u;
    if (++m->tries == m->fail_at) return NULL;
    void *p = malloc(size);
    if (p) m->live++;
    return p;
}
static void discard(void *u, void *p) {
    memory *m = u;
    assert(m->live && p);
    m->live--;
    free(p);
}
static ak_context *context(memory *m) {
    ak_platform host = {.alloc = reserve, .free = discard, .user = m};
    return ak_create(&host);
}
static ak_value num(int n) { return (ak_value){.tag = AK_INT, .as.integer = n}; }
typedef struct { const char *begin, *end; ak_value value; int ready; ak_next *next; } gate;
static int settled(void *u, ak_value *out) {
    gate *g = u;
    if (!g->ready) return 0;
    if (g->next) {
        *out = ak_next_call(g->next);
        assert(out->tag == AK_INT);
        out->as.integer *= 2;
    } else {
        *out = g->value;
    }
    if (g->end) puts(g->end);
    return 1;
}
static ak_value gated(void *u, const void *arg, ak_next *next) {
    (void)arg;
    gate *g = u;
    g->next = next;
    if (g->begin) puts(g->begin);
    return (ak_value){.tag = AK_FUTURE, .as.future = {settled, g}};
}
static ak_value immediate(void *u, const void *arg, ak_next *unused) {
    (void)arg;
    (void)unused;
    gate *g = u;
    if (g->begin) puts(g->begin);
    return g->value;
}
static ak_value unexpected(void *u, const void *arg, ak_next *next) {
    (void)u; (void)arg; (void)next;
    abort();
}
static ak_value add_three(void *u, const void *arg, ak_next *next) {
    (void)u; (void)arg;
    puts("water:in2");
    ak_value v = ak_next_call(next);
    assert(v.tag == AK_INT);
    puts("water:out2");
    return num((int)v.as.integer + 3);
}
static ak_value twice(void *u, const void *arg, ak_next *next) {
    (void)u; (void)arg;
    ak_value a = ak_next_call(next);
    ak_value b = ak_next_call(next);
    assert(a.tag == AK_INT && b.tag == AK_INT);
    return num((int)(a.as.integer + b.as.integer));
}
static ak_result drain(ak_event *op) {
    ak_result result;
    for (unsigned n = 0; n < 16; n++) {
        if (ak_poll(op, &result)) return result;
    }
    abort();
}
static void event_trace(ak_context *c) {
    ak_scope *s = ak_root(c);
    ak_result r;
    gate ea = {.begin = "emit:a-start", .end = "emit:a-end"};
    gate eb = {.begin = "emit:b"};
    assert(ak_on(s, "em", gated, &ea, 0));
    assert(ak_on(s, "em", immediate, &eb, 0));
    r = ak_dispatch(c, AK_EMIT, "em", NULL, NULL, NULL);
    assert(!r.errors && r.value.tag == AK_UNDEFINED);
    puts("emit:return");
    ea.ready = 1;
    assert(settled(&ea, &r.value));

    gate sa = {.begin = "serial:a-start", .end = "serial:a-end", .value = {.tag = AK_FALSE}};
    gate sb = {.begin = "serial:b", .value = num(0)};
    assert(ak_on(s, "sr", gated, &sa, 0));
    assert(ak_on(s, "sr", immediate, &sb, 0));
    assert(ak_on(s, "sr", unexpected, NULL, 0));
    ak_event *op = ak_begin(c, AK_SERIAL, "sr", NULL, NULL, NULL);
    assert(op && ak_poll(op, &r) == 0);
    puts("serial:pending");
    sa.ready = 1;
    r = drain(op);
    assert(r.value.tag == AK_INT && !r.errors);
    printf("serial:result=%" PRId64 "\n", r.value.as.integer);
    ak_event_release(op);

    gate p1 = {.begin = "parallel:1-start", .end = "parallel:1-end",
               .value = {.tag = AK_ERROR, .as.string = "first"}};
    gate p2 = {.begin = "parallel:2-start", .end = "parallel:2-end",
               .value = {.tag = AK_ERROR, .as.string = "second"}};
    assert(ak_on(s, "pr", gated, &p1, 0));
    assert(ak_on(s, "pr", gated, &p2, 0));
    op = ak_begin(c, AK_PARALLEL, "pr", NULL, NULL, NULL);
    assert(op && !ak_poll(op, &r));
    puts("parallel:pending");
    p2.ready = 1;
    assert(!ak_poll(op, &r));
    p1.ready = 1;
    r = drain(op);
    assert(r.errors == 2 && r.value.tag == AK_ERROR);
    printf("parallel:errors=%s,%s\n", ak_event_error(op, 0).as.string, ak_event_error(op, 1).as.string);
    ak_event_release(op);

    gate bail[] = {
        {.begin = "bail:null", .value = {.tag = AK_NULL}},
        {.begin = "bail:undefined"},
        {.begin = "bail:false", .value = {.tag = AK_FALSE}},
        {.begin = "bail:empty", .value = {.tag = AK_STRING, .as.string = ""}}
    };
    for (size_t i = 0; i < 4; i++) assert(ak_on(s, "ba", immediate, &bail[i], 0));
    assert(ak_on(s, "ba", unexpected, NULL, 0));
    r = ak_dispatch(c, AK_BAIL, "ba", NULL, NULL, NULL);
    assert(r.value.tag == AK_STRING && !r.errors);
    printf("bail:result=\"%s\"\n", r.value.as.string);
    gate promise = {.value = {.tag = AK_FALSE}, .ready = 1};
    assert(ak_on(s, "promise", gated, &promise, 0));
    assert(ak_on(s, "promise", unexpected, NULL, 0));
    r = ak_dispatch(c, AK_BAIL, "promise", NULL, NULL, NULL);
    assert(r.value.tag == AK_FUTURE && ak_await(c, r.value).tag == AK_FALSE);
    puts("bail:future=false");

    gate outer = {.begin = "water:in1", .end = "water:out1"};
    gate inner = {.begin = "water:inner", .value = num(4)};
    assert(ak_on(s, "wf", add_three, NULL, 0));
    assert(ak_on(s, "wf", gated, &outer, 1));
    op = ak_begin(c, AK_WATERFALL, "wf", NULL, immediate, &inner);
    assert(op && !ak_poll(op, &r));
    puts("water:pending");
    outer.ready = 1;
    r = drain(op);
    assert(r.value.tag == AK_INT);
    printf("water:result=%" PRId64 "\n", r.value.as.integer);
    ak_event_release(op);
    gate blocked = {.value = {.tag = AK_STRING, .as.string = "blocked"}};
    assert(ak_on(s, "veto", immediate, &blocked, 0));
    r = ak_dispatch(c, AK_WATERFALL, "veto", NULL, unexpected, NULL);
    assert(r.value.tag == AK_STRING);
    printf("water:veto=%s\n", r.value.as.string);
    gate seven = {.value = num(7)}, two = {.value = num(2)};
    assert(ak_on(s, "twice", twice, NULL, 0));
    assert(ak_on(s, "twice", immediate, &seven, 0));
    r = ak_dispatch(c, AK_WATERFALL, "twice", NULL, immediate, &two);
    assert(r.value.tag == AK_INT);
    printf("water:twice=%" PRId64 "\n", r.value.as.integer);
}
static void mark(void *u) { puts(u); }
static int start_consumer(ak_scope *s, void *u) {
    assert(ak_get(s, "dep", 1) && !ak_get(s, "dep", 2));
    puts("consumer:start");
    assert(ak_effect(s, mark, "consumer:clean1"));
    assert(ak_effect(s, mark, "consumer:clean2"));
    assert(ak_on(s, "owned", immediate, u, 0));
    return 0;
}
static int start_provider(ak_scope *s, void *u) {
    puts(u);
    return !ak_provide(s, "dep", 1, u);
}
static void lifecycle_trace(ak_context *c) {
    ak_dep deps[] = {{"dep", 1, 0}, {"missing", 1, 1}, {0}};
    ak_key provides[] = {{"dep", 1}, {0}};
    ak_plugin consumer = {.abi=AK_ABI, .name="consumer", .deps=deps, .start=start_consumer};
    ak_plugin provider = {.abi=AK_ABI, .name="provider", .provides=provides, .start=start_provider};
    gate hook = {.begin="consumer:hook"};
    ak_scope *a, *b;
    assert(!ak_install(c, &consumer, &hook, &a) && ak_status(a) == AK_WAITING);
    puts("consumer:waiting");
    assert(!ak_install(c, &provider, "provider:start", &b) && ak_status(a) == AK_ACTIVE);
    ak_dispatch(c, AK_EMIT, "owned", NULL, NULL, NULL);
    assert(!ak_remove(b) && ak_status(a) == AK_WAITING);
    ak_dispatch(c, AK_EMIT, "owned", NULL, NULL, NULL);
    puts("consumer:waiting-again");
    assert(!ak_install(c, &provider, "provider:again", &b));
    ak_dispatch(c, AK_EMIT, "owned", NULL, NULL, NULL);
    assert(!ak_remove(a));
    ak_dispatch(c, AK_EMIT, "owned", NULL, NULL, NULL);
    assert(!ak_remove(b));
    puts("consumer:gone");
}

typedef struct { unsigned init, start, stop, unload, dispose; int fail_init, fail_start; } counts;
static int init(ak_scope *s, void *u) {
    (void)s; counts *n = u; n->init++; return n->fail_init;
}
static void disposed(void *u) { ((counts *)u)->dispose++; }
static int start(ak_scope *s, void *u) {
    counts *n = u;
    n->start++;
    if (!ak_effect(s, disposed, u) || !ak_provide(s, "service", 1, u)) return 1;
    return n->fail_start;
}
static void stop(ak_scope *s, void *u) { (void)s; ((counts *)u)->stop++; }
static void unload(ak_scope *s, void *u) { (void)s; ((counts *)u)->unload++; }

typedef struct {
    counts child_calls;
    ak_scope *child;
    int fail;
} family;
static int start_parent(ak_scope *s, void *u) {
    family *f = u;
    static const ak_key keys[] = {{"service", 1}, {0}};
    static const ak_plugin child = {
        .abi=AK_ABI, .name="child", .provides=keys, .init=init,
        .start=start, .stop=stop, .unload=unload
    };
    assert(ak_install_in(s, &child, &f->child_calls, &f->child) == AK_OK);
    assert(ak_parent(f->child) == s && ak_status(f->child) == AK_WAITING);
    return f->fail;
}
static void tree(void) {
    memory m = {0};
    ak_context *c = context(&m);
    family f = {0};
    ak_dep deps[] = {{"power", 1, 0}, {0}};
    ak_plugin parent = {.abi=AK_ABI, .name="parent", .start=start_parent, .deps=deps};
    ak_scope *s;
    assert(ak_install(c, &parent, &f, &s) == AK_OK);
    ak_handle power = ak_provide(ak_root(c), "power", 1, &f);
    assert(power && ak_status(f.child) == AK_ACTIVE);
    assert(f.child_calls.start == 1 && !f.child_calls.stop);
    ak_event *op = ak_begin(c, AK_EMIT, "empty", NULL, NULL, NULL);
    assert(op && ak_remove(s) == AK_BUSY && ak_destroy(c) == AK_BUSY);
    ak_event_release(op);
    assert(ak_revoke(c, power) && ak_status(s) == AK_WAITING);
    assert(f.child_calls.stop == 1 && f.child_calls.dispose == 1 && f.child_calls.unload == 1);
    assert(!ak_get(ak_root(c), "service", 1));
    f.fail = 1;
    assert(ak_provide(ak_root(c), "power", 1, &f));
    assert(ak_status(s) == AK_FAILED);
    assert(f.child_calls.start == 1 && f.child_calls.unload == 2);
    assert(ak_destroy(c) == AK_OK && !m.live);
}

static void serial_ready(void) {
    memory m = {0};
    ak_context *c = context(&m); /* no pump */
    gate a = {.ready = 1, .value = {.tag = AK_FALSE}};
    gate b = {.ready = 1, .value = num(7)};
    gate nested = {.ready = 1, .value = {.tag=AK_FUTURE, .as.future={settled, &b}}};
    assert(ak_on(ak_root(c), "ready", gated, &a, 0));
    assert(ak_on(ak_root(c), "ready", gated, &nested, 0));
    ak_result r = ak_dispatch(c, AK_SERIAL, "ready", NULL, NULL, NULL);
    assert(!r.errors && r.value.tag == AK_INT && r.value.as.integer == 7);
    b.ready = 0;
    r = ak_dispatch(c, AK_SERIAL, "ready", NULL, NULL, NULL);
    assert(r.errors && r.value.tag == AK_ERROR);
    assert(ak_destroy(c) == AK_OK && !m.live);
}
static void handoff(ak_scope *closing, void *user) {
    /* A host service that becomes available as part of a plugin handoff. */
    assert(ak_provide(ak_parent(closing), "handoff", 1, user));
}
static void remove_handoff(void) {
    memory m = {0};
    ak_context *c = context(&m);
    ak_dep dep[] = {{"handoff", 1, 0}, {0}};
    ak_plugin producer = {.abi=AK_ABI, .name="producer", .unload=handoff};
    ak_plugin consumer = {.abi=AK_ABI, .name="consumer", .deps=dep};
    ak_scope *a, *b;
    assert(!ak_install(c, &producer, c, &a));
    assert(!ak_install(c, &consumer, NULL, &b));
    assert(ak_status(b) == AK_WAITING);
    assert(!ak_remove(a) && ak_status(b) == AK_ACTIVE);
    assert(ak_destroy(c) == AK_OK && !m.live);
}

static void unit(void) {
    serial_ready();
    remove_handoff();
    tree();
    ak_key keys[] = {{"service", 1}, {0}};
    ak_dep deps[] = {{"input", 1, 0}, {"extra", 2, 1}, {0}};
    ak_plugin p = {AK_ABI, "test", keys, deps, init, start, stop, unload};
    for (size_t i = 0; i < 12; i++) {
        memory m = {.fail_at=i};
        ak_context *c = context(&m);
        if (!c) { assert(!m.live); continue; }
        counts n = {0};
        ak_handle in = ak_provide(ak_root(c), "input", 1, &n);
        ak_scope *s;
        int rc = ak_install(c, &p, &n, &s);
        if (rc == AK_OK && in) {
            assert(ak_status(s) == AK_ACTIVE);
            assert(!ak_provide(s, "service", 1, &n)); /* duplicate */
            assert(!ak_provide(s, "other", 1, &n));   /* undeclared */
            assert(ak_revoke(c, in) && !ak_revoke(c, in));
            assert(ak_status(s) == AK_WAITING && n.dispose == 1);
            assert(!ak_get(ak_root(c), "service", 1));
        } else if (rc == AK_CALLBACK)
            assert(ak_status(s) == AK_FAILED && !ak_get(ak_root(c), "service", 1));
        ak_destroy(c);
        assert(!m.live);
    }
    memory m = {0};
    ak_context *c = context(&m);
    counts n = {.fail_init=1};
    assert(ak_provide(ak_root(c), "input", 1, &n));
    ak_scope *s;
    assert(ak_install(c, &p, &n, &s) == AK_CALLBACK && !s);
    assert(n.init == 1 && !n.start && n.unload == 1);
    n.fail_init = 0;
    n.fail_start = 1;
    assert(ak_install(c, &p, &n, &s) == AK_CALLBACK && s);
    assert(n.start == 1 && n.stop == 1 && n.dispose == 1 && !ak_get(ak_root(c), "service", 1));
    assert(ak_remove(s) == AK_OK && n.unload == 2);
    ak_dep dx[] = {{"y",1,0},{0}}, dy[] = {{"x",1,0},{0}};
    ak_key kx[] = {{"x",1},{0}}, ky[] = {{"y",1},{0}};
    ak_plugin x = {.abi=1,.name="X",.deps=dx,.provides=kx}, y = {.abi=1,.name="Y",.deps=dy,.provides=ky};
    assert(ak_install(c, &x, NULL, &s) == AK_OK && ak_status(s) == AK_WAITING);
    assert(ak_install(c, &y, NULL, NULL) == AK_CYCLE);
    assert(ak_install(c, &x, NULL, NULL) == AK_CONFLICT);
    assert(!ak_provide(ak_root(c), "x", 1, &n));
    p.abi++;
    assert(ak_install(c, &p, NULL, NULL) == AK_INVALID);
    assert(!ak_remove(s));
    ak_destroy(c);
    assert(!m.live);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--trace")) {
#ifdef _WIN32
        /* Preserve the fixed LF oracle instead of CRT text-mode CRLF output. */
        assert(_setmode(_fileno(stdout), _O_BINARY) != -1);
#endif
        memory m = {0};
        ak_context *c = context(&m);
        event_trace(c);
        lifecycle_trace(c);
        ak_destroy(c);
        assert(!m.live);
    } else {
        unit();
        puts("kernel unit tests passed");
    }
    return 0;
}
