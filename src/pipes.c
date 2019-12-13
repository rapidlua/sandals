#include "sandals.h"
#include "jshelper.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int pipe_count(const struct sandals_request *request, int *real_count) {
    const jstr_token_t *item;
    int count;
    *real_count = 0;
    if (request->pipes) {
        JSARRAY_FOREACH(request->pipes, item) (*real_count)++;
    }
    count = *real_count;
    if (request->copy_files) {
        JSARRAY_FOREACH(request->copy_files, item) count++;
    }
    return count;
}

static void pipe_init(
    const struct sandals_request *request, const jstr_token_t *pipedef,
    struct sandals_pipe *pipe) {

    const char *key;
    const jstr_token_t *value;

    jsget_object(request->json_root, pipedef);
    JSOBJECT_FOREACH(pipedef, key, value) {

        if (!strcmp(key, "dest")) {
            pipe->dest = jsget_str(request->json_root, value);
            continue;
        }

        if (!strcmp(key, "src")) {
            pipe->src = jsget_str(request->json_root, value);
            continue;
        }

        if (!strcmp(key, "stdout")) {
            pipe->as_stdout = jsget_bool(request->json_root, value);
            continue;
        }

        if (!strcmp(key, "stderr")) {
            pipe->as_stderr = jsget_bool(request->json_root, value);
            continue;
        }

        if (!strcmp(key, "limit")) {
            double v = jsget_udouble(request->json_root, value);
            pipe->limit = v < LONG_MAX ? (long)v : LONG_MAX;
            continue;
        }

        jsunknown(request->json_root, value);
    }

    if (!pipe->dest)
        jserror(request->json_root, pipedef, "'dest' missing");
}

void pipe_foreach(
    const struct sandals_request *request, void(*fn)(), void *userdata) {

    const jstr_token_t *pipedef;
    int index = 0;

    if (request->pipes) {
        JSARRAY_FOREACH(request->pipes, pipedef) {

            struct sandals_pipe pipe = { .limit = LONG_MAX };
            pipe_init(request, pipedef, &pipe);

            if (!pipe.as_stdout && !pipe.as_stderr && !pipe.src)
                jserror(request->json_root, pipedef,
                    "'stdout' or 'stderr' or 'src' is required");

            fn(index++, &pipe, userdata);
        }
    }

    if (request->copy_files) {
        JSARRAY_FOREACH(request->copy_files, pipedef) {

            struct sandals_pipe pipe = { .limit = LONG_MAX };
            pipe_init(request, pipedef, &pipe);

            if (!pipe.src)
                jserror(request->json_root, pipedef, "'src' missing");

            fn(index++, &pipe, userdata);
        }
    }
}
