#define WIN32_LEAN_AND_MEAN
#include "dunlin/compact/session.h"
#include <windows.h>
#include <json-c/json.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char temp[MAX_PATH],dir[MAX_PATH];assert(GetTempPathA(MAX_PATH,temp));
    snprintf(dir,sizeof dir,"%sdunlin-session-%lu",temp,(unsigned long)GetCurrentProcessId());
    ah_session_service service={0};assert(!ah_windows_sessions(&service,dir));
    void *first=NULL,*second=NULL;struct json_object *state=(struct json_object *)1;
    assert(service.open(service.ctx,"alpha",&first,&state)==AH_DONE&&first&&!state);
    assert(service.open(service.ctx,"alpha",&second,&state)==AH_BUSY&&!second&&!state);
    state=json_tokener_parse("{\"version\":2,\"active\":false,\"messages\":[{\"role\":\"user\",\"content\":\"你好\"}]}");
    assert(state&&service.save(first,state)==0);json_object_put(state);service.close(first);
    assert(service.open(service.ctx,"alpha",&first,&state)==AH_DONE&&state);
    struct json_object *version=NULL;assert(json_object_object_get_ex(state,"version",&version));
    assert(json_object_get_int(version)==2);json_object_put(state);service.close(first);
    assert(ah_session_read_snapshot(&service,"alpha",&state)==AH_DONE&&state);json_object_put(state);
    struct json_object *list=ah_session_list(&service);assert(list&&json_object_array_length(list)==1);
    assert(!strcmp(json_object_get_string(json_object_array_get_idx(list,0)),"alpha"));json_object_put(list);
    assert(service.open(service.ctx,"../bad",&first,&state)==AH_FAILED);
    ah_windows_sessions_close(&service);
    char path[MAX_PATH];snprintf(path,sizeof path,"%s\\alpha.json",dir);assert(DeleteFileA(path));
    snprintf(path,sizeof path,"%s\\alpha.lock",dir);assert(DeleteFileA(path));assert(RemoveDirectoryA(dir));
    puts("Windows session adapter tests passed");return 0;
}
