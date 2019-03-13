#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "helpers.h"

static void configure_net(const struct sandbozo_config *conf) {

    // bring lo interface up
    int s;
    struct ifreq ifr = { .ifr_name = "lo"};

    if ((s = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0)) == -1)
        report_failure_and_exit(kStatusInternalError,
            "socket: %s", strerror(errno));

    if (ioctl(s, SIOCGIFFLAGS, &ifr) == -1
        || (ifr.ifr_flags |= IFF_UP|IFF_RUNNING,
            ioctl(s, SIOCSIFFLAGS, &ifr) == -1
    )) report_failure_and_exit(kStatusInternalError,
        "ioctl(SIOCGIFFLAGS,SIOCSIFFLAGS): %s", strerror(errno));

    close(s);

    // set hostname & domainname
    if (sethostname(conf->host_name, strlen(conf->host_name)) == -1)
        report_failure_and_exit(kStatusInternalError,
            "sethostname: %s", strerror(errno));

    if (setdomainname(conf->domain_name, strlen(conf->domain_name)) == -1)
        report_failure_and_exit(kStatusInternalError,
            "setdomainname: %s", strerror(errno));
}

static int open_checked(const char *path, int flags) {
    int fd;
    if ((fd = open(path, flags)) == -1)
        report_failure_and_exit(kStatusInternalError,
            "open('%s'): %s", path, strerror(errno));
    return fd;
}

static int write_checked(
    int fd, const void *data, size_t size, const char *path
) {
    if (write(fd, data, size) != size)
        report_failure_and_exit(kStatusInternalError,
            "Writing '%s': %s", path, strerror(errno));
    return fd;
}

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

struct map_user_and_group_ctx {
    int procselfuidmap_fd;
    int procselfgidmap_fd;
};

static const char kProcSelfUidmapPath[] = "/proc/self/uid_map";
static const char kProcSelfGidmapPath[] = "/proc/self/gid_map";
static const char kProcSelfSetgroupsPath[] = "/proc/self/setgroups";

static void map_user_and_group_begin(
    struct map_user_and_group_ctx *ctx
) {
    ctx->procselfuidmap_fd = open_checked(kProcSelfUidmapPath, O_WRONLY|O_CLOEXEC);
    ctx->procselfgidmap_fd = open_checked(kProcSelfGidmapPath, O_WRONLY|O_CLOEXEC);

    close(
        write_checked(
            open_checked(kProcSelfSetgroupsPath, O_WRONLY|O_CLOEXEC),
            "deny", 4, kProcSelfSetgroupsPath));

    // TODO /proc/self/projid_map
}

static void map_user_and_group_complete(
    const struct sandbozo_config *conf,
    struct map_user_and_group_ctx *ctx
) {
    uid_t uid = proc_uid;
    gid_t gid = proc_gid;
    char buf[64];
    size_t size;

    if (conf->user) {
        struct passwd *pw = getpwnam(conf->user);
        if (!pw) report_failure_and_exit(kStatusInternalError,
            "Lookup user '%s': %s", conf->user, strerror(errno));
        uid = pw->pw_uid;
        gid = pw->pw_gid;
    }

    if (conf->group) {
        struct group *gr = getgrnam(conf->group);
        if (!gr) report_failure_and_exit(kStatusInternalError,
            "Lookup group '%s': %s", conf->group, strerror(errno));
        gid = gr->gr_gid;
    }

    close(
        write_checked(
            ctx->procselfuidmap_fd,
            buf, sprintf(buf, "%d %d 1", (int)uid, (int)proc_uid),
            kProcSelfUidmapPath));

    close(
        write_checked(
            ctx->procselfgidmap_fd,
            buf, sprintf(buf, "%d %d 1", (int)gid, (int)proc_gid),
            kProcSelfGidmapPath));
}

