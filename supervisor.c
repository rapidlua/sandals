#define _GNU_SOURCE
#include "sandals.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

struct sandals_sink {
    const char *file;
    const char *fifo;
    int fd;
    int splice;
    long limit;
};

static void sink_init(
    int index, const struct sandals_pipe *pipe,
    struct sandals_sink *sink) {

    sink[index].file = pipe->file;
    sink[index].fifo = pipe->fifo;
    sink[index].fd = open_checked(
        pipe->file, O_CLOEXEC|O_WRONLY|O_TRUNC|O_CREAT|O_NOCTTY, 0600);
    sink[index].splice = 1;
    sink[index].limit = pipe->limit;
    // We depend on fd being in blocking IO mode. This is guaranteed
    // since we are explicitly requesting this mode via open() flags
    // (even when opening /proc/self/fd/*).
}

enum {
    MEMORYEVENTS_INDEX,
    STATUSFIFO_INDEX,
    TIMER_INDEX,
    SPAWNEROUT_INDEX,
    PIPE0_INDEX
};

//           MEMORYEVENTS
//          /        PIPE0
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
    void *cmsgbuf;
    struct sandals_response response, uresponse;
};

static int do_statusfifo(struct sandals_supervisor *s) {
    enum { TOKEN_COUNT = 64 };
    jstr_parser_t parser;
    jstr_token_t root[TOKEN_COUNT];
    const jstr_token_t *root_end, *tok;
    ssize_t rc = read(
        s->pollfd[STATUSFIFO_INDEX].fd,
        s->uresponse.buf+s->uresponse.size,
        (sizeof s->uresponse.buf)-s->uresponse.size+1);
    if (rc>0) {
        s->uresponse.size += rc;
        return 0;
    }
    if (rc==-1) {
        if (errno==EAGAIN || errno==EWOULDBLOCK) return 0;
        fail(kStatusInternalError,
            "Receiving response: %s", strerror(errno));
    }
    s->pollfd[STATUSFIFO_INDEX].fd = -1;
    if (!s->uresponse.size) goto bad_response;
    // copy first since validation mutates uresponse
    memcpy(
        s->response.buf, s->uresponse.buf,
        s->response.size = s->uresponse.size);
    jstr_init(&parser);
    s->uresponse.buf[s->uresponse.size] = 0;
    if (s->uresponse.size > sizeof s->uresponse.buf
        || jstr_parse(
            &parser, s->uresponse.buf,
            root, TOKEN_COUNT) != s->uresponse.size
        || jstr_type(root)!=JSTR_OBJECT
    ) goto bad_response;
    root_end = jstr_next(root); tok = root+1;
    while (tok!=root_end) {
        if (!strcmp(jstr_value(tok), "status")
            && (jstr_type(tok+1)!=JSTR_STRING
                || !strncmp(jstr_value(tok+1), "sys.", 4))
        ) goto bad_response;
        tok = jstr_next(tok+1);
    }
    return 1;
bad_response:
    // don't use fail(), might lose piped data
    s->response.size = 0;
    response_append_raw(&s->response, "{\"status\":\"");
    response_append_esc(&s->response, kStatusStatusInvalid);
    response_append_raw(&s->response, "\"}\n");
    return -1;
}

static int do_spawnerout(struct sandals_supervisor *s) {
    struct iovec iovec = {
        .iov_base = s->response.buf+s->response.size,
        .iov_len = (sizeof s->response.buf)-s->response.size+1
    };
    struct msghdr msghdr = {
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = s->cmsgbuf,
        .msg_controllen = CMSG_SPACE(sizeof(int)*(2+s->npipe))
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
            *(s->npipe+(s->request->status_fifo!=NULL)))
    ) {
        // Unpack PIPE0 ... PIPEn, STATUSFIFO descriptors
        // (transmitted in this order).
        const int *fd = (const int *)CMSG_DATA(cmsghdr);
        for (int i = 0; i < s->npipe; ++i) {
            s->pollfd[PIPE0_INDEX+i].fd = fd[i];
            s->pollfd[PIPE0_INDEX+i].events = POLLIN;
            s->pollfd[PIPE0_INDEX+i].revents = 0;
        };
        if (s->request->status_fifo)
            s->pollfd[STATUSFIFO_INDEX].fd = fd[s->npipe];
        s->npollfd = PIPE0_INDEX+s->npipe;
        return 0;
    }
    s->response.size += rc;
    // Spawner response is NL-terminated.
    return s->response.buf[s->response.size-1] == '\n';
}

