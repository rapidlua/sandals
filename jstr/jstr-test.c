#include "jstr.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *jstr_type_to_str(jstr_type_t t) {
    static const char* types[] = {
       "OBJECT", "ARRAY", "STRING", "NUMBER", "TRUE", "FALSE", "NULL"
    };
    if (!(t&0x7f) || (t&(t-1))) return "<ERROR>";
    return types[__builtin_ctzl(t)];
}

int err = 0;

static void test(int lineno, const char *json, ssize_t rc_expected, ... ) {
#define test(...) test(__LINE__, __VA_ARGS__)
    jstr_parser_t parser;
    jstr_token_t *token = NULL;
    size_t token_count = 0;
    ssize_t rc;
    char *json_copy;

    jstr_init(&parser);
    json_copy = strdup(json);
    while ((rc = jstr_parse(
        &parser, json_copy, token, token_count)) == JSTR_NOMEM
    ) {
        token = realloc(token, ++token_count*(sizeof *token));
    }
    if (rc != rc_expected) {
        fprintf(
            stderr, "@%04d: (expected) %zi != %zi\n", lineno, rc_expected, rc
        );
        ++err;
    }
    if (rc > 0 && rc_expected > 0) {
        va_list ap;
        va_start(ap, rc_expected);
        for (size_t i = 0; ; ++i) {
            int type_match;
            jstr_type_t type_expected = va_arg(ap, int);
            if (!type_expected) break;
            if (!(type_match = jstr_type(token+i) == type_expected)) {
                fprintf(
                    stderr, "@%04d: [%03zu] (expected) %s != %s (%d != %d)\n",
                    lineno, i,
                    jstr_type_to_str(type_expected),
                    jstr_type_to_str(jstr_type(token+i)),
                    type_expected, jstr_type(token+i)
                );
                ++err;
            }
            if (type_expected&(JSTR_ARRAY|JSTR_OBJECT)) {
                size_t offset_expected = va_arg(ap, size_t);
                if (type_match && (token+i)->value__ != offset_expected) {
                    fprintf(
                        stderr, "@%04d: [%03zu] %6s (expected) %zu != %zu\n",
                        lineno, i, jstr_type_to_str(type_expected),
                        offset_expected, (token + i)->value__
                    );
                }
            } else {
                const char *value_expected = va_arg(ap, const char *);
                if (type_match && strcmp(jstr_value(token+i), value_expected)) {
                    fprintf(
                        stderr, "@%04d: [%03zu] %6s (expected) '%s' != '%s'\n",
                        lineno, i, jstr_type_to_str(type_expected),
                        value_expected, jstr_value(token + i)
                    );
                    ++err;
                }
            }
        }
        va_end(ap);
    }
    free(token);
    free(json_copy);
}

