#include "dunlin/static_config.h"
#include <json-c/json.h>
#include <string.h>
#include <stdio.h>
int main(void){
 const char *source="providers:\n  local:\n    models: [one, two]\n    enabled: true\n";
 dc_static_config_error error;struct json_object *value=dc_static_config_parse(source,strlen(source),&error),*providers=NULL,*local=NULL,*models=NULL,*enabled=NULL;
 if(!value||error.code||!json_object_object_get_ex(value,"providers",&providers)||!json_object_object_get_ex(providers,"local",&local)||!json_object_object_get_ex(local,"models",&models)||json_object_array_length(models)!=2||!json_object_object_get_ex(local,"enabled",&enabled)||!json_object_get_boolean(enabled))return 1;
 json_object_put(value);source="a: 1\na: 2\n";value=dc_static_config_parse(source,strlen(source),&error);if(value||error.code!=DC_STATIC_DUPLICATE||error.line!=2||error.column!=1){json_object_put(value);return 2;}
 source="a: &x [1,2]\nb: *x\n";value=dc_static_config_parse(source,strlen(source),&error);if(value||error.code!=DC_STATIC_ALIAS){json_object_put(value);return 3;}
 puts("PASS standalone bounded C static config linked without kernel/agent/UI/interpreter");return 0;
}
