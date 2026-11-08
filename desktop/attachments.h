#ifndef DUNLIN_DESKTOP_ATTACHMENTS_H
#define DUNLIN_DESKTOP_ATTACHMENTS_H
#include "internal.h"
#include "dunlin/compact/runtime.h"
/* Host-only owner authorization and optional format upgrade. No schema or
 * source mutation on read-only connections. Source writes are serialized. */
int dc_attachments_initialize(dc_store *,int create);
struct json_object *dc_attachments_request(dc_store *,struct json_object *);
struct json_object *dc_attachment_view(const dl_attachment_draft *);
/* Canonical message manifest; source hashes are over the full message JSON,
 * separate from the existing display-text hash used by lexical source jumps. */
int dc_attachment_message_hash(struct json_object *,char hash[65]);
struct json_object *dc_attachment_message_view(dc_store *,const char *,int64_t,struct json_object *);
int dc_attachment_manifest_matches(struct json_object *,const dl_attachment_draft *);
/* Changes no source. NULL expected requires no pending draft; admission is
 * exact even for repeated identical bytes. Resume does not consume drafts. */
int dc_attachment_admit(dc_store *,const char *chat,const char *expected,
                      struct json_object *settings,dl_attachment_draft *,char *,size_t);
int dc_attachment_model_images(struct json_object *settings);
/* 1=image requests, 0=explicit text-only projection, -1=invalid storage. */
int dc_attachment_policy(dc_store *,const char *chat);
int dc_attachment_set_policy(dc_store *,const char *chat,int images);
#endif
