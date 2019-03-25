#define _GNU_SOURCE

#include "helpers.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char kStatusExited[]         = "sys.exited";
const char kStatusKilled[]         = "sys.killed";
const char kStatusTimeLimit[]      = "sys.time.limit";
const char kStatusPipeLimit[]      = "sys.pipe.limit";
const char kStatusInternalError[]  = "sys.internalerror";
const char kStatusRequestInvalid[] = "sys.request.invalid";
const char kStatusResponseTooBig[] = "sys.response.toobig";
const char kStatusStatusInvalid[]  = "sys.status.invalid";

static char response[RESPONSE_MAX+6];
static char *response_end = response;

void write_response(const char *data) {
    char *p = response_end;
    for (; p <= response+RESPONSE_MAX && *data; ++data) {
        unsigned c = *(const unsigned char *)data;
        if (c>=0x20 && c!='\\' && c!='\"') *p++ = *data;
        else {
            static const char hex[]="0123456789abcdef";
            p[0] = '\\'; p[1] = 'u'; p[2] = '0'; p[3] = '0';
            p[4] = hex[c>>4]; p[5] = hex[c&15];
            p += 6;
            // see response[] definition - fine to overrun RESPONSE_MAX
            // by at most 6 characters
        }
    }
    response_end = p;
}

void write_response_raw(const char *data) {
    size_t len = strlen(data);
    if (response_end+len <= response+RESPONSE_MAX)
        memcpy(response_end, data, len);
    response_end += len;
}

void write_response_int(int i) {
    char buf[16];
    sprintf(buf, "%d", i);
    write_response_raw(buf);
}

void send_response() {
    if (response_end > response+RESPONSE_MAX) {
        reset_response();
        return report_failure(kStatusResponseTooBig, NULL);
    }
    char *p = response;
    while (p != response_end) {
        ssize_t rc = write(STDOUT_FILENO, p, response_end - p);
        if (rc > 0) p += rc;
        else if (errno != EINTR) exit(EXIT_FAILURE);
    }
    reset_response();
}

void reset_response() {
    response_end = response;
}

static void vreport_failure(const char *status, const char *fmt, va_list ap) {
    write_response_raw("{\"status\":\"");
    write_response_raw(status);
    if (fmt) {
        char buf[RESPONSE_MAX];
        int st;
        st = vsnprintf(buf, sizeof buf, fmt, ap);
        if (st>0 && (size_t)st<sizeof buf) {
            write_response_raw("\",\"description\":\"");
            write_response(buf);
        } else {
            reset_response();
            report_failure(kStatusResponseTooBig, NULL);
        }
    }
    write_response_raw("\"}\n");
    send_response();
}

void report_failure(const char *status, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport_failure(status, fmt, ap);
}

void report_failure_and_exit(const char *status, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport_failure(status, fmt, ap);
    exit(EXIT_FAILURE);
}

// returns request length or 0 on EOF;
// request ends with \n (no NUL-termination)
size_t read_request(char **request) {
    enum { BUF_MIN = 1024 };
    static char *buf, *buf_end;
    static char *data, *data_end;
    static int last_fd = -1;

    char *p = data;
    ssize_t rc;

    // secret knock: discard buffered data (done once after fork)
    if (!request) {
        data = data_end = buf;
        return 0;
    }

    while (!(p = memchr(p, '\n', data_end-p))) {
        if (buf_end-data_end < BUF_MIN) {
            size_t buf_size = buf_end-buf;
            if (2*(data_end-data) < buf_size) {
                memmove(buf, data, data_end-data);
                data_end = buf+(data_end-data); data = buf;
            } else {
                char *new_buf = realloc(
                    buf, buf_size ? (buf_size*=2) : (buf_size=2*BUF_MIN));
                if (!new_buf) report_failure_and_exit(
                    kStatusInternalError, "malloc");
                buf_end = new_buf+buf_size;
                data += new_buf-buf;
                data_end += new_buf-buf;
                buf = new_buf;
            }
        }
        p = data_end;
        rc = read(STDIN_FILENO, data_end, buf_end-data_end);
        if (rc < 0) {
            if (errno == EINTR) continue;
            report_failure_and_exit(
                kStatusInternalError, "Reading request: %s", strerror(errno));
        }
        if (rc == 0) {
            if (data==data_end) return 0;
            *data_end++ = '\n';
        }
        data_end += rc;
    }
    *request = data; data = p+1;
    return data-*request;
}