int main() {

    // jstr_type_to_str tests
    test("\"OBJECT\"", 8, JSTR_STRING, jstr_type_to_str(JSTR_OBJECT), 0);
    test("\"ARRAY\"", 7, JSTR_STRING, jstr_type_to_str(JSTR_ARRAY), 0);
    test("\"STRING\"", 8, JSTR_STRING, jstr_type_to_str(JSTR_STRING), 0);
    test("\"NUMBER\"", 8, JSTR_STRING, jstr_type_to_str(JSTR_NUMBER), 0);
    test("\"TRUE\"", 6, JSTR_STRING, jstr_type_to_str(JSTR_TRUE), 0);
    test("\"FALSE\"", 7, JSTR_STRING, jstr_type_to_str(JSTR_FALSE), 0);
    test("\"NULL\"", 6, JSTR_STRING, jstr_type_to_str(JSTR_NULL), 0);

    test("", JSTR_INVAL);
    test("   ", JSTR_INVAL);

    // string
    test("\"", JSTR_INVAL);
    test("\"foo", JSTR_INVAL);
    test("\"\"", 2, JSTR_STRING, "", 0);
    test("\"\"junk", JSTR_INVAL);
    test("\"foo\"", 5, JSTR_STRING, "foo", 0);
    test("  \"foo\"", 7, JSTR_STRING, "foo", 0);
    test("\"foo\"  ", 7, JSTR_STRING, "foo", 0);
    test(" \t\n\"foo\"", 8, JSTR_STRING, "foo", 0);
    test("\"foo\"junk", JSTR_INVAL);
    test("\"\\", JSTR_INVAL);
    test("\"--\\\"--\"", 8, JSTR_STRING, "--\"--", 0);
    test("\"--\\\\--\"", 8, JSTR_STRING, "--\\--", 0);
    test("\"--\\/--\"", 8, JSTR_STRING, "--/--", 0);
    test("\"--\\b--\"", 8, JSTR_STRING, "--\b--", 0);
    test("\"--\\f--\"", 8, JSTR_STRING, "--\f--", 0);
    test("\"--\\n--\"", 8, JSTR_STRING, "--\n--", 0);
    test("\"--\\r--\"", 8, JSTR_STRING, "--\r--", 0);
    test("\"--\\t--\"", 8, JSTR_STRING, "--\t--", 0);
    test("\"--\\a--\"", JSTR_INVAL);
    test("\"--\\c--\"", JSTR_INVAL);
    test("\"--\\d--\"", JSTR_INVAL);
    test("\"--\\e--\"", JSTR_INVAL);
    test("\"--\\g--\"", JSTR_INVAL);
    test("\"--\\h--\"", JSTR_INVAL);
    test("\"--\\'--\"", JSTR_INVAL);
    test("\"--\\u--\"", JSTR_INVAL);
    test("\"--\\u0--\"", JSTR_INVAL);
    test("\"--\\u00--\"", JSTR_INVAL);
    test("\"--\\u000--\"", JSTR_INVAL);
    test("\"--\\u000", JSTR_INVAL);
    test("\"--\\u0041--\"", 12, JSTR_STRING, "--A--", 0);
    test("\"--\\u00410--\"", 13, JSTR_STRING, "--A0--", 0);
    test("\"--\\u002d--\"", 12, JSTR_STRING, "-----", 0);
    test("\"--\\u002D--\"", 12, JSTR_STRING, "-----", 0);
    test("\"--\\u0000--\"", 12, JSTR_STRING, "--�--", 0);
    test("\"--\\ud83d--\"", 12, JSTR_STRING, "--�--", 0);
    test("\"--\\ud83d\\ude02--\"", 18, JSTR_STRING, "--😂--", 0);
    test("\"--\\ude02\\ud83d--\"", 18, JSTR_STRING, "--��--", 0);
    test("\"--\\ude02\\ud83d\\ude02--\"", 24, JSTR_STRING, "--�😂--", 0);
    test("\"--\\ud800--\"", 12, JSTR_STRING, "--�--", 0);
    test("\"--\\ud7ff--\"", 12, JSTR_STRING, "--퟿--", 0);
    test("\"--\\udfff--\"", 12, JSTR_STRING, "--�--", 0);
    test("\"--\\ue000--\"", 12, JSTR_STRING, "----", 0);
    test("\"\001\"", JSTR_INVAL);
    test("\"\002\"", JSTR_INVAL);
    test("\"\003\"", JSTR_INVAL);
    test("\"\037\"", JSTR_INVAL);
    test("\"\040\"", 3, JSTR_STRING, " ", 0);
    test("\"\177\"", 3, JSTR_STRING, "\177", 0);
    test("\"\200\"", JSTR_INVAL /* stray continuation byte */);
    test("\"\300\200\"", JSTR_INVAL);
    test("\"\301\"", JSTR_INVAL);
    test("\"\301\200\"", JSTR_INVAL);
    test("\"\302\"", JSTR_INVAL);
    test("\"\302 \"", JSTR_INVAL);
    test("\"\302\200\"", 4, JSTR_STRING, "\302\200", 0);
    test("\"Привет, мир!\"", 23, JSTR_STRING, "Привет, мир!", 0);
    test("\"\360\237\230\202\"", 6, JSTR_STRING, "😂", 0);
    test("\"\360\237\230-", JSTR_INVAL);
    test("\"\364\217\277\277\"", 6, JSTR_STRING, "\364\217\277\277", 0);
    test("\"\364\217\277\300\"", JSTR_INVAL /* out-of-bounds */ );
    test("\"\364\277\277\277\"", JSTR_INVAL);
    test("\"\365\200\200\200\"", JSTR_INVAL);
    test("\"\355\237\277\"", 5, JSTR_STRING, "\355\237\277", 0);
    test("\"\355\237\300\"", JSTR_INVAL /* surrogate*/ );
    test("\"\356\200\177\"", JSTR_INVAL /* surrogate */);
    test("\"\356\200\200\"", 5, JSTR_STRING, "\356\200\200", 0);
    test("\"\xf0\x82\x82\xac\"", JSTR_INVAL /* overlong */ );

    // number
    test("0", 1, JSTR_NUMBER, "0", 0);
    test("42", 2, JSTR_NUMBER, "42", 0);
    test("42junk", JSTR_INVAL);
    test("42 true ", JSTR_INVAL);
    test("1,2", JSTR_INVAL);
    test("999", 3, JSTR_NUMBER, "999", 0);
    test("-0", 2, JSTR_NUMBER, "-0", 0);
    test("+0", JSTR_INVAL);
    test("01", JSTR_INVAL);
    test("-01", JSTR_INVAL);
    test("0.", JSTR_INVAL);
    test("0.0", 3, JSTR_NUMBER, "0.0", 0);
    test("-0.77", 5, JSTR_NUMBER, "-0.77", 0);
    test("0E123", 5, JSTR_NUMBER, "0E123", 0);
    test("0e0123", 6, JSTR_NUMBER, "0e0123", 0);
    test("-0.77E-42", 9, JSTR_NUMBER, "-0.77E-42", 0);
    test("-0.938e+310", 11, JSTR_NUMBER, "-0.938e+310", 0);

    // object
    test("{", JSTR_INVAL);
    test("}", JSTR_INVAL);
    test("{]", JSTR_INVAL);
    test("{}", 2, JSTR_OBJECT, (size_t)1, 0);
    test("{},", JSTR_INVAL);
    test("{}}", JSTR_INVAL);
    test("{}junk", JSTR_INVAL);
    test("{1}", JSTR_INVAL);
    test("{{}:1}", JSTR_INVAL);
    test("{[]:1}", JSTR_INVAL);
    test("{true:1}", JSTR_INVAL);
    test("{\"number\":42,\"true\":true,\"false\":false,"
        "\"null\":null,\"string\":\"string\"}",
        69,
        JSTR_OBJECT, (size_t) 11,
        JSTR_STRING, "number", JSTR_NUMBER, "42",
        JSTR_STRING, "true", JSTR_TRUE, "true",
        JSTR_STRING, "false", JSTR_FALSE, "false",
        JSTR_STRING, "null", JSTR_NULL, "null",
        JSTR_STRING, "string", JSTR_STRING, "string",
        0
    );
    test("{\"A\":{\"B\":{\"C\":{}}}}", 20,
        JSTR_OBJECT, (size_t)7,
        JSTR_STRING, "A",
        JSTR_OBJECT, (size_t)5,
        JSTR_STRING, "B",
        JSTR_OBJECT, (size_t)3,
        JSTR_STRING, "C",
        JSTR_OBJECT, (size_t)1,
        0
    );
    test("{\"A\":{\"B\":{\"C\":{}}},\"D\":42}", 27,
        JSTR_OBJECT, (size_t)9,
        JSTR_STRING, "A",
        JSTR_OBJECT, (size_t)5,
        JSTR_STRING, "B",
        JSTR_OBJECT, (size_t)3,
        JSTR_STRING, "C",
        JSTR_OBJECT, (size_t)1,
        JSTR_STRING, "D",
        JSTR_NUMBER, "42",
        0
    );
    test("{\"A\":{\"B\":{\"C\":[1,2,3]}},\"D\":42}", 32,
        JSTR_OBJECT, (size_t)12,
        JSTR_STRING, "A",
        JSTR_OBJECT, (size_t)8,
        JSTR_STRING, "B",
        JSTR_OBJECT, (size_t)6,
        JSTR_STRING, "C",
        JSTR_ARRAY, (size_t)4,
        JSTR_NUMBER, "1", JSTR_NUMBER, "2", JSTR_NUMBER, "3",
        JSTR_STRING, "D",
        JSTR_NUMBER, "42",
        0
    );
    test("{\"A\":{\"B\":{,\"C\":{}}}}", JSTR_INVAL);
    test("{\"A\":{\"B\":{\"C\":{}}},\"D\"}", JSTR_INVAL),
    test("{\"A\":{\"B\":{\"C\":{}}}\"D\":42}", JSTR_INVAL),
    test("  {              }  ", 20,
        JSTR_OBJECT, (size_t)1, 0
    );
    test("  {    \"foo\" : 11 , \"\" : 0   }  ", 32,
        JSTR_OBJECT, (size_t)5,
        JSTR_STRING, "foo",
        JSTR_NUMBER, "11",
        JSTR_STRING, "",
        JSTR_NUMBER, "0", 0
    );

    // array
    test("[", JSTR_INVAL);
    test("]", JSTR_INVAL);
    test("[}", JSTR_INVAL);
    test("[]", 2, JSTR_ARRAY, (size_t)1, 0);
    test("[]junk", JSTR_INVAL);
    test("[,1,2,3,4,5,6,7]", JSTR_INVAL);
    test("[0,1,2,3,,5,6,7]", JSTR_INVAL);
    test("[0,1,2,3,4,5,6,]", JSTR_INVAL);
    test("[0,1,2,3,4,5,6,7]", 17,
        JSTR_ARRAY, (size_t)9,
        JSTR_NUMBER, "0",
        JSTR_NUMBER, "1",
        JSTR_NUMBER, "2",
        JSTR_NUMBER, "3",
        JSTR_NUMBER, "4",
        JSTR_NUMBER, "5",
        JSTR_NUMBER, "6",
        JSTR_NUMBER, "7", 0
    );
    test("[[[[]]]]", 8,
        JSTR_ARRAY, (size_t)4,
        JSTR_ARRAY, (size_t)3,
        JSTR_ARRAY, (size_t)2,
        JSTR_ARRAY, (size_t)1, 0
    );
    test("[[[[42]]]]", 10,
        JSTR_ARRAY, (size_t)5,
        JSTR_ARRAY, (size_t)4,
        JSTR_ARRAY, (size_t)3,
        JSTR_ARRAY, (size_t)2,
        JSTR_NUMBER, "42", 0
    );
    test("[[[[]]],1]", 10,
        JSTR_ARRAY, (size_t)5,
        JSTR_ARRAY, (size_t)3,
        JSTR_ARRAY, (size_t)2,
        JSTR_ARRAY, (size_t)1,
        JSTR_NUMBER, "1", 0
    );
    test(" [           ] ", 15,
        JSTR_ARRAY, (size_t)1, 0
    );
    test(" [ 0 , 1 , 2 ] ", 15,
        JSTR_ARRAY, (size_t)4,
        JSTR_NUMBER, "0",
        JSTR_NUMBER, "1",
        JSTR_NUMBER, "2", 0
    );

    // true, false, null
    test("true", 4, JSTR_TRUE, "true", 0);
    test("false", 5, JSTR_FALSE, "false", 0);
    test("null", 4, JSTR_NULL, "null", 0);
    test("tru", JSTR_INVAL);
    test("tree", JSTR_INVAL);
    test("fals", JSTR_INVAL);
    test("fruit", JSTR_INVAL);
    test("nul", JSTR_INVAL);
    test("nullnullnull", JSTR_INVAL);
    test("taaaaaaaaaaaaaaaaaaaaaaaaaaaaatrue", JSTR_INVAL); // bug fixed
    test("taaaaaaaaaaaaaaaaaaaaaaaaaaaaanull", JSTR_INVAL); // bug fixed

    return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
