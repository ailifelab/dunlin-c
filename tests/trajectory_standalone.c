#define _XOPEN_SOURCE 700
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "dunlin/trajectory.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef struct json_object J;
static J *field(J *o,const char *key){J *v=NULL;assert(json_object_object_get_ex(o,key,&v));return v;}
static J *request(dl_trajectory *t,const char *text){J *r=json_tokener_parse(text);assert(r);J *out=dl_trajectory_request(t,r);json_object_put(r);assert(out);assert(json_object_get_boolean(field(out,"ok")));return out;}
int main(void){
 char directory[]="/tmp/dunlin-trajectory-standalone-XXXXXX";assert(mkdtemp(directory));
 dl_trajectory *t=dl_trajectory_open(directory);assert(t);assert(!dl_trajectory_open(directory));
 const char secret[]="owned-test-key";
 dl_trace_span *span=dl_trajectory_begin(t,"chat","run","fixture","chat",0,secret);assert(span);
 const char *input="{\"messages\":[{\"role\":\"user\",\"content\":\"owned-test-key\"}],\"_dunlin_provider\":{\"encrypted_content\":\"private-opaque\"}}";
 dl_trajectory_event(span,"model.request",input,strlen(input));
 dl_trajectory *reader=dl_trajectory_open_reader(directory);assert(reader);
 J *out=request(reader,"{\"op\":\"trace.segments\",\"chat\":\"chat\"}");
 J *segment=json_object_array_get_idx(field(field(out,"data"),"items"),0);assert(segment);assert(!strcmp(json_object_get_string(field(segment,"status")),"recording"));
 char id[33];snprintf(id,sizeof id,"%s",json_object_get_string(field(segment,"id")));json_object_put(out);dl_trajectory_close(reader);
 dl_trajectory_event(span,"model.request.end","{\"accepted\":true}",17);dl_trajectory_finish(span,"completed",2);
 char query[256];snprintf(query,sizeof query,"{\"op\":\"trace.export\",\"chat\":\"chat\",\"segment\":\"%s\"}",id);out=request(t,query);
 const char *raw=json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN);assert(strstr(raw,"[REDACTED]"));assert(!strstr(raw,secret));assert(!strstr(raw,"private-opaque"));assert(!strcmp(json_object_get_string(field(field(field(out,"data"),"metadata"),"status")),"completed"));json_object_put(out);
 dl_trajectory_close(t);
 const char *names[]={"trajectory.sqlite","trajectory.sqlite-wal","trajectory.sqlite-shm","trajectory.lock"};
 for(size_t i=0;i<sizeof names/sizeof *names;i++){char path[512];snprintf(path,sizeof path,"%s/%s",directory,names[i]);unlink(path);}assert(!rmdir(directory));
 puts("PASS standalone C trajectory links/runs without desktop, agent, Swift, DSH or interpreter; writer ownership, live reader, redacted export and completion metadata");return 0;
}
