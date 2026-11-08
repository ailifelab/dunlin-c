#ifndef DUNLIN_KERNEL_H
#define DUNLIN_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AK_ABI 1u

typedef struct ak_context ak_context;
typedef struct ak_scope ak_scope;
typedef uint64_t ak_handle;

typedef struct {
    void *user;
    void *(*alloc)(void *user, size_t size);
    void (*free)(void *user, void *ptr);
    /* Advance the host scheduler when an event listener has pending work. */
    int (*pump)(void *user);
    uint64_t (*now_ms)(void *user);
} ak_platform;

typedef struct { const char *name; unsigned version; } ak_key;
typedef struct { const char *name; unsigned version; int optional; } ak_dep;
typedef enum { AK_WAITING, AK_STARTING, AK_ACTIVE, AK_STOPPING, AK_FAILED, AK_REMOVED } ak_state;
typedef struct {
    unsigned abi;
    const char *name;
    const ak_key *provides; /* NULL or name-terminated array, one provider per key */
    const ak_dep *deps; /* NULL or name-terminated array */
    int (*init)(ak_scope *, void *);
    int (*start)(ak_scope *, void *);
    void (*stop)(ak_scope *, void *);
    void (*unload)(ak_scope *, void *);
} ak_plugin;

enum { AK_OK, AK_INVALID, AK_NOMEM, AK_CONFLICT, AK_CYCLE, AK_CALLBACK, AK_BUSY };

ak_context *ak_create(const ak_platform *);
int ak_destroy(ak_context *);
ak_scope *ak_root(ak_context *);
const ak_platform *ak_host(ak_scope *);
int ak_install(ak_context *, const ak_plugin *, void *config, ak_scope **out);
int ak_install_in(ak_scope *parent, const ak_plugin *, void *config, ak_scope **out);
ak_context *ak_context_of(ak_scope *);
ak_scope *ak_parent(ak_scope *);
int ak_remove(ak_scope *);
ak_state ak_status(const ak_scope *);
ak_handle ak_provide(ak_scope *, const char *name, unsigned version, const void *iface);
const void *ak_get(ak_scope *, const char *name, unsigned version);
ak_handle ak_effect(ak_scope *, void (*dispose)(void *), void *user);
int ak_revoke(ak_context *, ak_handle); /* 1 once, otherwise 0 */

/* An async listener starts its work before returning a future. poll returns
 * 0 while pending, 1 with the final value, -1 on failure. Futures, payload,
 * names, and callback data are borrowed; see docs/kernel.md for lifetimes. */
typedef struct ak_value ak_value;
typedef struct {
    int (*poll)(void *user, ak_value *out);
    void *user;
} ak_future;
typedef enum { AK_UNDEFINED, AK_NULL, AK_FALSE, AK_TRUE, AK_INT, AK_STRING,
               AK_FUTURE, AK_ERROR } ak_tag;
struct ak_value {
    ak_tag tag;
    union { int64_t integer; const char *string; ak_future future; } as;
};
typedef struct ak_next ak_next;
typedef ak_value (*ak_listener)(void *user, const void *payload, ak_next *next);
typedef enum { AK_EMIT, AK_SERIAL, AK_PARALLEL, AK_BAIL, AK_WATERFALL } ak_mode;
typedef struct { ak_value value; size_t errors; } ak_result;
ak_handle ak_on(ak_scope *, const char *event, ak_listener, void *user, int prepend);
ak_result ak_dispatch(ak_context *, ak_mode, const char *event, const void *payload,
                      ak_listener inner, void *inner_user);
typedef struct ak_event ak_event;
/* Nonblocking entry point. poll returns 1 when final, 0 when still running.
 * Release before destroying context; release on a pending event abandons its
 * borrowed futures. Listeners using next may resume only inside poll(). */
ak_event *ak_begin(ak_context *, ak_mode, const char *, const void *,
                   ak_listener inner, void *inner_user);
int ak_poll(ak_event *, ak_result *);
void ak_event_release(ak_event *);
ak_value ak_event_error(const ak_event *, size_t index);
ak_value ak_next_call(ak_next *);
ak_value ak_await(ak_context *, ak_value);

#ifdef __cplusplus
}
#endif
#endif
