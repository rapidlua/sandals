#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    JSTR_OBJECT = 0x01,
    JSTR_ARRAY  = 0x02,
    JSTR_STRING = 0x04,
    JSTR_NUMBER = 0x08,
    JSTR_TRUE   = 0x10,
    JSTR_FALSE  = 0x20,
    JSTR_NULL   = 0x40
} jstr_type_t;

typedef struct {
    jstr_type_t type__;
    uintptr_t value__;
} jstr_token_t;

static inline jstr_type_t jstr_type(const jstr_token_t *token) {
    return token->type__;
}

static inline const char *jstr_value(const jstr_token_t *token) {
    return (const char *)token->value__;
}

static inline const jstr_token_t *jstr_next(const jstr_token_t *token) {
    return token + ((token->type__&(JSTR_OBJECT|JSTR_ARRAY)) ?
        token->value__ : 1);
}

typedef struct {
    size_t parse_pos;
    size_t cur_offset;
    size_t parent_offset;
} jstr_parser_t;

static inline void jstr_init(jstr_parser_t *parser) {
    parser->parse_pos = 0;
    parser->cur_offset = 0;
    parser->parent_offset = 0;
}

enum {
    JSTR_INVAL = -1, // parse error
    JSTR_NOMEM = -2, // token array too small
};

ssize_t jstr_parse(
    jstr_parser_t *parser, char *str,
    jstr_token_t *token, size_t token_count
);