char *copy_request(const char *request, size_t size) {
    static char *buf;
    static size_t buf_size;
    if (buf_size < size) {
        char *new_buf = realloc(buf, size);
        if (!new_buf) {
            report_failure(kStatusInternalError, "malloc");
            return NULL;
        }
        buf = new_buf; buf_size = size;
    }
    return memcpy(buf, request, size);
}

static jstr_token_t *parse_request(char *request, size_t size) {
    enum { TOKEN_MIN = 64 };
    static jstr_token_t *token;
    static size_t token_count = 0;
    ssize_t rc;
    jstr_parser_t parser;

    assert(size);
    request[size-1] = 0;
    jstr_init(&parser);
    while ((rc=jstr_parse(&parser, request, token, token_count))==JSTR_NOMEM) {
        token_count = token_count ? 2*token_count : TOKEN_MIN;
        if(!(token = realloc(token, sizeof(token[0])*token_count))) {
            report_failure(kStatusInternalError, "malloc");
            return NULL;
        }
    }
    if (size-1 != rc || jstr_type(token) != JSTR_OBJECT) {
        report_failure(kStatusRequestInvalid, NULL);
        return NULL;
    }
    return token;
}

void *match_key(const char *str, ...) {
    void *result = NULL;
    va_list ap;
    va_start(ap, str);
    for (;;) {
        const char *key;
        void *dest;
        if (!(key = va_arg(ap, const char *))) break;
        dest = va_arg(ap, void *);
        if (!strcmp(key, str)) { result = dest; break; }
    }
    va_end(ap);
    return result;
}

const char kMountsKey[] = "mounts";
const char kCgroupConfigKey[] = "cgroupConfig";

void read_config(struct sandbozo_config *conf) {
    char *request;
    size_t size;
    const jstr_token_t *root, *root_end, *tok;
    void *dest;
    if (!(size = read_request(&request))) report_failure_and_exit(
        kStatusRequestInvalid, "unexpected EOF");
    if (!(root = parse_request(request, size))) exit(EXIT_FAILURE);
    root_end = jstr_next(root); tok = root+1;
    while (tok!=root_end) {
        const char *key=jstr_value(tok);
        if ((dest = match_key(key,
                    "chroot", &conf->chroot,
                    "cgroupRoot", &conf->cgroup_root,
                    "user", &conf->user,
                    "group", &conf->group,
                    "hostName", &conf->host_name,
                    "domainName", &conf->domain_name,
                    NULL))) {
            if (jstr_type(tok+1)!=JSTR_STRING) report_failure_and_exit(
                kStatusRequestInvalid, "%s: expecting a string", key);
            *(const char **)dest = jstr_value(tok+1); tok+=2;
        } else if ((dest = match_key(key,
                    "ro", &conf->ro,
                    "vaRandomize", &conf->va_randomize,
                    NULL))) {
            jstr_type_t t = jstr_type(tok+1);
            if (!(t&(JSTR_TRUE|JSTR_FALSE)))
                report_failure_and_exit(kStatusRequestInvalid,
                    "%s: expecting a boolean", key);
            *(int *)dest = t==JSTR_TRUE; tok+=2;
        } else if (!strcmp(key, kMountsKey)) {
            if (jstr_type(tok+1)!=JSTR_ARRAY)
                report_failure_and_exit(kStatusRequestInvalid,
                    "%s: expecting an array", key);
            tok = jstr_next(conf->mounts = tok+1);
        } else if (!strcmp(key, kCgroupConfigKey)) {
            if (jstr_type(tok+1)!=JSTR_OBJECT)
                report_failure_and_exit(kStatusRequestInvalid,
                    "%s: expecting an object", key);
            tok = jstr_next(conf->cgroup_config = tok+1);
        } else {
            report_failure_and_exit(
                kStatusRequestInvalid, "Unknown key '%s'", key);
        }
    }
}