static void chroot_relative(
    const struct sandbozo_config *conf, const char *path,
    char buf[PATH_MAX]
) {
    size_t len = strlen(conf->chroot);
    int rc;
    while (len && conf->chroot[len-1]=='/') --len;
    while (*path=='/') ++path;

    if (SIZE_MAX <= (size_t)snprintf(
        buf, PATH_MAX, "%.*s/%s", (int)len, conf->chroot, path))
        report_failure_and_exit(kStatusInternalError, "Path too long");
}

// Perform mounts as prescribed by config.
static void do_mounts(const struct sandbozo_config *conf) {

    const jstr_token_t *mounts_end, *mnt, *tok;
    if (!conf->mounts) return;

    mounts_end = jstr_next(conf->mounts); mnt = conf->mounts + 1;
    for (size_t i = 0; mnt != mounts_end; ++i) {

        const char *type = NULL, *src = NULL, *dest = NULL;
        int flags = 0;
        char chrooted_dest[PATH_MAX];

        if (jstr_type(mnt) != JSTR_OBJECT)
            report_failure_and_exit(kStatusRequestInvalid,
                "%s[%zu]: expecting an object", kMountsKey, i);

        for (tok = mnt+1, mnt = jstr_next(mnt); tok != mnt; tok += 2) {
            const char *key = jstr_value(tok);
            const char **pv;
            if ((pv = match_key(key,
                        "type", &type, "src", &src, "dest", &dest,
                        NULL))) {
                if (jstr_type(tok+1) != JSTR_STRING)
                    report_failure_and_exit(kStatusRequestInvalid,
                        "%s[%zu].%s: expecting a string", kMountsKey, i, key);
                *pv = jstr_value(tok+1);
            } else {
                report_failure_and_exit(kStatusRequestInvalid,
                    "%s[%zu]: unknown key '%s'", kMountsKey, i, key);
            }
        }

        if (!type) report_failure_and_exit(kStatusRequestInvalid,
            "%s[%zu]: 'type' missing", kMountsKey, i);

        if (!dest) report_failure_and_exit(kStatusRequestInvalid,
            "%s[%zu]: 'dest' missing", kMountsKey, i);

        if (!strcmp(type, "bind")) {
            if (!src) report_failure_and_exit(kStatusRequestInvalid,
                "%s[%zu]: 'src' missing", kMountsKey, i);
            flags = MS_BIND|MS_REC;
        } else {
            src = type;
        }

        chroot_relative(conf, dest, chrooted_dest);

        if (mount(src, chrooted_dest, type, flags, "") == -1)
            report_failure_and_exit(kStatusInternalError,
                "mount('%s','%s','%s'): %s",
                src, chrooted_dest, type, strerror(errno));
    }
}

struct cgroup_ctx {
    int cgroupprocs_fd;
    int pidsmax_fd;
    char conf_pidsmax[32];
};

