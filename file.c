#include "sandbozo.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int open_checked(const char *path, int flags, mode_t mode) {
    int fd;
    if ((fd = open(path, flags, mode)) == -1)
        fail(kStatusInternalError,
            "Opening '%s': %s", path, strerror(errno));
    return fd;
}

void write_checked(
    int fd, const void *data, size_t size, const char *path) {
    if (write(fd, data, size) != size)
        fail(kStatusInternalError,
            "Writing '%s': %s", path, strerror(errno));
}

void close_stray_fds_except(int except1, int except2) {
    static const char kProcSelfFdPath[] = "/proc/self/fd";
    DIR *dir;
    struct dirent *dirent;
    if (!(dir = opendir(kProcSelfFdPath))) fail(
        kStatusInternalError, "opendir(%s): %s",
        kProcSelfFdPath, strerror(errno));
    while (errno = 0, (dirent = readdir(dir))) {
        // parsing non-numeric entries yields fd=0, which is fine
        int fd = strtol(dirent->d_name, NULL, 0);
        if (fd > STDERR_FILENO
            && fd != except1 && fd != except2 && fd != dirfd(dir)
        ) close(fd);
    }
    if (errno) fail(
        kStatusInternalError, "readdir(%s): %s",
        kProcSelfFdPath, strerror(errno));
    closedir(dir);
}

