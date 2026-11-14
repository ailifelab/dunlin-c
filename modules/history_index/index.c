#define _XOPEN_SOURCE 700
#include "dunlin/history_index.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
struct dl_index {
    sqlite3 *db;
    int last_error;
}
;
static char *copy_text(const char *s){
    size_t n=strlen(s)+1;
    char *p=malloc(n);
    if(p)memcpy(p,s,n);
    return p;
}
/* Bounded search shadow; CJK pairs are retrieval units, not semantic words. */
char *dl_index_terms(const char *s,int query) {
    if(!s)return NULL;
    size_t n=strlen(s);
    if(n>262144)return NULL;
    char *out=calloc(n*5+64,1);
    if(!out)return NULL;
    size_t k=0,i=0,prev=0,plen=0;
    int tokens=0;
    while(i<n) {
        unsigned char c=(unsigned char)s[i];
        size_t len=c<128?1:(c>=0xc2&&c<=0xdf?2:(c>=0xe0&&c<=0xef?3:(c>=0xf0&&c<=0xf4?4:0)));
        if(!len||i+len>n){
            free(out);
            return NULL;
        }
        for(size_t j=1;j<len;j++)if(((unsigned char)s[i+j]&0xc0)!=0x80){
            free(out);
            return NULL;
        }
        if((len==3&&c==0xe0&&(unsigned char)s[i+1]<0xa0)||(len==3&&c==0xed&&(unsigned char)s[i+1]>=0xa0)||(len==4&&c==0xf0&&(unsigned char)s[i+1]<0x90)||(len==4&&c==0xf4&&(unsigned char)s[i+1]>=0x90)){
            free(out);
            return NULL;
        }
        int han=len==3 && c>=0xe3;
        /* Unicode BMP CJK baseline, supplementary characters are separate terms. */
        size_t from=i,take=len;
        if(c<128&&isalnum(c)) {
            plen=0;
            while(i<n&&((unsigned char)s[i]<128)&&(isalnum((unsigned char)s[i])||s[i]=='_'))i++;
            take=i-from;
        }
        else {
            i+=len;
            if(han&&plen){
                from=prev;
                take=plen+len;
            }
            if(han){
                prev=i-len;
                plen=len;
            }
            else plen=0;
            if(c<128)continue;
        }
        if(query&&tokens>=32)break;
        if(tokens++) {
            if(query){
                memcpy(out+k," OR ",4);
                k+=4;
            }
            else out[k++]=' ';
        }
        if(query)out[k++]='"';
        for(size_t j=0;j<take;j++)out[k++]=((unsigned char)s[from+j]<128?(char)tolower((unsigned char)s[from+j]):s[from+j]);
        if(query)out[k++]='"';
    }
    return out;
}
dl_index *dl_index_open(const char *path) {
    /* macOS /var and /tmp are system links. Resolve the parent only, so
     * SQLITE_OPEN_NOFOLLOW still rejects a linked database leaf. */
    if(!path||strlen(path)>=PATH_MAX)return NULL;
    char parent[PATH_MAX],canonical[PATH_MAX],resolved[PATH_MAX];
    snprintf(parent,sizeof parent,"%s",path);
    char *slash=strrchr(parent,'/');
    const char *leaf=slash?slash+1:path;
    char name[PATH_MAX];snprintf(name,sizeof name,"%s",leaf);
    if(!*name||!strcmp(name,".")||!strcmp(name,".."))return NULL;
    if(slash){if(slash==parent)slash[1]=0;else *slash=0;}else snprintf(parent,sizeof parent,".");
    if(!realpath(parent,resolved)||snprintf(canonical,sizeof canonical,"%s/%s",resolved,name)>=(int)sizeof canonical)return NULL;
    struct stat st;
    if(!lstat(canonical,&st)){if(!S_ISREG(st.st_mode)||st.st_nlink!=1)return NULL;}
    else if(errno!=ENOENT)return NULL;
    dl_index *x=calloc(1,sizeof *x);
    if(!x)return NULL;
    int flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags|=SQLITE_OPEN_NOFOLLOW;
#endif
    if(sqlite3_open_v2(canonical,&x->db,flags,NULL)!=SQLITE_OK || sqlite3_exec(x->db,"CREATE VIRTUAL TABLE IF NOT EXISTS history_fts USING fts5(scope UNINDEXED,chat UNINDEXED,sequence UNINDEXED,text UNINDEXED,terms);",0,0,0)!=SQLITE_OK){
        dl_index_close(x);
        return NULL;
    }
    return x;
}
void dl_index_close(dl_index *x){
    if(x){
        sqlite3_close(x->db);
        free(x);
    }
}
void dl_index_free(void *p){
    free(p);
}
int dl_index_put(dl_index *x,const char *scope,const char *chat,int sequence,const char *text){
    if(!x||!scope||!chat||!text||sequence<0)return 0;
    char *t=dl_index_terms(text,0);
    if(!t){x->last_error=SQLITE_ERROR;return 0;}
    sqlite3_stmt *s=NULL;
    int ok=sqlite3_exec(x->db,"BEGIN IMMEDIATE",0,0,0)==SQLITE_OK;
    if(ok)ok=sqlite3_prepare_v2(x->db,"DELETE FROM history_fts WHERE scope=? AND chat=? AND sequence=?",-1,&s,0)==SQLITE_OK;
    if(ok){
        sqlite3_bind_text(s,1,scope,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,2,chat,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(s,3,sequence);
        ok=sqlite3_step(s)==SQLITE_DONE;
    }
    sqlite3_finalize(s);
    s=NULL;
    if(ok)ok=sqlite3_prepare_v2(x->db,"INSERT INTO history_fts VALUES(?,?,?,?,?)",-1,&s,0)==SQLITE_OK;
    if(ok){
        sqlite3_bind_text(s,1,scope,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,2,chat,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(s,3,sequence);
        sqlite3_bind_text(s,4,text,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,5,t,-1,SQLITE_TRANSIENT);
        ok=sqlite3_step(s)==SQLITE_DONE;
    }
    sqlite3_finalize(s);
    free(t);
    x->last_error=ok?SQLITE_OK:sqlite3_errcode(x->db);
    if(!ok){
        /* SQLITE_FULL may already have rolled the transaction back. Preserve
         * that cause instead of replacing it with "no transaction active". */
        sqlite3_exec(x->db,"ROLLBACK",0,0,0);
    }else if(sqlite3_exec(x->db,"COMMIT",0,0,0)!=SQLITE_OK){
        x->last_error=sqlite3_errcode(x->db);sqlite3_exec(x->db,"ROLLBACK",0,0,0);ok=0;
    }
    return ok;
}
int dl_index_delete(dl_index *x,const char *scope,const char *chat){
    sqlite3_stmt *s=NULL;
    if(!x||!scope||!chat||sqlite3_prepare_v2(x->db,"DELETE FROM history_fts WHERE scope=? AND chat=?",-1,&s,0)!=SQLITE_OK)return 0;
    sqlite3_bind_text(s,1,scope,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,chat,-1,SQLITE_TRANSIENT);
    int ok=sqlite3_step(s)==SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
char *dl_index_search(dl_index *x,const char *scope,const char *query){
    return dl_index_search_page(x,scope,query,0);
}
char *dl_index_search_page(dl_index *x,const char *scope,const char *query,int offset){
    if(!x||!scope||!query||offset<0)return NULL;
    char *t=dl_index_terms(query,1);
    if(!t)return NULL;
    sqlite3_stmt *s=NULL;
    if(!*t){
        free(t);
        return copy_text("[]");
    }
    int ok=sqlite3_prepare_v2(x->db,"SELECT chat,sequence,text,snippet(history_fts,4,'[',']','…',12) FROM history_fts WHERE history_fts MATCH ? AND scope=? ORDER BY bm25(history_fts),rowid LIMIT 50 OFFSET ?",-1,&s,0)==SQLITE_OK;
    if(ok){
        sqlite3_bind_text(s,1,t,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,2,scope,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(s,3,offset);
    }
    free(t);
    sqlite3_str *out=sqlite3_str_new(NULL);
    sqlite3_str_appendall(out,"[");
    int count=0,rc=SQLITE_DONE;
    while(ok&&(rc=sqlite3_step(s))==SQLITE_ROW){
        count++;
        sqlite3_stmt *q=NULL;
        if(sqlite3_prepare_v2(x->db,"SELECT json_object('chat',?,'sequence',?,'text',?,'matchSnippet',?)",-1,&q,0)!=SQLITE_OK){
            ok=0;
            break;
        }
        sqlite3_bind_text(q,1,(const char*)sqlite3_column_text(s,0),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(q,2,sqlite3_column_int(s,1));
        sqlite3_bind_text(q,3,(const char*)sqlite3_column_text(s,2),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(q,4,(const char*)sqlite3_column_text(s,3),-1,SQLITE_TRANSIENT);
        if(sqlite3_step(q)==SQLITE_ROW){
            if(count>1)sqlite3_str_appendall(out,",");
            sqlite3_str_appendall(out,(const char*)sqlite3_column_text(q,0));
        }
        else ok=0;
        sqlite3_finalize(q);
    }
    sqlite3_finalize(s);
    sqlite3_str_appendall(out,"]");
    char *raw=sqlite3_str_finish(out);
    char *result=ok&&rc==SQLITE_DONE&&raw?copy_text(raw):NULL;
    sqlite3_free(raw);
    return result;
}
static long long scalar(dl_index *x,const char *sql){
 if(!x)return -1;sqlite3_stmt *q=NULL;if(sqlite3_prepare_v2(x->db,sql,-1,&q,NULL)!=SQLITE_OK)return -1;long long v=sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int64(q,0):-1;sqlite3_finalize(q);return v;
}
size_t dl_index_bytes(dl_index *x){long long pages=scalar(x,"PRAGMA page_count"),size=scalar(x,"PRAGMA page_size");return pages<0||size<0||(unsigned long long)pages>SIZE_MAX/(unsigned long long)(size?size:1)?0:(size_t)pages*(size_t)size;}
long long dl_index_count(dl_index *x){return scalar(x,"SELECT count(*) FROM history_fts");}
int dl_index_last_error(dl_index *x){return x?x->last_error:SQLITE_MISUSE;}
int dl_index_limit_bytes(dl_index *x,size_t bytes){
 long long page=scalar(x,"PRAGMA page_size");if(page<=0||bytes<(size_t)page)return 0;size_t pages=bytes/(size_t)page;if(pages>2147483646)pages=2147483646;char sql[96];snprintf(sql,sizeof sql,"PRAGMA max_page_count=%zu",pages);long long actual=scalar(x,sql);return actual>=0&&(unsigned long long)actual<=pages;
}
