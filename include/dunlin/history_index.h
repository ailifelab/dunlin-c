#ifndef DUNLIN_HISTORY_INDEX_H
#define DUNLIN_HISTORY_INDEX_H
#include <stddef.h>
typedef struct dl_index dl_index;
/* Serialized by host; desktop schedules work on a background queue. No network,
 * model, UI or runtime dependency. Returned strings released with dl_index_free. */
/* Owned normalized retrieval shadow; not a semantic summary. */
char *dl_index_terms(const char *text,int query);
dl_index *dl_index_open(const char *path);
void dl_index_close(dl_index *);
int dl_index_put(dl_index *,const char *scope,const char *chat,int sequence,const char *text);
int dl_index_delete(dl_index *,const char *scope,const char *chat);
char *dl_index_search(dl_index *,const char *scope,const char *query);
char *dl_index_search_page(dl_index *,const char *scope,const char *query,int offset);
void dl_index_free(void *);
/* Main database allocation only, not whole application/journal peak space. */
size_t dl_index_bytes(dl_index *);
long long dl_index_count(dl_index *);
int dl_index_limit_bytes(dl_index *,size_t);
int dl_index_last_error(dl_index *); /* SQLite result code, no text or secrets. */
#endif
