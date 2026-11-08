#ifndef DUNLIN_INTENT_INTERNAL_H
#define DUNLIN_INTENT_INTERNAL_H
#include <json-c/json.h>
/* Owned request view, or NULL when there is no unambiguous installation follow-up. */
struct json_object *dunlin_install_followup(struct json_object *history);
/* Reject an immediate repeat of a successful step in a resolved installation task. */
int dunlin_install_repeat(struct json_object *history,struct json_object *answer);
int dunlin_install_needs_verification(struct json_object *history);
#endif
