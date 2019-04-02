#define _GNU_SOURCE
#include "sandbozo.h"
#include <errno.h>
#include <fcntl.h>
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
    size_t index, const struct sandbozo_pipe *pipe, int fd[]) {

    fd[SECCOMPUSERNOTIFY+index] = do_create_fifo(pipe->fifo);
}

static void create_fifos(
    const struct sandbozo_request *request, struct msghdr *msghdr) {

    size_t npipes, sizefds;
    struct cmsghdr *cmsghdr;

    msghdr->msg_name = NULL;
    msghdr->msg_namelen = 0;
    msghdr->msg_flags = 0;
    msghdr->msg_control = NULL;
    msghdr->msg_controllen = 0;

    npipes = pipe_count(request);
    sizefds = sizeof(int)
        *(SECCOMPUSERNOTIFY+npipes+(request->status_fifo!=NULL));

    if (!SECCOMPUSERNOTIFY && !sizefds) return;

    if (!(msghdr->msg_control = malloc(
        msghdr->msg_controllen = CMSG_SPACE(sizefds)))
    ) fail(kStatusInternalError, "malloc");

    cmsghdr = CMSG_FIRSTHDR(msghdr);
    cmsghdr->cmsg_level = SOL_SOCKET;
    cmsghdr->cmsg_type = SCM_RIGHTS;
    cmsghdr->cmsg_len = CMSG_LEN(sizefds);

    pipe_foreach(request, create_fifo, (int*)CMSG_DATA(cmsghdr));

    if (request->status_fifo)
        ((int*)CMSG_DATA(cmsghdr))[SECCOMPUSERNOTIFY+npipes] =
            do_create_fifo(request->status_fifo);
}

static void configure_seccomp(
    const struct sandbozo_request *request, struct msghdr *msghdr) {
    // TODO
#if SECCOMPUSERNOTIFY
    *(int *)CMSG_DATA(CMSG_FIRSTHDR(msghdr)) = -1;
#endif
}

int spawner(const struct sandbozo_request *request, int hyper_fd) {

    int devnull_fd;
    struct map_user_and_group_ctx map_user_and_group_ctx;
    struct msghdr msghdr;
    volatile int *exec_errno;
    pid_t child_pid, pid;
    int status;
    struct sandbozo_response response;

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

    if (chdir(request->work_dir) == -1)
        fail(kStatusInternalError,
            "Setting work dir to '%s': %s",
            request->work_dir, strerror(errno));

    // grab shared memory page for exec_errno
    exec_errno = mmap(
        NULL, sysconf(_SC_PAGE_SIZE), PROT_READ|PROT_WRITE,
        MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (exec_errno == MAP_FAILED)
        fail(kStatusInternalError,
            "mmap(SHARED+ANONYMOUS): %s", strerror(errno));

    create_fifos(request, &msghdr);
    configure_seccomp(request, &msghdr);

    // send file descriptros to supervisor
    if (SECCOMPUSERNOTIFY || msghdr.msg_control) {
        char buf[1] = {};
        struct iovec iovec = { .iov_base = buf, .iov_len = sizeof buf };
        msghdr.msg_iov = &iovec;
        msghdr.msg_iovlen = 1;
        if (sendmsg(hyper_fd, &msghdr, 0) == -1)
            fail(kStatusInternalError, "sendmsg: %s", strerror(errno));
    }

    // Fork child process
    switch ((child_pid = fork())) {
    case -1:
        fail(kStatusInternalError, "fork: %s", strerror(errno));
    case 0:
        dup3(devnull_fd, STDIN_FILENO, 0) != -1 /*
        && dup3(devnull_fd, STDOUT_FILENO, 0) != -1
        && dup3(devnull_fd, STDERR_FILENO, 0) != -1 */
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
