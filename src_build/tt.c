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

typedef struct {
    int linenum;
    const char *filename;
} Line_Directive;

void update_linenum(String_View s, Line_Directive *ld) {
    for (uint64_t i = 0; i < s.count; ++i) {
        if (s.data[i] == '\n') {
            ++ld->linenum;
        }
    }
}

void compile_c_code(String_View s, Line_Directive *ld) {
    printf("#line %d \"%s\"\n", ld->linenum, ld->filename);
    update_linenum(s, ld);
    printf("%.*s\n", (int) s.count, s.data);
}

void compile_byte_array(String_View s, Line_Directive *ld) {
    printf("#line %d \"%s\"\n", ld->linenum, ld->filename);
    update_linenum(s, ld);
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
    Line_Directive ld = (Line_Directive) {
        .linenum = 1,
        .filename = filepath
    };

    int c_code_mode = 0;
    while (temp.count) {
        String_View token = sv_chop_by_delim(&temp, '%');
        if (c_code_mode) {
            compile_c_code(token, &ld);
        } else {
            compile_byte_array(token, &ld);
        }
        c_code_mode = !c_code_mode;
    }

    return 0;
}
