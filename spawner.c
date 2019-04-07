#define _GNU_SOURCE
#include "sandals.h"
#include "kafel/include/kafel.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int do_create_fifo(const char *path) {
    if (mkfifo(path, 0600) == -1)
        fail(kStatusInternalError,
            "Creating fifo '%s': %s", path, strerror(errno));
    return open_checked(path, O_RDONLY|O_NOCTTY|O_CLOEXEC|O_NONBLOCK, 0);
}

static void create_fifo(
    int index, const struct sandals_pipe *pipe, int fd[]) {

    fd[index] = do_create_fifo(pipe->fifo);
}

static void create_fifos(
    const struct sandals_request *request, struct msghdr *msghdr) {

    size_t npipes, sizefds;
    struct cmsghdr *cmsghdr;

    npipes = pipe_count(request);
    sizefds = sizeof(int)*(npipes+(request->status_fifo!=NULL));

    if (!sizefds) return;

    if (!(msghdr->msg_control = malloc(
        msghdr->msg_controllen = CMSG_SPACE(sizefds)))
    ) fail(kStatusInternalError, "malloc");

    cmsghdr = CMSG_FIRSTHDR(msghdr);
    cmsghdr->cmsg_level = SOL_SOCKET;
    cmsghdr->cmsg_type = SCM_RIGHTS;
    cmsghdr->cmsg_len = CMSG_LEN(sizefds);

    pipe_foreach(request, create_fifo, (int*)CMSG_DATA(cmsghdr));

    if (request->status_fifo)
        ((int*)CMSG_DATA(cmsghdr))[npipes] =
            do_create_fifo(request->status_fifo);
}

static void configure_seccomp(
    const struct sandals_request *request, struct sock_fprog *sock_fprog) {

    if (!request->seccomp_policy) return;
    kafel_ctxt_t ctx = kafel_ctxt_create();
    kafel_set_input_string(ctx, request->seccomp_policy);
    if (kafel_compile(ctx, sock_fprog))
        fail(kStatusRequestInvalid,
            "Seccomp policy: %s", kafel_error_msg(ctx));
}

int spawner(const struct sandals_request *request) {

    int devnull_fd;
    struct map_user_and_group_ctx map_user_and_group_ctx;
    struct msghdr msghdr = {};
    volatile int *exec_errno;
    struct sock_fprog sock_fprog = {};
    pid_t child_pid, pid;
    int status;
    struct sandals_response response;

    // ifup lo
    configure_net(request);

    // open /dev/null, strictly before altering mounts
    devnull_fd = open_checked("/dev/null", O_CLOEXEC|O_RDWR|O_NOCTTY, 0);

    // strictly before altering mounts - /proc may disappear
    map_user_and_group_begin(&map_user_and_group_ctx);

    // mount things
    do_mounts(request);

    if (chroot(request->chroot)==-1)
        fail(kStatusInternalError,
            "chroot('%s'): %s", request->chroot, strerror(errno));

    // strictly after chroot - so that ${CHROOT}/etc/passwd is used to
    // resolve user/group name
    map_user_and_group_complete(request, &map_user_and_group_ctx);

    // we are exposed to untrusted children via /proc; plug the leaks
    // (strictly after map_user_and_group)
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == -1)
        fail(kStatusInternalError,
            "prctl(PR_SET_DUMPABLE): %s", strerror(errno));

    // configure address space randomization
    if (personality(PER_LINUX
        |(request->va_randomize ? 0 : ADDR_NO_RANDOMIZE)) == -1
    ) fail(kStatusInternalError,
        "personality: %s", strerror(errno));

    // chdir, beware relative paths
    if (*request->work_dir != '/' && chdir("/") == -1
        || chdir(request->work_dir) == -1
    ) fail(kStatusInternalError,
        "Setting work dir to '%s': %s",
        request->work_dir, strerror(errno));

    // grab shared memory page for exec_errno
    exec_errno = mmap(
        NULL, sysconf(_SC_PAGE_SIZE), PROT_READ|PROT_WRITE,
        MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (exec_errno == MAP_FAILED)
        fail(kStatusInternalError,
            "mmap(SHARED+ANONYMOUS): %s", strerror(errno));

    create_fifos(request, &msghdr); // allocates cmsg buffer

    // send file descriptors to supervisor
    if (msghdr.msg_control) {
        char buf[1] = {};
        struct iovec iovec = { .iov_base = buf, .iov_len = sizeof buf };
        msghdr.msg_iov = &iovec;
        msghdr.msg_iovlen = 1;
        if (sendmsg(response_fd, &msghdr, 0) == -1)
            fail(kStatusInternalError, "sendmsg: %s", strerror(errno));
    }

    configure_seccomp(request, &sock_fprog);

    // Fork child process
    switch ((child_pid = fork())) {
    case -1:
        fail(kStatusInternalError, "fork: %s", strerror(errno));
    case 0:
        dup3(devnull_fd, STDIN_FILENO, 0) != -1
        && dup3(devnull_fd, STDOUT_FILENO, 0) != -1
        && dup3(devnull_fd, STDERR_FILENO, 0) != -1
        && (!sock_fprog.len || prctl(
            PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &sock_fprog, 0, 0) != -1)
        && execvpe(request->cmd[0], (char **)request->cmd, (char **)request->env);
        *exec_errno = errno;
        exit(EXIT_FAILURE);
    }

    // wait for the child process; we may be getting notifications
    // about other processes since we are pid 1 in a namespace
    do pid = wait(&status); while (pid != child_pid);

    if (*exec_errno)
        fail(kStatusInternalError,
            "exec '%s': %s", request->cmd[0], strerror(*exec_errno));

    response.size = 0;
    response_append_raw(&response, "{\"status\":\"");
    if (WIFEXITED(status)) {
        response_append_esc(&response, kStatusExited);
        response_append_raw(&response, "\",\"code\":");
        response_append_int(&response, WEXITSTATUS(status));
        response_append_raw(&response, "}\n");
    } else {
        response_append_esc(&response, kStatusKilled);
        response_append_raw(&response, "\",\"signal\":");
        response_append_int(&response, WTERMSIG(status));
        response_append_raw(&response, ",\"description\":\"");
        response_append_esc(&response, strsignal(WTERMSIG(status)));
        response_append_raw(&response, "\"}\n");
    }
    response_send(&response);
    return EXIT_SUCCESS;
}
