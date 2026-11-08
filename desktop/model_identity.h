#ifndef DUNLIN_MODEL_IDENTITY_H
#define DUNLIN_MODEL_IDENTITY_H
/* Shared read projection and mutation guard. Queries alias chats as c and
 * chat_properties as m; the join introduces r. While active, admitted run
 * identity takes priority over a preferred-model UI setting. Legacy missing
 * identity remains NULL so mutation guards can fail conservatively. */
#define DC_ACTIVE_RUN_JOIN \
 " LEFT JOIN run_records r ON r.chat=c.id AND r.id=CASE WHEN m.active<>0 THEN CASE WHEN json_valid(c.state) THEN json_extract(c.state,'$._dunlin_run') END END "
#define DC_CHAT_MODEL_ID \
 "COALESCE(CASE WHEN m.active<>0 THEN CASE WHEN json_valid(r.snapshot) THEN CASE WHEN json_type(r.snapshot,'$._public_settings.desktop_profile_id')='text' THEN NULLIF(json_extract(r.snapshot,'$._public_settings.desktop_profile_id'),'') END END END,NULLIF(m.model,''))"
#endif
