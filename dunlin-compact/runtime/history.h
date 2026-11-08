#ifndef DUNLIN_HISTORY_INTERNAL_H
#define DUNLIN_HISTORY_INTERNAL_H
#include <json-c/json.h>
#include <stdint.h>
#include <string.h>
/* Request view only. Drop one oldest closed user turn, preserve leading policy,
 * newest turn and trailing request guidance. Durable journals never enter here. */
static inline int dunlin_history_evict(struct json_object **history) {
 size_t first=SIZE_MAX,second=SIZE_MAX,n=json_object_array_length(*history);
 for(size_t i=0;i<n;i++) {
  struct json_object *role=NULL;
  json_object_object_get_ex(json_object_array_get_idx(*history,i),"role",&role);
  const char *text=json_object_get_string(role);
  if(text && !strcmp(text,"user")){if(first==SIZE_MAX)first=i;else{second=i;break;}}
 }
 if(second==SIZE_MAX)return 0;
 /* Failed turns may end in paired tool observations. Never evict an open batch. */
 for(size_t i=first+1;i<second;i++) {
  struct json_object *message=json_object_array_get_idx(*history,i),*role=NULL,*calls=NULL;
  json_object_object_get_ex(message,"role",&role);
  const char *text=json_object_get_string(role);
  if(!text || strcmp(text,"assistant"))return 0;
  if(!json_object_object_get_ex(message,"tool_calls",&calls))continue;
  if(!json_object_is_type(calls,json_type_array))return 0;
  size_t count=json_object_array_length(calls);
  if(count>=second-i)return 0;
  for(size_t j=0;j<count;j++) {
   struct json_object *call=json_object_array_get_idx(calls,j),*id=NULL,*pair=NULL,*paired_role=NULL;
   struct json_object *result=json_object_array_get_idx(*history,i+1+j);
   json_object_object_get_ex(call,"id",&id);json_object_object_get_ex(result,"tool_call_id",&pair);
   json_object_object_get_ex(result,"role",&paired_role);
   const char *a=json_object_get_string(id),*b=json_object_get_string(pair),*r=json_object_get_string(paired_role);
   if(!a || !b || !r || strcmp(r,"tool") || strcmp(a,b))return 0;
  }
  i+=count;
 }
 struct json_object *view=json_object_new_array();if(!view)return 0;
 for(size_t i=0;i<n;i++) {
  if(i>=first && i<second)continue;
  struct json_object *message=json_object_get(json_object_array_get_idx(*history,i));
  if(json_object_array_add(view,message)) {json_object_put(message);json_object_put(view);return 0;}
 }
 json_object_put(*history);*history=view;return 1;
}
#endif
