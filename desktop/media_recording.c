#define _POSIX_C_SOURCE 200809L
#include "media_recording.h"
#include "dunlin/content_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
typedef struct json_object J;
struct dc_media_capture {FILE *file;dl_record_writer *writer;char call[65];int status,video;};
static sqlite3_stmt *prep(dc_store *s,const char *sql){sqlite3_stmt *q=NULL;return sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)==SQLITE_OK?q:NULL;}
static void bind(sqlite3_stmt *q,int n,const char *v){sqlite3_bind_text(q,n,v,-1,SQLITE_TRANSIENT);}
static int done(sqlite3_stmt *q){int ok=q&&sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);return ok;}
static int path_for(dc_store *s,const char *call,char out[PATH_MAX],int create){
 if(!call||!*call||strlen(call)>64)return 0;char directory[PATH_MAX],hash[65];dc_sha256(call,strlen(call),hash);
 if(snprintf(directory,sizeof directory,"%s/media-recordings",s->directory)>=(int)sizeof directory)return 0;
 if(create&&mkdir(directory,0700)&&errno!=EEXIST)return 0;struct stat st;if(lstat(directory,&st))return 0;if(!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){errno=EINVAL;return 0;}
 return snprintf(out,PATH_MAX,"%s/%s.dcr",directory,hash)<PATH_MAX;
}
int dc_recording_schema(dc_store *s){return sqlite3_exec(s->db,
 "CREATE TABLE IF NOT EXISTS media_recording_policy(call TEXT PRIMARY KEY,enabled INTEGER NOT NULL DEFAULT 0);"
 "CREATE TABLE IF NOT EXISTS media_recordings(call TEXT PRIMARY KEY,state TEXT NOT NULL,bytes INTEGER NOT NULL DEFAULT 0);"
 "UPDATE media_recordings SET state='interrupted' WHERE state='writing';",NULL,NULL,NULL)==SQLITE_OK;}
