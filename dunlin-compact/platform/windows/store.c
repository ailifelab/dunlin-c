#define WIN32_LEAN_AND_MEAN
#include "store.h"
#include <windows.h>
#include <aclapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DWS_PATH 1024
#define DWS_LIMIT (32u * 1024u * 1024u)

struct dws_store { wchar_t directory[DWS_PATH]; HANDLE directory_handle; FILE_ID_INFO identity; };
struct dws_lease {
    dws_store *store;
    HANDLE lock;
    wchar_t id[65];
};

static int valid_id(const char *id) {
    if (!id || !*id || strlen(id) > 64) return 0;
    return id[strspn(id,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")] == 0;
}
static int wide(const char *text,wchar_t *out,size_t capacity) {
    if (!text || !out || capacity > INT_MAX) return 0;
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,out,(int)capacity);
    return n>0 && (size_t)n<=capacity;
}
static int path(const dws_store *s,const wchar_t *name,wchar_t out[DWS_PATH]) {
    int n=_snwprintf_s(out,DWS_PATH,_TRUNCATE,L"%ls\\%ls",s->directory,name);
    return n>0 && n<DWS_PATH;
}
static int tagged(HANDLE h,int directory) {
    FILE_ATTRIBUTE_TAG_INFO info;
    return GetFileInformationByHandleEx(h,FileAttributeTagInfo,&info,sizeof info) &&
        !(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
        (!!(info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)==!!directory);
}
static int same_directory(const dws_store *s) {
    HANDLE h=CreateFileW(s->directory,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    FILE_ID_INFO identity={0};
    int ok=h!=INVALID_HANDLE_VALUE&&tagged(h,1)&&
        GetFileInformationByHandleEx(h,FileIdInfo,&identity,sizeof identity)&&
        identity.VolumeSerialNumber==s->identity.VolumeSerialNumber&&
        !memcmp(&identity.FileId,&s->identity.FileId,sizeof identity.FileId);
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return ok;
}
static int broad_write(PACL acl,WELL_KNOWN_SID_TYPE type) {
    BYTE sid[SECURITY_MAX_SID_SIZE];DWORD n=sizeof sid;
    if(!CreateWellKnownSid(type,NULL,sid,&n))return 1;
    ACCESS_MASK write=FILE_GENERIC_WRITE|DELETE|WRITE_DAC|WRITE_OWNER;
    for(DWORD i=0;i<acl->AceCount;i++) {
        void *entry=NULL;if(!GetAce(acl,i,&entry))return 1;
        ACE_HEADER *header=entry;
        if(header->AceType==ACCESS_ALLOWED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE *ace=entry;
            if(EqualSid(sid,&ace->SidStart) && (ace->Mask&write))return 1;
        }
    }
    return 0;
}
static int secure_new_directory(HANDLE h) {
    HANDLE token=NULL;DWORD need=0;TOKEN_USER *user=NULL;PACL acl=NULL;int ok=0;
    BYTE system_sid[SECURITY_MAX_SID_SIZE],admin_sid[SECURITY_MAX_SID_SIZE];
    DWORD system_n=sizeof system_sid,admin_n=sizeof admin_sid;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))goto done;
    GetTokenInformation(token,TokenUser,NULL,0,&need);user=need?malloc(need):NULL;
    if(!user||!GetTokenInformation(token,TokenUser,user,need,&need) ||
       !CreateWellKnownSid(WinLocalSystemSid,NULL,system_sid,&system_n) ||
       !CreateWellKnownSid(WinBuiltinAdministratorsSid,NULL,admin_sid,&admin_n))goto done;
    EXPLICIT_ACCESS_W access[3]={0};PSID sids[3]={user->User.Sid,system_sid,admin_sid};
    TRUSTEE_TYPE types[3]={TRUSTEE_IS_USER,TRUSTEE_IS_USER,TRUSTEE_IS_GROUP};
    for(size_t i=0;i<3;i++) {
        access[i].grfAccessPermissions=GENERIC_ALL;access[i].grfAccessMode=SET_ACCESS;
        access[i].grfInheritance=NO_INHERITANCE;access[i].Trustee.TrusteeForm=TRUSTEE_IS_SID;
        access[i].Trustee.TrusteeType=types[i];access[i].Trustee.ptstrName=(LPWSTR)sids[i];
    }
    if(SetEntriesInAclW(3,access,NULL,&acl)!=ERROR_SUCCESS)goto done;
    ok=SetSecurityInfo(h,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|
        PROTECTED_DACL_SECURITY_INFORMATION,user->User.Sid,NULL,acl,NULL)==ERROR_SUCCESS;
done:
    if(acl)LocalFree(acl);free(user);if(token)CloseHandle(token);return ok;
}
static int private_directory(HANDLE h) {
    PSID owner=NULL;PACL acl=NULL;PSECURITY_DESCRIPTOR sd=NULL;
    DWORD rc=GetSecurityInfo(h,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
        &owner,NULL,&acl,NULL,&sd);
    if(rc!=ERROR_SUCCESS || !owner || !acl){if(sd)LocalFree(sd);return 0;}
    HANDLE token=NULL;DWORD need=0;TOKEN_USER *user=NULL;int ok=0;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
        GetTokenInformation(token,TokenUser,NULL,0,&need);
        user=need?malloc(need):NULL;
        if(user && GetTokenInformation(token,TokenUser,user,need,&need))
            ok=EqualSid(owner,user->User.Sid) &&
               !broad_write(acl,WinWorldSid) && !broad_write(acl,WinAuthenticatedUserSid);
    }
    free(user);if(token)CloseHandle(token);LocalFree(sd);return ok;
}
int dws_open(dws_store **out,const char *utf8_directory) {
    if(out)*out=NULL;
    wchar_t input[DWS_PATH],full[DWS_PATH];
    if(!out || !wide(utf8_directory,input,DWS_PATH))return DWS_FAILED;
    DWORD n=GetFullPathNameW(input,DWS_PATH,full,NULL);
    if(!n || n>=DWS_PATH || !wcsncmp(full,L"\\\\.\\",4) || !wcsncmp(full,L"\\\\?\\",4))
        return DWS_FAILED;
    int created=CreateDirectoryW(full,NULL)!=0;
    if(!created && GetLastError()!=ERROR_ALREADY_EXISTS)return DWS_FAILED;
    HANDLE h=CreateFileW(full,READ_CONTROL|FILE_READ_ATTRIBUTES|(created?(WRITE_DAC|WRITE_OWNER):0),
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE)return DWS_FAILED;
    int ok=tagged(h,1)&&(!created||secure_new_directory(h))&&private_directory(h);
    if(!ok){CloseHandle(h);return DWS_FAILED;}
    dws_store *s=calloc(1,sizeof *s);if(!s){CloseHandle(h);return DWS_FAILED;}
    if(!GetFileInformationByHandleEx(h,FileIdInfo,&s->identity,sizeof s->identity)) {
        CloseHandle(h);free(s);return DWS_FAILED;
    }
    wcscpy_s(s->directory,DWS_PATH,full);s->directory_handle=h;*out=s;return DWS_OK;
}
void dws_close(dws_store *s){if(s){if(s->directory_handle!=INVALID_HANDLE_VALUE)CloseHandle(s->directory_handle);free(s);}}
static int filename(const char *id,const wchar_t *suffix,wchar_t out[80]) {
    wchar_t name[65];
    if(!valid_id(id)||!wide(id,name,65))return 0;
    return _snwprintf_s(out,80,_TRUNCATE,L"%ls%ls",name,suffix)>0;
}
int dws_acquire(dws_store *s,const char *id,dws_lease **out) {
    if(out)*out=NULL;wchar_t name[80],full[DWS_PATH];
    if(!s||!out||!same_directory(s)||!filename(id,L".lock",name)||!path(s,name,full))return DWS_FAILED;
    HANDLE h=CreateFileW(full,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_SHARING_VIOLATION?DWS_BUSY:DWS_FAILED;
    if(!tagged(h,0)){CloseHandle(h);return DWS_FAILED;}
    dws_lease *l=calloc(1,sizeof *l);if(!l){CloseHandle(h);return DWS_FAILED;}
    l->store=s;l->lock=h;wide(id,l->id,65);*out=l;return DWS_OK;
}
void dws_release(dws_lease *l){if(l){if(l->lock!=INVALID_HANDLE_VALUE)CloseHandle(l->lock);free(l);}}
int dws_read(dws_store *s,const char *id,void **data,size_t *size) {
    if(data)*data=NULL;if(size)*size=0;wchar_t name[80],full[DWS_PATH];
    if(!s||!data||!size||!same_directory(s)||!filename(id,L".json",name)||!path(s,name,full))return DWS_FAILED;
    HANDLE h=CreateFileW(full,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND?DWS_MISSING:DWS_FAILED;
    LARGE_INTEGER length={0};int ok=tagged(h,0)&&GetFileSizeEx(h,&length)&&length.QuadPart>0&&length.QuadPart<=DWS_LIMIT;
    size_t total=ok?(size_t)length.QuadPart:0;unsigned char *p=ok?malloc(total):NULL;size_t at=0;
    while(p&&at<total){DWORD chunk=(DWORD)(total-at),got=0;if(!ReadFile(h,p+at,chunk,&got,NULL)||!got)break;at+=got;}
    CloseHandle(h);
    if(!p||at!=total){free(p);return DWS_FAILED;}
    *data=p;*size=total;return DWS_OK;
}
int dws_save(dws_lease *l,const void *data,size_t size) {
    if(!l||!data||!size||size>DWS_LIMIT||!same_directory(l->store))return DWS_FAILED;
    wchar_t tmpname[80],name[80],tmp[DWS_PATH],final[DWS_PATH];
    if(_snwprintf_s(tmpname,80,_TRUNCATE,L".%ls.tmp",l->id)<=0 ||
       _snwprintf_s(name,80,_TRUNCATE,L"%ls.json",l->id)<=0 ||
       !path(l->store,tmpname,tmp)||!path(l->store,name,final))return DWS_FAILED;
    if(!DeleteFileW(tmp) && GetLastError()!=ERROR_FILE_NOT_FOUND)return DWS_FAILED;
    HANDLE h=CreateFileW(tmp,GENERIC_WRITE,0,NULL,CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE||!tagged(h,0)){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return DWS_FAILED;}
    size_t at=0;const unsigned char *p=data;int ok=1;
    while(at<size){DWORD chunk=(DWORD)(size-at),wrote=0;if(!WriteFile(h,p+at,chunk,&wrote,NULL)||!wrote){ok=0;break;}at+=wrote;}
    if(ok)ok=FlushFileBuffers(h);if(!CloseHandle(h))ok=0;
    if(ok)ok=MoveFileExW(tmp,final,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(!ok)DeleteFileW(tmp);return ok?DWS_OK:DWS_FAILED;
}
static int compare_ids(const void *left,const void *right) {
    const char *const *a=left,*const *b=right;return strcmp(*a,*b);
}
void dws_list_free(char **ids,size_t count) {
    if(ids){for(size_t i=0;i<count;i++)free(ids[i]);free(ids);}
}
int dws_list(dws_store *s,char ***ids,size_t *count) {
    if(ids)*ids=NULL;if(count)*count=0;
    if(!s||!ids||!count||!same_directory(s))return DWS_FAILED;
    wchar_t pattern[DWS_PATH];
    if(!path(s,L"*.json",pattern))return DWS_FAILED;
    WIN32_FIND_DATAW found;HANDLE search=FindFirstFileW(pattern,&found);
    if(search==INVALID_HANDLE_VALUE)
        return GetLastError()==ERROR_FILE_NOT_FOUND?DWS_OK:DWS_FAILED;
    char **items=NULL;size_t used=0,capacity=0;int ok=1;
    do {
        size_t length=wcslen(found.cFileName);
        if(length<=5 || wcscmp(found.cFileName+length-5,L".json"))continue;
        wchar_t id_wide[65];size_t id_length=length-5;
        if(id_length>64)continue;
        memcpy(id_wide,found.cFileName,id_length*sizeof *id_wide);id_wide[id_length]=0;
        char id[65];int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,id_wide,-1,id,sizeof id,NULL,NULL);
        if(bytes<=0||!valid_id(id))continue;
        wchar_t full[DWS_PATH];if(!path(s,found.cFileName,full)){ok=0;break;}
        HANDLE file=CreateFileW(full,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,
            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        int regular=file!=INVALID_HANDLE_VALUE&&tagged(file,0);
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        if(!regular)continue;
        if(used==capacity) {
            size_t next=capacity?capacity*2:8;
            char **grown=realloc(items,next*sizeof *items);
            if(!grown){ok=0;break;}items=grown;capacity=next;
        }
        items[used]=_strdup(id);if(!items[used]){ok=0;break;}used++;
    } while(FindNextFileW(search,&found));
    if(GetLastError()!=ERROR_NO_MORE_FILES)ok=0;
    FindClose(search);
    if(!ok){dws_list_free(items,used);return DWS_FAILED;}
    if(used>1)qsort(items,used,sizeof *items,compare_ids);
    *ids=items;*count=used;return DWS_OK;
}
