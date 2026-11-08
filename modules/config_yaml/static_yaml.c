#define _POSIX_C_SOURCE 200809L
#include "dunlin/static_config.h"
#include <json-c/json.h>
#include <yaml.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#ifdef _WIN32
#define strdup _strdup
#endif

typedef struct json_object J;
typedef struct {
 yaml_parser_t parser;
 dc_static_config_error error;
 size_t nodes;
} reader;
static void fail(reader *r,dc_static_config_code code,yaml_mark_t mark){
 if(r->error.code==DC_STATIC_OK)r->error=(dc_static_config_error){code,mark.line+1,mark.column+1};
}
static int next(reader *r,yaml_event_t *event){
 if(!yaml_parser_parse(&r->parser,event)){fail(r,r->parser.error==YAML_READER_ERROR?DC_STATIC_ENCODING:r->parser.error==YAML_MEMORY_ERROR?DC_STATIC_MEMORY:DC_STATIC_SYNTAX,r->parser.problem_mark);return 0;}
 return 1;
}
static int scalar_allowed(reader *r,yaml_event_t *e){
 if(e->data.scalar.anchor){fail(r,DC_STATIC_ALIAS,e->start_mark);return 0;}
 if(e->data.scalar.tag){fail(r,DC_STATIC_TAG,e->start_mark);return 0;}
 const char *v=(const char*)e->data.scalar.value;size_t n=e->data.scalar.length;
 if(n>1048576||strlen(v)!=n){fail(r,DC_STATIC_ENCODING,e->start_mark);return 0;}return 1;
}
static int json_number(const char *v){
 const char *p=v;if(*p=='-')p++;
 if(*p=='0')p++;else{if(*p<'1'||*p>'9')return 0;while(isdigit((unsigned char)*p))p++;}
 if(*p=='.'){p++;if(!isdigit((unsigned char)*p))return 0;while(isdigit((unsigned char)*p))p++;}
 if(*p=='e'||*p=='E'){p++;if(*p=='+'||*p=='-')p++;if(!isdigit((unsigned char)*p))return 0;while(isdigit((unsigned char)*p))p++;}
 return !*p;
}
static J *scalar(reader *r,yaml_event_t *e){
 if(!scalar_allowed(r,e))return NULL;const char *v=(const char*)e->data.scalar.value;size_t n=e->data.scalar.length;
 if(e->data.scalar.style==YAML_PLAIN_SCALAR_STYLE){
  if(!n||!strcmp(v,"~")||!strcmp(v,"null")||!strcmp(v,"Null")||!strcmp(v,"NULL"))return NULL;
  if(!strcmp(v,"true")||!strcmp(v,"True")||!strcmp(v,"TRUE")||!strcmp(v,"false")||!strcmp(v,"False")||!strcmp(v,"FALSE")){J *b=json_object_new_boolean(*v=='t'||*v=='T');if(!b)fail(r,DC_STATIC_MEMORY,e->start_mark);return b;}
  if(n&&json_number(v)){
   char *end=NULL;errno=0;
   J *number=NULL;
   if(strchr(v,'.')||strchr(v,'e')||strchr(v,'E')){
    /* The shared strict parser intentionally accepts object/array roots only.
     * Here the lexical grammar is already checked, so use json-c's scalar
     * parser, including its locale handling, and reject nonrepresentable data. */
    struct json_tokener *tok=json_tokener_new();if(!tok){fail(r,DC_STATIC_MEMORY,e->start_mark);return NULL;}
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT);number=json_tokener_parse_ex(tok,v,(int)n+1);int ok=json_tokener_get_error(tok)==json_tokener_success;json_tokener_free(tok);
    int nonzero=0;for(const char *p=v;*p&&*p!='e'&&*p!='E';p++)if(*p>='1'&&*p<='9')nonzero=1;
    if(!ok||!number||!isfinite(json_object_get_double(number))||(json_object_get_double(number)==0&&nonzero)){json_object_put(number);fail(r,DC_STATIC_LIMIT,e->start_mark);return NULL;}
   }else{
    long long value=strtoll(v,&end,10);if(errno==ERANGE){fail(r,DC_STATIC_LIMIT,e->start_mark);return NULL;}number=json_object_new_int64(value);
   }
   if(!number)fail(r,DC_STATIC_MEMORY,e->start_mark);return number;
  }
 }
 J *text=json_object_new_string_len(v,(int)n);if(!text)fail(r,DC_STATIC_MEMORY,e->start_mark);return text;
}
/* Takes ownership of the event, including every error path. JSON null is a
 * valid NULL return; reader.error distinguishes it from failed parsing. */
