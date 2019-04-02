#define _GNU_SOURCE
#include "sandbozo.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

// I love fork()!
static inline int myclone(int flags) {
    return syscall(SYS_clone, SIGCHLD|flags, NULL);
}

int main() {
    int spawnerout[2], hyper[2];
    struct sandbozo_request request;
    struct cgroup_ctx cgroup_ctx;

    // otherwize log_write() becomes non-atomic
    setvbuf(stderr, NULL, _IOLBF, 0);

    request_recv(&request);
    if (pipe2(spawnerout, O_CLOEXEC) == -1)
        fail(kStatusInternalError, "pipe2: %s", strerror(errno));
    if (socketpair(AF_UNIX,
        SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0, hyper) == -1
    ) fail(kStatusInternalError,
        "socketpair(AF_UNIX, SOCK_STREAM): %s", strerror(errno));
    create_cgroup(&request, &cgroup_ctx);
    switch ((spawner_pid = myclone(CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET
        |CLONE_NEWUTS|CLONE_NEWNS))) {
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

        // join cgroup
        write_checked(cgroup_ctx.cgroupprocs_fd, "0", 1, "cgroup.procs");

        // start new cgroup namespace
        if (unshare(CLONE_NEWCGROUP) == -1)
            fail(kStatusInternalError,
                "New cgroup namespace: %s", strerror(errno));

        close_stray_fds_except(spawnerout[1], hyper[1]);

        return spawner(&request, hyper[1]);
    default:
        // stdout, stderr or pipe files may SIGPIPE
        signal(SIGPIPE, SIG_IGN);
        close(spawnerout[1]);
        close(hyper[1]);
        return supervisor(&request, &cgroup_ctx, spawnerout[0], hyper[0]);
    }
}
