#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "model_helpers.h"
#include "media_records.h"
#include "agents.h"
#include "trajectory.h"
#include "file_history.h"
#include "grants.h"
#include "config_registry.h"
#include "model_switch.h"
#include "tool_records.h"
#include "chat_workspace.h"
#include "attachments.h"
#include "dunlin/compact/json.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
char *dc_read_view(const char *directory,const char *raw){
 if(!directory||strlen(directory)>3968||!raw||strlen(raw)>4096)return NULL;
 char parent[4096],path[4096];struct stat st;
 if(!realpath(directory,parent)||stat(parent,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))return NULL;
 snprintf(path,sizeof path,"%s/conversations.sqlite",parent);if(lstat(path,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=geteuid())return NULL;
 dc_store s={.directory=parent,.lock=-1};if(sqlite3_open_v2(path,&s.db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOFOLLOW,NULL)!=SQLITE_OK){sqlite3_close(s.db);return NULL;}sqlite3_busy_timeout(s.db,1000);
 if(!dc_attachments_initialize(&s,0)){sqlite3_close(s.db);return NULL;}
 struct json_object *request=dunlin_json_parse(raw,strlen(raw)),*v=NULL;if(request)json_object_object_get_ex(request,"op",&v);const char *op=json_object_get_string(v);struct json_object *result=NULL;
 if(op&&(!strcmp(op,"trace.status")||!strcmp(op,"trace.segments")||!strcmp(op,"trace.events")||!strcmp(op,"trace.export"))){s.trajectory=dl_trajectory_open_reader(parent);result=dc_trajectory_request(&s,request);}
 else if(op&&(!strcmp(op,"receipts.list")||!strcmp(op,"receipts.detail")||!strcmp(op,"receipts.status")||!strcmp(op,"receipts.pending")))result=dc_file_history_request(&s,request);
 else if(op&&(!strcmp(op,"media.recording")||!strcmp(op,"media.list")||!strcmp(op,"media.items")||!strcmp(op,"media.frames")||!strcmp(op,"media.frame.read")))result=dc_media_request(&s,request);
 else if(op&&!strcmp(op,"chats.metadata"))result=dc_chat_metadata(&s,request);
 else if(op&&!strcmp(op,"grants.list"))result=dc_grants_request(&s,request);
 else if(op&&(!strcmp(op,"agents.list")||!strcmp(op,"agents.next")||!strcmp(op,"agents.pending")||!strcmp(op,"agents.nativePending")||!strcmp(op,"agents.messages")||!strcmp(op,"agents.members")))result=dc_agents_request(&s,request);
 else if(op&&!strcmp(op,"helpers.pending"))result=dc_helpers_pending(&s);
 else if(op&&(!strcmp(op,"config.list")||!strcmp(op,"config.model")))result=dc_config_request(&s,request);
 else if(op&&(!strcmp(op,"tools.list")||!strcmp(op,"tools.detail")||!strcmp(op,"tools.locate")))result=dc_tool_records_request(&s,request);
 else if(op&&!strcmp(op,"chats.model.preview"))result=dc_model_switch_request(&s,request);
 else if(op&&(!strcmp(op,"chats.workspace.get")||!strcmp(op,"chats.workspace.preview")||!strcmp(op,"chats.workspace.migration")))result=dc_chat_workspace_request(&s,request);
 else if(op&&(!strcmp(op,"attachments.pending")||!strcmp(op,"attachments.read")))result=dc_attachments_request(&s,request);
 else if(op&&!strcmp(op,"chat.messages")){
  struct json_object *chat=NULL,*cursor=NULL;json_object_object_get_ex(request,"chat",&chat);
  const char *id=json_object_is_type(chat,json_type_string)?json_object_get_string(chat):NULL;
  int has_cursor=json_object_object_get_ex(request,"through",&cursor);
  int valid_cursor=!has_cursor||(json_object_is_type(cursor,json_type_int)&&json_object_get_int64(cursor)>=0&&json_object_get_uint64(cursor)<=INT64_MAX);
  char *messages=id&&strlen(id)<=64&&(size_t)json_object_get_string_len(chat)==strlen(id)&&valid_cursor?dc_messages_through(&s,id,has_cursor?json_object_get_int64(cursor):INT64_MAX):NULL;
  struct json_object *data=messages?dunlin_json_parse(messages,strlen(messages)):NULL;free(messages);
  if(data){result=json_object_new_object();json_object_object_add(result,"ok",json_object_new_boolean(1));json_object_object_add(result,"data",data);}
 }
 if(!result){result=json_object_new_object();json_object_object_add(result,"ok",json_object_new_boolean(0));json_object_object_add(result,"error",json_object_new_string("unsupported_read_view"));}
 char *out=strdup(json_object_to_json_string_ext(result,JSON_C_TO_STRING_PLAIN));json_object_put(result);json_object_put(request);dl_trajectory_close(s.trajectory);dl_attachments_close(s.attachments);sqlite3_close(s.db);return out;
}
