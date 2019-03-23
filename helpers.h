#pragma once
#include "jstr/jstr.h"
#include <stdint.h>
#include <sys/types.h>

enum { RESPONSE_MAX = 4090 };

// response written to STDOUT in JSON format
void write_response(const char *data);
void write_response_raw(const char *data);
void write_response_int(int i);
void send_response();
void reset_response();

// failure response
void report_failure(const char *status, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void report_failure_and_exit(const char *status, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

extern const char kStatusExited[]; // = "sys.exited"
extern const char kStatusKilled[]; // = "sys.killed"
extern const char kStatusTimeLimit[]; // = "sys.time.limit"
extern const char kStatusPipeLimit[]; // = "sys.pipe.limit"
extern const char kStatusInternalError[]; // = "sys.inernal.error"
extern const char kStatusRequestInvalid[]; // = "sys.request.invalid"
extern const char kStatusResponseTooBig[]; // = "sys.response.toobig"
extern const char kStatusCustomStatusInvalid[]; // = "sys.customstatus.invalid"

// returns request length or 0 on EOF;
// request ends with \n (no NUL-termination)
size_t read_request(char **request);
char *copy_request(const char *request, size_t size);

void *match_key(const char *str, ...)
    __attribute__((sentinel));;

struct sandbozo_command {
    const char **cmd;
    const char **env;
    const char *work_dir;
    struct timespec time_limit;
    const char *status_fifo;
    const jstr_token_t *pipes;
    const char *pids_max;
};

extern const char kPipesKey[]; // = "pipes"

int parse_command(
    struct sandbozo_command *command, char *request, size_t size);

struct sandbozo_config {
    const char *chroot;
    const jstr_token_t *mounts;
    const char *cgroup_root;
    const jstr_token_t *cgroup_config;
    const char *user;
    const char *group;
    const char *host_name;
    const char *domain_name;
    int va_randomize; // address space randomisation
    int ro; // read-only filesystem
};

extern const char kMountsKey[]; // = "mounts"
extern const char kCgroupConfigKey[]; // = "cgroupConfig"

void read_config(struct sandbozo_config *config);
