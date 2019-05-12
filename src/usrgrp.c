#include "sandals.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static const char kProcSelfUidmapPath[] = "/proc/self/uid_map";
static const char kProcSelfGidmapPath[] = "/proc/self/gid_map";
static const char kProcSelfSetgroupsPath[] = "/proc/self/setgroups";

void map_user_and_group(const struct sandals_request *request)
{
    int fd;
    char buf[64];

    fd = open_checked(kProcSelfSetgroupsPath, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0);
    write_checked(fd, "deny", 4, kProcSelfSetgroupsPath);
    close(fd);

    fd = open_checked(kProcSelfUidmapPath, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0);

    write_checked(
        fd,
        buf, sprintf(buf, "%d %d 1", (int)request->uid, (int)uid),
        kProcSelfUidmapPath);

    close(fd);

    fd = open_checked(kProcSelfGidmapPath, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0);

    write_checked(
        fd,
        buf, sprintf(buf, "%d %d 1", (int)request->gid, (int)gid),
        kProcSelfGidmapPath);

    close(fd);
}
