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

            if (!strcmp(key, "dest")) {
                pipe.dest = jsget_str(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "src")) {
                pipe.src = jsget_str(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "stdout")) {
                pipe.as_stdout = jsget_bool(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "stderr")) {
                pipe.as_stderr = jsget_bool(request->json_root, value);
                continue;
            }

            if (!strcmp(key, "limit")) {
                double v = jsget_udouble(request->json_root, value);
                pipe.limit = v < LONG_MAX ? (long)v : LONG_MAX;
                continue;
            }

            jsunknown(request->json_root, value);
        }

        if (!pipe.dest)
            jserror(request->json_root, pipedef, "'dest' missing");

        if (!pipe.as_stdout && !pipe.as_stderr && !pipe.src)
            jserror(request->json_root, pipedef,
                "'stdout' or 'stderr' or 'src' is required");

        fn(index++, &pipe, userdata);
    }
}
