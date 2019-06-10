#define _GNU_SOURCE
#include "sandals.h"
#include "stdstreams.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

struct sandals_sink {
    struct sandals_pipe pipe;
    int fd;
    int splice;
    long limit;
};

static void sink_init(
    int index, const struct sandals_pipe *pipe,
    struct sandals_sink *sink) {

    sink[index].pipe = *pipe;
    if (!pipe->fifo)
        sink[index].pipe.fifo = pipe->as_stdout ? "@stdout" : "@stderr";
    sink[index].splice = 1;
    sink[index].limit = pipe->limit;
    sink[index].fd = open_checked(
        pipe->file, O_CLOEXEC|O_WRONLY|O_TRUNC|O_CREAT|O_NOCTTY, 0600);
    // We depend on fd being in blocking IO mode. This is guaranteed
    // since we are explicitly requesting this mode via open() flags
    // (even when opening /proc/self/fd/*).
}

enum {
    MEMORYEVENTS_INDEX,
    PIDSEVENTS_INDEX,
    TIMER_INDEX,
    SPAWNEROUT_INDEX,
    STDSTREAMS_INDEX,
    PIPE0_INDEX // assumption: STDSTREAMS_INDEX+1 == PIPE0_INDEX
};

//           MEMORYEVENTS
//          /        STDSTREAMS
//         /        /
// pollfd [.....................]
// sink            [............]
//
// I.e. two parallel arrays with an offset.
struct sandals_supervisor {
    int exiting;
    const struct sandals_request *request;
    int npipe;
    int npollfd;
    struct sandals_sink *sink;
    struct pollfd *pollfd;
    char *spawnerout_cmsgbuf;
    char *stdstreams_recvbuf;
    socklen_t stdstreams_szrecvbuf;
    struct sandals_response response;
};

static bool cgroup_counter_nonzero(
    int fd, const char* key, const char *filename
) {
    char buf[128];
    ssize_t rc, i;
    off_t offset = 0;
    int state = 0;
    while ((rc = pread(fd, buf, sizeof buf, offset))) {

        if (rc == -1)
            fail(kStatusInternalError,
                "Reading '%s': %s", filename, strerror(errno));

        // State machine matching '\w(?P=key)[ ]*[1-9]'.
        offset += rc; i = 0;
        switch (state) {
        matchmore:
            if (++i==rc) break;
            if (!key[state]) {
                if (buf[i]==' ') goto matchmore;
                return buf[i]>'0' && buf[i]<='9';
            } else
        default:
            if (buf[i]==key[state]) {
                ++state;
                goto matchmore;
            }
            // fallthrough
        skipmore:
        case -1:
            if (buf[i]==' ' || buf[i]=='\n') {
                state = 0;
                goto matchmore;
            } else {
                if (++i==rc) { state = -1; break; }
                goto skipmore;
            }
        }
    }
    return false;
}

static int do_memoryevents(struct sandals_supervisor *s) {
    int fd = s->pollfd[MEMORYEVENTS_INDEX].fd;
    if (fd!=-1 && cgroup_counter_nonzero(fd, "oom_kill ", "memory.events")) {
        s->response.size = 0;
        response_append_raw(&s->response, "{\"status\":\"");
        response_append_esc(&s->response, kStatusMemoryLimit);
        response_append_raw(&s->response, "\"}\n");
        return -1;
    }
    return 0;
}

static int do_pidsevents(struct sandals_supervisor *s) {
    int fd = s->pollfd[PIDSEVENTS_INDEX].fd;
    if (fd!=-1 && cgroup_counter_nonzero(fd, "max ", "pids.events")) {
        s->response.size = 0;
        response_append_raw(&s->response, "{\"status\":\"");
        response_append_esc(&s->response, kStatusPidsLimit);
        response_append_raw(&s->response, "\"}\n");
        return -1;
    }
    return 0;
}

