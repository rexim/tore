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

void compile_c_code(String_View s) {
    printf("%.*s\n", (int) s.count, s.data);
}

void compile_byte_array(String_View s) {
    printf("OUT(\"");
    for (uint64_t i = 0; i < s.count; ++i) {
        printf("\\x%02x", s.data[i]);
    }
    printf("\", %lu);\n", s.count);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: ./tt <template.h.tt>\n");
        return 1;
    }
    const char *filepath = argv[1];
    String_Builder sb = {0};
    if (!nob_read_entire_file(filepath, &sb)) return 1;
    String_View temp = sb_to_sv(sb);
    int c_code_mode = 0;
    // TODO: Generate line control preprocessor directives
    // - GCC: https://gcc.gnu.org/onlinedocs/cpp/Line-Control.html
    // - MSVC: https://learn.microsoft.com/en-us/cpp/preprocessor/hash-line-directive-c-cpp
    while (temp.count) {
        String_View token = sv_chop_by_delim(&temp, '%');
        if (c_code_mode) {
            compile_c_code(token);
        } else {
            compile_byte_array(token);
        }
        c_code_mode = !c_code_mode;
    }

    return 0;
}
