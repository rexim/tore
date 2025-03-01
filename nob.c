#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#define NOB_GRU_DELETE_OLD_BINARY
#include "nob.h"

#include "./src_build/flags.c"
typedef enum {
    BF_FORCE,
    BF_ASAN,
    BF_WATCH,
    BF_HELP,
    COUNT_BUILD_FLAGS
} Build_Flag_Index;
static_assert(COUNT_BUILD_FLAGS == 4, "Amount of build flags has changed");
static Flag build_flags[COUNT_BUILD_FLAGS] = {
    [BF_FORCE] = {.name = "-f",     .description = "Force full rebuild"},
    [BF_ASAN]  = {.name = "-asan",  .description = "Enable address sanitizer"},
    [BF_WATCH] = {.name = "-watch", .description = "Run process in watch mode and rebuild on any source code changes. Only works for `run` command."},
    [BF_HELP]  = {.name = "-h",     .description = "Print build flags"},
};

// Folder must end with forward slash /
#define BUILD_FOLDER "./build/"
#define SRC_FOLDER "./src/"
#define SRC_BUILD_FOLDER "./src_build/"
#define RESOURCES_FOLDER "./resources/"
#define GIT_HASH_FILE BUILD_FOLDER"git-hash.txt"
#define TORE_BIN_PATH (build_flags[BF_ASAN].value ? BUILD_FOLDER"tore-asan" : BUILD_FOLDER"tore")
#define SQLITE3_OBJ_PATH (build_flags[BF_ASAN].value ? BUILD_FOLDER"sqlite3-asan.o" : BUILD_FOLDER"sqlite3.o")

#define builder_compiler(cmd) cmd_append(cmd, "clang")
void builder_common_flags(Cmd *cmd)
{
    if (build_flags[BF_ASAN].value) cmd_append(cmd, "-fsanitize=address");
    cmd_append(cmd,
            "-Wall",
            "-Wextra",
            "-Wswitch-enum",
            "-ggdb",
            "-I.",
            "-I"BUILD_FOLDER,
            "-I"SRC_FOLDER"sqlite-amalgamation-3460100/");
}
#define builder_output(cmd, output_path) cmd_append(cmd, "-o", (output_path))
#define builder_inputs(cmd, ...) cmd_append(cmd, __VA_ARGS__)

bool build_sqlite3(Nob_Cmd *cmd)
{
    const char *output_path = SQLITE3_OBJ_PATH;
    const char *input_path = SRC_FOLDER"sqlite-amalgamation-3460100/sqlite3.c";
    int rebuild_is_needed = nob_needs_rebuild1(output_path, input_path);
    if (rebuild_is_needed < 0) return false;
    if (rebuild_is_needed || build_flags[BF_FORCE].value) {
        // NOTE: We are omitting extension loading because it depends on dlopen which prevents us from makeing tore statically linked
        builder_compiler(cmd);
        builder_common_flags(cmd);
        cmd_append(cmd, "-DSQLITE_OMIT_LOAD_EXTENSION", "-O3", "-c");
        builder_output(cmd, output_path);
        builder_inputs(cmd, input_path);
        if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    } else {
        nob_log(NOB_INFO, "%s is up to date", output_path);
    }
    return true;
}

bool set_environment_variable(const char *name, const char *value)
{
    nob_log(INFO, "SETENV: %s = %s", name, value);
    if (setenv(name, value, 1) < 0) {
        nob_log(ERROR, "Could not set variable %s: %s", name, strerror(errno));
        return false;
    }
    return true;
}

// TODO: mark "dirty" hash with "+" symbol similar to how meson does it
char *get_git_hash(Cmd *cmd)
{
    char *result = NULL;
    String_Builder sb = {0};
    Fd fdout = fd_open_for_write(GIT_HASH_FILE);
    if (fdout == INVALID_FD) return_defer(NULL);
    cmd_append(cmd, "git", "rev-parse", "HEAD");
    if (!cmd_run_sync_redirect_and_reset(cmd, (Nob_Cmd_Redirect) {
        .fdout = &fdout
    })) return_defer(NULL);
    if (!read_entire_file(GIT_HASH_FILE, &sb)) return_defer(NULL);
    while (sb.count > 0 && isspace(sb.items[sb.count - 1])) sb.count -= 1;
    sb_append_null(&sb);
    return_defer(sb.items);
defer:
    if (result == NULL) free(sb.items);
    return result;
}

void usage(const char *program_name)
{
    printf("Usage: %s [Build Flags] [Command] [Command Flags]\n", program_name);
    printf("Build flags:\n");
    print_flags(build_flags, COUNT_BUILD_FLAGS);
}

