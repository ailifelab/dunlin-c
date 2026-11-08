#include "dunlin/compact/session.h"
#include "dunlin/compact/json.h"
#include "store.h"
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

typedef struct { dws_store *bytes; } windows_store;
typedef struct { dws_lease *bytes; } windows_lease;

static int valid_id(const char *id) {
    return id&&*id&&strlen(id)<=64&&
        id[strspn(id,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")]==0;
}
static ah_status decode(dws_store *store,const char *id,struct json_object **state) {
    void *data=NULL;size_t size=0;*state=NULL;
    int rc=dws_read(store,id,&data,&size);
    if(rc==DWS_MISSING)return AH_DONE;
    if(rc!=DWS_OK)return AH_FAILED;
    *state=dunlin_json_parse(data,size);free(data);return *state?AH_DONE:AH_FAILED;
}
static ah_status begin(void *context,const char *id,void **lease,struct json_object **state) {
    if(lease)*lease=NULL;if(state)*state=NULL;
    if(!context||!lease||!state||!valid_id(id))return AH_FAILED;
    windows_store *store=context;dws_lease *bytes=NULL;
    int rc=dws_acquire(store->bytes,id,&bytes);
    if(rc==DWS_BUSY)return AH_BUSY;
    if(rc!=DWS_OK)return AH_FAILED;
    windows_lease *opened=malloc(sizeof *opened);
    if(!opened){dws_release(bytes);return AH_FAILED;}
    opened->bytes=bytes;
    ah_status status=decode(store->bytes,id,state);
    if(status!=AH_DONE){dws_release(bytes);free(opened);return status;}
    *lease=opened;return AH_DONE;
}
static int save(void *opaque,struct json_object *state) {
    windows_lease *lease=opaque;
    if(!lease||!state)return -1;
    const char *json=json_object_to_json_string_ext(state,JSON_C_TO_STRING_PLAIN);
    return json&&dws_save(lease->bytes,json,strlen(json))==DWS_OK?0:-1;
}
static void end(void *opaque) {
    windows_lease *lease=opaque;
    if(lease){dws_release(lease->bytes);free(lease);}
}
int ah_windows_sessions(ah_session_service *out,const char *directory) {
    if(out)*out=(ah_session_service){0};
    if(!out||!directory)return -1;
    windows_store *store=calloc(1,sizeof *store);
    if(!store||dws_open(&store->bytes,directory)!=DWS_OK){free(store);return -1;}
    *out=(ah_session_service){store,begin,save,end};return 0;
}
void ah_windows_sessions_close(ah_session_service *out) {
    if(out&&out->ctx){windows_store *store=out->ctx;dws_close(store->bytes);free(store);*out=(ah_session_service){0};}
}
ah_status ah_session_read_snapshot(const ah_session_service *service,const char *id,struct json_object **state) {
    if(state)*state=NULL;
    if(!service||!service->ctx||!state||!valid_id(id))return AH_FAILED;
    return decode(((windows_store *)service->ctx)->bytes,id,state);
}
struct json_object *ah_session_list(const ah_session_service *service) {
    if(!service||!service->ctx)return NULL;
    char **ids=NULL;size_t count=0;
    if(dws_list(((windows_store *)service->ctx)->bytes,&ids,&count)!=DWS_OK)return NULL;
    struct json_object *list=json_object_new_array();
    for(size_t i=0;list&&i<count;i++) {
        struct json_object *id=json_object_new_string(ids[i]);
        if(!id||json_object_array_add(list,id)){json_object_put(id);json_object_put(list);list=NULL;}
    }
    dws_list_free(ids,count);return list;
}
