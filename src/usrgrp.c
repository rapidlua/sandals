#include "sandals.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

// these need to be captured in advance since when map_user_and_group()
// is called uid and gid maps aren't populated yet, hence any id comes
// out as nobody/nogroup
static uid_t proc_uid;
static gid_t proc_gid;

static void init_proc_uid_gid() __attribute__((constructor));
void init_proc_uid_gid() {
    proc_uid = getuid();
    proc_gid = getgid();
}

static const char kProcSelfUidmapPath[] = "/proc/self/uid_map";
static const char kProcSelfGidmapPath[] = "/proc/self/gid_map";

void map_user_and_group_begin(
    struct map_user_and_group_ctx *ctx) {

    static const char kProcSelfSetgroupsPath[] = "/proc/self/setgroups";
    int fd;

    ctx->procselfuidmap_fd = open_checked(
        kProcSelfUidmapPath, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0);
    ctx->procselfgidmap_fd = open_checked(
        kProcSelfGidmapPath, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0);

    fd = open_checked(kProcSelfSetgroupsPath, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0);
    write_checked(fd, "deny", 4, kProcSelfSetgroupsPath);
    close(fd);

    // TODO /proc/self/projid_map
}

void map_user_and_group_complete(
    const struct sandals_request *request,
    struct map_user_and_group_ctx *ctx) {

    uid_t uid = proc_uid;
    gid_t gid = proc_gid;
    char buf[64];

    if (request->user) {
        struct passwd *pw = getpwnam(request->user);
        if (!pw) fail(kStatusInternalError,
            "Lookup user '%s': %s", request->user, strerror(errno));
        uid = pw->pw_uid;
        gid = pw->pw_gid;
    }

    if (request->group) {
        struct group *gr = getgrnam(request->group);
        if (!gr) fail(kStatusInternalError,
            "Lookup group '%s': %s", request->group, strerror(errno));
        gid = gr->gr_gid;
    }

    write_checked(
        ctx->procselfuidmap_fd,
        buf, sprintf(buf, "%d %d 1", (int)uid, (int)proc_uid),
        kProcSelfUidmapPath);

    close(ctx->procselfuidmap_fd);

    write_checked(
        ctx->procselfgidmap_fd,
        buf, sprintf(buf, "%d %d 1", (int)gid, (int)proc_gid),
        kProcSelfGidmapPath);

    close(ctx->procselfgidmap_fd);
}
