#define WIN32_LEAN_AND_MEAN
#include "../dunlin-compact/platform/windows/store.h"
#include <windows.h>
#include <aclapi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_insecure(const char *path) {
    wchar_t wide[MAX_PATH];assert(MultiByteToWideChar(CP_UTF8,0,path,-1,wide,MAX_PATH));
    BYTE sid[SECURITY_MAX_SID_SIZE];DWORD n=sizeof sid;assert(CreateWellKnownSid(WinWorldSid,NULL,sid,&n));
    EXPLICIT_ACCESS_W access={0};access.grfAccessPermissions=GENERIC_ALL;access.grfAccessMode=SET_ACCESS;
    access.grfInheritance=NO_INHERITANCE;access.Trustee.TrusteeForm=TRUSTEE_IS_SID;
    access.Trustee.TrusteeType=TRUSTEE_IS_WELL_KNOWN_GROUP;access.Trustee.ptstrName=(LPWSTR)sid;
    PACL acl=NULL;assert(SetEntriesInAclW(1,&access,NULL,&acl)==ERROR_SUCCESS);
    assert(SetNamedSecurityInfoW(wide,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
        NULL,NULL,acl,NULL)==ERROR_SUCCESS);LocalFree(acl);
}

static int hold_lease(const char *dir,const char *ready) {
    dws_store *store=NULL;dws_lease *lease=NULL;
    if(dws_open(&store,dir)!=DWS_OK||dws_acquire(store,"crash",&lease)!=DWS_OK)return 2;
    HANDLE marker=CreateFileA(ready,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(marker==INVALID_HANDLE_VALUE)return 3;CloseHandle(marker);Sleep(INFINITE);
    dws_release(lease);dws_close(store);return 4;
}

static void touch(const char *path) {
    HANDLE file=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    assert(file!=INVALID_HANDLE_VALUE);CloseHandle(file);
}

static void crash_release(dws_store *store,const char *dir) {
    char exe[MAX_PATH],ready[MAX_PATH],command[MAX_PATH*3];
    assert(GetModuleFileNameA(NULL,exe,MAX_PATH));snprintf(ready,sizeof ready,"%s\\child.ready",dir);
    snprintf(command,sizeof command,"\"%s\" --hold \"%s\" \"%s\"",exe,dir,ready);
    STARTUPINFOA startup={.cb=sizeof startup};PROCESS_INFORMATION child={0};
    assert(CreateProcessA(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&child));
    for(unsigned i=0;i<100&&GetFileAttributesA(ready)==INVALID_FILE_ATTRIBUTES;i++)Sleep(10);
    assert(GetFileAttributesA(ready)!=INVALID_FILE_ATTRIBUTES);
    dws_lease *lease=NULL;assert(dws_acquire(store,"crash",&lease)==DWS_BUSY&&!lease);
    assert(TerminateProcess(child.hProcess,77));assert(WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    assert(dws_acquire(store,"crash",&lease)==DWS_OK);dws_release(lease);
    assert(DeleteFileA(ready));char lock[MAX_PATH];snprintf(lock,sizeof lock,"%s\\crash.lock",dir);assert(DeleteFileA(lock));
}

int main(int argc,char **argv) {
    if(argc==4&&!strcmp(argv[1],"--hold"))return hold_lease(argv[2],argv[3]);
    char temp[MAX_PATH],dir[MAX_PATH];
    assert(GetTempPathA(MAX_PATH,temp));
    snprintf(dir,sizeof dir,"%sdunlin-store-%lu",temp,(unsigned long)GetCurrentProcessId());
    char insecure[MAX_PATH];snprintf(insecure,sizeof insecure,"%s-insecure",dir);
    assert(CreateDirectoryA(insecure,NULL));make_insecure(insecure);
    dws_store *rejected=NULL;assert(dws_open(&rejected,insecure)==DWS_FAILED&&!rejected);
    assert(RemoveDirectoryA(insecure));
    char target[MAX_PATH],link[MAX_PATH];
    snprintf(target,sizeof target,"%s-target",dir);snprintf(link,sizeof link,"%s-link",dir);
    assert(CreateDirectoryA(target,NULL));
    assert(CreateSymbolicLinkA(link,target,SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
    assert(dws_open(&rejected,link)==DWS_FAILED&&!rejected);
    assert(RemoveDirectoryA(link)&&RemoveDirectoryA(target));
    dws_store *store=NULL;assert(dws_open(&store,dir)==DWS_OK);
    char moved[MAX_PATH];snprintf(moved,sizeof moved,"%s-moved",dir);
    char **ids=(char **)1;size_t count=99;assert(dws_list(store,&ids,&count)==DWS_OK&&!ids&&!count);
    assert(MoveFileA(dir,moved));assert(CreateDirectoryA(dir,NULL));
    assert(dws_list(store,&ids,&count)==DWS_FAILED&&!ids&&!count);
    assert(RemoveDirectoryA(dir)&&MoveFileA(moved,dir));
    dws_lease *bad=NULL;assert(dws_acquire(store,"../bad",&bad)==DWS_FAILED&&!bad);
    dws_lease *first=NULL,*second=NULL;
    assert(dws_acquire(store,"sample",&first)==DWS_OK);
    assert(dws_acquire(store,"sample",&second)==DWS_BUSY&&!second);
    void *data=NULL;size_t size=0;assert(dws_read(store,"sample",&data,&size)==DWS_MISSING&&!data&&!size);
    const char one[]="{\"version\":1}";assert(dws_save(first,one,sizeof one-1)==DWS_OK);
    assert(dws_read(store,"sample",&data,&size)==DWS_OK&&size==sizeof one-1&&!memcmp(data,one,size));free(data);
    const char two[]="{\"version\":2,\"active\":false}";assert(dws_save(first,two,sizeof two-1)==DWS_OK);
    assert(dws_save(first,two,0)==DWS_FAILED);
    assert(dws_read(store,"sample",&data,&size)==DWS_OK&&size==sizeof two-1&&!memcmp(data,two,size));free(data);
    dws_release(first);assert(dws_acquire(store,"sample",&second)==DWS_OK);dws_release(second);
    char path[MAX_PATH],alias[MAX_PATH];
    snprintf(path,sizeof path,"%s\\zed.json",dir);touch(path);
    snprintf(path,sizeof path,"%s\\bad!.json",dir);touch(path);
    snprintf(path,sizeof path,"%s\\ignored.lock",dir);touch(path);
    snprintf(path,sizeof path,"%s\\folder.json",dir);assert(CreateDirectoryA(path,NULL));
    snprintf(alias,sizeof alias,"%s\\alias.json",dir);
    snprintf(path,sizeof path,"%s\\sample.json",dir);
    assert(CreateSymbolicLinkA(alias,path,0));
    assert(dws_list(store,&ids,&count)==DWS_OK&&count==2);
    assert(!strcmp(ids[0],"sample")&&!strcmp(ids[1],"zed"));dws_list_free(ids,count);
    assert(DeleteFileA(alias));
    snprintf(path,sizeof path,"%s\\folder.json",dir);assert(RemoveDirectoryA(path));
    snprintf(path,sizeof path,"%s\\ignored.lock",dir);assert(DeleteFileA(path));
    snprintf(path,sizeof path,"%s\\bad!.json",dir);assert(DeleteFileA(path));
    snprintf(path,sizeof path,"%s\\zed.json",dir);assert(DeleteFileA(path));
    crash_release(store,dir);dws_close(store);
    char file[MAX_PATH];snprintf(file,sizeof file,"%s\\sample.json",dir);assert(DeleteFileA(file));
    snprintf(file,sizeof file,"%s\\sample.lock",dir);assert(DeleteFileA(file));assert(RemoveDirectoryA(dir));
    puts("Windows store tests passed");return 0;
}
