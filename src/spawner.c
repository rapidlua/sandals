#define _GNU_SOURCE
#include "sandals.h"
#include "stdstreams.h"
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
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static const char signals[][10] = {
    [SIGABRT] = "SIGABRT",
    [SIGALRM] = "SIGALRM",
    [SIGBUS] = "SIGBUS",
    [SIGCHLD] = "SIGCHLD",
    [SIGCONT] = "SIGCONT",
    [SIGFPE] = "SIGFPE",
    [SIGHUP] = "SIGHUP",
    [SIGILL] = "SIGILL",
    [SIGINT] = "SIGINT",
    [SIGIO] = "SIGIO",
    [SIGKILL] = "SIGKILL",
    [SIGPIPE] = "SIGPIPE",
    [SIGPOLL] = "SIGPOLL",
    [SIGPROF] = "SIGPROF",
    [SIGPWR] = "SIGPWR",
    [SIGQUIT] = "SIGQUIT",
    [SIGSEGV] = "SIGSEGV",
    [SIGSTKFLT] = "SIGSTKFLT",
    [SIGSTOP] = "SIGSTOP",
    [SIGSYS] = "SIGSYS",
    [SIGTERM] = "SIGTERM",
    [SIGTRAP] = "SIGTRAP",
    [SIGTSTP] = "SIGTSTP",
    [SIGTTIN] = "SIGTTIN",
    [SIGTTOU] = "SIGTTOU",
    [SIGURG] = "SIGURG",
    [SIGUSR1] = "SIGUSR1",
    [SIGUSR2] = "SIGUSR2",
    [SIGVTALRM] = "SIGVTALRM",
    [SIGWINCH] = "SIGWINCH",
    [SIGXCPU] = "SIGXCPU",
    [SIGXFSZ] = "SIGXFSZ"
};

static int childstdout_fd;
static int childstderr_fd;

static void make_pipe(
    int index, const struct sandals_pipe *pipe, int fd[]) {

    int pipe_fd[2];

    if (pipe->src) {
        if (mkfifo(pipe->src, 0600) == -1)
            fail(kStatusInternalError,
                "Creating fifo '%s': %s", pipe->src, strerror(errno));
        pipe_fd[0] = open_checked(
            pipe->src, O_RDONLY|O_NOCTTY|O_CLOEXEC|O_NONBLOCK, 0);
        if (pipe->as_stdout || pipe->as_stderr)
            pipe_fd[1] = open_checked(
                pipe->src, O_WRONLY|O_NOCTTY|O_CLOEXEC, 0);
    } else {
        if (pipe2(pipe_fd, O_CLOEXEC) == -1)
            fail(kStatusInternalError, "pipe2: %s", strerror(errno));
        if (fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK) == -1)
            fail(kStatusInternalError, "fcntl(F_SETFL, O_NONBLOCK): %s",
                strerror(errno));
    }

    if (pipe->as_stdout) childstdout_fd = pipe_fd[1];
    if (pipe->as_stderr) childstderr_fd = pipe_fd[1];
    fd[index] = pipe_fd[0];
}

static int make_socket(const void *addr, socklen_t addrlen) {
    struct sockaddr_un addr_un;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd==-1) fail(kStatusInternalError, "socket: %s", strerror(errno));
    addr_un.sun_family = AF_UNIX;
    memcpy(addr_un.sun_path, addr, addrlen);
    if (bind(
        fd, (struct sockaddr *)&addr_un,
        addrlen+offsetof(struct sockaddr_un, sun_path))==-1
    ) fail(kStatusInternalError, "bind: %s", strerror(errno));
    return fd;
}

static void connect_checked(int fd, const void *addr, socklen_t addrlen) {
    struct sockaddr_un addr_un;
    addr_un.sun_family = AF_UNIX;
    memcpy(addr_un.sun_path, addr, addrlen);
    if (connect(
        fd, (struct sockaddr *)&addr_un,
        addrlen+offsetof(struct sockaddr_un, sun_path))==-1
    ) fail(kStatusInternalError, "connect: %s", strerror(errno));
}

static void create_pipes(
    const struct sandals_request *request, struct msghdr *msghdr) {

    size_t npipes, sizefds;
    struct cmsghdr *cmsghdr;

    npipes = pipe_count(request);
    sizefds = sizeof(int)*(npipes + (request->stdstreams_dest!=NULL));

    if (!sizefds) return;

    if (!(msghdr->msg_control = malloc(
        msghdr->msg_controllen = CMSG_SPACE(sizefds)))
    ) fail(kStatusInternalError, "malloc");

    cmsghdr = CMSG_FIRSTHDR(msghdr);
    cmsghdr->cmsg_level = SOL_SOCKET;
    cmsghdr->cmsg_type = SCM_RIGHTS;
    cmsghdr->cmsg_len = CMSG_LEN(sizefds);

    pipe_foreach(request, make_pipe, (int*)CMSG_DATA(cmsghdr));

    if (request->stdstreams_dest) {
        ((int*)CMSG_DATA(cmsghdr))[npipes] =
            make_socket(kStdStreamsAddr, sizeof(kStdStreamsAddr));

        childstdout_fd = make_socket(kStdoutAddr, sizeof(kStdoutAddr));

        connect_checked(
            childstdout_fd, kStdStreamsAddr, sizeof(kStdStreamsAddr));

        childstderr_fd = make_socket(kStderrAddr, sizeof(kStderrAddr));

        connect_checked(
            childstderr_fd, kStdStreamsAddr, sizeof(kStdStreamsAddr));
    }
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
    childstdout_fd = childstderr_fd = devnull_fd;

    // strictly before altering mounts - /proc may disappear
    // + do_mounts() requires configured uid/gid maps
    map_user_and_group(request);

    // mount things
    do_mounts(request);

    if (chroot(request->chroot)==-1)
        fail(kStatusInternalError,
            "chroot('%s'): %s", request->chroot, strerror(errno));

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

    create_pipes(request, &msghdr); // allocates cmsg buffer

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
        && dup3(childstdout_fd, STDOUT_FILENO, 0) != -1
        && dup3(childstderr_fd, STDERR_FILENO, 0) != -1
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
        int sig = WTERMSIG(status);
        response_append_esc(&response, kStatusKilled);
        response_append_raw(&response, "\",\"signal\":\"");
        if (sig > 0 && (unsigned)sig <= sizeof(signals)/sizeof(signals[0])
            && signals[sig]
        ) {
            response_append_raw(&response, signals[sig]);
        } else {
            response_append_int(&response, sig);
        }
        response_append_raw(&response, "\"}\n");
    }
    response_send(&response);
    return EXIT_SUCCESS;
}
