#include "sandals.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

int response_fd = STDOUT_FILENO;

void response_append_raw(struct sandals_response *response, const char *str) {
    size_t len = strlen(str);
    response->size += len;
    if (response->size <= sizeof response->buf)
        memcpy(response->buf + response->size - len, str, len);
}

void response_append_esc(struct sandals_response *response, const char *str) {
    char *p = response->buf + response->size, *e = response->overflow;
    for (; *str && p < e; ++str) {
        unsigned c = *(const unsigned char *)str;
        if (c >= 0x20 && c != '\\' && c != '"') *p++ = *str;
        else {
            static const char hex[]="0123456789abcdef";
            *p = '\\'; p += 2;
            switch (c) {
            case '"':
            case '\\': p[-1] = c; break;
            case '\b': p[-1] = 'b'; break;
            case '\f': p[-1] = 'f'; break;
            case '\n': p[-1] = 'n'; break;
            case '\r': p[-1] = 'r'; break;
            case '\t': p[-1] = 't'; break;
            default:
                p[-1] = 'u'; p[0] = p[1] = '0';
                p[2] = hex[c>>4]; p[3] = hex[c&15];
                p += 4;
            }
        }
    }
    response->size = p - response->buf;
}

void response_append_int(struct sandals_response *response, int value) {
    char buf[16];
    sprintf(buf, "%d", value);
    response_append_raw(response, buf);
}

void response_send(const struct sandals_response *response) {
    const char *p, *e;
    size_t rc;

    if (response->size > sizeof response->buf)
        fail(kStatusResponseTooBig, NULL);
    if (response_fd == -1) return;

    p = response->buf; e = p + response->size;
    while (p != e) {
        rc = write(response_fd, p, e - p);
        if (rc < 0) {
            if (errno == EINTR) continue;
            log_error("Sending response: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        p += rc;
    }

    close(response_fd); response_fd = -1;
}
