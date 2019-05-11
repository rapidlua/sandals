#include "sandals.h"
#include "jshelper.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int pipe_count(const struct sandals_request *request) {
    const jstr_token_t *item;
    int count = 0;
    if (!request->pipes) return 0;
    JSARRAY_FOREACH(request->pipes, item) count++;
    return count;
}

void pipe_foreach(
    const struct sandals_request *request, void(*fn)(), void *userdata) {

    const jstr_token_t *pipedef, *value;
    const char *key;
    int index = 0;

    if (!request->pipes) return;
    JSARRAY_FOREACH(request->pipes, pipedef) {

        struct sandals_pipe pipe = { .limit = LONG_MAX };
        jsget_object(request->json_root, pipedef);
        JSOBJECT_FOREACH(pipedef, key, value) {

            if (!strcmp(key, "file")) {
                pipe.file = jsget_str(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "fifo")) {
                pipe.fifo = jsget_str(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "stdout")) {
                pipe.stdout = jsget_bool(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "stderr")) {
                pipe.stderr = jsget_bool(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "limit")) {
                double v = jsget_udouble(request->json_root, value);
                pipe.limit = v < LONG_MAX ? (long)v : LONG_MAX;
                continue;
            }

            jsunknown(request->json_root, value);
        }

        if (!pipe.file)
            jserror(request->json_root, pipedef, "'file' missing");

        if (!pipe.stdout && !pipe.stderr && !pipe.fifo)
            jserror(request->json_root, pipedef,
                "'stdout' or 'stderr' or 'fifo' is required");

        fn(index++, &pipe, userdata);
    }
}