#if 0
static const jstr_token_t *convert_pipes_array(
    struct sandbozo_command *cmd, const char *key, const jstr_token_t *array) {
    const jstr_token_t *array_end, *tok;
    struct sandbozo_pipe *pipe;
    if (jstr_type(array)!=JSTR_ARRAY) {
        report_failure(kStatusRequestInvalid, "%s: expecting an array", key);
        return NULL;
    }
    array_end = jstr_next(array);
    assert(sizeof(struct sandbozo_pipe) <= 5*sizeof(jstr_token_t));
    pipe = cmd->pipes = (struct sandbozo_pipe *)array;
    for (tok = array+1; tok!=array_end; ++tok, ++pipe) {
        const jstr_token_t *i;
#define ERROR(msg, ...) do {\
        report_failure(kStatusRequestInvalid, "%s[%zu]" msg, \
            key, (size_t)(pipe-cmd->pipes), ## __VA_ARGS__\
        ); return NULL; } while (0)
        if (jstr_type(tok)!=JSTR_OBJECT) ERROR(": expecting an object");
        pipe->fifo = pipe->file = NULL;
        pipe->size_limit = cmd->pipe_size_limit_default;
        for (i=tok+1, tok=jstr_next(tok); i!=tok; i+=2) {
            const char *key2 = jstr_value(i);
            if (!strcmp(key2, "sizeLimit")) {
                double v;
                if (jstr_type(i+1)!=JSTR_NUMBER
                    || (v=strtod(jstr_value(i+1), NULL)) < 0.0
                ) ERROR(".%s: expecting non-negative number", key2);
                pipe->size_limit = v < LONG_MAX ? LONG_MAX : (long)v;
            } else {
                const char **p;
                if (!strcmp(key2, "file")) p = &pipe->file;
                else if (!strcmp(key2, "fifo")) p = &pipe->fifo;
                else ERROR(": unknown key '%s'", key2);
                if (jstr_type(i+1)!=JSTR_STRING) ERROR(".%s: expecting a string", key2);
                *p = jstr_value(i+1);
            }
        }
        if (!pipe->fifo) ERROR(": fifo missing");
        if (!pipe->file) ERROR(": file missing");
#undef ERROR
    }
    cmd->pipes_len = pipe-cmd->pipes;
    return array_end;
}
#endif

const char kPipesKey[] = "pipes";

int parse_command(struct sandbozo_command *cmd, char *request, size_t size) {
    const jstr_token_t *root, *root_end, *tok, *i;
    void *dest;
    if (!(root = parse_request(request, size))) return -1;
    root_end = jstr_next(root); tok = root+1;
    while (tok!=root_end) {
        const char *key=jstr_value(tok);
        if ((dest = match_key(key,
                    "workDir",  &cmd->work_dir,
                    "statusFifo",  &cmd->status_fifo,
                    "pids.max", &cmd->pids_max,
                    NULL))) {
            if (jstr_type(tok+1)!=JSTR_STRING) {
                report_failure(
                    kStatusRequestInvalid, "%s: expecting a string", key);
                return -1;
            }
            *(const char **)dest = jstr_value(tok+1); tok+=2;
        } else if ((dest = match_key(key,
                    "cmd", &cmd->cmd,
                    "env", &cmd->env,
                    NULL))) {
            // convert json array to a NULL-terminated char* array inplace
            const jstr_token_t *i;
            const char **p;
            if (jstr_type(tok+1)!=JSTR_ARRAY) {
                report_failure(kStatusRequestInvalid, "%s: expecting an array", key);
                return -1;
            }
            p = *(const char ***)dest = (const char **)tok;
            for (i = tok+2, tok = jstr_next(tok+1); i!=tok; ++i) {
                if (jstr_type(i)!=JSTR_STRING) {
                    report_failure(kStatusRequestInvalid,
                        "%s[%zu]: expecting a string",
                        key, (size_t)(p-*(const char ***)dest));
                    return -1;
                }
                *p++ = jstr_value(i);
            }
            *p = NULL;
        } else if (!strcmp(key, kPipesKey)) {
            if (jstr_type(tok+1)!=JSTR_ARRAY) {
                report_failure(kStatusRequestInvalid, "%s: expecting an array", key);
                return -1;
            }
            cmd->pipes = tok+1; tok+=2;
        } else if (!strcmp(key, "timeLimit")) {
            double v;
            if (jstr_type(tok+1)!=JSTR_NUMBER
                || (v=strtod(jstr_value(tok+1), NULL)) < 0.0
            ) {
                report_failure(kStatusRequestInvalid,
                    "%s: expecting non-negative number", key);
                return -1;
            }
            tok+=2;
            cmd->time_limit.tv_nsec = (long)(modf(v, &v)*1e9);
            // time_t===long in GNU and MUSL C library
            cmd->time_limit.tv_sec =
                v > LONG_MAX ? (time_t)LONG_MAX : (time_t)v;
        } else {
            report_failure(kStatusRequestInvalid, "Unknown key '%s'", key);
            return -1;
        }
    }
    if (!cmd->cmd || !cmd->cmd[0]) {
        report_failure(kStatusRequestInvalid, "'cmd' empty or missing");
        return -1;
    }
    return 0;
}
