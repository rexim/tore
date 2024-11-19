#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#define NOB_GRU_DELETE_OLD_BINARY
#include "nob.h"

#include "./src_build/flags.c"
typedef enum {
    BF_FORCE,
    BF_ASAN,
    BF_HELP,
    COUNT_BUILD_FLAGS
} Build_Flag_Index;
static_assert(COUNT_BUILD_FLAGS == 3, "Amount of build flags has changed");
static Flag build_flags[COUNT_BUILD_FLAGS] = {
    [BF_FORCE] = {.name = "-f",    .description = "Force full rebuild"},
    [BF_ASAN]  = {.name = "-asan", .description = "Enable address sanitizer"},
    [BF_HELP]  = {.name = "-h",    .description = "Print build flags"},
};

// Folder must end with forward slash /
#define BUILD_FOLDER "./build/"
#define SRC_FOLDER "./src/"
#define SRC_BUILD_FOLDER "./src_build/"
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

char *get_git_hash(Cmd *cmd)
{
    char *result = NULL;
    String_Builder sb = {0};
    Fd fdout = fd_open_for_write(GIT_HASH_FILE);
    if (fdout == INVALID_FD) return_defer(NULL);
    cmd_append(cmd, "git", "show", "--no-patch", "--pretty=%H (committed on: %aD)", "HEAD");
    if (!cmd_run_sync_redirect_and_reset(cmd, (Nob_Cmd_Redirect) {
        .fdout = &fdout
    })) return_defer(NULL);
    if (!read_entire_file(GIT_HASH_FILE, &sb)) return_defer(NULL);
    while (sb.count > 0) { if (!isspace(sb.items[sb.count-1])) break; sb.count -= 1; }
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

    // Templates
    builder_compiler(&cmd);
    builder_common_flags(&cmd);
    builder_output(&cmd, BUILD_FOLDER"tt");
    builder_inputs(&cmd, SRC_BUILD_FOLDER"tt.c");
    if (!cmd_run_sync_and_reset(&cmd)) return 1;

    Fd index_fd = fd_open_for_write(BUILD_FOLDER"index_page.h");
    if (index_fd == INVALID_FD) return 1;
    cmd_append(&cmd, BUILD_FOLDER"tt", SRC_FOLDER"index_page.h.tt");
    if (!cmd_run_sync_redirect_and_reset(&cmd, (Nob_Cmd_Redirect) {
        .fdout = &index_fd,
    })) return 1;

    char *git_hash = get_git_hash(&cmd);
    builder_compiler(&cmd);
    builder_common_flags(&cmd);
    if (!build_flags[BF_ASAN].value) cmd_append(&cmd, "-static");
    if (git_hash) {
        cmd_append(&cmd, temp_sprintf("-DGIT_HASH=\"%s\"", git_hash));
        free(git_hash);
    } else {
        cmd_append(&cmd, temp_sprintf("-DGIT_HASH=\"Unknown\""));
    }
    builder_output(&cmd, TORE_BIN_PATH);
    builder_inputs(&cmd, SRC_FOLDER"tore.c", SQLITE3_OBJ_PATH);
    if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;

    if (argc <= 0) return 0;
    const char *command_name = shift(argv, argc);

    if (strcmp(command_name, "run") == 0 || strcmp(command_name, "chroot") == 0) {
        // NOTE: this command runs the developed tore with some special
        // environment variables set so it does not damage your "production"
        // database file.
        // NOTE: the name of the command is `chroot` because of historical
        // reasons. It was originally using chroot, but it turned out that just
        // setting a bunch of environment variables is enough. Maybe it should
        // be renamed to something else in the future.
        const char *current_dir = get_current_dir_temp();
        if (current_dir == NULL) return 1;
        if (!set_environment_variable("HOME", temp_sprintf("%s/"BUILD_FOLDER, current_dir))) return 1;
        if (!set_environment_variable("TORE_TRACE_MIGRATION_QUERIES", "1")) return 1;
        cmd_append(&cmd, TORE_BIN_PATH);
        da_append_many(&cmd, argv, argc);
        if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
        if (strcmp(command_name, "chroot") == 0) {
            nob_log(WARNING, "`chroot` command name is deprecated, just call it as `run`");
        }
        return 0;
    }

    nob_log(ERROR, "Unknown command %s", command_name);
    return 1;
}
