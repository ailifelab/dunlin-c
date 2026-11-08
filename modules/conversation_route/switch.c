#define _POSIX_C_SOURCE 200809L
#include "../../desktop/model_switch.h"
#include "../../desktop/config_registry.h"
#include "../../desktop/attachments.h"
#include "dunlin/content_hash.h"
#include "dunlin/context_budget.h"
#include "dunlin/compact/json.h"
#include "dunlin/compact/agent.h"
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
typedef struct json_object J;
static J *get(J *o,const char *key){J *v=NULL;if(json_object_is_type(o,json_type_object))json_object_object_get_ex(o,key,&v);return v;}
static const char *str(J *o,const char *key){J *v=get(o,key);if(!json_object_is_type(v,json_type_string))return NULL;const char *s=json_object_get_string(v);return strlen(s)==(size_t)json_object_get_string_len(v)?s:NULL;}
static int eq(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static int ident(const char *s,int empty){return s&&(empty||*s)&&strlen(s)<=64&&strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(s);}
static int integer(J *v){return json_object_is_type(v,json_type_int)&&json_object_get_int64(v)>=0&&json_object_get_uint64(v)<=INT64_MAX;}
static J *reply(J *data,const char *error){J *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(error==NULL));if(error)json_object_object_add(r,"error",json_object_new_string(error));if(data)json_object_object_add(r,"data",data);return r;}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;if(sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)!=SQLITE_OK)return NULL;return q;}
static void bind(sqlite3_stmt *q,int at,const char *s){if(q)sqlite3_bind_text(q,at,s,-1,SQLITE_TRANSIENT);}
static int sql(dc_store *s,const char *raw){return sqlite3_exec(s->db,raw,NULL,NULL,NULL)==SQLITE_OK;}
static void text(J *o,const char *key,const char *value){json_object_object_add(o,key,json_object_new_string(value?value:""));}
static void number(J *o,const char *key,int64_t value){json_object_object_add(o,key,json_object_new_int64(value));}
static J *find(J *models,const char *id){for(size_t i=0;i<json_object_array_length(models);i++){J *m=json_object_array_get_idx(models,i);if(eq(str(m,"id"),id))return m;}return NULL;}
static int has_image(J *model){J *input=get(model,"inputModalities");for(size_t i=0;input&&i<json_object_array_length(input);i++){J *v=json_object_array_get_idx(input,i);if(json_object_is_type(v,json_type_string)&&eq(json_object_get_string(v),"image"))return 1;}return 0;}
static int history_counts(J *checkpoint,int64_t *count,int64_t *tools,int64_t *images){
 if(!checkpoint){*count=*tools=*images=0;return 1;}
 J *messages=get(checkpoint,"messages"),*active=get(checkpoint,"active");
 J *version=get(checkpoint,"version"),*rounds=get(checkpoint,"rounds");
 if(!json_object_is_type(checkpoint,json_type_object)||!json_object_is_type(messages,json_type_array)||!json_object_is_type(active,json_type_boolean)||!integer(version)||(json_object_get_int64(version)!=1&&json_object_get_int64(version)!=2)||!integer(rounds)||json_object_get_int64(rounds)>INT_MAX)return 0;
 *count=(int64_t)json_object_array_length(messages);*tools=*images=0;
 for(size_t i=0;i<json_object_array_length(messages);i++){
  J *m=json_object_array_get_idx(messages,i),*content=get(m,"content");const char *role=str(m,"role");
  if(!role||(!eq(role,"user")&&!eq(role,"assistant")&&!eq(role,"tool")))return 0;
  if(eq(role,"tool"))(*tools)++;
  J *attachments=get(m,"_dunlin_attachments");
  if(attachments){
   if(!eq(role,"user")||!json_object_is_type(attachments,json_type_array)||json_object_array_length(attachments)!=1)return 0;
   const char *format=str(json_object_array_get_idx(attachments,0),"format");
   if(eq(format,"png"))(*images)++;else if(!eq(format,"text"))return 0;
  }
  if(json_object_is_type(content,json_type_array)){
   if(!eq(role,"user"))return 0;
   for(size_t j=0;j<json_object_array_length(content);j++){J *part=json_object_array_get_idx(content,j);const char *type=str(part,"type");if(eq(type,"image_url"))(*images)++;else if(!eq(type,"text"))return 0;}
  }else if(!json_object_is_type(content,json_type_string)&&!(eq(role,"assistant")&&!content))return 0;
 }return 1;
}
static J *review(dc_store *s,J *r,int apply){
 const char *chat=str(r,"chat"),*target=str(r,"target");J *revision=get(r,"revision");
 if(!ident(chat,0)||!ident(target,0)||!integer(revision))return reply(NULL,"model_switch_invalid");
 const char *image_policy=get(r,"imagePolicy")?str(r,"imagePolicy"):"images";
 if(!eq(image_policy,"images")&&!eq(image_policy,"textOnly"))return reply(NULL,"model_switch_invalid");
 const char *expected=str(r,"currentID");
 if(apply&&(!expected||strlen(expected)>128||!ah_utf8_text(expected,strlen(expected))||!str(r,"historyHash")||strlen(str(r,"historyHash"))!=64||strspn(str(r,"historyHash"),"0123456789abcdef")!=64))return reply(NULL,"model_switch_invalid");
 J *query=json_object_new_object();text(query,"op","config.list");J *registry_reply=dc_config_request(s,query);json_object_put(query);
 J *registry=get(registry_reply,"data"),*models=get(registry,"models");
 if(!json_object_get_boolean(get(registry_reply,"ok"))){json_object_put(registry_reply);return reply(NULL,"model_switch_storage_error");}
 if(json_object_get_int64(get(registry,"revision"))!=json_object_get_int64(revision)){json_object_put(registry_reply);return reply(NULL,"model_switch_configuration_changed");}
 J *model=find(models,target);if(!model){json_object_put(registry_reply);return reply(NULL,"model_switch_model_missing");}
 query=json_object_new_object();text(query,"op","config.model");text(query,"id",target);json_object_object_add(query,"revision",json_object_get(revision));J *mapped=dc_config_request(s,query);json_object_put(query);
 if(!json_object_get_boolean(get(mapped,"ok"))){json_object_put(mapped);json_object_put(registry_reply);return reply(NULL,"model_switch_model_unusable");}json_object_put(mapped);
 sqlite3_stmt *q=prep(s,"SELECT c.state,COALESCE(m.model,'') FROM chats c LEFT JOIN chat_properties m ON m.chat=c.id WHERE c.id=?");bind(q,1,chat);
 int row=q?sqlite3_step(q):SQLITE_ERROR;
 if(row!=SQLITE_ROW){sqlite3_finalize(q);json_object_put(registry_reply);return reply(NULL,row==SQLITE_DONE?"model_switch_chat_missing":"model_switch_storage_error");}
 const char *raw=(const char*)sqlite3_column_text(q,0),*old=(const char*)sqlite3_column_text(q,1);size_t bytes=(size_t)sqlite3_column_bytes(q,0);int has_state=raw!=NULL;
 int raw_ok=!raw||(bytes<=4194304&&strlen(raw)==bytes);J *checkpoint=raw&&raw_ok?dunlin_json_parse(raw,bytes):NULL;
 char hash[65];dc_sha256(raw_ok&&raw?raw:"null",raw_ok&&raw?bytes:4,hash);
 char *current=old?strdup(old):NULL;sqlite3_finalize(q);int64_t count=0,tools=0,images=0;
 const char *error=!current?"model_switch_storage_error":!raw_ok||(has_state&&!checkpoint)||!history_counts(checkpoint,&count,&tools,&images)?"model_switch_history_invalid":NULL;
 int64_t legacy_images=0;
 J *messages=get(checkpoint,"messages");
 for(size_t i=0;!error&&json_object_is_type(messages,json_type_array)&&i<json_object_array_length(messages);i++){
  J *m=json_object_array_get_idx(messages,i),*content=get(m,"content");
  if(json_object_is_type(content,json_type_array))for(size_t k=0;k<json_object_array_length(content);k++)if(eq(str(json_object_array_get_idx(content,k),"type"),"image_url"))legacy_images++;
  if(get(m,"_dunlin_attachments")){J *meta=dc_attachment_message_view(s,chat,(int64_t)i,m);if(!meta)error="model_switch_attachment_source_invalid";json_object_put(meta);}
 }
 if(!error&&(json_object_get_boolean(get(checkpoint,"active"))||get(checkpoint,"pending")))error="model_switch_active_checkpoint";
 if(!error&&apply&&(!eq(current,str(r,"currentID"))||!eq(hash,str(r,"historyHash"))))error="model_switch_history_changed";
 json_object_put(checkpoint);
 if(error){free(current);json_object_put(registry_reply);return reply(NULL,error);}
 dl_attachment_draft pending={0};dl_attachment_status pending_status=s->attachments?dl_attachments_draft(s->attachments,DL_ATTACHMENT_CHAT,chat,&pending):DL_ATTACHMENT_NOT_FOUND;
 int current_images=dc_attachment_policy(s,chat);
 if((pending_status!=DL_ATTACHMENT_OK&&pending_status!=DL_ATTACHMENT_NOT_FOUND)||current_images<0){free(current);json_object_put(registry_reply);return reply(NULL,"model_switch_storage_error");}
 const char *current_policy=current_images?"images":"textOnly";
 const char *expected_pending=str(r,"pendingID"),*expected_policy=str(r,"currentImagePolicy");
 if(apply&&((!eq(expected_pending?expected_pending:"",pending.id))||(expected_policy&&!eq(expected_policy,current_policy))
    ||(s->attachments&&!expected_policy))){free(current);json_object_put(registry_reply);return reply(NULL,"model_switch_history_changed");}
 q=prep(s,"SELECT count(*) FROM file_operations WHERE chat=? AND state='applied'");bind(q,1,chat);row=q?sqlite3_step(q):SQLITE_ERROR;
 int64_t receipts=row==SQLITE_ROW?sqlite3_column_int64(q,0):0;sqlite3_finalize(q);
 if(row!=SQLITE_ROW){free(current);json_object_put(registry_reply);return reply(NULL,"model_switch_storage_error");}
 const char *protocol=str(model,"protocolName");int image_protocol=eq(protocol,"chat");
 int pending_images=pending.id[0]&&pending.content.format==DL_ATTACHMENT_PNG;
 const char *blocked=eq(image_policy,"textOnly")?(legacy_images?"model_switch_legacy_image_projection_unavailable":NULL):
  images||pending_images?(!image_protocol?"model_switch_image_protocol_unsupported":!has_image(model)?"model_switch_image_capability_unconfirmed":NULL):NULL;
 if(apply&&blocked){free(current);json_object_put(registry_reply);return reply(NULL,blocked);}
 J *data=json_object_new_object(),*old_model=find(models,current);
 text(data,"chat",chat);text(data,"targetID",target);text(data,"targetName",str(model,"name"));text(data,"currentID",current);text(data,"currentName",old_model?str(old_model,"name"):*current?current:"未配置");text(data,"protocol",protocol);text(data,"historyHash",hash);
 number(data,"configRevision",json_object_get_int64(revision));number(data,"messageCount",count);number(data,"toolResults",tools);number(data,"fileReceipts",receipts);number(data,"imageCount",images);
 number(data,"pendingImageCount",pending_images);text(data,"pendingID",pending.id);text(data,"imagePolicy",image_policy);text(data,"currentImagePolicy",current_policy);
 json_object_object_add(data,"canTextOnly",json_object_new_boolean(legacy_images==0&&(images||pending_images)));
 json_object_object_add(data,"canSwitch",json_object_new_boolean(blocked==NULL));if(blocked)text(data,"reason",blocked);text(data,"contextFit","unmeasured");
 dc_context_capabilities cap={.model_limit=(uint64_t)json_object_get_int64(get(model,"contextLimit")),.deployment_limit=(uint64_t)json_object_get_int64(get(model,"deploymentLimit")),.selected_limit=(uint64_t)json_object_get_int64(get(model,"contextBudget")),.client_limit=4194304,.output_limit=65536,.requested_output=(uint64_t)json_object_get_int64(get(model,"maxOutput")),.overhead_reserve=get(model,"contextReserve")?(uint64_t)json_object_get_int64(get(model,"contextReserve")):512};
 const char *limits[]={"outputLimit","advertisedOutputLimit"};for(size_t i=0;i<2;i++){J *v=get(model,limits[i]);if(v&&(uint64_t)json_object_get_int64(v)<cap.output_limit)cap.output_limit=(uint64_t)json_object_get_int64(v);}
 dc_context_range budget={0};int known=dc_context_budget(&cap,&budget)==DC_CONTEXT_VALID;json_object_object_add(data,"contextKnown",json_object_new_boolean(known));if(known)number(data,"contextSelected",(int64_t)budget.selected);
 if(apply){
  q=prep(s,"INSERT OR IGNORE INTO chat_properties(chat) VALUES(?)");bind(q,1,chat);int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);
  q=ok?prep(s,"UPDATE chat_properties SET model=? WHERE chat=? AND model=?"):NULL;bind(q,1,target);bind(q,2,chat);bind(q,3,current);ok=q&&sqlite3_step(q)==SQLITE_DONE&&sqlite3_changes(s->db)==1;sqlite3_finalize(q);
  if(ok&&s->attachments)ok=dc_attachment_set_policy(s,chat,eq(image_policy,"images"));
  if(!ok){free(current);json_object_put(data);json_object_put(registry_reply);return reply(NULL,"model_switch_storage_error");}
 }
 free(current);json_object_put(registry_reply);return reply(data,NULL);
}
J *dc_model_switch_request(dc_store *s,J *r){
 const char *op=str(r,"op");int apply=eq(op,"chats.model.apply");if(!s||(!apply&&!eq(op,"chats.model.preview")))return reply(NULL,"model_switch_invalid");
 if(!sqlite3_get_autocommit(s->db)||!sql(s,apply?"BEGIN IMMEDIATE":"BEGIN"))return reply(NULL,"model_switch_storage_error");
 J *result=review(s,r,apply);int success=json_object_get_boolean(get(result,"ok"));
 if(!sql(s,success?"COMMIT":"ROLLBACK")){sql(s,"ROLLBACK");json_object_put(result);return reply(NULL,"model_switch_storage_error");}return result;
}