static void run_server(
    const struct sandbozo_config *conf,
    const struct cgroup_ctx *cgroup_ctx
) {
    char *request;
    size_t size;
    int devnull_fd;
    volatile int *exec_errno;
    struct map_user_and_group_ctx map_user_and_group_ctx;

    // configure address space randomization
    if (personality(PER_LINUX
        |(conf->va_randomize ? 0 : ADDR_NO_RANDOMIZE)) == -1
    ) report_failure_and_exit(kStatusInternalError,
        "personality: %s", strerror(errno));

    // open /dev/null, strictly before chroot
    if ((devnull_fd = open("/dev/null", O_CLOEXEC|O_RDWR)) == -1)
        report_failure_and_exit(kStatusInternalError,
            "open('/dev/null'): %s", strerror(errno));

    // grab shared memory page for exec_errno
    exec_errno = mmap(
        NULL, sysconf(_SC_PAGE_SIZE), PROT_READ|PROT_WRITE,
        MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (exec_errno == MAP_FAILED)
        report_failure_and_exit(kStatusInternalError,
            "mmap(SHARED+ANONYMOUS): %s", strerror(errno));

    // turn chroot filesystem read-only (optional)
    if (conf->ro && (
        mount(conf->chroot, conf->chroot, "", MS_BIND|MS_REC, "") == -1
        || mount("", conf->chroot, "", MS_BIND|MS_REC|MS_REMOUNT|MS_RDONLY, "") == -1
    )) {
        report_failure_and_exit(kStatusInternalError,
            "mount('%s',bind,ro): %s", conf->chroot, strerror(errno));
    }

    // mount things, strictly after turning chroot filesystem read-only
    do_mounts(conf);

    // strictly before chroot - /proc may disappear after chroot
    map_user_and_group_begin(&map_user_and_group_ctx);

    if (chroot(conf->chroot)==-1)
        report_failure_and_exit(kStatusInternalError,
            "chroot('%s'): %s", conf->chroot, strerror(errno));

    // strictly after chroot - so that ${CHROOT}/etc/passwd is used to
    // resolve user/group name
    map_user_and_group_complete(conf, &map_user_and_group_ctx);

    // we are exposed to untrusted children via /proc; plug the leaks
    // (strictly after map_user_and_group)
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == -1)
        report_failure_and_exit(kStatusInternalError,
            "prctl(PR_SET_DUMPABLE): %s", strerror(errno));

    configure_net(conf);

    // TODO configure_seccomp()
    // must be the very last step in initialization since it limits
    // available syscalls

    write_response_raw("{\"status\":\"ok\"}\n"); send_response();

    while ((size = read_request(&request))) {

        const char *env_default[] = { NULL };
        struct sandbozo_command cmd = { .env = env_default, .work_dir = "/" };
        pid_t pid, child_pid;
        int status;

        if (parse_command(&cmd, request, size) == -1) continue;

        if (chdir(cmd.work_dir)) {
            report_failure(kStatusInternalError, "chdir: %s", strerror(errno));
            continue;
        }

        // pids.max <- value-for-pids-max
        write_checked(
            cgroup_ctx->pidsmax_fd,
            cgroup_ctx->conf_pidsmax, strlen(cgroup_ctx->conf_pidsmax),
            "pids.max");

        *exec_errno = 0;

        switch ((child_pid = fork())) {
        case -1:
            report_failure(kStatusInternalError, "fork: %s", strerror(errno));
            continue;
        case 0:
            dup3(devnull_fd, STDIN_FILENO, 0) != -1 /*
            && dup3(devnull_fd, STDOUT_FILENO, 0) != -1
            && dup3(devnull_fd, STDERR_FILENO, 0) != -1 */
            && execvpe(cmd.cmd[0], (char **)cmd.cmd, (char **)cmd.env);
            *exec_errno = errno;
            exit(EXIT_FAILURE);
        }

        // wait for the child process; we may be getting notifications
        // about other processes since we are pid 1 in a namespace
        do pid = wait(&status); while (pid != child_pid);

        if (*exec_errno) {
            report_failure(kStatusInternalError,
                "exec: %s", strerror(*exec_errno));
            continue;
        }

        // kill remaining child processes
        // pids.max <- 0 to avoid fork vs. kill race
        write_checked(cgroup_ctx->pidsmax_fd, "0", 1, "pids.max");

        // man 2 kill: on Linux the call kill(-1,sig) does not signal
        // the calling process
        if (kill(-1, SIGKILL) == -1 && errno != ESRCH)
            report_failure_and_exit(kStatusInternalError,
                "kill: %s", strerror(errno));

        // wait for remaining processes to terminate
        do pid = wait(NULL); while (pid != -1 || errno != ECHILD);

        if (WIFEXITED(status)) {
            write_response_raw("{\"status\":\"");
            write_response(kStatusExited);
            write_response_raw("\",\"code\":");
            write_response_int(WEXITSTATUS(status));
            write_response_raw("}\n");
        } else {
            write_response_raw("{\"status\":\"");
            write_response(kStatusKilled);
            write_response_raw("\",\"signal\":");
            write_response_int(WTERMSIG(status));
            write_response_raw(",\"description\":\"");
            write_response(strsignal(WTERMSIG(status)));
            write_response_raw("\"}\n");
        }
        send_response();
    }
}

