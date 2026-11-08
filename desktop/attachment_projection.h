#ifndef DUNLIN_ATTACHMENT_PROJECTION_H
#define DUNLIN_ATTACHMENT_PROJECTION_H
#include "attachments.h"
typedef struct {
    dunlin_context_projector_v1 service;
    const dunlin_context_projector_v1 *next;
    dc_store *store;
    const char *chat;
    struct json_object **canonical;
    const ah_limits *limits;
    const dc_hooks *hooks;
    int images;
} dc_attachment_projection;
const dunlin_context_projector_v1 *dc_attachment_projector(dc_attachment_projection *,
    dc_store *,const char *,struct json_object **canonical,int images,
    const dunlin_context_projector_v1 *,const ah_limits *,const dc_hooks *);
#endif
