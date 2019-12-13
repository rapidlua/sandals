#include "sandals.h"
#include "jshelper.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char **copy_str_array(
    const jstr_token_t *root, const jstr_token_t *array
) {
    const jstr_token_t *value;
    size_t index = 0;
    const char **vec = malloc(
        sizeof(vec[0]) * (1 + (jstr_next(array) - array)));
    if (!vec) fail(kStatusInternalError, "malloc");
    jsget_array(root, array);
    JSARRAY_FOREACH(array, value) {
        vec[index++] = jsget_str(root, value);
    }
    vec[index] = NULL;
    return vec;
}

void request_parse(struct sandals_request *request, const jstr_token_t *root) {

    const char *key;
    const jstr_token_t *value, *stdstreams = NULL;

    request->json_root = jsget_object(NULL, root);
    JSOBJECT_FOREACH(root, key, value) {

        if (!strcmp(key, "hostName")) {
            request->host_name = jsget_str(root, value);
            continue;
        }

        if (!strcmp(key, "domainName")) {
            request->domain_name = jsget_str(root, value);
            continue;
        }

        if (!strcmp(key, "uid")) {
            double v = jsget_udouble(root, value);
            if (v > INT32_MAX) jserror(root, value, "Value too big");
            request->uid = (uid_t)v;
            continue;
        }

        if (!strcmp(key, "gid")) {
            double v = jsget_udouble(root, value);
            if (v > INT32_MAX) jserror(root, value, "Value too big");
            request->gid = (gid_t)v;
            continue;
        }

        if (!strcmp(key, "chroot")) {
            request->chroot = jsget_str(root, value);
            continue;
        }

        if (!strcmp(key, "mounts")) {
            request->mounts = jsget_array(root, value);
            continue;
        }

        if (!strcmp(key, "cgroup")) {
            request->cgroup = jsget_str(root, value);
            continue;
        }

        if (!strcmp(key, "cgroupRoot")) {
            request->cgroup_root = jsget_str(root, value);
            continue;
        }

        if (!strcmp(key, "cgroupConfig")) {
            request->cgroup_config = jsget_object(root, value);
            continue;
        }

        if (!strcmp(key, "seccompPolicy")) {
            request->seccomp_policy = jsget_str(root, value);
            continue;
        }

        if (!strcmp(key, "vaRandomize")) {
            request->va_randomize = jsget_bool(root, value);
            continue;
        }

        if (!strcmp(key, "cmd")) {
            request->cmd = copy_str_array(root, value);
            continue;
        }

        if (!strcmp(key, "env")) {
            request->env = copy_str_array(root, value);
            continue;
        }

        if (!strcmp(key, "workDir")) {
            request->work_dir = jsget_str(root, value);
            continue;
        }

        if (!strcmp(key, "timeLimit")) {
            double v = jsget_udouble(root, value);
            request->time_limit.tv_nsec = (long)(modf(v, &v)*1e9);
            // time_t===long in GNU and MUSL C library
            request->time_limit.tv_sec =
                v > LONG_MAX ? (time_t)LONG_MAX : (time_t)v;
            continue;
        }

        if (!strcmp(key, "stdStreams")) {
            stdstreams = jsget_object(root, value);
            continue;
        }

        if (!strcmp(key, "pipes")) {
            request->pipes = jsget_array(root, value);
            continue;
        }

        jsunknown(root, value);
    }

    if (stdstreams) {
        JSOBJECT_FOREACH(stdstreams, key, value) {
            if (!strcmp(key, "dest")) {
                request->stdstreams_dest = jsget_str(root, value);
                continue;
            }
            if (!strcmp(key, "limit")) {
                double v = jsget_udouble(root, value);
                request->stdstreams_limit =
                    v < LONG_MAX ? (long)v : LONG_MAX;
                continue;
            }
            jsunknown(root, value);
        }

        if (!request->stdstreams_dest)
            jserror(root, stdstreams, "'dest' missing");
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

    request_parse(request, root);
}
