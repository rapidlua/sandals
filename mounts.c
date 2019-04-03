#include "sandals.h"
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

    const jstr_token_t *mounts_end, *mnt, *tok;
    if (!request->mounts) return;

    mounts_end = jstr_next(request->mounts);
    mnt = request->mounts + 1;
    for (size_t i = 0; mnt != mounts_end; ++i) {

        const char *type = NULL, *src = NULL, *dest = NULL, *options = "";
        int flags = 0, ro = 0, create_missing = 1;
        char chrooted_dest[PATH_MAX];

        if (jstr_type(mnt) != JSTR_OBJECT)
            fail(kStatusRequestInvalid,
                "%s[%zu]: expecting an object", kMountsKey, i);

        for (tok = mnt+1, mnt = jstr_next(mnt); tok != mnt; tok += 2) {
            const char *key = jstr_value(tok);
            const char **pv;
            if ((pv = match_key(key,
                        "type", &type, "src", &src, "dest", &dest,
                        "options", &options,
                        NULL))) {
                if (jstr_type(tok+1) != JSTR_STRING)
                    fail(kStatusRequestInvalid,
                        "%s[%zu].%s: expecting a string", kMountsKey, i, key);
                *pv = jstr_value(tok+1);
            } else if (!strcmp(key, "ro")) {
                jstr_type_t t = jstr_type(tok+1);
                if (!(t&(JSTR_TRUE|JSTR_FALSE)))
                    fail(kStatusRequestInvalid,
                        "%s[%zu].%s: expecting a boolean", kMountsKey, i, key);
                ro = t==JSTR_TRUE;
            } else {
                fail(kStatusRequestInvalid,
                    "%s[%zu]: unknown key '%s'", kMountsKey, i, key);
            }
        }

        if (!type) fail(kStatusRequestInvalid,
            "%s[%zu]: 'type' missing", kMountsKey, i);

        if (!dest) fail(kStatusRequestInvalid,
            "%s[%zu]: 'dest' missing", kMountsKey, i);

        if (!strcmp(type, "bind")) {
            if (!src) fail(kStatusRequestInvalid,
                "%s[%zu]: 'src' missing", kMountsKey, i);
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

