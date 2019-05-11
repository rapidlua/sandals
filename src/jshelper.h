#pragma once
#include "jstr/jstr.h"
#include <stdbool.h>

#define JSARRAY_FOREACH(array, item) \
    for ((item) = (array) + 1; (item) != jstr_next(array); \
        (item) = jstr_next(item))

#define JSOBJECT_FOREACH(object, key, value) \
    for ((value) = (object) + 2; \
        (value) < jstr_next(object) && (key = jstr_value(value-1), 1); \
        (value) = jstr_next(value) + 1)

const jstr_token_t *jsget_object(
    const jstr_token_t *root, const jstr_token_t *value);

const jstr_token_t *jsget_array(
    const jstr_token_t *root, const jstr_token_t *value);

const char *jsget_str(
    const jstr_token_t *root, const jstr_token_t *value);

double jsget_udouble(
    const jstr_token_t *root, const jstr_token_t *value);

bool jsget_bool(
    const jstr_token_t *root, const jstr_token_t *value);

void jsunknown(
    const jstr_token_t *root, const jstr_token_t *value)
__attribute__((noreturn));

void jserror(
    const jstr_token_t *root, const jstr_token_t *value,
    const char *fmt, ...)
__attribute__((noreturn, format(printf, 3, 4)));

