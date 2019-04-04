#include "sandals.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int pipe_count(const struct sandals_request *request) {
    const jstr_token_t *pipes_end, *tok;
    int count = 0;
    if (!request->pipes) return 0;
    pipes_end = jstr_next(request->pipes);
    for (tok = request->pipes+1; tok != pipes_end; tok = jstr_next(tok))
        ++count;
    return count;
}

void pipe_foreach(
    const struct sandals_request *request, void(*fn)(), void *userdata) {

    const jstr_token_t *pipes_end, *tok, *i;
    int index=0;
    if (!request->pipes) return;
    pipes_end = jstr_next(request->pipes);
    for (tok = request->pipes+1; tok != pipes_end; ) {
        struct sandals_pipe pipe = { .limit = LONG_MAX };
        if (jstr_type(tok)!=JSTR_OBJECT)
            fail(kStatusRequestInvalid,
                "%s[%d]: expecting an object", kPipesKey, index);
        for (i=tok+1, tok=jstr_next(tok); i!=tok; i+=2) {
            const char *key = jstr_value(i), **dest;
            if ((dest = match_key(key,
                "file", &pipe.file, "fifo", &pipe.fifo, NULL))
            ) {
                if (jstr_type(i+1)!=JSTR_STRING)
                    fail(kStatusRequestInvalid,
                        "%s[%d].%s: expecting a string",
                        kPipesKey, index, key);
                *dest = jstr_value(i+1);
            } else if (!strcmp(key, "limit")) {
                double v;
                if (jstr_type(i+1)!=JSTR_NUMBER
                    || (v=strtod(jstr_value(i+1), NULL)) < 0.0
                ) fail(kStatusRequestInvalid,
                    "%s[%d].%s: expecting non-negative number",
                    kPipesKey, index, key);
                pipe.limit = v < LONG_MAX ? (long)v : LONG_MAX;
            } else
                fail(kStatusRequestInvalid,
                    "%s[%d]: unknown key '%s'", kPipesKey, index, key);
        }
        if (!pipe.file)
            fail(kStatusRequestInvalid,
                "%s[%d]: 'file' missing", kPipesKey, index);
        if (!pipe.fifo)
            fail(kStatusRequestInvalid,
                "%s[%d]: 'fifo' missing", kPipesKey, index);
        fn(index++, &pipe, userdata);
    }
}
