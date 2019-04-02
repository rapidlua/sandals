#pragma once
#include "jstr/jstr.h"
#include <unistd.h>
#include <limits.h>

void log_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

extern int response_fd;

void fail(const char *status, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

extern const char kStatusExited[];         // = "sys.exited"
extern const char kStatusKilled[];         // = "sys.killed"
extern const char kStatusTimeLimit[];      // = "sys.time.limit"
extern const char kStatusPipeLimit[];      // = "sys.pipe.limit"
extern const char kStatusInternalError[];  // = "sys.internal.error"
extern const char kStatusRequestInvalid[]; // = "sys.request.invalid"
extern const char kStatusResponseTooBig[]; // = "sys.response.toobig"
extern const char kStatusStatusInvalid[];  // = "sys.status.invalid"

struct sandbozo_request {
    const char *host_name;
    const char *domain_name;
    const char *user;
    const char *group;
    const char *chroot;
    const jstr_token_t *mounts;
    const char *cgroup_root;
    const jstr_token_t *cgroup_config;
    int va_randomize; // address space randomisation
    const char **cmd;
    const char **env;
    const char *work_dir;
    struct timespec time_limit;
    const char *status_fifo;
    const jstr_token_t *pipes;
};

extern const char kMountsKey[];       // = "mounts"
extern const char kCgroupConfigKey[]; // = "cgroupConfig"
extern const char kPipesKey[];        // = "pipes"

void *match_key(const char *str, ...);

void request_recv(struct sandbozo_request *request);

struct sandbozo_response {
    size_t size;
    char buf[PIPE_BUF];
    char overflow[8];
};

void response_append_raw(struct sandbozo_response *response, const char *str);
void response_append_esc(struct sandbozo_response *response, const char *str);
void response_append_int(struct sandbozo_response *response, int value);
void response_send(const struct sandbozo_response *response);

static inline int response_too_big(const struct sandbozo_response *response) {
    return response->size > sizeof response->buf;
}

int open_checked(const char *path, int flags, mode_t mode);
void write_checked(int fd, const void *buf, size_t size, const char *path);
void close_stray_fds_except(int except1, int except2);

void configure_net(const struct sandbozo_request *request);

void do_mounts(const struct sandbozo_request *request);

struct map_user_and_group_ctx {
    int procselfuidmap_fd;
    int procselfgidmap_fd;
};

void map_user_and_group_begin(struct map_user_and_group_ctx *ctx);

void map_user_and_group_complete(
    const struct sandbozo_request *request,
    struct map_user_and_group_ctx *ctx);

struct sandbozo_pipe {
    const char *file;
    const char *fifo;
    long limit;
};

size_t pipe_count(const struct sandbozo_request *request);

void pipe_foreach(
    const struct sandbozo_request *request, void(*fn)(), void *userdata);

struct cgroup_ctx {
    int cgroupprocs_fd; // "cgroup.procs"
    int memoryevents_fd; // "memory.events"
};

void create_cgroup(
    const struct sandbozo_request *request, struct cgroup_ctx *ctx);

extern pid_t spawner_pid;

int supervisor(
    const struct sandbozo_request *request,
    const struct cgroup_ctx *cgroup_ctx,
    int spawnerout_fd, int hyper_fd);

int spawner(const struct sandbozo_request *request, int hyper_fd);

#define SECCOMPUSERNOTIFY 0