bool compile_template(Cmd *cmd, const char *src_path, const char *dst_path)
{
    Fd index_fd = fd_open_for_write(dst_path);
    if (index_fd == INVALID_FD) return false;;
    cmd_append(cmd, BUILD_FOLDER"tt", src_path);
    if (!cmd_run_sync_redirect_and_reset(cmd, (Nob_Cmd_Redirect) {
        .fdout = &index_fd,
    })) return false;;
    return true;
}

// TODO: maybe automatically recursively collect all the *.png, *.css files in "./resources/"?
struct {
    const char *file_path;
    size_t offset;
    size_t size;
} resources[] = {
    { .file_path = RESOURCES_FOLDER"images/tore.png" },
    { .file_path = RESOURCES_FOLDER"css/reset.css"   },
    { .file_path = RESOURCES_FOLDER"css/main.css"    },
};

#define genf(out, ...) \
    do { \
        fprintf((out), __VA_ARGS__); \
        fprintf((out), " // %s:%d\n", __FILE__, __LINE__); \
    } while(0)

bool generate_resource_bundle(void)
{
    bool result = true;
    Nob_String_Builder bundle = {0};
    Nob_String_Builder content = {0};
    FILE *out = NULL;

    const char *bundle_h_path = BUILD_FOLDER"bundle.h";

    // bundle  = [aaaaaaaaabbbbb]
    //            ^        ^
    // content = []
    // 0, 9

    for (size_t i = 0; i < NOB_ARRAY_LEN(resources); ++i) {
        nob_log(NOB_INFO, "Bundling %s into %s", resources[i].file_path, bundle_h_path);
        content.count = 0;
        if (!nob_read_entire_file(resources[i].file_path, &content)) nob_return_defer(false);
        resources[i].offset = bundle.count;
        resources[i].size = content.count;
        nob_da_append_many(&bundle, content.items, content.count);
        nob_da_append(&bundle, 0);
    }

    out = fopen(bundle_h_path, "wb");
    if (out == NULL) {
        nob_log(NOB_ERROR, "Could not open file %s for writing: %s", bundle_h_path, strerror(errno));
        nob_return_defer(false);
    }

    genf(out, "#ifndef BUNDLE_H_");
    genf(out, "#define BUNDLE_H_");
    genf(out, "typedef struct {");
    genf(out, "    const char *file_path;");
    genf(out, "    size_t offset;");
    genf(out, "    size_t size;");
    genf(out, "} Resource;");
    genf(out, "size_t resources_count = %zu;", NOB_ARRAY_LEN(resources));
    genf(out, "Resource resources[] = {");
    for (size_t i = 0; i < NOB_ARRAY_LEN(resources); ++i) {
        genf(out, "    {.file_path = \"%s\", .offset = %zu, .size = %zu},",
             resources[i].file_path, resources[i].offset, resources[i].size);
    }
    genf(out, "};");

    genf(out, "unsigned char bundle[] = {");
    size_t row_size = 20;
    for (size_t i = 0; i < bundle.count; ) {
        fprintf(out, "     ");
        for (size_t col = 0; col < row_size && i < bundle.count; ++col, ++i) {
            fprintf(out, "0x%02X, ", (unsigned char)bundle.items[i]);
        }
        genf(out, "");
    }
    genf(out, "};");
    genf(out, "#endif // BUNDLE_H_");

defer:
    if (out) fclose(out);
    free(content.items);
    free(bundle.items);
    return result;
}

struct {
    const char *src_path;
    const char *dst_path;
} page_templates[] = {
    { .src_path = SRC_FOLDER"root_page.h.tt",    .dst_path = BUILD_FOLDER"root_page.h"    },
    { .src_path = SRC_FOLDER"index_page.h.tt",   .dst_path = BUILD_FOLDER"index_page.h"   },
    { .src_path = SRC_FOLDER"error_page.h.tt",   .dst_path = BUILD_FOLDER"error_page.h"   },
    { .src_path = SRC_FOLDER"notif_page.h.tt",   .dst_path = BUILD_FOLDER"notif_page.h"   },
    { .src_path = SRC_FOLDER"version_page.h.tt", .dst_path = BUILD_FOLDER"version_page.h" },
};

