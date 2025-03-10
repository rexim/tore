#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

// TODO: Document the whole tt paradigm (I could probably write an entire blog post about it)
// - C code mixed with text,
// - Inclusion of compiled templates into the body of C functions with macro parameters like OUT, ESCAPE_OUT, etc.,
// - Implied semicolons in template parameter macros,
// - Nested macros with %#include BODY% pattern,
// - HTML escaping,
// - ...

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

void compile_c_code(String_View s, int newline) {
    printf("%.*s%s", (int) s.count, s.data, (newline ? "\n" : ""));
}

void compile_byte_array(String_View s, int newline) {
    printf("OUT(\"");
    for (uint64_t i = 0; i < s.count; ++i) {
        printf("\\x%02x", s.data[i]);
    }
    printf("\", %lu);%s", s.count, (newline ? "\n" : ""));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: ./tt [--format] <template.h.tt>\n");
        return 1;
    }
    int want_format = 0; // This can be changed if the rendering function uses
                         // formatting and would need double % to work
    if (argc == 2) {
        if (strcmp("--format", argv[1]) == 0) {
            fprintf(stderr, "Missing file argument\n");
            fprintf(stderr, "Usage: ./tt [--format] <template.h.tt>\n");
            return 1;
        }
    } else {
        if (strcmp("--format", argv[1]) == 0) {
            want_format = 1;
        }
    }

    const char *filepath = (want_format ? argv[2] : argv[1]);
    String_Builder sb = {0};
    if (!nob_read_entire_file(filepath, &sb)) return 1;
    String_View temp = sb_to_sv(sb);
    int c_code_mode = 0;
    int escaped_match = 0;
    int do_newline = 0;
    // TODO: Generate line control preprocessor directives
    // - GCC: https://gcc.gnu.org/onlinedocs/cpp/Line-Control.html
    // - MSVC: https://learn.microsoft.com/en-us/cpp/preprocessor/hash-line-directive-c-cpp
    while (temp.count) {
        String_View token = sv_chop_by_delim(&temp, '%');

        escaped_match = 0;
        if (token.data[token.count-1] == '\\') { // Handle escaped percent sign
            escaped_match = 1;
        }
        do_newline = !escaped_match;
        if (!escaped_match) {
           if (c_code_mode) {
                compile_c_code(token, do_newline);
            } else {
                compile_byte_array(token, do_newline);
            }
            c_code_mode = !c_code_mode;
        } else { // We make sure to properly print the token
            String_Builder sb = {0};
            sb_append_buf(&sb, token.data, token.count-1);
            if (c_code_mode || !want_format) {
                sb_append_cstr(&sb, "%");
            } else {
                sb_append_cstr(&sb, "%%");
            }
            String_View sv = sb_to_sv(sb);
            if (c_code_mode) {
                compile_c_code(sv, do_newline);
            } else {
                compile_byte_array(sv, do_newline);
            }
            sb_free(sb);
        }
    }

    return 0;
}
