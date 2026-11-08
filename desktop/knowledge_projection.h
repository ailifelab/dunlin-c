#ifndef DUNLIN_KNOWLEDGE_PROJECTION_H
#define DUNLIN_KNOWLEDGE_PROJECTION_H
#include "dunlin/desktop.h"
#include "dunlin/compact/runtime.h"
typedef struct {
 dunlin_context_projector_v1 service;
 const dunlin_context_projector_v1 *next;
 struct json_object *snapshot;
 const ah_limits *limits;
 const dc_hooks *hooks;
} dc_knowledge_projection;
const dunlin_context_projector_v1 *dc_knowledge_projector(dc_knowledge_projection *,struct json_object *,const dunlin_context_projector_v1 *,const ah_limits *,const dc_hooks *);
#endif