bool build_tore(Cmd *cmd)
{
    // Templates
    builder_compiler(cmd);
    builder_common_flags(cmd);
    builder_output(cmd, BUILD_FOLDER"tt");
    builder_inputs(cmd, SRC_BUILD_FOLDER"tt.c");
    if (!cmd_run_sync_and_reset(cmd)) return false;
    for (size_t i = 0; i < ARRAY_LEN(page_templates); ++i) {
        if (!compile_template(cmd, page_templates[i].src_path, page_templates[i].dst_path)) {
            return false;
        }
    }
    if (!generate_resource_bundle()) return false;

    char *git_hash = get_git_hash(cmd);
    builder_compiler(cmd);
    builder_common_flags(cmd);
    if (!build_flags[BF_ASAN].value) cmd_append(cmd, "-static");
    if (git_hash) {
        cmd_append(cmd, temp_sprintf("-DGIT_HASH=\"%s\"", git_hash));
        free(git_hash);
    } else {
        cmd_append(cmd, temp_sprintf("-DGIT_HASH=\"Unknown\""));
    }
    builder_output(cmd, TORE_BIN_PATH);
    builder_inputs(cmd, SRC_FOLDER"tore.c", SQLITE3_OBJ_PATH);
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;

    return true;
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF_PLUS(argc, argv, "./src_build/flags.c");

    const char *program_name = shift(argv, argc);
    Nob_Cmd cmd = {0};

    parse_flags(&argc, &argv, build_flags, COUNT_BUILD_FLAGS);

    if (build_flags[BF_HELP].value) {
        usage(program_name);
        return 1;
    }

    if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;
    if (!build_sqlite3(&cmd)) return 1;
    if (!build_tore(&cmd)) return 1;

    if (argc <= 0) return 0;
    const char *command_name = shift(argv, argc);

    if (strcmp(command_name, "run") == 0) {
        // NOTE: this command runs the developed tore with some special
        // environment variables set so it does not damage your "production"
        // database file.
        const char *current_dir = get_current_dir_temp();
        if (current_dir == NULL) return 1;
        if (!set_environment_variable("HOME", temp_sprintf("%s/"BUILD_FOLDER, current_dir))) return 1;
        if (!set_environment_variable("TORE_TRACE_MIGRATION_QUERIES", "1")) return 1;
        cmd_append(&cmd, TORE_BIN_PATH);
        da_append_many(&cmd, argv, argc);
        if (build_flags[BF_WATCH].value) {
#ifdef _WIN32
            nob_log(ERROR, "Watch mode is not supported on Windows yet");
            return 1;
#else // _WIN32
            Proc p = nob_cmd_run_async_and_reset(&cmd);
            File_Paths tore_inputs = {0};
            // TODO: this is an extra place to modify if the dependencies have changed
            da_append(&tore_inputs, SRC_FOLDER"tore.c");
            da_append(&tore_inputs, SQLITE3_OBJ_PATH);
            for (size_t i = 0; i < ARRAY_LEN(page_templates); ++i) {
                da_append(&tore_inputs, page_templates[i].src_path);
            }
            for (size_t i = 0; i < ARRAY_LEN(resources); ++i) {
                da_append(&tore_inputs, resources[i].file_path);
            }
            for (;;) {
                // TODO: check if the process have died at this point.
                //   If the process has died, we should probably just finish the watch mode
                // TODO: check if nob itself requires a rebuild and restart it.
                int yes = nob_needs_rebuild(TORE_BIN_PATH, tore_inputs.items, tore_inputs.count);
                if (yes < 0) return 1;
                if (yes) {
                    if (build_tore(&cmd)) {
                        kill(p, SIGINT); // TODO: we need a cross-platform nob_kill of some sort
                        cmd_append(&cmd, TORE_BIN_PATH);
                        da_append_many(&cmd, argv, argc);
                        p = nob_cmd_run_async_and_reset(&cmd);
                    } else {
                        cmd_append(&cmd, "touch", TORE_BIN_PATH); // TODO: don't depend on external POSIX utils for "touching" the binary
                        if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
                    }
                }
                // TODO: Use file watch mechanisms of the available Operating System
                //   May require implementing file watch mechanism in nob
                usleep(100*1000); // TODO: don't depend on POSIX api for sleeping
            }
#endif // _WIN32
        } else {
            if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
            return 0;
        }
    }

    if (strcmp(command_name, "svg") == 0) {
        cmd_append(&cmd, "convert",
                "-background", "None", "./assets/images/tore.svg",
                "-resize", "32x32", "./assets/images/tore.png");
        if (!cmd_run_sync_and_reset(&cmd)) return 1;
        return 0;
    }

    nob_log(ERROR, "Unknown command %s", command_name);
    return 1;
}
// TODO: automatic record/replay testing
// TODO: merge src_build/tt.c into nob.c
//   It does not need to be a separate tool. It's all written in the same language.
