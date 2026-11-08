#ifndef DUNLIN_INTERNAL_H
#define DUNLIN_INTERNAL_H
#include "dunlin/compact/runtime.h"
#include "dunlin/compact/protocol.h"
#include <json-c/json.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
typedef struct {
    dunlin_model_config model;
    unsigned max_tokens, reasoning_supported;
    dunlin_protocol protocol;
    dunlin_reasoning_level effort;
    int chat_metadata_complete;
    struct json_object *omlx_chat_template, *enable_thinking, *temperature;
} dunlin_profile;
typedef struct {
    struct json_object *document;
    struct json_object *plugins, *enabled_tools, *models;
    dunlin_profile base, active;
    const char *active_id;
    int show_reasoning;
    ah_limits limits;
    const char *system_prompt, *tool_result_prompt, *tool_result_for; /* borrowed from document */
    char workspace[PATH_MAX], sessions[PATH_MAX];
} dunlin_config;
int dunlin_config_read(dunlin_config *,const char *,char *,size_t);
int dunlin_profiles_read(dunlin_config *,char *,size_t);
int dunlin_profile_select(dunlin_config *,const char *,char *,size_t);
int dunlin_profile_usable(const dunlin_profile *,char *,size_t);
int dunlin_profile_effort(dunlin_profile *,const char *,char *,size_t);
const char *dunlin_profile_protocol_name(dunlin_protocol);
const char *dunlin_profile_effort_name(dunlin_reasoning_level);
#endif