static size_t write_file(void *p,const void *b,size_t n){return fwrite(b,1,n,p);}
static size_t read_file(void *p,void *b,size_t n){return fread(b,1,n,p);}
dc_media_capture *dc_media_recording_open(dc_store *s,const char *call,unsigned long long limit){
 if(!s||!call||!*call||strlen(call)>64||limit<40||limit>2147483648ULL)return NULL;
 sqlite3_stmt *q=prep(s,"SELECT c.mode FROM media_calls c JOIN media_recording_policy p ON p.call=c.id WHERE c.id=? AND p.enabled=1 AND c.state IN('connecting','live') AND NOT EXISTS(SELECT 1 FROM media_recordings r WHERE r.call=c.id)");if(q)bind(q,1,call);int allowed=q&&sqlite3_step(q)==SQLITE_ROW;int video=allowed&&!strcmp((const char*)sqlite3_column_text(q,0),"video");sqlite3_finalize(q);if(!allowed)return NULL;
 char path[PATH_MAX];if(!path_for(s,call,path,1))return NULL;int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return NULL;
 FILE *f=fdopen(fd,"wb");if(!f){close(fd);unlink(path);return NULL;}dc_media_capture *capture=calloc(1,sizeof *capture);if(!capture){fclose(f);unlink(path);return NULL;}
 capture->file=f;capture->video=video;capture->writer=dl_record_open(write_file,f,limit);snprintf(capture->call,sizeof capture->call,"%s",call);
 q=capture->writer?prep(s,"INSERT INTO media_recordings(call,state) VALUES(?,'writing')"):NULL;if(q)bind(q,1,call);
 if(!done(q)){dl_record_close(capture->writer);fclose(f);free(capture);unlink(path);return NULL;}return capture;
}
int dc_media_recording_append(dc_media_capture *capture,int track,unsigned long long stamp,const void *bytes,size_t n){
 if(!capture)return DL_RECORD_INVALID;if(capture->status)return capture->status;if(stamp>3600000ULL||(track==DL_RECORD_VIDEO&&!capture->video)){capture->status=DL_RECORD_INVALID;return capture->status;}capture->status=dl_record_append(capture->writer,(dl_record_track)track,stamp,bytes,n);return capture->status;
}
char *dc_media_recording_finish(dc_store *s,dc_media_capture *capture){
 if(!s||!capture)return NULL;int ended=dl_record_finish(capture->writer);dl_record_close(capture->writer);
 int durable=!fflush(capture->file)&&!fsync(fileno(capture->file));long size=ftell(capture->file);if(fclose(capture->file))durable=0;
 const char *state=!durable||ended!=DL_RECORD_OK?"failed":capture->status?"partial":"ready";
 sqlite3_stmt *q=prep(s,"UPDATE media_recordings SET state=?,bytes=? WHERE call=? AND state='writing'");if(q){bind(q,1,state);sqlite3_bind_int64(q,2,size>=0?size:0);bind(q,3,capture->call);}int ok=done(q)&&sqlite3_changes(s->db)==1;free(capture);
 J *out=json_object_new_object();json_object_object_add(out,"ok",json_object_new_boolean(ok));J *data=json_object_new_object();json_object_object_add(data,"state",json_object_new_string(state));json_object_object_add(data,"bytes",json_object_new_int64(size>=0?size:0));json_object_object_add(out,"data",data);char *raw=strdup(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN));json_object_put(out);return raw;
}
typedef struct {dc_media_recording_visitor visit;void *user;} visitor;
static int visit(void *p,dl_record_track track,uint64_t stamp,const void *bytes,size_t n){visitor *v=p;return v->visit(v->user,(int)track,stamp,bytes,n);}
struct dc_media_replay {FILE *file;};
dc_media_replay *dc_media_recording_read_open(dc_store *s,const char *call){
 if(!s||!call)return NULL;
 sqlite3_stmt *q=prep(s,"SELECT 1 FROM media_recordings r JOIN media_calls c ON c.id=r.call JOIN media_policies p ON p.call=c.id WHERE r.call=? AND r.state IN('ready','partial') AND c.ended IS NOT NULL AND c.ended+p.retention_days*86400>unixepoch()");if(q)bind(q,1,call);int allowed=q&&sqlite3_step(q)==SQLITE_ROW;sqlite3_finalize(q);if(!allowed)return NULL;
 char path[PATH_MAX];if(!path_for(s,call,path,0))return NULL;int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return NULL;struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<0||st.st_size>2147483648LL){close(fd);return NULL;}
 FILE *f=fdopen(fd,"rb");if(!f){close(fd);return NULL;}dc_media_replay *replay=calloc(1,sizeof *replay);if(!replay){fclose(f);return NULL;}replay->file=f;return replay;
}
int dc_media_recording_scan(dc_media_replay *replay,dc_media_recording_visitor callback,void *user){
 if(!replay||!callback)return DL_RECORD_INVALID;visitor v={callback,user};int result=dl_record_scan(read_file,replay->file,2147483648ULL,visit,&v);return ferror(replay->file)?DL_RECORD_IO:result;
}
void dc_media_recording_read_close(dc_media_replay *replay){if(replay){fclose(replay->file);free(replay);}}
static int remove_call(dc_store *s,const char *call){char path[PATH_MAX];if(!path_for(s,call,path,0)){if(errno!=ENOENT)return 0;}else if(unlink(path)&&errno!=ENOENT)return 0;sqlite3_stmt *q=prep(s,"DELETE FROM media_recordings WHERE call=?");if(q)bind(q,1,call);return done(q);}
static int remove_batch(dc_store *s,const char *query,const char *argument,int *count){
 sqlite3_stmt *q=prep(s,query);if(!q)return 0;if(argument)bind(q,1,argument);J *calls=json_object_new_array();int rc;
 while((rc=sqlite3_step(q))==SQLITE_ROW)json_object_array_add(calls,json_object_new_string((const char*)sqlite3_column_text(q,0)));
 sqlite3_finalize(q);int ok=rc==SQLITE_DONE;*count=(int)json_object_array_length(calls);
 for(size_t i=0;ok&&i<json_object_array_length(calls);i++)ok=remove_call(s,json_object_get_string(json_object_array_get_idx(calls,i)));
 json_object_put(calls);return ok;
}
int dc_recording_drop_chat(dc_store *s,const char *chat){
 int count;do{if(!remove_batch(s,"SELECT r.call FROM media_recordings r JOIN media_calls c ON c.id=r.call WHERE c.chat=? LIMIT 128",chat,&count))return 0;}while(count);
 sqlite3_stmt *q=prep(s,"DELETE FROM media_recording_policy WHERE call IN(SELECT id FROM media_calls WHERE chat=?)");if(q)bind(q,1,chat);return done(q);
}
int dc_recording_collect(dc_store *s){int count;return remove_batch(s,"SELECT r.call FROM media_recordings r JOIN media_calls c ON c.id=r.call JOIN media_policies p ON p.call=c.id WHERE c.ended IS NOT NULL AND c.ended+p.retention_days*86400<=unixepoch() LIMIT 128",NULL,&count);}
J *dc_recording_request(dc_store *s,J *r){J *id=NULL;json_object_object_get_ex(r,"id",&id);const char *call=json_object_get_string(id);J *out=json_object_new_object(),*data=json_object_new_object();sqlite3_stmt *q=prep(s,"SELECT r.state,r.bytes FROM media_recordings r JOIN media_calls c ON c.id=r.call JOIN media_policies p ON p.call=c.id WHERE r.call=? AND (c.ended IS NULL OR c.ended+p.retention_days*86400>unixepoch())");if(q)bind(q,1,call?call:"");int rc=q?sqlite3_step(q):SQLITE_ERROR;if(rc==SQLITE_ROW){json_object_object_add(data,"state",json_object_new_string((const char*)sqlite3_column_text(q,0)));json_object_object_add(data,"bytes",json_object_new_int64(sqlite3_column_int64(q,1)));}else json_object_object_add(data,"state",json_object_new_string("unavailable"));sqlite3_finalize(q);json_object_object_add(out,"ok",json_object_new_boolean(rc==SQLITE_ROW||rc==SQLITE_DONE));json_object_object_add(out,"data",data);return out;}