// We exit() on unrecoverable error. The only resource in need of
// cleanup in sandbozo is the cgroup we've created.  For simplicity, we
// implement cleanup in a global destructor.
//
// Note: it is quite natural that the destructor is enabled by
// server_pid value, since only the parent process should perform the
// cleanup.
static pid_t server_pid;
static char cgroup_path[PATH_MAX];
static int cgroupevents_fd; // "cgroup.events"

static void cleanup_cgroup() __attribute__((destructor));
void cleanup_cgroup() {
    char buf[128];
    struct pollfd pollfd = {
        .fd = cgroupevents_fd,
        .events = POLLPRI
    };
    if (!server_pid) return;
    if (server_pid != -1) kill(server_pid, SIGKILL);
    // wait for cgroup to be vacated and remove it
    while (rmdir(cgroup_path)==-1 && errno==EBUSY) {
        // read resets internal 'updates pending' flag;
        // without read poll will be returning immediately
        if (read(cgroupevents_fd, buf, sizeof buf) == -1) return;
        poll(&pollfd, 1, INT_MAX);
    }
}

static void create_cgroup(
    const struct sandbozo_config *conf,
    struct cgroup_ctx *ctx
) {
    const char *prefix = "", *cgroup_root = conf->cgroup_root;
    size_t len;
    char path_buf[PATH_MAX];
    const jstr_token_t *tok, *conf_end;

    if (cgroup_root) {
        len = strlen(cgroup_root);
        while (len && cgroup_root[len-1]=='/') --len;
    } else {
        // get current cgroup and use the parent
        static const char kProcSelfCgroupPath[] = "/proc/self/cgroup";
        int fd = open_checked(kProcSelfCgroupPath, O_RDONLY|O_CLOEXEC);
        ssize_t rc = read(fd, path_buf, sizeof path_buf);
        if (rc < 0)
            report_failure_and_exit(kStatusInternalError,
                "Reading '%s': %s", kProcSelfCgroupPath, strerror(errno));

        if (rc < 3 || memcmp(path_buf, "0::", 3) || path_buf[rc-1]!='\n')
            report_failure_and_exit(kStatusInternalError,
                "Cgroup info in '%s' too long or in Cgroups v1 format",
                kProcSelfCgroupPath);

        close(fd);

        prefix = "/sys/fs/cgroup";
        do --rc; while (rc && path_buf[rc] != '/');
        len = rc-3;
        cgroup_root = path_buf+3;
    }

    // Format cgroup_path
    if (snprintf(cgroup_path, sizeof cgroup_path, "%s%.*s/sandbozo-%d",
            prefix, (int)len, cgroup_root, getpid()) >= sizeof cgroup_path
    ) report_failure_and_exit(kStatusInternalError, "Path too long");

    if (mkdir(cgroup_path, S_IXUSR|S_IRUSR) == -1)
        report_failure_and_exit(kStatusInternalError,
            "Creating '%s': %s", cgroup_path, strerror(errno));

    // enable cleanup_cgroup() destructor
    assert(!server_pid);
    server_pid = -1;

    // ensure we can safely append any of cgroup.procs, pids.max or
    // cgroup.events suffixes
    if (strlen(cgroup_path) + 16 > sizeof path_buf)
        report_failure_and_exit(kStatusInternalError, "Path too long");

    // open cgroup.procs
    snprintf(path_buf, sizeof path_buf, "%s/cgroup.procs", cgroup_path);
    ctx->cgroupprocs_fd = open_checked(path_buf, O_RDWR|O_CLOEXEC);

    // open pids.max
    snprintf(path_buf, sizeof path_buf, "%s/pids.max", cgroup_path);
    ctx->pidsmax_fd = open_checked(path_buf, O_RDWR|O_CLOEXEC);

    // open cgroup.events
    snprintf(path_buf, sizeof path_buf, "%s/cgroup.events", cgroup_path);
    cgroupevents_fd = open_checked(path_buf, O_RDONLY|O_CLOEXEC);

    // apply config
    if (!conf->cgroup_config) return;

    for (conf_end = jstr_next(conf->cgroup_config), tok = conf->cgroup_config+1
        ; tok != conf_end; tok += 2
    ) {
        const char *key = jstr_value(tok), *value;
        if (jstr_type(tok+1) != JSTR_STRING)
            report_failure_and_exit(kStatusRequestInvalid,
                "%s['%s']: expecting a string", kCgroupConfigKey, key);
        value = jstr_value(tok+1);
        while (*key=='/') ++key;
        if (!strcmp(key, "pids.max")) {
            strncpy(ctx->conf_pidsmax, value, sizeof ctx->conf_pidsmax);
            if (ctx->conf_pidsmax[(sizeof ctx->conf_pidsmax)-1])
                report_failure_and_exit(kStatusRequestInvalid,
                    "%s['%s']: value too long", kCgroupConfigKey, key);
        } else {
            if (snprintf(path_buf, sizeof path_buf, "%s/%s", cgroup_path, key)
            >= sizeof path_buf) report_failure_and_exit(kStatusInternalError,
                "Path too long");

            close(
                write_checked(
                    open_checked(path_buf, O_WRONLY|O_CLOEXEC),
                    value, strlen(value), path_buf));
        }
    }
}