static int do_spawnerout(struct sandals_supervisor *s) {
    struct iovec iovec = {
        .iov_base = s->response.buf+s->response.size,
        .iov_len = (sizeof s->response.buf)-s->response.size+1
    };
    struct msghdr msghdr = {
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = s->spawnerout_cmsgbuf,
        .msg_controllen = CMSG_SPACE(sizeof(int)*(1+s->npipe))
    };
    struct cmsghdr *cmsghdr;
    ssize_t rc = recvmsg(
        s->pollfd[SPAWNEROUT_INDEX].fd, &msghdr, MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if (rc==-1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        fail(kStatusInternalError,
            "Receiving response: %s", strerror(errno));
    }
    if (!rc) {
        if (!s->response.size)
            fail(kStatusInternalError, "Empty response");
        return 1;
    }
    if ((cmsghdr = CMSG_FIRSTHDR(&msghdr)) // might be NULL
        && cmsghdr->cmsg_level == SOL_SOCKET
        && cmsghdr->cmsg_type == SCM_RIGHTS
        && cmsghdr->cmsg_len == CMSG_LEN(sizeof(int)
            *(s->npipe + (s->request->stdstreams_file!=NULL)))
    ) {
        // Unpack PIPE0 ... PIPEn, STDSTREAMSSOCKET?
        // (transmitted in this order).
        const int *fd = (const int *)CMSG_DATA(cmsghdr);
        for (int i = 0; i < s->npipe; ++i) {
            s->pollfd[PIPE0_INDEX+i].fd = fd[i];
            s->pollfd[PIPE0_INDEX+i].events = POLLIN;
            s->pollfd[PIPE0_INDEX+i].revents = 0;
        };
        s->npollfd = PIPE0_INDEX+s->npipe;
        if (s->request->stdstreams_file) {
            s->pollfd[STDSTREAMS_INDEX].fd = fd[s->npipe];
        }
        return 0;
    }
    s->response.size += rc;
    // Spawner response is NL-terminated.
    return s->response.buf[s->response.size-1] == '\n';
}

static ssize_t receive_stdstreams_packet(struct sandals_supervisor *s) {
    int fd = s->pollfd[STDSTREAMS_INDEX].fd;
    if (!s->stdstreams_szrecvbuf) {
        socklen_t len = sizeof(s->stdstreams_szrecvbuf);
        if (getsockopt(
            fd, SOL_SOCKET, SO_RCVBUF, &s->stdstreams_szrecvbuf, &len) == -1
        ) fail(kStatusInternalError, "getsockopt: %s", strerror(errno));
        if (!(s->stdstreams_recvbuf =
            malloc(sizeof(uint32_t)+s->stdstreams_szrecvbuf))
        ) fail(kStatusInternalError, "malloc");
    }
    do {
        struct sockaddr_un addr;
        socklen_t addr_len = sizeof(addr);
        ssize_t rc = recvfrom(
            fd, s->stdstreams_recvbuf+sizeof(uint32_t),
            s->stdstreams_szrecvbuf, MSG_DONTWAIT,
            (struct sockaddr*)&addr, &addr_len);

        if (rc <= 0) return rc;

        addr_len -= offsetof(struct sockaddr_un, sun_path);

        if (addr_len == sizeof(kStdoutAddr)
            && !memcmp(kStdoutAddr, addr.sun_path, sizeof(kStdoutAddr))
        ) {
            *(uint32_t *)s->stdstreams_recvbuf = htonl(rc);
            return rc+sizeof(uint32_t);
        }

        if (addr_len == sizeof(kStderrAddr)
            && !memcmp(kStderrAddr, addr.sun_path, sizeof(kStderrAddr))
        ) {
            *(uint32_t *)s->stdstreams_recvbuf = htonl(UINT32_C(0x80000000)|rc);
            return rc+sizeof(uint32_t);
        }

    } while (s->exiting);
    // Packet rejected because source address didn't match.
    // Don't retry recvfrom() since otherwize adversary may flood us with
    // messages and evade the time limit.
    errno = EWOULDBLOCK;
    return -1;
}

static int do_pipes(struct sandals_supervisor *s) {
    int status = 0;
    for (int i = s->npollfd; --i >= STDSTREAMS_INDEX; ) {
        if (s->pollfd[i].fd==-1 || !s->pollfd[i].revents && !s->exiting)
            continue;
        ssize_t rc;
        struct sandals_sink *sink  = s->sink+i-STDSTREAMS_INDEX;
        if (sink->limit && sink->splice) {
            if ((rc = splice(
                s->pollfd[i].fd, NULL, sink->fd, NULL, sink->limit,
                SPLICE_F_NONBLOCK)) == -1
            ) {
                if (errno==EINVAL) { sink->splice = 0; ++i; continue; }
                if (errno==EAGAIN) continue;
                fail(kStatusInternalError,
                    "Splicing '%s' and '%s': %s",
                    sink->pipe.fifo, sink->pipe.file, strerror(errno));
            }
        } else {
            char buf[PIPE_BUF];
            char *p, *e;
            if (i==STDSTREAMS_INDEX) {
                rc = receive_stdstreams_packet(s);
                p = s->stdstreams_recvbuf;
            } else {
                rc = read(s->pollfd[i].fd, buf, sizeof buf);
                p = buf;
            }
            if (rc == -1) {
                if (errno==EAGAIN || errno==EWOULDBLOCK) continue;
                fail(kStatusInternalError,
                    "Reading '%s': %s", sink->pipe.fifo, strerror(errno));
            }
            e = p+(rc > sink->limit ? sink->limit : rc);
            while (p != e) {
                ssize_t sizewr = write(sink->fd, p, e-p);
                if (sizewr==-1) {
                    if (errno==EINTR) continue;
                    fail(kStatusInternalError,
                        "Writing '%s': %s", sink->pipe.file, strerror(errno));
                }
                p += sizewr;
            }
        }
        if (rc && rc <= sink->limit) {
            sink->limit -= rc;
            i += s->exiting;
            // in 'exiting' mode process the same pipe until fully
            // drained or limit exceeded
        } else {
            close(s->pollfd[i].fd);
            s->pollfd[i].fd = -1;
            if (rc) {
                s->response.size = 0;
                response_append_raw(&s->response, "{\"status\":\"");
                response_append_esc(&s->response, kStatusFileLimit);
                response_append_raw(&s->response, "\"}\n");
                status = -1;
            }
        }
    }
    return status;
}

int supervisor(
    const struct sandals_request *request,
    const struct cgroup_ctx *cgroup_ctx,
    int spawnerout_fd) {

    struct sandals_supervisor s; // no initializer - large embedded buffers
    int timer_fd;
    struct itimerspec itimerspec = { .it_value = request->time_limit };

    s.exiting = 0;
    s.request = request;
    s.npipe = pipe_count(request);
    s.npollfd = PIPE0_INDEX;
    if (!(s.sink = malloc(sizeof(struct sandals_sink)*(1+s.npipe)
        +sizeof(struct pollfd)*(PIPE0_INDEX+s.npipe)
        +CMSG_SPACE(sizeof(int)*(1+s.npipe)))
    )) fail(kStatusInternalError, "malloc");
    s.pollfd = (struct pollfd *)(s.sink+1+s.npipe);
    s.spawnerout_cmsgbuf = (void *)(s.pollfd+PIPE0_INDEX+s.npipe);
    s.stdstreams_recvbuf = NULL;
    s.stdstreams_szrecvbuf = 0;
    s.response.size = 0;

    // User may trick us into re-opening any object we've created (using
    // /proc/self/fd/* paths). Fortunately, timers and sockets can't be
    // reopened (but pipes can!)
    pipe_foreach(request, sink_init, s.sink+1);

    if (request->stdstreams_file) {
        struct sandals_pipe pipe = {
            .file = request->stdstreams_file,
            .limit = request->stdstreams_limit,
            .fifo = "@stdstreams"
        };
        sink_init(0, &pipe, s.sink);
        s.sink[0].splice = 0;
    }

    if ((timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)) == -1)
        fail(kStatusInternalError, "Create timer: %s", strerror(errno));
    if (!itimerspec.it_value.tv_nsec)
        itimerspec.it_value.tv_nsec = 1; // zero timeout disables timer
    if (timerfd_settime(timer_fd, 0, &itimerspec, NULL) == -1)
        fail(kStatusInternalError, "Set timer: %s", strerror(errno));

    s.pollfd[MEMORYEVENTS_INDEX].fd = cgroup_ctx->memoryevents_fd;
    s.pollfd[MEMORYEVENTS_INDEX].events = POLLPRI;
    s.pollfd[PIDSEVENTS_INDEX].fd = cgroup_ctx->pidsevents_fd;
    s.pollfd[PIDSEVENTS_INDEX].events = POLLPRI;
    s.pollfd[TIMER_INDEX].fd = timer_fd;
    s.pollfd[TIMER_INDEX].events = POLLIN;
    s.pollfd[SPAWNEROUT_INDEX].fd = spawnerout_fd;
    s.pollfd[SPAWNEROUT_INDEX].events = POLLIN;
    s.pollfd[STDSTREAMS_INDEX].fd = -1;
    s.pollfd[STDSTREAMS_INDEX].events = POLLIN;

    for (;;) {
        if (poll(s.pollfd, s.npollfd, -1) == -1 && errno != EINTR)
            fail(kStatusInternalError, "poll: %s", strerror(errno));

        if (s.pollfd[MEMORYEVENTS_INDEX].revents && do_memoryevents(&s)) break;

        if (s.pollfd[PIDSEVENTS_INDEX].revents && do_pidsevents(&s)) break;

        if (s.pollfd[TIMER_INDEX].revents) {
            s.response.size = 0;
            response_append_raw(&s.response, "{\"status\":\"");
            response_append_esc(&s.response, kStatusTimeLimit);
            response_append_raw(&s.response, "\"}\n");
            break;
        }

        if (s.pollfd[SPAWNEROUT_INDEX].revents && do_spawnerout(&s)) {
            // Cgroup event notifications are asynchronous;
            // ex: when pids.max limit is hit, a job is queued to
            // wake up pollers. Luckily, the state reported through
            // pids.max and alike is updated synchronously.
            do_pidsevents(&s);
            do_memoryevents(&s);
            break;
        }

        if (do_pipes(&s)) break;
    }

    kill(spawner_pid, SIGKILL); spawner_pid = -1;
    s.exiting = 1; do_pipes(&s);
    response_send(&s.response);
    return EXIT_SUCCESS;
}
