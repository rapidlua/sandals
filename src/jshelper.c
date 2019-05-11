#include "sandals.h"
#include "jshelper.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

const jstr_token_t *jsget_object(
    const jstr_token_t *root, const jstr_token_t *value)
{
    if (jstr_type(value) != JSTR_OBJECT)
        jserror(root, value, "Expecting an object");
    return value;
}

const jstr_token_t *jsget_array(
    const jstr_token_t *root, const jstr_token_t *value)
{
    if (jstr_type(value) != JSTR_ARRAY)
        jserror(root, value, "Expecting an array");
    return value;
}

const char *jsget_str(
    const jstr_token_t *root, const jstr_token_t *value)
{
    if (jstr_type(value) != JSTR_STRING)
        jserror(root, value, "Expecting a string");
    return jstr_value(value);
}

double jsget_udouble(
    const jstr_token_t *root, const jstr_token_t *value)
{
    double v;
    if (jstr_type(value) != JSTR_NUMBER
        || (v = strtod(jstr_value(value), NULL)) < 0.0)
        jserror(root, value, "Expecting non-negative number");
    return v;
}

bool jsget_bool(
    const jstr_token_t *root, const jstr_token_t *value)
{
    jstr_type_t t = jstr_type(value);
    if (!(t & (JSTR_TRUE|JSTR_FALSE)))
        jserror(root, value, "Expecting a boolean");
    return t == JSTR_TRUE;
}

void jsunknown(
    const jstr_token_t *root, const jstr_token_t *value)
{
    jserror(root, value, "Unknown key");
}

void jserror(
    const jstr_token_t *root, const jstr_token_t *value,
    const char *fmt, ...)
{
    char msg[PIPE_BUF];
    size_t msglen = 0;
    va_list ap;

    if (!root) root = value;

    // find path from %root to %value and write to msg
    while (root != value && msglen < sizeof(msg)) {
        const char *key;
        const jstr_token_t *next;
        int index;
        switch (jstr_type(root)) {
        case JSTR_OBJECT:
            JSOBJECT_FOREACH(root, key, next) {
                if (next <= value && jstr_next(next) > value) {
                    msglen += snprintf(
                        msg + msglen, sizeof(msg) - msglen,
                        ".%s" + (msglen == 0), key);
                    goto descent_into_next;
                }
            }
            break;
        case JSTR_ARRAY:
            index = 0;
            JSARRAY_FOREACH(root, next) {
                if (next <= value && jstr_next(next) > value) {
                    msglen += snprintf(
                        msg + msglen, sizeof(msg) - msglen, "[%d]", index);
                    goto descent_into_next;
                }
                ++index;
            }
            break;
        }
        msglen = 0;
        break;
descent_into_next:
        root = next;
    }

    if (msglen && msglen + 2 < sizeof(msg)) {
        strcpy(msg + msglen, ": ");
        msglen += 2;
    }

    va_start(ap, fmt);
    if (msglen >= sizeof(msg)
        || msglen + vsnprintf(msg + msglen, sizeof(msg) - msglen, fmt, ap)
        >= sizeof(msg))
    {
        fail(kStatusResponseTooBig, NULL);
    }

    fail(kStatusRequestInvalid, "%s", msg);
}
