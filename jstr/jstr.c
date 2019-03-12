#include "jstr.h"

static inline void token_init(
    jstr_token_t *token, jstr_type_t type, uintptr_t value
) {
    token->type__ = type;
    token->value__ = value;
}

// > 0: success, end pos
// < 0: error
ssize_t jstr_parse(
    jstr_parser_t *parser, char *str,
    jstr_token_t *token, size_t token_count
) {
    static const unsigned char ws[8] = {
        [9&7]=9, [0xa&7]=0xa, [0xd&7]=0xd, [0x20&7]=0x20
    };
#define WS(v) (ws[(v)&7]==(v))
    unsigned char *p = (unsigned char *)str + parser->parse_pos;
    jstr_token_t *token_cur = token + parser->cur_offset;
    jstr_token_t *token_end = token + token_count;
    jstr_token_t *token_parent = token + parser->parent_offset;
    enum {
        TOP, OBJECT_VALUE = JSTR_OBJECT, ARRAY_ITEM = JSTR_ARRAY, OBJECT_KEY
    } cs = token_parent == token_cur ? TOP : jstr_type(token_parent);
parse_generic:
    if (token_end - token_cur < 2) {
        parser->parse_pos = (char *)p-str;
        parser->cur_offset = token_cur - token;
        parser->parent_offset = token_parent - token;
        return JSTR_NOMEM;
    }
    while (WS(*p)) ++p;
    switch (*p) {
    default:
        return JSTR_INVAL;
    case '[':
        token_init(token_cur, JSTR_ARRAY, token_cur - token_parent);
        token_parent = token_cur++; cs = ARRAY_ITEM;
        do { ++p; } while (WS(*p));
        if (*p==']') { ++p; goto pop_context; }
        goto parse_generic;
    case '{':
        token_init(token_cur, JSTR_OBJECT, token_cur - token_parent);
        token_parent = token_cur++; cs = OBJECT_KEY;
        do { ++p; } while (WS(*p));
        if (*p=='}') { ++p; goto pop_context; }
        goto parse_object_key;
    case '-':
    case '0'...'9':
        token_init(token_cur++, JSTR_NUMBER, (uintptr_t)p);
        if (*p=='-') ++p;
        if (*p=='0' && (unsigned)p[1]-'0'<=9) return JSTR_INVAL;
        while ((unsigned)*p-'0'<=9) ++p;
        if (*p=='.') {
            if ((unsigned)p[1]-'0'>9) return JSTR_INVAL;
            p += 2;
            while ((unsigned)*p-'0'<=9) ++p;
        }
        if ((*p|0x20)=='e') {
            ++p;
            if (*p=='-'||*p=='+') ++p;
            if ((unsigned)*p-'0'>9) return JSTR_INVAL;
            while ((unsigned)*p-'0'<=9) ++p;
        }
        goto commit_token;
    case '"':
        goto parse_string;
    case 't':
        token_init(token_cur++, JSTR_TRUE, (uintptr_t)p);
        goto parse_true_false_or_null;
    case 'f':
        token_init(token_cur++, JSTR_FALSE, (uintptr_t)p);
        goto parse_true_false_or_null;
    case 'n':
        token_init(token_cur++, JSTR_NULL, (uintptr_t)p);
        goto parse_true_false_or_null;
    }

parse_string: {
        unsigned char *out = ++p;
        token_init(token_cur++, JSTR_STRING, (uintptr_t)out);
        while (1) {
parse_char:
            if ((unsigned)*p-0x20 <= 0x7f-0x20) { // ascii
                if (*p=='\\') goto parse_escape;
                if (*p=='"') {
                    *out = 0; ++p; goto commit_token;
                }
                *out++ = *p++;
            } else if ((unsigned)*p-0xc2 <= 0xf4-0xc2) { // utf8 2..4 bytes
                static const unsigned char ltab[] = { 0,0,0,0, 1,1, 2 };
                unsigned len = ltab[(*p>>3)&7]; // len+2 is the len
                unsigned code = *p&(0x1f>>len) | 1<<((3-len)*6);
                *out++ = *p++;
                while (code <= 0x1fffff) {
                    if ((unsigned)*p-0x80 > 0xbf-0x80) return JSTR_INVAL;
                    code = (code<<6) | (*p&0x3f);
                    *out++ = *p++;
                }
                code &= 0x1fffff;
                if ( // overlong encoding, surrogate or out of range?
                    !(code&(0x7c0<<(len*5))) ||
                    code-0xd800 <= 0xdfff-0xd800 || code > 0x10ffff
                ) return JSTR_INVAL;
            } else return JSTR_INVAL;
        }

parse_escape: {
#define ESCIDX(v) ((((v)*7)>>4)&15)
            static const char escmap[16][2] = {
                [ESCIDX('"')] = {'"', '"'},
                [ESCIDX('\\')] = {'\\', '\\'},
                [ESCIDX('/')] = {'/', '/'},
                [ESCIDX('b')] = {'b', '\b'},
                [ESCIDX('f')] = {'f', '\f'},
                [ESCIDX('n')] = {'n', '\n'},
                [ESCIDX('r')] = {'r', '\r'},
                [ESCIDX('t')] = {'t', '\t'}
            };
            const char *e = escmap[ESCIDX(p[1])];
            if (p[1] == e[0]) {
                *out++=e[1]; p+=2;
            } else if (p[1] == 'u') {
                unsigned code = 1, codehi = 0;
parse_u_escape:
                p += 2;
                while (code <= 0xffff) {
                    if ((unsigned)*p - '0' <= 9) {
                        code = (code<<4) | (*p++-'0');
                    } else if ((unsigned)(*p|0x20)-'a' <= 'f'-'a') {
                        code = (code<<4) | ((*p++|0x20)-'a'+10);
                    } else return JSTR_INVAL;
                }
                code &= 0xffff;
                if (codehi) {
                    if (code - 0xdc00 <= 0x3ff) {
                        code = codehi | (code - 0xdc00);
                    } else {
                        *out = 0xef; out[1] = 0xbf; out[2] = 0xbd;
                        out += 3;
                    }
                }
                if (code - 0xd800 <= 0x3ff) {
                    if (*p=='\\' && p[1]=='u') {
                        codehi = 0x10000 + ((code - 0xd800) << 10);
                        code = 1; goto parse_u_escape;
                    }
                    code = 0xfffd;
                }
                if (!code || code - 0xdc00 <= 0x3ff) code = 0xfffd;
                if (code <= 0x7f) *out++ = code;
                else {
                    int len = code<=0x7ff ? 2 : code<=0xffff ? 3 : 4;
                    code <<= 6*(4-len);
                    out[0] = (0xf0 << (4-len)) | (code >> 18);
                    out[1] = 0x80 | (code >> 12) & 0x3f;
                    out[2] = 0x80 | (code >> 6) & 0x3f;
                    out[3] = 0x80 | code & 0x3f;
                    out += len;
                }
            } else return JSTR_INVAL; // Unrecognized \escape
            goto parse_char;
        }
    }

parse_true_false_or_null: {
        unsigned v = 0;
        do {
            v = (v<<5)|(*p-'a');
        } while (((unsigned)*++p-'a')<='z'-'a');
        if (v==640644 || v==5254724 || v==446827) goto commit_token;
        return JSTR_INVAL;
    }

parse_object_key:
    while (WS(*p)) ++p;
    if (*p != '"') return JSTR_INVAL;
    goto parse_string;

commit_token: {
        unsigned char c = *p;
        *p++ = 0;
        while (WS(c)) c=*p++;
        switch (cs) {
        case TOP:
            if (c) return JSTR_INVAL;
            return (char *)p-str-1;
        case OBJECT_KEY:
            if (c != ':') return JSTR_INVAL;
            cs = OBJECT_VALUE; goto parse_generic;
        case OBJECT_VALUE:
            if (c == '}') goto pop_context;
            if (c != ',') return JSTR_INVAL;
            cs = OBJECT_KEY;
            goto parse_object_key;
        case ARRAY_ITEM:
            if (c == ']') goto pop_context;
            if (c != ',') return JSTR_INVAL;
            goto parse_generic;
        }
    }

pop_context: {
        jstr_token_t *token_grandparent = token_parent
            - token_parent->value__;
        token_parent->value__ = token_cur-token_parent;
        cs = token_parent == token_grandparent
            ? TOP : jstr_type(token_grandparent);
        token_parent = token_grandparent;
        goto commit_token;
    }
}
