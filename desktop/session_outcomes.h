#ifndef DUNLIN_SESSION_OUTCOMES_H
#define DUNLIN_SESSION_OUTCOMES_H
#include <json-c/json.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
/* Canonical desktop metadata only. Model messages and compact runtime are
 * unchanged. Non-success turn outcomes survive the next turn's status field. */
static inline json_object *dc_outcome_get(json_object *o,const char *key){json_object *v=NULL;json_object_object_get_ex(o,key,&v);return v;}
static inline json_object *dc_outcome_row(json_object *state,const char *run){
 json_object *messages=dc_outcome_get(state,"messages"),*status=dc_outcome_get(state,"status");
 if(!json_object_is_type(messages,json_type_array)||json_object_get_boolean(dc_outcome_get(state,"active"))||!json_object_is_type(status,json_type_int)||json_object_get_int(status)<=0||json_object_get_int(status)>9)return NULL;
 size_t count=json_object_array_length(messages),index=count;
 while(index){index--;const char *role=json_object_get_string(dc_outcome_get(json_object_array_get_idx(messages,index),"role"));if(role&&!strcmp(role,"user"))break;}
 const char *role=count?json_object_get_string(dc_outcome_get(json_object_array_get_idx(messages,index),"role")):NULL;if(!role||strcmp(role,"user"))return NULL;
 json_object *row=json_object_new_object();if(!row)return NULL;
 json_object_object_add(row,"message",json_object_new_int64((int64_t)index));json_object_object_add(row,"through",json_object_new_int64((int64_t)count-1));json_object_object_add(row,"status",json_object_get(status));
 const char *detail=json_object_get_string(dc_outcome_get(state,"detail"));if(!detail)detail="Native turn did not complete";size_t length=strlen(detail),n=length>2048?2048:length;while(n<length&&((unsigned char)detail[n]&0xc0)==0x80)n--;
 json_object_object_add(row,"detail",json_object_new_string_len(detail,(int)n));if(n<length)json_object_object_add(row,"truncated",json_object_new_boolean(1));
 if(run&&strlen(run)<=64)json_object_object_add(row,"run",json_object_new_string(run));return row;
}
/* A fork copies only outcomes wholly inside its actual committed prefix. */
static inline json_object *dc_outcomes_copy(json_object *source,size_t limit){
 json_object *out=json_object_new_array(),*rows=dc_outcome_get(source,"_dunlin_turn_outcomes"),*messages=dc_outcome_get(source,"messages");if(!out)return NULL;
 if(rows&&!json_object_is_type(rows,json_type_array)){json_object_put(out);return NULL;}
 size_t count=rows?json_object_array_length(rows):0,total=messages?json_object_array_length(messages):0;int64_t previous=-1;
 for(size_t i=0;i<count;i++){
  json_object *row=json_object_array_get_idx(rows,i),*start=dc_outcome_get(row,"message"),*end=dc_outcome_get(row,"through"),*status=dc_outcome_get(row,"status"),*detail=dc_outcome_get(row,"detail");int64_t a=json_object_get_int64(start),b=json_object_get_int64(end);
  if(!json_object_is_type(start,json_type_int)||!json_object_is_type(end,json_type_int)||a<0||a<=previous||b<a||(uint64_t)b>=total||!json_object_is_type(status,json_type_int)||json_object_get_int(status)<1||json_object_get_int(status)>9||!json_object_is_type(detail,json_type_string)||json_object_get_string_len(detail)>2048){json_object_put(out);return NULL;}
  previous=a;if((uint64_t)b<limit)json_object_array_add(out,json_object_get(row));
 }
 // Upgrade a pre-metadata terminal failure from its actual C status, once.
 json_object *legacy=dc_outcome_row(source,json_object_get_string(dc_outcome_get(source,"_dunlin_run")));
 if(legacy){int64_t a=json_object_get_int64(dc_outcome_get(legacy,"message")),b=json_object_get_int64(dc_outcome_get(legacy,"through"));if(a>previous&&(uint64_t)b<limit)json_object_array_add(out,legacy);else json_object_put(legacy);}
 return out;
}
static inline int dc_outcomes_save(json_object *next,json_object *original,const char *run){
 size_t count=json_object_array_length(dc_outcome_get(next,"messages"));json_object *rows=dc_outcomes_copy(original,count);if(!rows)return 0;
 json_object *row=dc_outcome_row(next,run);
 if(row){int64_t start=json_object_get_int64(dc_outcome_get(row,"message"));size_t n=json_object_array_length(rows);if(n&&json_object_get_int64(dc_outcome_get(json_object_array_get_idx(rows,n-1),"message"))==start)json_object_array_put_idx(rows,n-1,row);else json_object_array_add(rows,row);}
 // Never accept runtime/plugin-supplied outcome metadata in place of C source.
 json_object_object_del(next,"_dunlin_turn_outcomes");if(json_object_array_length(rows))json_object_object_add(next,"_dunlin_turn_outcomes",rows);else json_object_put(rows);return 1;
}
#endif
