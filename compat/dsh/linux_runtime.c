#if defined(__linux__)
#define _XOPEN_SOURCE 700
#include "linux_runtime.h"
#include <unistd.h>
#if defined(__GLIBC__) && (defined(__aarch64__) || defined(__x86_64__))
#include <gnu/libc-version.h>
#include <openssl/evp.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

struct artifact {
    const char *name;
    const char *digest;
};
/* Ubuntu noble-updates libc6 2.39-0ubuntu8.12. Archive digests come from
   Ubuntu's signed Packages index. Only these runtime objects are installed. */
#include "linux_runtime_pins.inc"

static int joined(char out[PATH_MAX], const char *base, const char *name) {
    return snprintf(out, PATH_MAX, "%s/%s", base, name) < PATH_MAX;
}

static int copy_and_verify(const char *source, const char *destination, mode_t mode,
                           const char *digest, off_t limit) {
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) return 0;
    struct stat st;
    int ok = !fstat(in, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= limit;
    int out = -1;
    if (ok && destination) {
        out = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
        ok = out >= 0;
    }
    EVP_MD_CTX *hash = EVP_MD_CTX_new();
    ok = ok && hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1;
    unsigned char bytes[65536], actual[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    off_t total = 0;
    while (ok) {
        ssize_t n = read(in, bytes, sizeof bytes);
        if (n < 0 && errno == EINTR) continue;
        if (!n) break;
        if (n < 0 || n > limit - total || EVP_DigestUpdate(hash, bytes, (size_t)n) != 1) {
            ok = 0;
            break;
        }
        total += n;
        for (ssize_t used = 0; out >= 0 && used < n;) {
            ssize_t k = write(out, bytes + used, (size_t)(n - used));
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { ok = 0; break; }
            used += k;
        }
    }
    ok = ok && total == st.st_size && EVP_DigestFinal_ex(hash, actual, &length) == 1 && length == 32;
    char hex[65];
    for (unsigned i = 0; ok && i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", actual[i]);
    if (ok && digest) ok = !strcmp(hex, digest);
    if (out >= 0) {
        if (fsync(out)) ok = 0;
        if (close(out)) ok = 0;
        if (!ok) unlink(destination);
    }
    close(in);
    EVP_MD_CTX_free(hash);
    return ok;
}

static int validate_directory(const char *directory) {
    struct stat st;
    if (lstat(directory, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077)) return 0;
    DIR *stream = opendir(directory);
    if (!stream) return 0;
    size_t count = 0;
    int ok = 1;
    struct dirent *entry;
    while (ok && (entry = readdir(stream))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        size_t i = 0;
        while (i < sizeof objects / sizeof objects[0] && strcmp(objects[i].name, name)) i++;
        if (i == sizeof objects / sizeof objects[0]) { ok = 0; break; }
        char path[PATH_MAX];
        ok = joined(path, directory, name) && !lstat(path, &st) && S_ISREG(st.st_mode) &&
             st.st_uid == geteuid() && st.st_nlink == 1 && !(st.st_mode & 077) &&
             (i || !access(path, X_OK)) && copy_and_verify(path, NULL, 0, objects[i].digest, 8*1024*1024);
        count++;
    }
    closedir(stream);
    return ok && count == sizeof objects / sizeof objects[0];
}

static int extract(const char *archive, const char *directory) {
    char *args[] = {"/usr/bin/dpkg-deb", "--extract", (char *)archive, (char *)directory, NULL};
    char *env[] = {"PATH=/usr/bin:/bin", "LC_ALL=C", NULL};
    pid_t child;
    if (posix_spawn(&child, args[0], NULL, NULL, args, env)) return 0;
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int clean_path(const char *path, const struct stat *st, int type, struct FTW *walk) {
    (void)st; (void)type; (void)walk;
    return remove(path);
}

static int cancelled(const dc_hooks *hooks) {
    return hooks && hooks->cancelled && hooks->cancelled(hooks->user);
}

static const char *install_runtime(const char *home, const dc_hooks *hooks,
                                   dc_dsh_runtime_fetch fetch, const dc_dsh_node_abi *abi) {
    char stage[PATH_MAX], archive[PATH_MAX], cache[PATH_MAX], extracted[PATH_MAX], ready[PATH_MAX];
    char from[PATH_MAX], to[PATH_MAX];
    const char *error = "engine_private_runtime_staging_failed";
    if (!joined(stage, home, ".runtime-XXXXXX") || !mkdtemp(stage)) return error;
    if (!joined(archive, stage, "libc.deb") || !joined(cache, home, ARCHIVE) ||
        !joined(extracted, stage, "unpacked") || !joined(ready, stage, "ready") || mkdir(ready, 0700))
        goto done;
    if (hooks && hooks->event) hooks->event(hooks->user, "compat.phase", "downloading");
    struct stat st;
    if (!lstat(cache, &st)) {
        /* An optional user-supplied exact archive permits offline installs. */
        if (!copy_and_verify(cache, archive, 0600, ARCHIVE_HASH, 8*1024*1024)) {
            error = "engine_private_runtime_digest_failed";
            goto done;
        }
    } else if (errno != ENOENT || !fetch(BASE_URL ARCHIVE, ARCHIVE_HASH, archive, hooks)) {
        error = "engine_private_runtime_download_or_digest_failed";
        goto done;
    }
    if (cancelled(hooks)) goto done;
    if (hooks && hooks->event) hooks->event(hooks->user, "compat.phase", "verifying");
    if (!extract(archive, extracted)) { error = "engine_private_runtime_extract_failed"; goto done; }
    for (size_t i = 0; i < sizeof objects / sizeof objects[0]; i++) {
        const char *name = objects[i].name;
        int notice = !strcmp(name, "COPYRIGHT");
        char fromdir[PATH_MAX];
        if (!joined(fromdir, extracted, notice ? "usr/share/doc/libc6" : "usr/lib/" TRIPLET) ||
            !joined(from, fromdir, notice ? "copyright" : name) || !joined(to, ready, name) ||
            !copy_and_verify(from, to, i == 0 ? 0700 : 0600, objects[i].digest, 8*1024*1024)) {
            error = "engine_private_runtime_digest_failed";
            goto done;
        }
    }
    if (cancelled(hooks)) goto done;
    if (!validate_directory(ready)) { error = "engine_private_runtime_invalid"; goto done; }
    if (rename(ready, abi->directory) && !validate_directory(abi->directory)) goto done;
    error = NULL;
done:
    nftw(stage, clean_path, 16, FTW_DEPTH | FTW_PHYS);
    return cancelled(hooks) ? "cancelled" : error;
}

/* glibc's loader presents itself as /proc/self/exe. Node uses that path for
   process.execPath, including child_process.fork(). Supply a private launcher
   so every Node child uses the same interpreter and preload as its parent.
   Environment variables (including LD_LIBRARY_PATH and NODE_OPTIONS) remain
   untouched for unrelated binaries executed by a plugin. */
static char *shell_word(const char *name) {
    size_t length = strlen(name);
    char *word = malloc(4 * length + 3);
    if (!word) return NULL;
    char *next = word;
    *next++ = '\'';
    for (const char *p = name; *p; p++) {
        if (*p == '\'') { memcpy(next, "'\\''", 4); next += 4; }
        else *next++ = *p;
    }
    *next++ = '\'';
    *next = 0;
    return word;
}

static int launcher_file(const char *path, const char *contents, int executable, int detect_only) {
    size_t length = strlen(contents);
    struct stat st;
    if (!lstat(path, &st)) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned n = 0;
        char hex[65];
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 077) ||
            (executable && access(path, X_OK)) ||
            !EVP_Digest(contents, length, digest, &n, EVP_sha256(), NULL) || n != 32) return 0;
        for (unsigned i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        return copy_and_verify(path, NULL, 0, hex, (off_t)length);
    }
    if (errno != ENOENT || detect_only) return 0;
    char stage[PATH_MAX];
    if (snprintf(stage, sizeof stage, "%s-XXXXXX", path) >= (int)sizeof stage) return 0;
    int fd = mkstemp(stage);
    if (fd < 0) return 0;
    int ok = !fchmod(fd, executable ? 0700 : 0600);
    for (size_t i = 0; ok && i < length;) {
        ssize_t sent = write(fd, contents + i, length - i);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) { ok = 0; break; }
        i += (size_t)sent;
    }
    if (fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (ok) ok = !rename(stage, path);
    if (!ok) unlink(stage);
    return ok;
}

const char *dc_dsh_linux_child_launcher(const char *node, const char *home, int detect_only, dc_dsh_node_abi *abi) {
    char canonical[PATH_MAX];
    /* For a newly downloaded engine the final path is not published until
       the worker handshakes. configuration() boots the worker from staging.
       Subsequent plugin child processes receive the stable published path. */
    const char *target = realpath(node, canonical) ? canonical : node;
    if (target[0] != '/' || strpbrk(target, "\r\n")) return "engine_private_runtime_staging_failed";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    char filename[80] = "node-launch-";
    if (!EVP_Digest(target, strlen(target), digest, &n, EVP_sha256(), NULL) || n != 32)
        return "engine_private_runtime_staging_failed";
    for (unsigned i = 0; i < n; i++) snprintf(filename + 12 + i * 2, 3, "%02x", digest[i]);
    if (!joined(abi->launcher, home, filename) || !joined(abi->preload, home, "node-private-argv.cjs"))
        return "engine_private_runtime_staging_failed";
    char *words[5];
    const char *paths[] = {abi->loader, abi->launcher, abi->directory, target, abi->preload};
    size_t cap = 128;
    int ok = 1;
    for (size_t i = 0; i < 5; i++) {
        words[i] = shell_word(paths[i]);
        if (words[i]) cap += strlen(words[i]);
        else ok = 0;
    }
    char *script = ok ? malloc(cap) : NULL;
    if (script) {
        snprintf(script, cap, "#!/bin/sh\nexec %s --argv0 %s --library-path %s %s --require %s \"$@\"\n",
                 words[0], words[1], words[2], words[3], words[4]);
        ok = launcher_file(abi->preload, "process.execPath = process.argv0;\n", 0, detect_only) &&
             launcher_file(abi->launcher, script, 1, detect_only);
    } else ok = 0;
    free(script);
    for (size_t i = 0; i < 5; i++) free(words[i]);
    return ok ? NULL : detect_only ? "engine_download_required" : "engine_private_runtime_invalid";
}

const char *dc_dsh_linux_node_abi(const char *node, const char *home, int detect_only,
                                 const dc_hooks *hooks, dc_dsh_runtime_fetch fetch, dc_dsh_node_abi *abi) {
    int major = 0, minor = 0;
    if (sscanf(gnu_get_libc_version(), "%d.%d", &major, &minor) != 2 ||
        major > 2 || (major == 2 && minor >= 28)) return NULL;
    char canonical[PATH_MAX];
    /* User-supplied engines built for the local system keep their native path. */
    if (!realpath(node, canonical) || !copy_and_verify(canonical, NULL, 0, NODE_BINARY_HASH, 256*1024*1024))
        return NULL;
    if (!joined(abi->directory, home, "glibc-2.39-0ubuntu8.12-" ARCH) ||
        !joined(abi->loader, abi->directory, objects[0].name)) return "engine_private_runtime_staging_failed";
    struct stat st;
    if (!lstat(abi->directory, &st))
        return validate_directory(abi->directory) ? NULL : "engine_private_runtime_invalid";
    if (errno != ENOENT) return "engine_private_runtime_invalid";
    if (detect_only) return "engine_download_required";
    return install_runtime(home, hooks, fetch, abi);
}
#else
const char *dc_dsh_linux_child_launcher(const char *node, const char *home, int detect_only, dc_dsh_node_abi *abi) {
    (void)node; (void)home; (void)detect_only; (void)abi;
    return NULL;
}
const char *dc_dsh_linux_node_abi(const char *node, const char *home, int detect_only,
                                 const dc_hooks *hooks, dc_dsh_runtime_fetch fetch, dc_dsh_node_abi *abi) {
    (void)node; (void)home; (void)detect_only; (void)hooks; (void)fetch; (void)abi;
    return NULL;
}
#endif
#else
typedef int dunlin_private_runtime_linux_only;
#endif
