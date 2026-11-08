#ifndef DUNLIN_SESSION_H
#define DUNLIN_SESSION_H
#include "agent.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Opens or creates private directory. Parent must exist. */
int ah_linux_sessions(ah_session_service *, const char *directory);
void ah_linux_sessions_close(ah_session_service *);
int ah_windows_sessions(ah_session_service *, const char *directory);
void ah_windows_sessions_close(ah_session_service *);
struct json_object *ah_session_list(const ah_session_service *); /* owned array of IDs */
/* Builtin store only: read the last committed atomic snapshot without acquiring
 * a writer lease or creating a lock file. On AH_DONE caller owns *state; NULL
 * means no committed snapshot. Same private-file/size checks as open(). */
ah_status ah_session_read_snapshot(const ah_session_service *,const char *id,struct json_object **state);
uint64_t ah_linux_now_ms(void *);
#ifdef __cplusplus
}
#endif
#endif
