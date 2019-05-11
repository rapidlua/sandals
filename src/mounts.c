#include "sandals.h"
#include "jshelper.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

static void chroot_relative(
    const char *chroot, const char *path, char buf[PATH_MAX]) {

    size_t len = strlen(chroot);
    while (len && chroot[len-1]=='/') --len;
    while (*path=='/') ++path;

    if (PATH_MAX <= (size_t)snprintf(
        buf, PATH_MAX, "%.*s/%s", (int)len, chroot, path)
    ) fail(kStatusInternalError, "Path too long");
}

static void create_dirs(char *path) {
    // TODO
}

void do_mounts(const struct sandals_request *request) {

    const jstr_token_t *mntdef, *value;
    const char *key;

    if (!request->mounts) return;
    JSARRAY_FOREACH(request->mounts, mntdef) {

        const char *type = NULL, *src = NULL, *dest = NULL, *options = "";
        int flags = 0, ro = 0, create_missing = 1;
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
            create_missing = 0;
            create_dirs(chrooted_dest);
            goto retry_mount;
        }

        if (ro && mount(src, chrooted_dest, type, flags
                |MS_REMOUNT|MS_RDONLY, options) == -1)
            fail(kStatusInternalError,
                "mount('%s','%s','%s'): %s",
                src, chrooted_dest, type, strerror(errno));
    }
}

