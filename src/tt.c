#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

void compile_c_code(String_View s, int line, char *filename) {
    printf("#line %d \"%s\"\n", line, filename);
    printf("%.*s\n", (int) s.count, s.data);
}

void compile_byte_array(String_View s) {
    printf("OUT(\"");
    for (uint64_t i = 0; i < s.count; ++i) {
        printf("\\x%02x", s.data[i]);
    }
    printf("\", %lu);\n", s.count);
}

int lines_in_token(String_View s) {
    int lines = 0;
    for (uint64_t i = 0; i < s.count; ++i) {
        if (s.data[i] == '\n')
            lines += 1;
    }
    return lines;
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
    int line = 1;
    // TODO: Generate line control preprocessor directives
    // - GCC: https://gcc.gnu.org/onlinedocs/cpp/Line-Control.html
    // - MSVC: https://learn.microsoft.com/en-us/cpp/preprocessor/hash-line-directive-c-cpp
    while (temp.count) {
        String_View token = sv_chop_by_delim(&temp, '%');
        if (c_code_mode) {
            compile_c_code(token, line, filepath);
        } else {
            compile_byte_array(token);
        }
        line += lines_in_token(token);
        c_code_mode = !c_code_mode;
    }

    return 0;
}
