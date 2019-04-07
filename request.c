#include "sandals.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

const char kMountsKey[]       = "mounts";
const char kCgroupConfigKey[] = "cgroupConfig";
const char kPipesKey[]        = "pipes";

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

void request_parse(struct sandals_request *request, const jstr_token_t *root) {
    const jstr_token_t *root_end, *tok;
    struct sandals_request init = {
        .host_name    = "sandals",
        .domain_name  = "sandals",
        .chroot       = "/",
        .va_randomize = 1,
        .work_dir     = "/",
        .time_limit   = { .tv_sec = LONG_MAX }
    };

    *request = init;

    tok = root + 1; root_end = jstr_next(root);
    while (tok != root_end) {
        const char *key = jstr_value(tok);
        void *dest;
        if ((dest = match_key(key,
                "hostName",      &request->host_name,
                "domainName",    &request->domain_name,
                "user",          &request->user,
                "group",         &request->group,
                "chroot",        &request->chroot,
                "cgroupRoot",    &request->cgroup_root,
                "seccompPolicy", &request->seccomp_policy,
                "workDir",       &request->work_dir,
                "statusFifo",    &request->status_fifo,
                NULL))) {
            if (jstr_type(tok+1) != JSTR_STRING) fail(
                kStatusRequestInvalid, "%s: expecting a string", key);
            *(const char **)dest = jstr_value(tok + 1); tok += 2;

        } else if ((dest = match_key(key,
                kMountsKey, &request->mounts,
                kPipesKey,  &request->pipes,
                NULL))) {
            if (jstr_type(tok+1) != JSTR_ARRAY) fail(
                kStatusRequestInvalid, "%s: expecting an array", key);
            *(const jstr_token_t **)dest = tok+1;
            tok = jstr_next(tok+1);

        } else if ((dest = match_key(key,
                "cmd",      &request->cmd,
                "env",      &request->env,
                NULL))) {
            // convert json array to a NULL-terminated char* array in-place
            const jstr_token_t *i;
            const char **p;
            if (jstr_type(tok+1) != JSTR_ARRAY) fail(
                kStatusRequestInvalid, "%s: expecting an array", key);
            p = *(const char ***)dest = (const char **)tok;
            for (i = tok+2, tok = jstr_next(tok+1); i != tok; ++i) {
                if (jstr_type(i) != JSTR_STRING) {
                    fail(kStatusRequestInvalid,
                        "%s[%zu]: expecting a string",
                        key, (size_t)(p-*(const char ***)dest));
                }
                *p++ = jstr_value(i);
            }
            *p = NULL;

        } else if (!strcmp(key, kCgroupConfigKey)) {
            if (jstr_type(tok+1) != JSTR_OBJECT)
                fail(kStatusRequestInvalid, "%s: expecting an object", key);
            tok = jstr_next(request->cgroup_config = tok+1);

        } else if (!strcmp(key, "vaRandomize")) {
            jstr_type_t t = jstr_type(tok+1);
            if (!(t&(JSTR_TRUE|JSTR_FALSE)))
                fail(kStatusRequestInvalid, "%s: expecting a boolean", key);
            request->va_randomize = t==JSTR_TRUE; tok += 2;

        } else if (!strcmp(key, "timeLimit")) {
            double v;
            if (jstr_type(tok+1) != JSTR_NUMBER
                || (v=strtod(jstr_value(tok+1), NULL)) < 0.0
            ) fail(kStatusRequestInvalid,
                "%s: expecting non-negative number", key);
            tok += 2;
            request->time_limit.tv_nsec = (long)(modf(v, &v)*1e9);
            // time_t===long in GNU and MUSL C library
            request->time_limit.tv_sec =
                v > LONG_MAX ? (time_t)LONG_MAX : (time_t)v;

        } else fail(kStatusRequestInvalid, "Unknown key '%s'", key);
    }

    if (!request->cmd || !request->cmd[0])
        fail(kStatusRequestInvalid, "'cmd' missing or empty");
}

void request_recv(struct sandals_request *request) {
    enum { TOKEN_MIN = 64 };

    char *buf = NULL;
    size_t size = 0, data_size = 0;
    ssize_t rc;
    jstr_parser_t parser;
    jstr_token_t *root = NULL;
    size_t token_count = 0;

    while (1) {
        if (size - data_size <= PIPE_BUF/2) {
            size = size ? 2*size : PIPE_BUF;
            if (!(buf = realloc(buf, size)))
                fail(kStatusInternalError, "malloc");
        }
        if ((rc = read(STDIN_FILENO, buf+data_size, size-data_size-1)) < 0) {
            if (errno == EINTR) continue;
            fail(kStatusInternalError,
                "Reading request: %s", strerror(errno));
        }
        if (!rc) break;
        data_size += rc;
    }

    buf[data_size] = 0;
    jstr_init(&parser);
    while ((rc = jstr_parse(&parser, buf, root, token_count)) == JSTR_NOMEM) {
        token_count = token_count ? 2*token_count : TOKEN_MIN;
        if(!(root = realloc(root, sizeof(root[0])*token_count)))
            fail(kStatusInternalError, "malloc");
    }
    if (rc < 0 || (size_t)data_size != rc)
        fail(kStatusRequestInvalid, NULL);
    if (jstr_type(root) != JSTR_OBJECT)
        fail(kStatusRequestInvalid, "Expecting an object");

    request_parse(request, root);
}
