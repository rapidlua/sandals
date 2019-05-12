// This should be LDPRELOAD-ed into sandboxed task if stdStreams feature
// is being used. Preloading is arranged manually via env argument or
// using /etc/ld.preload. The library
//
// * switches stdout to line-buffered mode (fully buffered by
//   default on non-tty device);
//
// * binary-patches write() and writev() to retry calls with a smaller
//   data chunk if failed with EMSGSIZE. The failure happens when
//   write() is called with a UNIX dgram socket used for stdin/stderr.
#define _GNU_SOURCE 1
#include "stdstreams.h"
#include "hook_engine/hook_engine.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

static bool check_socket(int fd, socklen_t *pbufsz) {
    struct sockaddr_un peer_addr;
    socklen_t len = sizeof peer_addr, bufsize;
    if (
        getpeername(fd, (struct sockaddr *)&peer_addr, &len) == -1
        || len != offsetof(struct sockaddr_un, sun_path)
            + sizeof(kStdStreamsAddr)
        || memcmp(peer_addr.sun_path, kStdStreamsAddr, sizeof(kStdStreamsAddr))
    ) {
        errno = EMSGSIZE; // restore errno
        return false;
    }
    len = sizeof(bufsize);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, &len) == -1) {
        errno = EMSGSIZE; // restore errno
        return false;
    }
    *pbufsz = bufsize / 2;
    return true;
}

ssize_t __real__write(int fd, const void *buf, size_t count);
HOOK_DEFINE_TRAMPOLINE(__real__write);

static ssize_t __wrap__write(int fd, const void *buf, size_t count) {
    ssize_t rc = __real__write(fd, buf, count);
    socklen_t bufsz;
    if (rc == -1 && errno == EMSGSIZE && check_socket(fd, &bufsz)) {
        const void *p = buf;
        while (count && (rc = __real__write(
            fd, p, count <= bufsz ? count : bufsz)
        ) > 0) {
            p += rc;
            count -= rc;
        }
        return p==buf ? rc : p - buf;
    }
    return rc;
}

static int iov_copy(
#define IOV_COUNT 32 // 512 B
    struct iovec iovcopy[IOV_COUNT], const struct iovec *iov, int iovcnt,
    size_t offset, size_t size
);

ssize_t __real__writev(int fd, const struct iovec *iov, int iovcnt);
HOOK_DEFINE_TRAMPOLINE(__real__writev);

static ssize_t __wrap__writev(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t rc = __real__writev(fd, iov, iovcnt);
    socklen_t bufsz;
    if (rc == -1 && errno == EMSGSIZE && iovcnt && check_socket(fd, &bufsz)) {
        size_t total = 0;
        size_t offset = 0;
        struct iovec iovcopy[IOV_COUNT];
        while ((rc = __real__writev(
            fd, iovcopy, iov_copy(iovcopy, iov, iovcnt, offset, bufsz))
        ) > 0) {
            total += rc;
            offset += rc;
            while (iov[0].iov_len >= offset) {
                offset -= iov[0].iov_len;
                ++iov;
                if (!--iovcnt) return total;
            }
        }
        return total ? (ssize_t)total : rc;
    }
    return rc;
}

static int iov_copy(
    struct iovec iovcopy[IOV_COUNT], const struct iovec *iov, int iovcnt,
    size_t offset, size_t size
) {
    size_t limit = offset + size;
    int n = 0;
    while (n < IOV_COUNT && n < iovcnt) {
        iovcopy[n].iov_base = iov[n].iov_base;
        if (iov[n].iov_len >= limit) {
            iovcopy[n++].iov_len = limit;
            break;
        }
        limit -= (iovcopy[n].iov_len = iov[n].iov_len);
        ++n;
    }
    iovcopy[0].iov_base += offset;
    iovcopy[0].iov_len -= offset;
    return n;
}

#ifdef MUSL
// MUSL uses bare syscalls in its stdio implementation, forcing us to
// replace a whole function.
// https://github.com/ifduyue/musl/blob/79f653c6bc2881dd6855299c908a442f56cb7c2b/src/internal/stdio_impl.h#L21
struct _MUSL_FILE {
    unsigned flags;
    unsigned char *rpos, *rend;
    int (*close)(FILE *);
    unsigned char *wend, *wpos;
    unsigned char *mustbezero_1;
    unsigned char *wbase;
    size_t (*read)(struct _MUSL_FILE *, unsigned char *, size_t);
    size_t (*write)(struct _MUSL_FILE *, const unsigned char *, size_t);
    off_t (*seek)(struct _MUSL_FILE *, off_t, int);
    unsigned char *buf;
    size_t buf_size;
    struct _MUSL_FILE *prev, *next;
    int fd;
    // more irrelevant members follow
};

#define F_ERR 32

size_t __stdio_write(
    struct _MUSL_FILE *f, const unsigned char *buf, size_t len
);

// https://github.com/ifduyue/musl/blob/b4b1e10364c8737a632be61582e05a8d3acf5690/src/stdio/__stdio_write.c#L4
static size_t __wrap__stdio_write(
    struct _MUSL_FILE *f, const unsigned char *buf, size_t len
) {
    struct iovec iovs[2] = {
        { .iov_base = f->wbase, .iov_len = f->wpos-f->wbase },
        { .iov_base = (void *)buf, .iov_len = len }
    };
    struct iovec *iov = iovs;
    size_t rem = iov[0].iov_len + iov[1].iov_len;
    int iovcnt = 2;
    ssize_t cnt;
    for (;;) {
        cnt = __wrap__writev(f->fd, iov, iovcnt);
        if (cnt == rem) {
            f->wend = f->buf + f->buf_size;
            f->wpos = f->wbase = f->buf;
            return len;
        }
        if (cnt < 0) {
            f->wpos = f->wbase = f->wend = 0;
            f->flags |= F_ERR;
            return iovcnt == 2 ? 0 : len-iov[0].iov_len;
        }
        rem -= cnt;
        if (cnt > iov[0].iov_len) {
            cnt -= iov[0].iov_len;
            iov++; iovcnt--;
        }
        iov[0].iov_base = (char *)iov[0].iov_base + cnt;
        iov[0].iov_len -= cnt;
    }
}
#endif

void init(void) {
    if (
        hook_begin() != 0 ||
        hook_install(write,  __wrap__write,  __real__write ) != 0 ||
        hook_install(writev, __wrap__writev, __real__writev) != 0
#ifdef MUSL
        || hook_install(__stdio_write, __wrap__stdio_write, NULL) != 0
#endif
    ) {
        fprintf(
            stderr, "%s: %s\n", program_invocation_name, hook_last_error()
        );
        exit(EXIT_FAILURE);
    }
    hook_end();
    setvbuf(stdout, NULL, _IOLBF, 0);
}
