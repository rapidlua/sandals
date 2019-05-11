#include "sandals.h"
#include "jshelper.h"
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
    while (rmdir(cgroup_path)==-1) {
        // read resets internal 'updates pending' flag;
        if (errno != EBUSY
            || pread(cgroupevents_fd, buf, sizeof buf, 0) == -1
            || poll(&pollfd, 1, -1) == -1 && errno != EINTR
        ) {
            log_error("Removing cgroup '%s': %s",
                cgroup_path, strerror(errno));
            return;
        }
    }
}

void create_cgroup(
    const struct sandals_request *request, struct cgroup_ctx *ctx) {

    const char *prefix = "", *cgroup_root = request->cgroup_root;
    size_t len;
    int memoryevents = 0, pidevents = 0;
    char path_buf[PATH_MAX];

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

    // apply config
    if (request->cgroup_config) {
        const char *key;
        const jstr_token_t *value;
        JSOBJECT_FOREACH(request->cgroup_config, key, value) {

            int fd;
            const char *strval = jsget_str(request->json_root, value);

            while (*key=='/') ++key;

            if (snprintf(path_buf, sizeof path_buf, "%s/%s", cgroup_path, key)
            >= sizeof path_buf) fail(kStatusInternalError, "Path too long");

            fd = open_checked(path_buf, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0),
            write_checked(fd, strval, strlen(strval), path_buf);
            close(fd);

            if (!strncmp(key, "memory.", 7)) memoryevents = 1;
            if (!strncmp(key, "pids.", 5)) pidevents = 1;
        }
    }

    // ensure we can safely append any of cgroup.procs, memory.events or
    // cgroup.events suffixes
    struct suffix { const char data[14]; };
#define SUFFIX(s) (((struct suffix){(s)}),(s))

    if (strlen(cgroup_path) + sizeof(struct suffix) > sizeof path_buf)
        fail(kStatusInternalError, "Path too long");

    // open cgroup.procs
    sprintf(path_buf, "%s%s", cgroup_path, SUFFIX("/cgroup.procs"));
    ctx->cgroupprocs_fd = open_checked(path_buf, O_WRONLY|O_CLOEXEC|O_NOCTTY, 0);

    // open cgroup.events
    sprintf(path_buf, "%s%s", cgroup_path, SUFFIX("/cgroup.events"));
    cgroupevents_fd = open_checked(path_buf, O_RDONLY|O_CLOEXEC|O_NOCTTY, 0);

    // open memory.events
    ctx->memoryevents_fd = -1;
    if (memoryevents) {
        sprintf(path_buf, "%s%s", cgroup_path, SUFFIX("/memory.events"));
        ctx->memoryevents_fd = open_checked(
            path_buf, O_RDONLY|O_CLOEXEC|O_NOCTTY, 0);
    }

    // open pids.events
    ctx->pidsevents_fd = -1;
    if (pidevents) {
        sprintf(path_buf, "%s%s", cgroup_path, SUFFIX("/pids.events"));
        ctx->pidsevents_fd = open_checked(
            path_buf, O_RDONLY|O_CLOEXEC|O_NOCTTY, 0);
    }
}