static int do_pipes(struct sandals_supervisor *s) {
    int status = 0;
    ssize_t rc;
    struct sandals_sink *sink;
    // If multiple pipes exceeded their limit, report the first one
    // (that's why we are processing them in reverse order.)
    for (int i = s->npollfd; --i >= PIPE0_INDEX; ) {
        if (s->pollfd[i].fd==-1 || !s->pollfd[i].revents && !s->exiting)
            continue;
        sink = s->sink+i-PIPE0_INDEX;
        if (sink->limit && sink->splice) {
            if ((rc = splice(
                s->pollfd[i].fd, NULL, sink->fd, NULL, sink->limit,
                SPLICE_F_NONBLOCK)) == -1
            ) {
                if (errno==EINVAL) { sink->splice = 0; ++i; continue; }
                if (errno==EAGAIN) continue;
                fail(kStatusInternalError,
                    "Writing '%s': %s", sink->file, strerror(errno));
            }
        } else {
            char buf[PIPE_BUF];
            char *p, *e;
            if ((rc = read(s->pollfd[i].fd, buf, sizeof buf)) == -1) {
                if (errno==EAGAIN || errno==EWOULDBLOCK) continue;
                fail(kStatusInternalError,
                    "Reading '%s': %s", sink->fifo, strerror(errno));
            }
            p = buf; e = buf+(rc > sink->limit ? sink->limit : rc);
            while (p != e) {
                ssize_t sizewr = write(sink->fd, p, e-p);
                if (sizewr==-1) {
                    if (errno==EINTR) continue;
                    fail(kStatusInternalError,
                        "Writing '%s': %s", sink->file, strerror(errno));
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
                response_append_esc(&s->response, kStatusPipeLimit);
                response_append_raw(&s->response, "\",\"fifo\":\"");
                response_append_esc(&s->response, sink->fifo);
                response_append_raw(&s->response, "\",\"file\":\"");
                response_append_esc(&s->response, sink->file);
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
    if (!(s.sink = malloc(sizeof(struct sandals_sink)*s.npipe
        +sizeof(struct pollfd)*(PIPE0_INDEX+s.npipe)
        +CMSG_SPACE(sizeof(int)*(1+s.npipe)))
    )) fail(kStatusInternalError, "malloc");
    s.pollfd = (struct pollfd *)(s.sink+s.npipe);
    s.cmsgbuf = s.pollfd+PIPE0_INDEX+s.npipe;
    s.response.size = s.uresponse.size = 0;

    // User may trick us into re-opening any object we've created (using
    // /proc/self/fd/* paths). Fortunately, timers and sockets can't be
    // reopened (but pipes can!)
    pipe_foreach(request, sink_init, s.sink);

    if ((timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)) == -1)
        fail(kStatusInternalError, "Create timer: %s", strerror(errno));
    if (timerfd_settime(timer_fd, 0, &itimerspec, NULL) == -1)
        fail(kStatusInternalError, "Set timer: %s", strerror(errno));

    s.pollfd[MEMORYEVENTS_INDEX].fd = cgroup_ctx->memoryevents_fd;
    s.pollfd[MEMORYEVENTS_INDEX].events = POLLPRI;
    s.pollfd[STATUSFIFO_INDEX].fd = -1;
    s.pollfd[STATUSFIFO_INDEX].events = POLLIN;
    s.pollfd[TIMER_INDEX].fd = timer_fd;
    s.pollfd[TIMER_INDEX].events = POLLIN;
    s.pollfd[SPAWNEROUT_INDEX].fd = spawnerout_fd;
    s.pollfd[SPAWNEROUT_INDEX].events = POLLIN;

    for (;;) {
        if (poll(s.pollfd, s.npollfd, -1) == -1 && errno != EINTR)
            fail(kStatusInternalError, "poll: %s", strerror(errno));

        if (s.pollfd[MEMORYEVENTS_INDEX].revents) {
            // TODO memory events (OOM)
        }

        if (s.pollfd[STATUSFIFO_INDEX].revents && do_statusfifo(&s)) break;

        if (s.pollfd[TIMER_INDEX].revents) {
            s.response.size = 0;
            response_append_raw(&s.response, "{\"status\":\"");
            response_append_esc(&s.response, kStatusTimeLimit);
            response_append_raw(&s.response, "\"}\n");
            break;
        }

        if (s.pollfd[SPAWNEROUT_INDEX].revents && do_spawnerout(&s)) break;

        if (do_pipes(&s)) break;
    }

    kill(spawner_pid, SIGKILL); spawner_pid = -1;
    s.exiting = 1; do_pipes(&s);
    response_send(&s.response);
    return EXIT_SUCCESS;
}
