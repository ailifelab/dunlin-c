#ifndef DUNLIN_KNOWLEDGE_H
#define DUNLIN_KNOWLEDGE_H
#include "desktop.h"
/* Trusted host binds a physical store once to its selected principal. The model
 * cannot choose owner identity. A mismatched claim fails without relabelling. */
int dc_bind_identity(dc_store *,const char *principal);
/* C graph semantics, source-linked assertions and local lexical RAG use the
 * same transaction domain as conversations. No cloud/embedding dependency. */
char *dc_knowledge_call(dc_store *,const char *request_json);
#endif
