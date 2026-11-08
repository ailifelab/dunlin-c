#include "internal.h"
#include "project_context.h"
#include "project_files.h"
#include <string.h>
typedef struct json_object J;
static J *get(J *o,const char *k){J *v=NULL;if(o)json_object_object_get_ex(o,k,&v);return v;}
static const char *str(J *o,const char *k){return json_object_get_string(get(o,k));}
static void add(J *o,const char *k,const char *v){json_object_object_add(o,k,json_object_new_string(v));}
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v,-1,SQLITE_TRANSIENT);}
static int done(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
int dc_project_reference_schema(dc_store *s){return sqlite3_exec(s->db,"CREATE TABLE IF NOT EXISTS project_reference_baselines(project TEXT NOT NULL,path TEXT NOT NULL,sha256 TEXT NOT NULL,PRIMARY KEY(project,path));",NULL,NULL,NULL)==SQLITE_OK;}
int dc_project_reference_versions(const char *root,J *paths,J *versions){
 if(!versions)return 1;if(!json_object_is_type(versions,json_type_object)||json_object_object_length(versions)>24)return 0;
 J *project=json_object_new_object(),*selection=json_object_new_array();add(project,"path",root);json_object_object_add(project,"context",json_object_get(paths));
 json_object_object_foreach(versions,path,hash){J *ref=json_object_new_object();add(ref,"path",path);json_object_object_add(ref,"sha256",json_object_get(hash));json_object_array_add(selection,ref);}
 J *docs=dc_project_references(project,selection,0,NULL);int ok=docs!=NULL;json_object_put(docs);json_object_put(selection);json_object_put(project);return ok;
}
int dc_project_reference_sync(dc_store *s,const char *id,J *paths,J *versions,int reset){
 sqlite3_stmt *q=prep(s,reset?"DELETE FROM project_reference_baselines WHERE project=?":"DELETE FROM project_reference_baselines WHERE project=? AND path NOT IN(SELECT value FROM json_each(?))");if(q){bind(q,1,id);if(!reset)bind(q,2,json_object_to_json_string_ext(paths,JSON_C_TO_STRING_PLAIN));}if(!done(q))return 0;
 if(versions){json_object_object_foreach(versions,path,hash){q=prep(s,"INSERT OR REPLACE INTO project_reference_baselines(project,path,sha256) VALUES(?,?,?)");if(q){bind(q,1,id);bind(q,2,path);bind(q,3,json_object_get_string(hash));}if(!done(q))return 0;}}return 1;
}
J *dc_project_reference_list(dc_store *s,J *project){
 J *paths=get(project,"context"),*out=json_object_new_array();sqlite3_stmt *q=prep(s,"SELECT sha256 FROM project_reference_baselines WHERE project=? AND path=?");if(!q){json_object_put(out);return NULL;}
 for(size_t i=0;paths&&i<json_object_array_length(paths);i++){
  const char *path=json_object_get_string(json_object_array_get_idx(paths,i));J *row=json_object_new_object();add(row,"path",path);
  J *scope=get(project,"referenceBaselineApplicable");int applicable=!scope||json_object_get_boolean(scope);
  sqlite3_reset(q);sqlite3_clear_bindings(q);bind(q,1,applicable?str(project,"id"):"");bind(q,2,path);int rc=sqlite3_step(q);if(rc==SQLITE_ROW)add(row,"baselineSHA",(const char*)sqlite3_column_text(q,0));else if(rc!=SQLITE_DONE){json_object_put(row);json_object_put(out);sqlite3_finalize(q);return NULL;}
  J *request=json_object_new_object();add(request,"op","files.read");add(request,"path",path);J *doc=json_object_get_boolean(get(project,"available"))?dc_project_file_query_bound(str(project,"path"),str(project,"device"),str(project,"inode"),request):NULL;json_object_put(request);
  const char *current=doc?str(doc,"sha256"):NULL,*baseline=str(row,"baselineSHA");if(current)add(row,"currentSHA",current);
  add(row,"state",!current?"unavailable":!baseline?"unverified":strcmp(current,baseline)?"changed":"current");json_object_put(doc);json_object_array_add(out,row);
 }sqlite3_finalize(q);return out;
}
