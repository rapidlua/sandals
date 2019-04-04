#include "sandals.h"
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

// Intended for use with files on procfs or cgroupfs; partial
// writes do NOT happen. If they ever DO occur, the data will be
// misinterpreted hence we better treat this as an error.
// Ex: writing "0 0 4294967295" to /proc/self/uid_map,
// vs. (truncated) "0 0 42".
void write_checked(
    int fd, const void *data, size_t size, const char *path) {
    ssize_t rc;
    if ((rc = write(fd, data, size)) > 0 && (size_t)rc == size) return;
    fail(kStatusInternalError,
        "Writing '%s': %s", path, rc==-1?strerror(errno):"truncated");
}

void close_stray_fds_except(int except1, int except2) {
    static const char kProcSelfFdPath[] = "/proc/self/fd";
    DIR *dir;
    struct dirent *dirent;
    if (!(dir = opendir(kProcSelfFdPath))) fail(
        kStatusInternalError, "opendir('%s'): %s",
        kProcSelfFdPath, strerror(errno));
    while (errno = 0, (dirent = readdir(dir))) {
        // parsing non-numeric entries yields fd=0, which is fine
        int fd = strtol(dirent->d_name, NULL, 0);
        if (fd > STDERR_FILENO
            && fd != except1 && fd != except2 && fd != dirfd(dir)
        ) close(fd);
    }
    if (errno) fail(
        kStatusInternalError, "readdir('%s'): %s",
        kProcSelfFdPath, strerror(errno));
    closedir(dir);
}