// I love fork()!
static inline int myclone(int flags) {
    return syscall(SYS_clone, flags, NULL);
}

static void close_stray_fds() {
    static const char path[] = "/proc/self/fd";
    DIR *dir;
    struct dirent *dirent;
    if (!(dir = opendir(path))) report_failure_and_exit(
        kStatusInternalError, "opendir(%s): %s", path, strerror(errno));
    while (errno = 0, (dirent = readdir(dir))) {
        // parsing non-numeric entries yields fd=0, which is fine
        int fd = strtol(dirent->d_name, NULL, 0);
        if (fd > STDERR_FILENO && fd != dirfd(dir)) close(fd);
    }
    if (errno) report_failure_and_exit(
        kStatusInternalError, "readdir(%s): %s", path, strerror(errno));
    closedir(dir);
}

int main(int argc, char **argv) {
    struct sandbozo_config conf = {
        .chroot = "/",
        .va_randomize = 1,
        .ro = 1,
        .host_name = "sandbozo",
        .domain_name = "sandbozo"
    };
    struct cgroup_ctx cgroup_ctx = { .conf_pidsmax = "max" };

    read_config(&conf);
    close_stray_fds();
    create_cgroup(&conf, &cgroup_ctx);

    switch ((server_pid = myclone(
        SIGCHLD|CLONE_NEWUSER|CLONE_NEWIPC|CLONE_NEWNET|CLONE_NEWNS
        |CLONE_NEWPID|CLONE_NEWUTS))) {
    case -1:
        report_failure_and_exit(kStatusInternalError,
            "clone: %s", strerror(errno));
    case 0:
        read_request(NULL); // discard buffered data

        // detach from controlling TTY
        if (setsid() == -1)
            report_failure_and_exit(kStatusInternalError,
                "setsid: %s", strerror(errno));

        // enter cgroup
        if (write(cgroup_ctx.cgroupprocs_fd, "0", 1) != 1)
            report_failure_and_exit(kStatusInternalError,
                "Joining cgroup: %s", strerror(errno));

        // enter new cgroup namespace
        if (unshare(CLONE_NEWCGROUP) == -1)
            report_failure_and_exit(kStatusInternalError,
                "New cgroup namespace: %s", strerror(errno));

        run_server(&conf, &cgroup_ctx);
        exit(EXIT_SUCCESS);
    }

    // TODO proxy requests, implement timeout

    waitpid(server_pid, NULL, 0);
    server_pid = -1;
    return EXIT_SUCCESS;
}
