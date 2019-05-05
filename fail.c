#define _GNU_SOURCE
#include "sandals.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

const char kStatusExited[]         = "sys.exited";
const char kStatusKilled[]         = "sys.killed";
const char kStatusOom[]            = "sys.oom";
const char kStatusPids[]           = "sys.pids";
const char kStatusTimeLimit[]      = "sys.time.limit";
const char kStatusPipeLimit[]      = "sys.pipe.limit";
const char kStatusInternalError[]  = "sys.internalerror";
const char kStatusRequestInvalid[] = "sys.request.invalid";
const char kStatusResponseTooBig[] = "sys.response.toobig";
const char kStatusStatusInvalid[]  = "sys.status.invalid";

void fail(const char *status, const char *fmt, ...) {
    struct sandals_response response;
    char buf[PIPE_BUF];
    va_list ap;
    int rc;

    response.size = 0;
    response_append_raw(&response, "{\"status\":\"");
    response_append_esc(&response, status);

    va_start(ap, fmt);

    if (fmt && (rc = vsnprintf(buf, sizeof buf, fmt, ap)) > 0) {
       if (status == kStatusInternalError) log_error("%s", buf);
       if ((size_t)rc >= sizeof buf) fail(kStatusResponseTooBig, NULL);
       response_append_raw(&response, "\",\"description\":\"");
       response_append_esc(&response, buf);
    }
    response_append_raw(&response, "\"}\n");
    response_send(&response);
    exit(EXIT_FAILURE);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s[%d]: ", program_invocation_short_name, getpid());
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
