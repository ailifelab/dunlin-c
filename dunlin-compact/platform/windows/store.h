#ifndef DUNLIN_WINDOWS_STORE_H
#define DUNLIN_WINDOWS_STORE_H

#include <stddef.h>

typedef struct dws_store dws_store;
typedef struct dws_lease dws_lease;

enum { DWS_OK, DWS_BUSY, DWS_MISSING, DWS_FAILED };

int dws_open(dws_store **out, const char *utf8_directory);
void dws_close(dws_store *store);
int dws_acquire(dws_store *store, const char *id, dws_lease **out);
void dws_release(dws_lease *lease);
int dws_read(dws_store *store, const char *id, void **data, size_t *size);
int dws_save(dws_lease *lease, const void *data, size_t size);
/* Owned, bytewise-sorted UTF-8 IDs. Empty inventory returns DWS_OK with NULL/0. */
int dws_list(dws_store *store, char ***ids, size_t *count);
void dws_list_free(char **ids, size_t count);

#endif
