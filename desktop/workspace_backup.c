#define _XOPEN_SOURCE 700
#include "chat_workspace.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dc_backup_before_upgrade(dc_store *s,const char *component,char relative[160],uint64_t *bytes){
    if(relative)relative[0]=0;if(bytes)*bytes=0;
    if(!s||!relative||!bytes||!component||!*component||strlen(component)>48
       ||strspn(component,"abcdefghijklmnopqrstuvwxyz0123456789-")!=strlen(component)
       ||!sqlite3_get_autocommit(s->db))return 0;
    sqlite3_stmt *query=NULL;
    if(sqlite3_prepare_v2(s->db,"SELECT count(*) FROM chats",-1,&query,NULL)!=SQLITE_OK)return 0;
    int row=sqlite3_step(query),has_source=row==SQLITE_ROW&&sqlite3_column_int64(query,0)>0;
    sqlite3_finalize(query);if(row!=SQLITE_ROW)return 0;if(!has_source)return 1;
    char canonical[4096],directory[4096],temporary[4096],published[4096],suffix[4096];
    /* SQLite NOFOLLOW also rejects parent aliases such as macOS /var. Resolve
     * the already-owned data directory before reserving the backup file. */
    struct stat parent;
    if(!realpath(s->directory,canonical)||lstat(canonical,&parent)||!S_ISDIR(parent.st_mode)
       ||parent.st_uid!=geteuid()||(parent.st_mode&077))return 0;
    int n=snprintf(directory,sizeof directory,"%s/Backups",canonical);
    if(n<0||(size_t)n>=sizeof directory)return 0;
    if(mkdir(directory,0700)&&errno!=EEXIST)return 0;
    struct stat st;
    if(lstat(directory,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))return 0;
    int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return 0;
    n=snprintf(temporary,sizeof temporary,"%s/.before-%s-XXXXXX",directory,component);
    if(n<0||(size_t)n>=sizeof temporary){close(dir);return 0;}
    int fd=mkstemp(temporary);if(fd<0){close(dir);return 0;}
    sqlite3 *destination=NULL;int ok=0,linked=0;
    const char *base=strrchr(temporary,'/');base=base?base+1:temporary;
    n=snprintf(published,sizeof published,"%s/%s.sqlite",directory,base+1);
    if(n<0||(size_t)n>=sizeof published)goto finish;
    if(fcntl(fd,F_SETFD,FD_CLOEXEC)||fchmod(fd,0600)||sqlite3_open_v2(temporary,&destination,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOFOLLOW,NULL)!=SQLITE_OK)goto finish;
    sqlite3_backup *backup=sqlite3_backup_init(destination,"main",s->db,"main");
    if(!backup)goto finish;
    int step=sqlite3_backup_step(backup,-1),settled=sqlite3_backup_finish(backup);
    if(step!=SQLITE_DONE||settled!=SQLITE_OK)goto finish;
    if(sqlite3_close(destination)!=SQLITE_OK)goto finish;destination=NULL;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_nlink!=1||st.st_size<=0||fsync(fd))goto finish;
    /* Same-directory hard-link publication refuses an existing final name.
     * Only our fresh reserved temporary inode is unlinked afterward. */
    if(link(temporary,published))goto finish;linked=1;
    if(unlink(temporary)||fsync(dir))goto finish;
    n=snprintf(relative,160,"Backups/%s.sqlite",base+1);
    if(n<0||n>=160)goto finish;
    *bytes=(uint64_t)st.st_size;ok=1;
finish:
    if(destination)sqlite3_close(destination);
    close(fd);
    if(!ok){relative[0]=0;*bytes=0;if(linked)unlink(published);unlink(temporary);}
    const char *extensions[]={"-wal","-shm","-journal"};
    for(unsigned i=0;i<3;i++){
        n=snprintf(suffix,sizeof suffix,"%s%s",temporary,extensions[i]);
        if(n>0&&(size_t)n<sizeof suffix)unlink(suffix);
    }
    close(dir);return ok;
}
int dc_workspace_backup_before_upgrade(dc_store *s,char relative[160],uint64_t *bytes){
    return dc_backup_before_upgrade(s,"chat-workspaces-v1",relative,bytes);
}

struct json_object *dc_workspace_migration_view(dc_store *s){
    sqlite3_stmt *q=NULL;
    if(sqlite3_prepare_v2(s->db,"SELECT version,backup,backup_bytes,kind,created FROM desktop_workspace_migration WHERE id=1",-1,&q,NULL)!=SQLITE_OK)return NULL;
    struct json_object *out=NULL;
    if(sqlite3_step(q)==SQLITE_ROW){
        const char *backup=(const char*)sqlite3_column_text(q,1),*kind=(const char*)sqlite3_column_text(q,3);
        if(!backup||!kind||strlen(backup)!=(size_t)sqlite3_column_bytes(q,1)||strlen(kind)!=(size_t)sqlite3_column_bytes(q,3))goto done;
        const char *prefix="Backups/before-chat-workspaces-v1-";
        int valid=!*backup||(!strncmp(backup,prefix,strlen(prefix))&&!strstr(backup,"..")&&!strchr(backup+8,'/')&&strlen(backup)<160);
        if(!valid)goto done;
        out=json_object_new_object();if(!out)goto done;
        json_object_object_add(out,"version",json_object_new_int64(sqlite3_column_int64(q,0)));
        json_object_object_add(out,"backup",json_object_new_string(backup));
        json_object_object_add(out,"backupBytes",json_object_new_int64(sqlite3_column_int64(q,2)));
        json_object_object_add(out,"kind",json_object_new_string(kind));
        json_object_object_add(out,"created",json_object_new_int64(sqlite3_column_int64(q,4)));
        char path[4096];struct stat st;int n=snprintf(path,sizeof path,"%s/%s",s->directory,backup);
        int exists=*backup&&n>0&&(size_t)n<sizeof path&&!lstat(path,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&st.st_nlink==1&&!(st.st_mode&077)&&st.st_size==sqlite3_column_int64(q,2);
        json_object_object_add(out,"backupExists",json_object_new_boolean(exists));
    }
done:sqlite3_finalize(q);return out;
}
