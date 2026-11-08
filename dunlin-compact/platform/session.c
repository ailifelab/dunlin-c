#define _GNU_SOURCE
#include "dunlin/compact/session.h"
#include "dunlin/compact/json.h"
#include <json-c/json.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* Keep the injection point outside libc declarations and fortified wrappers. */
#ifdef DUNLIN_SESSION_FAULTS
extern int dunlin_profile_test_openat(int directory,const char *name,int flags,...);
#define SESSION_OPENAT dunlin_profile_test_openat
#else
#define SESSION_OPENAT openat
#endif

#define SNAPSHOT_LIMIT (32u * 1024 * 1024)
typedef struct { int dir; } store;
typedef struct { int dir, lock; char file[80], tmp[80]; } session;

static int valid_id(const char *id) {
    return id && *id && strlen(id) <= 64 &&
        id[strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")] == 0;
}
static int private_file_mode(int fd,struct stat *st,int snapshot) {
    return fstat(fd,st)==0 && S_ISREG(st->st_mode) &&
        (st->st_nlink==1 || (snapshot && st->st_nlink==0)) &&
        st->st_uid == geteuid() && !(st->st_mode & 077);
}
static int private_file(int fd,struct stat *st) {return private_file_mode(fd,st,0);}
static ah_status read_state(int dir,const char *file,struct json_object **state,int snapshot) {
    *state=NULL;
    int fd=SESSION_OPENAT(dir,file,O_NOFOLLOW|O_CLOEXEC|O_RDONLY|O_NONBLOCK);
    if(fd<0) return errno==ENOENT?AH_DONE:AH_FAILED;
    struct stat st;
    char *data=NULL;
    size_t offset=0;
    /* 无锁 snapshot 的 fd 可能已被合法原子替换而 nlink=0；它仍固定旧的
     * 已提交 inode。写租约与锁文件保持 nlink==1，不能随此分支放宽。 */
    if(private_file_mode(fd,&st,snapshot) && st.st_size>0 && st.st_size<=SNAPSHOT_LIMIT)
        data=malloc((size_t)st.st_size+1);
    while(data && offset<(size_t)st.st_size) {
        ssize_t n=read(fd,data+offset,(size_t)st.st_size-offset);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) break;
        offset+=(size_t)n;
    }
    close(fd);
    *state=data && offset==(size_t)st.st_size?dunlin_json_parse(data,offset):NULL;
    free(data);
    return *state?AH_DONE:AH_FAILED;
}
ah_status ah_session_read_snapshot(const ah_session_service *out,const char *id,struct json_object **state) {
    if(state) *state=NULL;
    if(!out || !out->ctx || !state || !valid_id(id)) return AH_FAILED;
    char file[80];snprintf(file,sizeof file,"%s.json",id);
    /* save 使用原子 rename；持有读 fd 即锁定一次已提交快照，不读取 .tmp。 */
    return read_state(((store *)out->ctx)->dir,file,state,1);
}
static void end(void *p) {
    session *s = p;
    if (s->lock >= 0) close(s->lock);
    free(s);
}
static ah_status begin(void *ctx, const char *id, void **lease, struct json_object **state) {
    if (!valid_id(id)) return AH_FAILED;
    session *s = calloc(1, sizeof(*s));
    if (!s) return AH_FAILED;
    s->dir = ((store *)ctx)->dir;
    char lockname[80];
    snprintf(lockname, sizeof(lockname), "%s.lock", id);
    snprintf(s->file, sizeof(s->file), "%s.json", id);
    snprintf(s->tmp, sizeof(s->tmp), ".%s.tmp", id);
    s->lock = SESSION_OPENAT(s->dir, lockname, O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|O_RDWR, 0600);
    struct stat st;
    if (s->lock < 0 || !private_file(s->lock, &st)) { end(s); return AH_FAILED; }
    if (flock(s->lock, LOCK_EX|LOCK_NB)) {
        ah_status status = errno == EWOULDBLOCK ? AH_BUSY : AH_FAILED;
        end(s);
        return status;
    }
    if(read_state(s->dir,s->file,state,0)!=AH_DONE) {end(s);return AH_FAILED;}
    *lease = s;
    return AH_DONE;
}
static int save(void *lease, struct json_object *state) {
    session *s = lease;
    const char *data = json_object_to_json_string_ext(state, JSON_C_TO_STRING_PLAIN);
    if (!data) return -1;
    size_t len = strlen(data);
    if (len > SNAPSHOT_LIMIT) return -1;
    if (unlinkat(s->dir, s->tmp, 0) && errno != ENOENT) return -1;
    int fd = SESSION_OPENAT(s->dir, s->tmp, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = write(fd, data + offset, len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += n;
    }
    int ok = offset == len && !fsync(fd);
    if (close(fd)) ok = 0;
    if (ok) ok = renameat(s->dir, s->tmp, s->dir, s->file) == 0;
    if (ok) ok = fsync(s->dir) == 0;
    if (!ok) unlinkat(s->dir, s->tmp, 0);
    return ok ? 0 : -1;
}
int ah_linux_sessions(ah_session_service *out, const char *directory) {
    if (!out || !directory || (mkdir(directory, 0700) && errno != EEXIST)) return -1;
    int fd = open(directory, O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    store *p = NULL;
    if (!fstat(fd, &st) && st.st_uid == geteuid() && !(st.st_mode & 077))
        p = malloc(sizeof(*p));
    if (!p) { close(fd); return -1; }
    p->dir = fd;
    *out = (ah_session_service){p, begin, save, end};
    return 0;
}
void ah_linux_sessions_close(ah_session_service *out) {
    if (out && out->ctx) {
        close(((store *)out->ctx)->dir);
        free(out->ctx);
        *out = (ah_session_service){0};
    }
}
struct json_object *ah_session_list(const ah_session_service *out) {
    int fd = SESSION_OPENAT(((store *)out->ctx)->dir, ".", O_RDONLY|O_CLOEXEC|O_DIRECTORY);
    if (fd < 0) return NULL;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return NULL; }
    struct json_object *list = json_object_new_array();
    struct dirent *item;
    while (list && (item = readdir(dir))) {
        size_t n = strlen(item->d_name);
        if (n <= 5 || n > 69 || strcmp(item->d_name+n-5, ".json")) continue;
        char id[65] = {0};
        memcpy(id, item->d_name, n-5);
        struct stat st;
        if (valid_id(id) && !fstatat(fd,item->d_name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode)) {
            struct json_object *value=json_object_new_string(id);
            if(!value || json_object_array_add(list,value)) {
                json_object_put(value);json_object_put(list);list=NULL;break;
            }
        }
    }
    closedir(dir);
    return list;
}
