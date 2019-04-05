#include "sandals.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

pid_t spawner_pid;

static char cgroup_path[PATH_MAX];
static int cgroupevents_fd; // "cgroup.events"

// We exit() on unrecoverable error. The only resource in need of
// cleanup in sandals is the cgroup we've created. For simplicity, we
// implement cleanup in a global destructor.
//
// Cleanup action is enabled by spawner_pid:
//    0  no cleanup (cgroup not created yet - initial state,
//       or we are the spawner and aren't responsible for cleaning up)
//   -1  rmdir cgroup (cgroup was created; fork pending or failed)
//  pid  kill pid, rmdir cgroup
static void cleanup_cgroup() __attribute__((destructor));
void cleanup_cgroup() {
    char buf[128];
    struct pollfd pollfd = {
        .fd = cgroupevents_fd,
        .events = POLLPRI
    };
    if (!spawner_pid) return;

    // Pending cgroup cleanup will take a while. Close early explicitly
    // so that a user will get the response faster.
    // CAVEAT: sandboxed processes may not terminate yet. This might be
    // an issue if a directory was mapped RW into the sandbox.
    close(response_fd);

    if (spawner_pid != -1) kill(spawner_pid, SIGKILL);
    // wait for cgroup to be vacated and remove it
    while (rmdir(cgroup_path)==-1 && errno==EBUSY) {
        // read resets internal 'updates pending' flag;
        if (pread(cgroupevents_fd, buf, sizeof buf, 0) == -1) return;
        poll(&pollfd, 1, -1);
    }
}

void create_cgroup(
    const struct sandals_request *request, struct cgroup_ctx *ctx) {

    const char *prefix = "", *cgroup_root = request->cgroup_root;
    size_t len;
    char path_buf[PATH_MAX];
    const jstr_token_t *tok, *conf_end;
    int fd;

    if (cgroup_root) {
        len = strlen(cgroup_root);
        while (len && cgroup_root[len-1]=='/') --len;
    } else {
        // get current cgroup and use the parent
        static const char kProcSelfCgroupPath[] = "/proc/self/cgroup";
        int fd = open_checked(
            kProcSelfCgroupPath, O_RDONLY|O_CLOEXEC|O_NOCTTY, 0);
        ssize_t rc = read(fd, path_buf, sizeof path_buf);
        if (rc < 0)
            fail(kStatusInternalError,
                "Reading '%s': %s", kProcSelfCgroupPath, strerror(errno));

        // TODO validation insufficient for mixed v1/v2 configuration
        if (rc < 3 || memcmp(path_buf, "0::", 3) || path_buf[rc-1]!='\n')
            fail(kStatusInternalError,
                "Cgroup info in '%s' too long or in Cgroups v1 format",
                kProcSelfCgroupPath);

        close(fd);

        prefix = "/sys/fs/cgroup";
        do --rc; while (rc && path_buf[rc] != '/');
        len = rc-3;
        cgroup_root = path_buf+3;
    }

    // Format cgroup_path
    if (snprintf(cgroup_path, sizeof cgroup_path, "%s%.*s/sandals-%d",
            prefix, (int)len, cgroup_root, getpid()) >= sizeof cgroup_path
    ) fail(kStatusInternalError, "Path too long");

    if (mkdir(cgroup_path, 0700) == -1)
        fail(kStatusInternalError,
            "Creating '%s': %s", cgroup_path, strerror(errno));

    // enable cleanup_cgroup() destructor
    spawner_pid = -1;

    // ensure we can safely append any of cgroup.procs, memory.events or
    // cgroup.events suffixes
    if (strlen(cgroup_path) + 16 > sizeof path_buf)
        fail(kStatusInternalError, "Path too long");

    // open cgroup.procs
    snprintf(path_buf, sizeof path_buf, "%s/cgroup.procs", cgroup_path);
    ctx->cgroupprocs_fd = open_checked(path_buf, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0);

    // open cgroup.events
    snprintf(path_buf, sizeof path_buf, "%s/cgroup.events", cgroup_path);
    cgroupevents_fd = open_checked(path_buf, O_RDONLY|O_CLOEXEC|O_NOCTTY, 0);

    // open memory.events (optional)
    snprintf(path_buf, sizeof path_buf, "%s/memory.events", cgroup_path);
    if ((ctx->memoryevents_fd = open(
            path_buf, O_RDONLY|O_CLOEXEC|O_NOCTTY, 0))==-1
        && errno != ENOENT
    ) fail(kStatusInternalError,
        "Opening '%s': %s", path_buf, strerror(errno));

    // apply config
    if (!request->cgroup_config) return;

    conf_end = jstr_next(request->cgroup_config);
    for (tok = request->cgroup_config + 1; tok != conf_end; tok += 2) {
        const char *key = jstr_value(tok), *value;

        if (jstr_type(tok+1) != JSTR_STRING)
            fail(kStatusRequestInvalid,
                "%s['%s']: expecting a string", kCgroupConfigKey, key);

        while (*key=='/') ++key;

        if (snprintf(path_buf, sizeof path_buf, "%s/%s", cgroup_path, key)
        >= sizeof path_buf) fail(kStatusInternalError, "Path too long");

        value = jstr_value(tok+1);
        fd = open_checked(path_buf, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0),
        write_checked(fd, value, strlen(value), path_buf);
        close(fd);
    }
}
