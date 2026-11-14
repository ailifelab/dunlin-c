#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../desktop/session_outcomes.h"
static json_object *parse(const char *text){json_object *o=json_tokener_parse(text);assert(o);return o;}
int main(void){
 json_object *empty=dc_outcomes_copy(NULL,0);assert(empty&&json_object_array_length(empty)==0);json_object_put(empty);
 json_object *failed=parse("{\"messages\":[{\"role\":\"user\",\"content\":\"A\"}],\"active\":false,\"status\":1,\"detail\":\"HTTP 503\"}");
 json_object *messages=dc_outcome_get(failed,"messages");char *before=strdup(json_object_to_json_string(messages));assert(before);
 assert(dc_outcomes_save(failed,NULL,"owned-run"));assert(!strcmp(before,json_object_to_json_string(messages)));free(before);
 json_object *rows=dc_outcome_get(failed,"_dunlin_turn_outcomes");assert(json_object_array_length(rows)==1);
 json_object *cut=dc_outcomes_copy(failed,0);assert(cut&&json_object_array_length(cut)==0);json_object_put(cut);
 cut=dc_outcomes_copy(failed,1);assert(cut&&json_object_equal(cut,rows));json_object_put(cut);
 json_object *next=parse("{\"messages\":[{\"role\":\"user\",\"content\":\"A\"},{\"role\":\"user\",\"content\":\"B\"},{\"role\":\"assistant\",\"content\":\"done\"}],\"active\":false,\"status\":0,\"_dunlin_turn_outcomes\":[{\"forged\":true}]}");
 assert(dc_outcomes_save(next,failed,"second-run"));assert(json_object_equal(dc_outcome_get(next,"_dunlin_turn_outcomes"),rows));
 // Byte-bound details stop before a UTF-8 continuation, with explicit truncation.
 char detail[2100];memset(detail,'a',2047);memcpy(detail+2047,"\xf0\x9f\xa6\x86 tail",10);detail[2057]=0;
 json_object_object_add(failed,"detail",json_object_new_string(detail));assert(dc_outcomes_save(failed,NULL,"unicode-run"));
 json_object *row=json_object_array_get_idx(dc_outcome_get(failed,"_dunlin_turn_outcomes"),0);assert(json_object_get_string_len(dc_outcome_get(row,"detail"))==2047);assert(json_object_get_boolean(dc_outcome_get(row,"truncated")));
 const char *invalid[]={"{\"messages\":[],\"_dunlin_turn_outcomes\":{}}","{\"messages\":[{}],\"_dunlin_turn_outcomes\":[{\"message\":-1,\"through\":0,\"status\":1,\"detail\":\"x\"}]}","{\"messages\":[{}],\"_dunlin_turn_outcomes\":[{\"message\":0,\"through\":1,\"status\":1,\"detail\":\"x\"}]}"};
 for(size_t i=0;i<sizeof invalid/sizeof *invalid;i++){json_object *bad=parse(invalid[i]);assert(!dc_outcomes_copy(bad,100));json_object_put(bad);}
 json_object_object_add(failed,"active",json_object_new_boolean(1));assert(!dc_outcome_row(failed,"active"));
 json_object_put(failed);json_object_put(next);puts("PASS C outcome persistence, empty source, prefix fence, immutable messages, untrusted metadata, UTF-8 truncation and malformed metadata");return 0;
}
