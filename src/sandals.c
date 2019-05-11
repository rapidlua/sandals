#define _GNU_SOURCE
#include "sandals.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

// clone() in libc is weird: it requires us to supply a stack for the
// new task - the underlying syscall may work fork-style (stack COW)
static inline int myclone(int flags) {
    // Using bare clone() syscall was reported to be interfering with
    // LIBC pid caching. Glibc removed the caching recently hence this
    // should work. MUSL doesn't have caching either.
    return syscall(SYS_clone, SIGCHLD|flags, NULL);
}

int main(int argc) {
    int spawnerout[2];
    struct sandals_request request = {
        .host_name        = "sandals",
        .domain_name      = "sandals",
        .chroot           = "/",
        .va_randomize     = 1,
        .work_dir         = "/",
        .stdstreams_limit = LONG_MAX,
        .time_limit       = { .tv_sec = LONG_MAX }
    };
    struct cgroup_ctx cgroup_ctx = {
        .cgroupprocs_fd = -1,
        .pidsevents_fd = -1,
        .memoryevents_fd = -1
    };

    // otherwize log_write() becomes non-atomic
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (argc>1) {
        log_error("Does not accept arguments");
        return EXIT_FAILURE;
    }

    request_recv(&request);
    // Spawner writes response into this socket, MUST use blocking IO.
    if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, spawnerout) == -1)
        fail(kStatusInternalError,
            "socketpair(AF_UNIX, SOCK_STREAM): %s", strerror(errno));
    create_cgroup(&request, &cgroup_ctx);
    switch ((spawner_pid = myclone(CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET
        |CLONE_NEWUTS|CLONE_NEWNS|CLONE_NEWIPC))) {
    case -1:
        fail(kStatusInternalError, "clone: %s", strerror(errno));
    case 0:
        response_fd = spawnerout[1];

        // get killed if parent dies
        if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == -1)
            fail(kStatusInternalError,
                "prctl(PR_SET_PDEATHSIG): %s", strerror(errno));

        // detach from TTY
        if (setsid() == -1)
            fail(kStatusInternalError, "setsid: %s", strerror(errno));

        // cgroup setup (optional)
        if (cgroup_ctx.cgroupprocs_fd != -1) {

            // join cgroup
            write_checked(cgroup_ctx.cgroupprocs_fd, "0", 1, "cgroup.procs");

            // start new cgroup namespace
            if (unshare(CLONE_NEWCGROUP) == -1)
                fail(kStatusInternalError,
                    "New cgroup namespace: %s", strerror(errno));
        }

        close_stray_fds_except(spawnerout[1]);

        return spawner(&request);
    default:
        // stdout, stderr or pipe files may SIGPIPE
        signal(SIGPIPE, SIG_IGN);
        close(spawnerout[1]);
        return supervisor(&request, &cgroup_ctx, spawnerout[0]);
    }
}