static J *node(reader *r,yaml_event_t *event,unsigned depth){
 if(depth>32||++r->nodes>65536){fail(r,DC_STATIC_LIMIT,event->start_mark);yaml_event_delete(event);return NULL;}
 if(event->type==YAML_SCALAR_EVENT){J *v=scalar(r,event);yaml_event_delete(event);return v;}
 int mapping=event->type==YAML_MAPPING_START_EVENT,sequence=event->type==YAML_SEQUENCE_START_EVENT;
 if(!mapping&&!sequence){fail(r,event->type==YAML_ALIAS_EVENT?DC_STATIC_ALIAS:DC_STATIC_SYNTAX,event->start_mark);yaml_event_delete(event);return NULL;}
 yaml_char_t *anchor=mapping?event->data.mapping_start.anchor:event->data.sequence_start.anchor;
 yaml_char_t *tag=mapping?event->data.mapping_start.tag:event->data.sequence_start.tag;
 if(anchor||tag){fail(r,anchor?DC_STATIC_ALIAS:DC_STATIC_TAG,event->start_mark);yaml_event_delete(event);return NULL;}
 J *out=mapping?json_object_new_object():json_object_new_array();if(!out){fail(r,DC_STATIC_MEMORY,event->start_mark);yaml_event_delete(event);return NULL;}yaml_event_delete(event);
 for(;;){
  yaml_event_t e;if(!next(r,&e))break;
  if((mapping&&e.type==YAML_MAPPING_END_EVENT)||(sequence&&e.type==YAML_SEQUENCE_END_EVENT)){yaml_event_delete(&e);return out;}
  char *key=NULL;
  if(mapping){
   if(e.type!=YAML_SCALAR_EVENT){fail(r,DC_STATIC_KEY,e.start_mark);yaml_event_delete(&e);break;}
   if(++r->nodes>65536||!scalar_allowed(r,&e)){if(r->error.code==DC_STATIC_OK)fail(r,DC_STATIC_LIMIT,e.start_mark);yaml_event_delete(&e);break;}
   const char *name=(const char*)e.data.scalar.value;J *old=NULL;
   if(!*name||!strcmp(name,"<<")||json_object_object_get_ex(out,name,&old)){fail(r,!strcmp(name,"<<")?DC_STATIC_ALIAS:*name?DC_STATIC_DUPLICATE:DC_STATIC_KEY,e.start_mark);yaml_event_delete(&e);break;}
   key=strdup(name);if(!key){fail(r,DC_STATIC_MEMORY,e.start_mark);yaml_event_delete(&e);break;}yaml_event_delete(&e);
   if(!next(r,&e)){free(key);break;}
  }
  J *value=node(r,&e,depth+1);
  if(r->error.code!=DC_STATIC_OK){free(key);json_object_put(value);break;}
  int rc=mapping?json_object_object_add(out,key,value):json_object_array_add(out,value);free(key);
  if(rc){json_object_put(value);fail(r,DC_STATIC_MEMORY,r->parser.mark);break;}
 }
 json_object_put(out);return NULL;
}
J *dc_static_config_parse(const char *source,size_t size,dc_static_config_error *error){
 if(error)*error=(dc_static_config_error){DC_STATIC_OK,0,0};
 if(!source||!size||size>1048576||memchr(source,0,size)){if(error)*error=(dc_static_config_error){size>1048576?DC_STATIC_LIMIT:DC_STATIC_ENCODING,1,1};return NULL;}
 reader r={0};if(!yaml_parser_initialize(&r.parser)){if(error)*error=(dc_static_config_error){DC_STATIC_MEMORY,1,1};return NULL;}
 yaml_parser_set_encoding(&r.parser,YAML_UTF8_ENCODING);yaml_parser_set_input_string(&r.parser,(const unsigned char*)source,size);J *out=NULL;yaml_event_t e;
 if(!next(&r,&e))goto end;
 if(e.type!=YAML_STREAM_START_EVENT){fail(&r,DC_STATIC_SYNTAX,e.start_mark);yaml_event_delete(&e);goto end;}yaml_event_delete(&e);
 if(!next(&r,&e))goto end;
 if(e.type!=YAML_DOCUMENT_START_EVENT||e.data.document_start.tag_directives.start!=e.data.document_start.tag_directives.end){fail(&r,DC_STATIC_DOCUMENT,e.start_mark);yaml_event_delete(&e);goto end;}
 if(e.data.document_start.version_directive&&(e.data.document_start.version_directive->major!=1||e.data.document_start.version_directive->minor!=2)){fail(&r,DC_STATIC_DOCUMENT,e.start_mark);yaml_event_delete(&e);goto end;}yaml_event_delete(&e);
 if(!next(&r,&e))goto end;out=node(&r,&e,0);if(r.error.code!=DC_STATIC_OK)goto end;
 if(!json_object_is_type(out,json_type_object)&&!json_object_is_type(out,json_type_array)){fail(&r,DC_STATIC_ROOT,r.parser.mark);goto end;}
 if(!next(&r,&e))goto end;
 if(e.type!=YAML_DOCUMENT_END_EVENT){fail(&r,DC_STATIC_DOCUMENT,e.start_mark);yaml_event_delete(&e);goto end;}yaml_event_delete(&e);
 if(!next(&r,&e))goto end;
 if(e.type!=YAML_STREAM_END_EVENT)fail(&r,DC_STATIC_DOCUMENT,e.start_mark);yaml_event_delete(&e);
end:
 yaml_parser_delete(&r.parser);if(r.error.code!=DC_STATIC_OK){json_object_put(out);out=NULL;}if(error)*error=r.error;return out;
}
