#include "../dunlin-compact/runtime/internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *text="{"
        "\"base_url\":\"http://127.0.0.1:8080/v1\",\"model\":\"base\","
        "\"models\":{"
          "\"local\":{\"protocol\":\"chat\",\"model\":\"local-model\",\"reasoning_capabilities\":[\"none\",\"levels\"],\"reasoning_effort\":\"low\"},"
          "\"cloud\":{\"protocol\":\"anthropic\",\"base_url\":\"https://api.anthropic.com/v1\",\"model\":\"claude-fixture\",\"key_env\":\"ANTHROPIC_API_KEY\",\"max_tokens\":1024}"
        "},\"active_model\":\"local\"}";
    dunlin_config config={0};config.document=json_tokener_parse(text);assert(config.document);
    config.base.model.timeout_ms=30000;config.base.model.max_response_bytes=1024*1024;config.base.model.stream=1;
    char error[192];assert(dunlin_profiles_read(&config,error,sizeof error));
    assert(!strcmp(config.active_id,"local")&&config.active.protocol==DUNLIN_PROTOCOL_CHAT);
    assert(!strcmp(config.active.model.model,"local-model")&&config.active.effort==DUNLIN_REASONING_LOW);
    assert(dunlin_profile_select(&config,"cloud",error,sizeof error));
    assert(config.active.protocol==DUNLIN_PROTOCOL_ANTHROPIC&&config.active.max_tokens==1024);
    assert(!strcmp(config.active.model.key_env,"ANTHROPIC_API_KEY"));
    assert(!dunlin_profile_effort(&config.active,"high",error,sizeof error));
    assert(dunlin_profile_select(&config,"local",error,sizeof error));
    assert(dunlin_profile_effort(&config.active,"none",error,sizeof error));
    assert(!dunlin_profile_select(&config,"missing",error,sizeof error));
    json_object_put(config.document);
    puts("Windows multi-model profile contracts passed");return 0;
}
