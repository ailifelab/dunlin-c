#define _XOPEN_SOURCE 700
#include "../desktop/project_files.h"
#include "dunlin/compact/tools.h"
#include <json-c/json.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static char root[4096],moved[4096];static int swap_on_open;
static unsigned result_events;
static uint64_t clock_ms(void *unused){
    (void)unused;struct timespec now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
}
static void event(void *unused,const char *name,const char *text,size_t size){
    (void)unused;
    if(!strcmp(name,"exec.result")){const char *prefix="exit_code=0\n";CHECK(size>=strlen(prefix)&&!memcmp(text,prefix,strlen(prefix)));result_events++;}
}
static void write_source(const char *directory,const char *value){
    char p[4096];CHECK(snprintf(p,sizeof p,"%s/source.txt",directory)<(int)sizeof p);
    int fd=open(p,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);CHECK(write(fd,value,strlen(value))==(ssize_t)strlen(value));CHECK(close(fd)==0);
}
/* Only project_files.c is compiled with -Dopen=dl_fixture_open. The platform
 * command service retains its actual normal open. Swap AFTER the file root
 * opens and BEFORE the independent command root opens, deterministically. */
int dl_fixture_open(const char *path,int flags,...){
    int fd;
    if(flags&O_CREAT){va_list ap;va_start(ap,flags);int mode=va_arg(ap,int);va_end(ap);fd=open(path,flags,mode);}
    else fd=open(path,flags);
    if(fd>=0&&swap_on_open&&!strcmp(path,root)&&(flags&O_DIRECTORY)){
        swap_on_open=0;CHECK(!rename(root,moved));CHECK(!mkdir(root,0700));write_source(root,"replacement");
    }
    return fd;
}
static void ids(const char *path,char dev[32],char ino[32]){
    struct stat st;CHECK(!stat(path,&st));snprintf(dev,32,"%ju",(uintmax_t)st.st_dev);snprintf(ino,32,"%ju",(uintmax_t)st.st_ino);
}
int main(int argc,char **argv){
    CHECK(argc==2);char parent[4096];CHECK(snprintf(parent,sizeof parent,"%s/root-snapshot-XXXXXX",argv[1])<(int)sizeof parent);CHECK(mkdtemp(parent));
    CHECK(snprintf(root,sizeof root,"%s/current",parent)<(int)sizeof root);
    CHECK(snprintf(moved,sizeof moved,"%s/original",parent)<(int)sizeof moved);
    CHECK(!mkdir(root,0700));write_source(root,"original");char old_dev[32],old_ino[32],new_dev[32],new_ino[32];ids(root,old_dev,old_ino);
    ah_tool_service split={0};swap_on_open=1;CHECK(dc_project_tools_open(&split,root,"ask"));ids(root,new_dev,new_ino);
    CHECK(strcmp(old_ino,new_ino));CHECK(!dc_project_tools_identity(&split,old_dev,old_ino));CHECK(!dc_project_tools_identity(&split,new_dev,new_ino));
    dc_project_tools_close(&split);
    ah_tool_service pinned={0};CHECK(dc_project_tools_open(&pinned,root,"ask"));CHECK(dc_project_tools_identity(&pinned,new_dev,new_ino));
    char relocated[4096];CHECK(snprintf(relocated,sizeof relocated,"%s/relocated",parent)<(int)sizeof relocated);CHECK(!rename(root,relocated));CHECK(!mkdir(root,0700));write_source(root,"third directory");
    CHECK(dc_project_tools_identity(&pinned,new_dev,new_ino));CHECK(!dc_project_tools_identity(&pinned,old_dev,old_ino));
    struct json_object *query=json_tokener_parse("{\"op\":\"files.read\",\"path\":\"source.txt\"}");CHECK(query);
    CHECK(!dc_project_file_query_bound(root,new_dev,new_ino,query));
    struct json_object *content=dc_project_file_query_bound(relocated,new_dev,new_ino,query),*v=NULL;CHECK(content);
    CHECK(json_object_object_get_ex(content,"text",&v)&&!strcmp(json_object_get_string(v),"replacement"));json_object_put(content);json_object_put(query);
    /* Reviewed deterministic service-level fixture, no model/Loop approval.
     * The public check must still require exact host approval for /bin/pwd. */
    struct json_object *args=json_tokener_parse("{\"executable\":\"/bin/pwd\",\"argv\":[]}");CHECK(args);
    int approval=0,effect=0;char error[256]={0};CHECK(pinned.check(pinned.ctx,"exec",args,&approval,&effect,error,sizeof error)==AH_DONE);CHECK(approval==1&&effect==1);
    ah_hooks hooks={.now_ms=clock_ms,.event=event};
    char *output=NULL;CHECK(pinned.call(pinned.ctx,"exec",args,&hooks,4096,2000,&output)==AH_DONE);CHECK(output&&result_events==1);
    char canonical[4096],expected[4100];CHECK(realpath(relocated,canonical));CHECK(snprintf(expected,sizeof expected,"%s\n",canonical)<(int)sizeof expected);CHECK(!strcmp(output,expected));
    free(output);json_object_put(args);dc_project_tools_close(&pinned);
    ah_tool_service commands={0};CHECK(!ah_linux_tools(&commands,relocated));uint64_t device=0,inode=0;CHECK(ah_linux_tools_root_identity(&commands,&device,&inode));
    struct stat st;CHECK(!stat(relocated,&st));CHECK(device==(uint64_t)st.st_dev&&inode==(uint64_t)st.st_ino);ah_linux_tools_close(&commands);
    ah_tool_service foreign={.ctx=(void*)1};device=99;inode=99;CHECK(!ah_linux_tools_root_identity(&foreign,&device,&inode)&&!device&&!inode);CHECK(!dc_project_tools_identity(&foreign,new_dev,new_ino));
    CHECK(!ah_linux_tools_root_identity(NULL,&device,&inode));
    puts("PASS real descriptor snapshots: deterministic split-open replacement rejects both identities; held file/command roots survive path rename; bound reads reject replacement; reviewed pwd executes in held original directory; foreign-service queries fail closed");
    const char *paths[]={root,moved,relocated};for(unsigned i=0;i<3;i++){char p[4096];CHECK(snprintf(p,sizeof p,"%s/source.txt",paths[i])<(int)sizeof p);CHECK(!unlink(p));CHECK(!rmdir(paths[i]));}CHECK(!rmdir(parent));return 0;
}
