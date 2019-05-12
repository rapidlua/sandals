#define _GNU_SOURCE
#include "sandals.h"
#include "jshelper.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

static void chroot_relative(
    const char *chroot, const char *path, char buf[PATH_MAX]) {

    size_t len = strlen(chroot);
    while (len && chroot[len-1]=='/') --len;
    while (*path=='/') ++path;

    if (PATH_MAX <= (size_t)snprintf(
        buf, PATH_MAX, "%.*s/%s", (int)len, chroot, path)
    ) fail(kStatusInternalError, "Path too long");
}

// create empty directory or file (depending on src type) to be used as
// the target of a mount
static void create_node(const char *src, char *dest)
{
    int n = 0, dirfd;
    struct stat statbuf = { .st_mode = S_IFDIR };
    char *p;

    if (src && stat(src, &statbuf) == -1)
        fail(kStatusInternalError, "stat('%s'): %s", src, strerror(errno));
    p = dest + strlen(dest);

    for (;;) {
        if (--p <= dest) {
            dirfd = AT_FDCWD;
            p = dest;
            goto create_part;
        }
        if (*p == '/') {
            *p = 0;
            dirfd = open(dest, O_PATH | O_DIRECTORY | O_CLOEXEC);
            if (dirfd != -1) break;
            if (errno != ENOENT)
                fail(kStatusInternalError,
                    "open('%s'): %s", dest, strerror(errno));
            ++n;
        }
    }

    do {
        int fd, flags;
        *p++ = '/';
        if (!*p) continue;
create_part:
        if (n || (statbuf.st_mode & S_IFMT) == S_IFDIR) {
            if (mkdirat(dirfd, p, 0700) == -1)
                fail(kStatusInternalError,
                    "mkdir('%s'): %s", dest, strerror(errno));
            if (!n) break;
            flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
        } else {
            flags = O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC;
        }
        fd = openat(dirfd, p, flags, 0600);
        if (fd == -1)
            fail(kStatusInternalError,
                "open('%s'): %s", dest, strerror(errno));
        p += strlen(p);
        if (dirfd != AT_FDCWD) close(dirfd);
        dirfd = fd;
    } while (n--);

    if (dirfd != AT_FDCWD) close(dirfd);
}

void do_mounts(const struct sandals_request *request) {

    const jstr_token_t *mntdef, *value;
    const char *key;

    if (!request->mounts) return;
    JSARRAY_FOREACH(request->mounts, mntdef) {

        const char *type = NULL, *src = NULL, *dest = NULL, *options = "";
        int flags = 0;
        bool ro = false, create_missing = true;
        char chrooted_dest[PATH_MAX];

        jsget_object(request->json_root, mntdef);
        JSOBJECT_FOREACH(mntdef, key, value) {

            if (!strcmp(key, "type")) {
                type = jsget_str(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "src")) {
                src = jsget_str(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "dest")) {
                dest = jsget_str(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "options")) {
                options = jsget_str(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "ro")) {
                ro = jsget_bool(request->json_root, value);
                continue;
            }

            jsunknown(request->json_root, value);
        }

        if (!type) jserror(request->json_root, mntdef, "'type' missing");

        if (!dest) jserror(request->json_root, mntdef, "'dest' missing");

        if (!strcmp(type, "bind")) {
            if (!src) jserror(request->json_root, mntdef, "'src' missing");
            flags = MS_BIND|MS_REC;
        } else {
            src = type;
        }

        chroot_relative(request->chroot, dest, chrooted_dest);

retry_mount:
        if (mount(src, chrooted_dest, type, flags, options) == -1) {
            if (!create_missing || errno != ENOENT)
                fail(kStatusInternalError,
                    "mount('%s','%s','%s'): %s",
                    src, chrooted_dest, type, strerror(errno));
            create_missing = false;
            create_node(flags & MS_BIND ? src : NULL, chrooted_dest);
            goto retry_mount;
        }

        if (ro && mount(src, chrooted_dest, type, flags
                |MS_REMOUNT|MS_RDONLY, options) == -1)
            fail(kStatusInternalError,
                "mount('%s','%s','%s'): %s",
                src, chrooted_dest, type, strerror(errno));
    }
}

