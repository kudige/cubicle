#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "../common/auth_crypto.h"
#include "../common/json.h"
#include "../cubeui/cubeui.h"

#include "cubicle/auth.h"
#include "cubicle/attachment.h"
#include "cubicle/client.h"
#include "cubicle/config.h"
#include "cubicle/rpc.h"
#include "cubicle/util.h"
#include "cubicle/workspace.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX CUBICLE_PATH_MAX
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#define CUBE_CHANNEL_STDIN 1U
#define CUBE_CHANNEL_STDOUT 2U
#define CUBE_CHANNEL_STDERR 4U
#define CUBE_CHANNEL_TTY 8U
#define CUBE_ATTACH_POLL_MS 50
#define CUBE_CONNECT_REPLAY_BYTES 16384ULL

typedef struct cube_options {
    const char *manager_socket;
    const char *workspace;
    int json;
} cube_options_t;

typedef struct cube_run_options {
    const char *name;
    const char *mode;
    const char *directory;
    int background;
    int generated_name;
} cube_run_options_t;

typedef struct cube_rpc_response {
    cubicle_error_code_t code;
    char *result_json;
    char error_message[CUBICLE_ERROR_MESSAGE_MAX];
} cube_rpc_response_t;

typedef struct cube_log_options {
    int follow;
    int stdout_only;
    int stderr_only;
    uint64_t start;
    uint64_t end;
    int has_end;
} cube_log_options_t;

static struct termios cube_saved_terminal;
static int cube_terminal_restore_active = 0;

static void cube_restore_terminal(void)
{
    if (cube_terminal_restore_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &cube_saved_terminal);
        cube_terminal_restore_active = 0;
    }
}

// LCOV_EXCL_START
static void cube_attach_signal_handler(int signal_number)
{
    cube_restore_terminal();
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}
// LCOV_EXCL_STOP

static void cleanup_rpc_response(cube_rpc_response_t *response);
static int json_string_field(yyjson_val *object,
                             const char *field,
                             char *buffer,
                             size_t buffer_size);

static void cube_sleep_poll_interval(void)
{
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = CUBE_ATTACH_POLL_MS * 1000000L,
    };
    nanosleep(&delay, NULL);
}

static int process_mode_uses_terminal(const char *mode)
{
    return strcmp(mode, "tty") == 0 || strcmp(mode, "term") == 0;
}

static const char *cube_mode_name(cubicle_process_mode_t mode)
{
    return cubicle_process_mode_name(mode);
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  cube [--config PATH] [--manager-socket PATH] [--workspace NAME] [--json] COMMAND [ARG...]\n"
            "  cube workspace [NAME]\n"
            "  cube workspace list|create|select|stop|delete ...\n"
            "  cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] [--dir DIR] COMMAND [ARG...]\n"
            "  cube ps [-a|--all-workspaces]\n"
            "  cube inspect NAME\n"
            "  cube logs [--follow] [--stdout|--stderr] [--start N] [--end N] NAME\n"
            "  cube events [--follow [--iterations N]]\n"
            "  cube connect [--ro] NAME\n"
            "  cube signal NAME SIGNAL\n"
            "  cube stop NAME\n"
            "  cube kill [--all] [--cleanup] [NAME]\n"
            "  cube save NAME\n"
            "  cube unsave NAME\n"
            "  cube remove NAME\n"
            "  cube cleanup\n"
            "  cube access list|add|set-role|remove|revoke ...\n"
            "  cube config show|effective|paths|validate\n"
            "  cube defaults show|set|reset ...\n"
            "\n"
            "Run and reconnect to persistent processes inside Cubicle workspaces.\n");
}

static int print_command_usage(const char *command, FILE *stream)
{
    if (strcmp(command, "workspace") == 0) {
        fprintf(stream,
                "Usage:\n"
                "  cube workspace [NAME]\n"
                "  cube workspace list\n"
                "  cube workspace create [--dir DIR] NAME\n"
                "  cube workspace select NAME\n"
                "  cube workspace stop NAME\n"
                "  cube workspace delete NAME\n");
        return 0;
    }
    if (strcmp(command, "run") == 0) {
        fprintf(stream,
                "Usage:\n"
                "  cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] [--dir DIR] COMMAND [ARG...]\n");
        return 0;
    }
    if (strcmp(command, "ps") == 0) {
        fprintf(stream, "Usage:\n  cube ps [-a|--all-workspaces]\n");
        return 0;
    }
    if (strcmp(command, "inspect") == 0) {
        fprintf(stream, "Usage:\n  cube inspect NAME\n");
        return 0;
    }
    if (strcmp(command, "logs") == 0) {
        fprintf(stream,
                "Usage:\n"
                "  cube logs [--follow] [--stdout|--stderr] [--start N] [--end N] NAME\n");
        return 0;
    }
    if (strcmp(command, "events") == 0) {
        fprintf(stream,
                "Usage:\n"
                "  cube events [--follow [--iterations N]]\n");
        return 0;
    }
    if (strcmp(command, "connect") == 0) {
        fprintf(stream, "Usage:\n  cube connect [--ro] NAME\n");
        return 0;
    }
    if (strcmp(command, "signal") == 0) {
        fprintf(stream, "Usage:\n  cube signal NAME SIGNAL\n");
        return 0;
    }
    if (strcmp(command, "stop") == 0) {
        fprintf(stream, "Usage:\n  cube stop NAME\n");
        return 0;
    }
    if (strcmp(command, "kill") == 0) {
        fprintf(stream,
                "Usage:\n"
                "  cube kill [--cleanup] NAME\n"
                "  cube kill --all [--cleanup]\n"
                "\n"
                "Options:\n"
                "  --all       Kill all running processes in the selected workspace.\n"
                "  --cleanup   Remove killed process records after they exit.\n");
        return 0;
    }
    if (strcmp(command, "save") == 0) {
        fprintf(stream, "Usage:\n  cube save NAME\n");
        return 0;
    }
    if (strcmp(command, "unsave") == 0) {
        fprintf(stream, "Usage:\n  cube unsave NAME\n");
        return 0;
    }
    if (strcmp(command, "remove") == 0) {
        fprintf(stream, "Usage:\n  cube remove NAME\n");
        return 0;
    }
    if (strcmp(command, "cleanup") == 0) {
        fprintf(stream, "Usage:\n  cube cleanup\n");
        return 0;
    }
    if (strcmp(command, "access") == 0) {
        fprintf(stream,
                "Usage:\n"
                "  cube access list\n"
                "  cube access add PUBLIC_KEY_OR_FILE [--role observer|operator|owner] [--label LABEL]\n"
                "  cube access set-role KEY_ID observer|operator|owner\n"
                "  cube access remove KEY_ID\n"
                "  cube access revoke KEY_ID\n");
        return 0;
    }
    if (strcmp(command, "config") == 0) {
        fprintf(stream,
                "Usage:\n  cube config show|effective|paths|validate\n");
        return 0;
    }
    if (strcmp(command, "defaults") == 0) {
        fprintf(stream,
                "Usage:\n"
                "  cube defaults [show]\n"
                "  cube defaults set launch foreground|background\n"
                "  cube defaults set mode stream|tty|term\n"
                "  cube defaults set kill-cleanup true|false\n"
                "  cube defaults reset [launch|mode|kill-cleanup]\n");
        return 0;
    }
    return -1;
}

static int parse_global_options(int argc,
                                char **argv,
                                cube_options_t *options,
                                int *command_index)
{
    memset(options, 0, sizeof(*options));
    *command_index = 1;

    while (*command_index < argc) {
        const char *argument = argv[*command_index];
        if (strcmp(argument, "--") == 0) {
            ++(*command_index);
            return 0;
        }
        if (strcmp(argument, "--help") == 0 ||
            strcmp(argument, "-h") == 0) {
            print_usage(stdout);
            return 1;
        }
        if (strcmp(argument, "--json") == 0) {
            options->json = 1;
            ++(*command_index);
            continue;
        }
        if (strcmp(argument, "--config") == 0) {
            if (*command_index + 1 >= argc) {
                fprintf(stderr, "cube: --config requires a path\n");
                return -1;
            }
            if (setenv("CUBICLE_CONFIG", argv[*command_index + 1], 1) < 0) {
                fprintf(stderr, "cube: failed to set config override: %s\n",
                        strerror(errno));
                return -1;
            }
            *command_index += 2;
            continue;
        }
        if (strcmp(argument, "--manager-socket") == 0) {
            if (*command_index + 1 >= argc) {
                fprintf(stderr,
                        "cube: --manager-socket requires a path\n");
                return -1;
            }
            options->manager_socket = argv[*command_index + 1];
            *command_index += 2;
            continue;
        }
        if (strcmp(argument, "--workspace") == 0) {
            if (*command_index + 1 >= argc) {
                fprintf(stderr, "cube: --workspace requires a name\n");
                return -1;
            }
            options->workspace = argv[*command_index + 1];
            *command_index += 2;
            continue;
        }
        if (argument[0] == '-' && argument[1] == '-') {
            fprintf(stderr, "cube: unknown option '%s'\n", argument);
            return -1;
        }
        return 0;
    }

    return 0;
}

static int command_requires_manager(const char *command)
{
    return strcmp(command, "workspace") == 0 ||
           strcmp(command, "run") == 0 ||
           strcmp(command, "ps") == 0 ||
           strcmp(command, "inspect") == 0 ||
           strcmp(command, "connect") == 0 ||
           strcmp(command, "signal") == 0 ||
           strcmp(command, "stop") == 0 ||
           strcmp(command, "kill") == 0 ||
           strcmp(command, "save") == 0 ||
           strcmp(command, "unsave") == 0 ||
           strcmp(command, "remove") == 0 ||
           strcmp(command, "cleanup") == 0 ||
           strcmp(command, "access") == 0 ||
           strcmp(command, "logs") == 0 ||
           strcmp(command, "events") == 0;
}

static int command_config(const cubicle_config_t *config,
                          const char *config_error,
                          int argc,
                          char **argv,
                          int command_index)
{
    if (command_index + 1 >= argc) {
        fprintf(stderr,
                "cube: config requires show, effective, paths, or validate\n");
        return 2;
    }

    const char *subcommand = argv[command_index + 1];
    if (strcmp(subcommand, "validate") == 0) {
        if (config_error != NULL && config_error[0] != '\0') {
            fprintf(stderr, "cube: configuration error: %s\n", config_error);
            return 1;
        }
        printf("configuration valid\n");
        return 0;
    }

    if (config_error != NULL && config_error[0] != '\0') {
        fprintf(stderr, "cube: configuration error: %s\n", config_error);
        return 1;
    }

    if (strcmp(subcommand, "paths") == 0) {
        printf("source=%s\n", config->source);
        printf("manager.state_dir=%s\n", config->manager_state_dir);
        printf("manager.runtime_dir=%s\n", config->manager_runtime_dir);
        printf("manager.log_dir=%s\n", config->manager_log_dir);
        printf("manager.socket_mode=%04o\n", config->manager_socket_mode);
        printf("manager.socket_group=%s\n", config->manager_socket_group);
        printf("manager.controller_binary=%s\n", config->controller_binary);
        printf("controller.debug=%s%s%s%s\n",
               config->controller_debug_input ? "input" : "",
               config->controller_debug_input &&
                       (config->controller_debug_library ||
                        config->controller_debug_terminal)
                   ? ","
                   : "",
               config->controller_debug_library ? "library" : "",
               config->controller_debug_terminal
                   ? (config->controller_debug_library ? ",terminal" : "terminal")
                   : (config->controller_debug_input ||
                              config->controller_debug_library
                          ? ""
                          : "none"));
        printf("cube.debug=%s\n",
               config->cube_debug_library ? "library" : "none");
        printf("desk.debug=%s%s%s\n",
               config->desk_debug_library ? "library" : "",
               config->desk_debug_library && config->desk_debug_terminal
                   ? ","
                   : "",
               config->desk_debug_terminal
                   ? "terminal"
                   : (config->desk_debug_library ? "" : "none"));
        return 0;
    }

    if (strcmp(subcommand, "show") == 0) {
        printf("source=%s\n", config->source);
        printf("installation.bindir=%s\n", config->bindir);
        printf("installation.libexecdir=%s\n", config->libexecdir);
        printf("manager.state_dir=%s\n", config->manager_state_dir);
        printf("manager.runtime_dir=%s\n", config->manager_runtime_dir);
        printf("manager.listen=%s\n", config->manager_listen_uri);
        printf("manager.controller_binary=%s\n", config->controller_binary);
        printf("manager.log_dir=%s\n", config->manager_log_dir);
        printf("manager.socket_mode=%04o\n", config->manager_socket_mode);
        printf("manager.socket_group=%s\n", config->manager_socket_group);
        printf("controller.debug=%s%s%s%s\n",
               config->controller_debug_input ? "input" : "",
               config->controller_debug_input &&
                       (config->controller_debug_library ||
                        config->controller_debug_terminal)
                   ? ","
                   : "",
               config->controller_debug_library ? "library" : "",
               config->controller_debug_terminal
                   ? (config->controller_debug_library ? ",terminal" : "terminal")
                   : (config->controller_debug_input ||
                              config->controller_debug_library
                          ? ""
                          : "none"));
        printf("cube.debug=%s\n",
               config->cube_debug_library ? "library" : "none");
        printf("desk.debug=%s%s%s\n",
               config->desk_debug_library ? "library" : "",
               config->desk_debug_library && config->desk_debug_terminal
                   ? ","
                   : "",
               config->desk_debug_terminal
                   ? "terminal"
                   : (config->desk_debug_library ? "" : "none"));
        printf("client.manager=%s\n", config->client_manager_uri);
        printf("defaults.launch=%s\n",
               cubicle_launch_default_name(config->default_launch));
        printf("defaults.mode=%s\n", cube_mode_name(config->default_mode));
        printf("defaults.kill_cleanup=%s\n",
               config->default_kill_cleanup ? "true" : "false");
        return 0;
    }

    if (strcmp(subcommand, "effective") == 0) {
        printf("Configuration sources:\n");
        const char *printed[CUBICLE_CONFIG_KEY_COUNT];
        size_t printed_count = 0;
        for (int i = 0; i < CUBICLE_CONFIG_KEY_COUNT; ++i) {
            const cubicle_config_origin_t *origin =
                cubicle_config_origin(config, (cubicle_config_key_t)i);
            const char *source =
                origin != NULL ? origin->source_path : "unknown";
            int seen = 0;
            for (size_t j = 0; j < printed_count; ++j) {
                if (strcmp(printed[j], source) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (!seen && printed_count < CUBICLE_CONFIG_KEY_COUNT) {
                printed[printed_count++] = source;
                printf("  %s\n", source);
            }
        }
        printf("\n");
        printf("Effective values:\n");
        for (int i = 0; i < CUBICLE_CONFIG_KEY_COUNT; ++i) {
            cubicle_config_key_t key = (cubicle_config_key_t)i;
            const cubicle_config_origin_t *origin =
                cubicle_config_origin(config, key);
            const char *value = "";
            char formatted[128];
            switch (key) {
            case CUBICLE_CONFIG_INSTALLATION_BINDIR:
                value = config->bindir;
                break;
            case CUBICLE_CONFIG_INSTALLATION_LIBEXECDIR:
                value = config->libexecdir;
                break;
            case CUBICLE_CONFIG_MANAGER_STATE_DIR:
                value = config->manager_state_dir;
                break;
            case CUBICLE_CONFIG_MANAGER_RUNTIME_DIR:
                value = config->manager_runtime_dir;
                break;
            case CUBICLE_CONFIG_MANAGER_LOG_DIR:
                value = config->manager_log_dir;
                break;
            case CUBICLE_CONFIG_MANAGER_LISTEN:
                value = config->manager_listen_uri;
                break;
            case CUBICLE_CONFIG_MANAGER_SOCKET_MODE:
                snprintf(formatted, sizeof(formatted), "%04o",
                         config->manager_socket_mode);
                value = formatted;
                break;
            case CUBICLE_CONFIG_MANAGER_SOCKET_GROUP:
                value = config->manager_socket_group;
                break;
            case CUBICLE_CONFIG_MANAGER_CONTROLLER_BINARY:
                value = config->controller_binary;
                break;
            case CUBICLE_CONFIG_CONTROLLER_DEBUG:
                snprintf(formatted, sizeof(formatted), "%s%s%s%s",
                         config->controller_debug_input ? "input" : "",
                         config->controller_debug_input &&
                                 (config->controller_debug_library ||
                                  config->controller_debug_terminal)
                             ? ","
                             : "",
                         config->controller_debug_library ? "library" : "",
                         config->controller_debug_terminal
                             ? (config->controller_debug_library ? ",terminal"
                                                                 : "terminal")
                             : (config->controller_debug_input ||
                                        config->controller_debug_library
                                    ? ""
                                    : "none"));
                value = formatted;
                break;
            case CUBICLE_CONFIG_CUBE_DEBUG:
                value = config->cube_debug_library ? "library" : "none";
                break;
            case CUBICLE_CONFIG_DESK_DEBUG:
                snprintf(formatted, sizeof(formatted), "%s%s%s",
                         config->desk_debug_library ? "library" : "",
                         config->desk_debug_library &&
                                 config->desk_debug_terminal
                             ? ","
                             : "",
                         config->desk_debug_terminal
                             ? "terminal"
                             : (config->desk_debug_library ? "" : "none"));
                value = formatted;
                break;
            case CUBICLE_CONFIG_CLIENT_MANAGER:
                value = config->client_manager_uri;
                break;
            case CUBICLE_CONFIG_CLIENT_SERVER_IDENTITY:
                value = config->client_server_identity;
                break;
            case CUBICLE_CONFIG_DEFAULTS_LAUNCH:
                value = cubicle_launch_default_name(config->default_launch);
                break;
            case CUBICLE_CONFIG_DEFAULTS_MODE:
                value = cube_mode_name(config->default_mode);
                break;
            case CUBICLE_CONFIG_DEFAULTS_KILL_CLEANUP:
                value = config->default_kill_cleanup ? "true" : "false";
                break;
            case CUBICLE_CONFIG_KEY_COUNT:
            default:
                continue;
            }
            printf("  %-35s %s\n", cubicle_config_key_name(key), value);
            printf("      source: %s",
                   origin != NULL ? origin->source_path : "unknown");
            if (origin != NULL) {
                printf(" (%s)",
                       cubicle_config_source_kind_name(origin->kind));
            }
            printf("\n");
        }
        return 0;
    }

    fprintf(stderr, "cube: unknown config command '%s'\n", subcommand);
    return 2;
}

typedef struct cube_defaults_update {
    int set_launch;
    int reset_launch;
    const char *launch;
    int set_mode;
    int reset_mode;
    const char *mode;
    int set_kill_cleanup;
    int reset_kill_cleanup;
    const char *kill_cleanup;
} cube_defaults_update_t;

static const char *cube_user_home_directory(void)
{
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        return home;
    }
    struct passwd *entry = getpwuid(geteuid());
    return entry != NULL ? entry->pw_dir : NULL;
}

static int cube_user_config_path(char path[CUBICLE_PATH_MAX])
{
    const char *config_home = getenv("XDG_CONFIG_HOME");
    int length;
    if (config_home != NULL && config_home[0] != '\0') {
        length = snprintf(path, CUBICLE_PATH_MAX, "%s/cubicle/config.cfg",
                          config_home);
    } else {
        const char *home = cube_user_home_directory();
        if (home == NULL || home[0] == '\0') {
            errno = ENOENT;
            return -1;
        }
        length = snprintf(path, CUBICLE_PATH_MAX,
                          "%s/.config/cubicle/config.cfg", home);
    }
    if (length < 0 || length >= CUBICLE_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int cube_parent_directory(const char *path,
                                 char parent[CUBICLE_PATH_MAX])
{
    int length = snprintf(parent, CUBICLE_PATH_MAX, "%s", path);
    if (length < 0 || length >= CUBICLE_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (slash == NULL || slash == parent) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    return 0;
}

static char *cube_read_optional_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            char *empty = calloc(1, 1);
            if (empty == NULL) {
                errno = ENOMEM;
            }
            return empty;
        }
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) < 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *buffer = malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        errno = ENOMEM;
        return NULL;
    }
    size_t read_count = fread(buffer, 1, (size_t)length, file);
    int read_error = ferror(file);
    fclose(file);
    if (read_error || read_count != (size_t)length) {
        free(buffer);
        errno = EIO;
        return NULL;
    }
    buffer[length] = '\0';
    return buffer;
}

static char *cube_trim_left(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' ||
           *text == '\n') {
        ++text;
    }
    return text;
}

static void cube_trim_right(char *text)
{
    size_t length = strlen(text);
    while (length > 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t' ||
            text[length - 1] == '\r' || text[length - 1] == '\n')) {
        text[--length] = '\0';
    }
}

static int cube_config_line_section(const char *line,
                                    char section[64])
{
    char copy[256];
    int length = snprintf(copy, sizeof(copy), "%s", line);
    if (length < 0 || (size_t)length >= sizeof(copy)) {
        return 0;
    }
    char *trimmed = cube_trim_left(copy);
    cube_trim_right(trimmed);
    size_t trimmed_length = strlen(trimmed);
    if (trimmed_length < 3 || trimmed[0] != '[' ||
        trimmed[trimmed_length - 1] != ']') {
        return 0;
    }
    trimmed[trimmed_length - 1] = '\0';
    length = snprintf(section, 64, "%s", trimmed + 1);
    return length >= 0 && length < 64;
}

static int cube_config_line_key_matches(const char *line, const char *key)
{
    char copy[256];
    int length = snprintf(copy, sizeof(copy), "%s", line);
    if (length < 0 || (size_t)length >= sizeof(copy)) {
        return 0;
    }
    char *trimmed = cube_trim_left(copy);
    if (*trimmed == '#' || *trimmed == ';' || *trimmed == '\0' ||
        *trimmed == '[') {
        return 0;
    }
    char *equals = strchr(trimmed, '=');
    if (equals == NULL) {
        return 0;
    }
    *equals = '\0';
    cube_trim_right(trimmed);
    return strcmp(trimmed, key) == 0;
}

static int cube_defaults_update_wants_key(const cube_defaults_update_t *update,
                                          const char *key)
{
    if (strcmp(key, "launch") == 0) {
        return update->set_launch || update->reset_launch;
    }
    if (strcmp(key, "mode") == 0) {
        return update->set_mode || update->reset_mode;
    }
    if (strcmp(key, "kill_cleanup") == 0) {
        return update->set_kill_cleanup || update->reset_kill_cleanup;
    }
    return 0;
}

static int cube_append_updated_defaults(cubicle_json_builder_t *output,
                                        const cube_defaults_update_t *update)
{
    if (update->set_launch &&
        cubicle_json_builder_appendf(output, "launch=%s\n",
                                     update->launch) < 0) {
        return -1;
    }
    if (update->set_mode &&
        cubicle_json_builder_appendf(output, "mode=%s\n", update->mode) < 0) {
        return -1;
    }
    if (update->set_kill_cleanup &&
        cubicle_json_builder_appendf(output, "kill_cleanup=%s\n",
                                     update->kill_cleanup) < 0) {
        return -1;
    }
    return 0;
}

static int cube_append_updated_defaults_block(cubicle_json_builder_t *output,
                                              const cube_defaults_update_t *update)
{
    if (output->length > 0 && output->data[output->length - 1] != '\n' &&
        cubicle_json_builder_append(output, "\n") < 0) {
        return -1;
    }
    return cube_append_updated_defaults(output, update);
}

static int cube_render_updated_config(const char *existing,
                                      const cube_defaults_update_t *update,
                                      char **rendered_out)
{
    cubicle_json_builder_t output = {0};
    int in_defaults = 0;
    int saw_defaults = 0;
    int inserted = 0;
    const char *cursor = existing;

    while (*cursor != '\0') {
        const char *line_start = cursor;
        const char *line_end = strchr(cursor, '\n');
        size_t line_length;
        if (line_end == NULL) {
            line_length = strlen(cursor);
            cursor += line_length;
        } else {
            line_length = (size_t)(line_end - line_start) + 1;
            cursor = line_end + 1;
        }

        char line[1024];
        size_t copy_length =
            line_length < sizeof(line) - 1 ? line_length : sizeof(line) - 1;
        memcpy(line, line_start, copy_length);
        line[copy_length] = '\0';

        char section[64];
        if (cube_config_line_section(line, section)) {
            if (in_defaults && !inserted) {
                if (cube_append_updated_defaults_block(&output, update) < 0) {
                    cubicle_json_builder_cleanup(&output);
                    return -1;
                }
                inserted = 1;
            }
            in_defaults = strcmp(section, "defaults") == 0;
            saw_defaults = saw_defaults || in_defaults;
        }

        if (in_defaults &&
            (cube_config_line_key_matches(line, "launch") ||
             cube_config_line_key_matches(line, "mode") ||
             cube_config_line_key_matches(line, "kill_cleanup"))) {
            if (cube_defaults_update_wants_key(update, "launch") &&
                cube_config_line_key_matches(line, "launch")) {
                continue;
            }
            if (cube_defaults_update_wants_key(update, "mode") &&
                cube_config_line_key_matches(line, "mode")) {
                continue;
            }
            if (cube_defaults_update_wants_key(update, "kill_cleanup") &&
                cube_config_line_key_matches(line, "kill_cleanup")) {
                continue;
            }
        }

        if (cubicle_json_builder_reserve(&output, line_length) < 0) {
            cubicle_json_builder_cleanup(&output);
            return -1;
        }
        memcpy(output.data + output.length, line_start, line_length);
        output.length += line_length;
        output.data[output.length] = '\0';
    }

    if (in_defaults && !inserted) {
        if (cube_append_updated_defaults_block(&output, update) < 0) {
            cubicle_json_builder_cleanup(&output);
            return -1;
        }
        inserted = 1;
    }
    if (!saw_defaults &&
        (update->set_launch || update->set_mode || update->set_kill_cleanup)) {
        if (output.length > 0 && output.data[output.length - 1] != '\n' &&
            cubicle_json_builder_append(&output, "\n") < 0) {
            cubicle_json_builder_cleanup(&output);
            return -1;
        }
        if (output.length > 0 &&
            cubicle_json_builder_append(&output, "\n") < 0) {
            cubicle_json_builder_cleanup(&output);
            return -1;
        }
        if (cubicle_json_builder_append(&output, "[defaults]\n") < 0 ||
            cube_append_updated_defaults(&output, update) < 0) {
            cubicle_json_builder_cleanup(&output);
            return -1;
        }
    }
    if (output.data == NULL &&
        cubicle_json_builder_append(&output, "") < 0) {
        cubicle_json_builder_cleanup(&output);
        return -1;
    }

    *rendered_out = output.data;
    output.data = NULL;
    cubicle_json_builder_cleanup(&output);
    return 0;
}

static int cube_atomic_write_user_config(const char *path, const char *content)
{
    char parent[CUBICLE_PATH_MAX];
    if (cube_parent_directory(path, parent) < 0 ||
        cubicle_mkdir_p(parent) < 0) {
        return -1;
    }

    char temporary[CUBICLE_PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                          (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    int result = cubicle_write_all(fd, content, strlen(content)) == 0 &&
                         fsync(fd) == 0
                     ? 0
                     : -1;
    int saved_errno = errno;
    if (close(fd) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }
    if (result == 0 && chmod(temporary, 0600) < 0) {
        result = -1;
        saved_errno = errno;
    }
    if (result == 0 && rename(temporary, path) < 0) {
        result = -1;
        saved_errno = errno;
    }

    if (result == 0) {
        int dir_fd = open(parent, O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            (void)fsync(dir_fd);
            (void)close(dir_fd);
        }
        return 0;
    }

    (void)unlink(temporary);
    errno = saved_errno;
    return -1;
}

static int cube_update_user_defaults(const cube_defaults_update_t *update,
                                     char *error,
                                     size_t error_size)
{
    char path[CUBICLE_PATH_MAX];
    if (cube_user_config_path(path) < 0) {
        snprintf(error, error_size, "failed to resolve user config path: %s",
                 strerror(errno));
        return -1;
    }

    char *existing = cube_read_optional_file(path);
    if (existing == NULL) {
        snprintf(error, error_size, "failed to read %s: %s", path,
                 strerror(errno));
        return -1;
    }

    char *rendered = NULL;
    if (cube_render_updated_config(existing, update, &rendered) < 0) {
        free(existing);
        snprintf(error, error_size, "failed to update defaults");
        return -1;
    }
    free(existing);

    if (cube_atomic_write_user_config(path, rendered) < 0) {
        snprintf(error, error_size, "failed to write %s: %s", path,
                 strerror(errno));
        free(rendered);
        return -1;
    }
    free(rendered);

    cubicle_config_t reloaded;
    char validation_error[512] = "";
    if (cubicle_config_load_client(&reloaded, validation_error,
                                   sizeof(validation_error)) < 0) {
        snprintf(error, error_size, "updated config did not validate: %s",
                 validation_error);
        return -1;
    }
    return 0;
}

static int cube_defaults_valid_key(const char *key, const char **canonical)
{
    if (strcmp(key, "launch") == 0 || strcmp(key, "mode") == 0) {
        *canonical = key;
        return 1;
    }
    if (strcmp(key, "kill-cleanup") == 0 ||
        strcmp(key, "kill_cleanup") == 0) {
        *canonical = "kill_cleanup";
        return 1;
    }
    return 0;
}

static int command_defaults_show(const cube_options_t *options,
                                 const cubicle_config_t *config)
{
    const char *launch = cubicle_launch_default_name(config->default_launch);
    const char *mode = cube_mode_name(config->default_mode);
    const char *kill_cleanup =
        config->default_kill_cleanup ? "true" : "false";
    if (options->json) {
        printf("{\"launch\":\"%s\",\"mode\":\"%s\",\"kill_cleanup\":%s}\n",
               launch, mode, kill_cleanup);
    } else {
        printf("launch=%s\n", launch);
        printf("mode=%s\n", mode);
        printf("kill_cleanup=%s\n", kill_cleanup);
    }
    return 0;
}

static int command_defaults(const cube_options_t *options,
                            const cubicle_config_t *config,
                            int argc,
                            char **argv,
                            int command_index)
{
    if (command_index + 1 >= argc ||
        strcmp(argv[command_index + 1], "show") == 0) {
        if (command_index + 2 < argc) {
            fprintf(stderr, "cube: defaults show does not take arguments\n");
            return 2;
        }
        return command_defaults_show(options, config);
    }

    const char *subcommand = argv[command_index + 1];
    cube_defaults_update_t update = {0};
    const char *changed_key = NULL;
    const char *changed_value = NULL;

    if (strcmp(subcommand, "set") == 0) {
        if (command_index + 4 != argc) {
            fprintf(stderr, "cube: defaults set requires a key and value\n");
            return 2;
        }
        const char *canonical = NULL;
        if (!cube_defaults_valid_key(argv[command_index + 2], &canonical)) {
            fprintf(stderr, "cube: unknown defaults key '%s'\n",
                    argv[command_index + 2]);
            return 2;
        }
        const char *value = argv[command_index + 3];
        if (strcmp(canonical, "launch") == 0) {
            if (strcmp(value, "foreground") != 0 &&
                strcmp(value, "background") != 0) {
                fprintf(stderr,
                        "cube: defaults.launch must be foreground or background\n");
                return 2;
            }
            update.set_launch = 1;
            update.launch = value;
        } else if (strcmp(canonical, "mode") == 0) {
            if (strcmp(value, "stream") != 0 && strcmp(value, "tty") != 0 &&
                strcmp(value, "term") != 0) {
                fprintf(stderr,
                        "cube: defaults.mode must be stream, tty, or term\n");
                return 2;
            }
            update.set_mode = 1;
            update.mode = value;
        } else {
            if (strcmp(value, "true") != 0 && strcmp(value, "false") != 0) {
                fprintf(stderr,
                        "cube: defaults.kill_cleanup must be true or false\n");
                return 2;
            }
            update.set_kill_cleanup = 1;
            update.kill_cleanup = value;
        }
        changed_key = canonical;
        changed_value = value;
    } else if (strcmp(subcommand, "reset") == 0) {
        if (command_index + 2 == argc) {
            update.reset_launch = 1;
            update.reset_mode = 1;
            update.reset_kill_cleanup = 1;
            changed_key = "all";
        } else if (command_index + 3 == argc) {
            const char *canonical = NULL;
            if (!cube_defaults_valid_key(argv[command_index + 2],
                                         &canonical)) {
                fprintf(stderr, "cube: unknown defaults key '%s'\n",
                        argv[command_index + 2]);
                return 2;
            }
            update.reset_launch = strcmp(canonical, "launch") == 0;
            update.reset_mode = strcmp(canonical, "mode") == 0;
            update.reset_kill_cleanup =
                strcmp(canonical, "kill_cleanup") == 0;
            changed_key = canonical;
        } else {
            fprintf(stderr, "cube: defaults reset takes at most one key\n");
            return 2;
        }
    } else {
        fprintf(stderr, "cube: unknown defaults command '%s'\n", subcommand);
        return 2;
    }

    char error[512] = "";
    if (cube_update_user_defaults(&update, error, sizeof(error)) < 0) {
        fprintf(stderr, "cube: %s\n", error);
        return 2;
    }

    if (options->json) {
        if (changed_value != NULL) {
            printf("{\"updated\":\"%s\",\"value\":\"%s\"}\n", changed_key,
                   changed_value);
        } else {
            printf("{\"reset\":\"%s\"}\n", changed_key);
        }
    } else if (changed_value != NULL) {
        printf("defaults.%s=%s\n", changed_key, changed_value);
    } else {
        printf("defaults.%s reset\n", changed_key);
    }
    return 0;
}

static void configure_library_debug(const cubicle_config_t *config,
                                    const char *program)
{
    if (config == NULL || !config->cube_debug_library) {
        return;
    }
    char log_path[CUBICLE_PATH_MAX];
    int length = snprintf(log_path, sizeof(log_path),
                          "%s/client-library.log", config->manager_log_dir);
    if (length < 0 || (size_t)length >= sizeof(log_path)) {
        return;
    }
    (void)cubicle_mkdir_p(config->manager_log_dir);
    (void)setenv("CUBICLE_LIBRARY_DEBUG", "library", 1);
    (void)setenv("CUBICLE_LIBRARY_DEBUG_PROGRAM", program, 1);
    (void)setenv("CUBICLE_LIBRARY_DEBUG_LOG", log_path, 1);
}

static int parse_u64_arg(const char *value, uint64_t *value_out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return -1;
    }
    *value_out = (uint64_t)parsed;
    return 0;
}

static int call_manager(const char *socket_path,
                        const char *method,
                        const char *params,
                        cube_rpc_response_t *response)
{
    memset(response, 0, sizeof(*response));

    cubicle_client_t *client = NULL;
    cubicle_error_code_t code = cubicle_client_connect_uri(socket_path, NULL,
                                                           &client);
    if (code != CUBICLE_OK) {
        response->code = code;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to connect to manager");
        return -1;
    }

    char *result_json = NULL;
    code = cubicle_client_call_json(client, method, params, &result_json);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *error = cubicle_client_last_error(client);
        response->code = code;
        snprintf(response->error_message, sizeof(response->error_message),
                 "%s", error != NULL && error->message[0] != '\0'
                           ? error->message
                           : "request failed");
        cubicle_client_disconnect(client);
        return -1;
    }

    cubicle_client_disconnect(client);
    response->code = CUBICLE_OK;
    response->result_json = result_json;
    return 0;
}

static void cleanup_rpc_response(cube_rpc_response_t *response)
{
    free(response->result_json);
    response->result_json = NULL;
}

static int print_rpc_error(const cube_rpc_response_t *response)
{
    fprintf(stderr, "cube: %s\n",
            response->error_message[0] == '\0' ? "request failed"
                                               : response->error_message);
    return response->code == CUBICLE_ERR_NOT_FOUND ? 1 : 2;
}

static int json_string_field(yyjson_val *object,
                             const char *field,
                             char *buffer,
                             size_t buffer_size)
{
    yyjson_val *value = yyjson_obj_get(object, field);
    if (!yyjson_is_str(value)) {
        return -1;
    }
    int length = snprintf(buffer, buffer_size, "%s", yyjson_get_str(value));
    return length < 0 || (size_t)length >= buffer_size ? -1 : 0;
}

static int json_bool_field(yyjson_val *object, const char *field, int *value)
{
    yyjson_val *field_value = yyjson_obj_get(object, field);
    if (!yyjson_is_bool(field_value)) {
        return -1;
    }
    *value = yyjson_get_bool(field_value) ? 1 : 0;
    return 0;
}

static int json_int_field(yyjson_val *object, const char *field, int *value)
{
    yyjson_val *field_value = yyjson_obj_get(object, field);
    if (!yyjson_is_int(field_value)) {
        return -1;
    }
    int64_t parsed = yyjson_get_int(field_value);
    if (parsed < INT_MIN || parsed > INT_MAX) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int json_u64_field(yyjson_val *object,
                          const char *field,
                          uint64_t *value)
{
    yyjson_val *field_value = yyjson_obj_get(object, field);
    if (!yyjson_is_uint(field_value)) {
        return -1;
    }
    *value = yyjson_get_uint(field_value);
    return 0;
}

static int valid_name(const char *name)
{
    return name != NULL && name[0] != '\0' &&
           strchr(name, '\t') == NULL && strchr(name, '\n') == NULL;
}

static int resolve_directory_path(const char *directory,
                                  char resolved[CUBICLE_PATH_MAX])
{
    if (directory == NULL || directory[0] == '\0') {
        return getcwd(resolved, CUBICLE_PATH_MAX) == NULL ? -1 : 0;
    }

    char real_path[CUBICLE_PATH_MAX];
    if (realpath(directory, real_path) == NULL) {
        return -1;
    }

    int length = snprintf(resolved, CUBICLE_PATH_MAX, "%s", real_path);
    return length < 0 || length >= CUBICLE_PATH_MAX ? -1 : 0;
}

static int valid_directory_field(const char *directory)
{
    return directory != NULL && directory[0] != '\0' &&
           strchr(directory, '\t') == NULL && strchr(directory, '\n') == NULL;
}

static int workspace_create_or_select(const char *manager_socket,
                                      const cube_options_t *options,
                                      const char *name,
                                      const char *directory_arg)
{
    if (!valid_name(name)) {
        fprintf(stderr, "cube: invalid workspace name\n");
        return 2;
    }

    char escaped_name[CUBICLE_NAME_MAX * 2];
    if (cubicle_json_escape(escaped_name, sizeof(escaped_name), name) < 0) {
        fprintf(stderr, "cube: workspace name is too long\n");
        return 2;
    }

    char directory[CUBICLE_PATH_MAX];
    if (resolve_directory_path(directory_arg, directory) < 0 ||
        !valid_directory_field(directory)) {
        fprintf(stderr, "cube: invalid workspace directory: %s\n",
                directory_arg == NULL ? "." : directory_arg);
        return 2;
    }

    char params[2048];
    snprintf(params, sizeof(params), "{\"workspace\":\"%s\"}", escaped_name);
    cube_rpc_response_t response;
    int selected_existing = call_manager(manager_socket, "workspace.get",
                                         params, &response) == 0;

    if (!selected_existing) {
        cleanup_rpc_response(&response);
        cubicle_json_builder_t create_params = {0};
        if (cubicle_json_builder_append(&create_params, "{\"name\":") < 0 ||
            cubicle_json_builder_append_string(&create_params, name) < 0 ||
            cubicle_json_builder_append(&create_params, ",\"directory\":") < 0 ||
            cubicle_json_builder_append_string(&create_params, directory) < 0 ||
            cubicle_json_builder_append(&create_params, "}") < 0) {
            cubicle_json_builder_cleanup(&create_params);
            fprintf(stderr, "cube: failed to encode workspace create request\n");
            return 2;
        }
        if (call_manager(manager_socket, "workspace.create", create_params.data,
                         &response) < 0) {
            cubicle_json_builder_cleanup(&create_params);
            return print_rpc_error(&response);
        }
        cubicle_json_builder_cleanup(&create_params);
    }

    if (cubeui_store_selected_workspace(name) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: failed to persist selected workspace: %s\n",
                strerror(errno));
        return 2;
    }

    if (options->json) {
        printf("%s\n", response.result_json);
    } else {
        printf("Workspace %s %s\n", name,
               selected_existing ? "selected" : "created and selected");
    }
    cleanup_rpc_response(&response);
    return 0;
}

static int workspace_list(const char *manager_socket,
                          const cube_options_t *options)
{
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "workspace.list", "{}",
                     &response) < 0) {
        return print_rpc_error(&response);
    }

    if (options->json) {
        printf("%s\n", response.result_json);
        cleanup_rpc_response(&response);
        return 0;
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid workspace list response\n");
        return 2;
    }

    yyjson_val *workspaces = yyjson_obj_get(document.root, "workspaces");
    if (!yyjson_is_arr(workspaces)) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid workspace list response\n");
        return 2;
    }

    printf("WORKSPACE ID\tNAME\n");
    size_t index;
    size_t max;
    yyjson_val *item;
    yyjson_arr_foreach(workspaces, index, max, item) {
        char id[CUBICLE_ID_STRING_LENGTH];
        char name[CUBICLE_NAME_MAX];
        if (json_string_field(item, "id", id, sizeof(id)) == 0 &&
            json_string_field(item, "name", name, sizeof(name)) == 0) {
            printf("%s\t%s\n", id, name);
        }
    }

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int workspace_show_current(const cube_options_t *options)
{
    char workspace[CUBICLE_NAME_MAX];
    if (cubeui_read_selected_workspace(workspace, sizeof(workspace)) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }
    if (options->json) {
        char escaped_workspace[CUBICLE_NAME_MAX * 2];
        if (cubicle_json_escape(escaped_workspace, sizeof(escaped_workspace),
                                workspace) < 0) {
            fprintf(stderr, "cube: workspace name is too long\n");
            return 2;
        }
        printf("{\"workspace\":\"%s\"}\n", escaped_workspace);
    } else {
        printf("Workspace %s selected\n", workspace);
    }
    return 0;
}

static int workspace_simple_action(const char *manager_socket,
                                   const cube_options_t *options,
                                   const char *method,
                                   const char *workspace)
{
    if (!valid_name(workspace)) {
        fprintf(stderr, "cube: invalid workspace name\n");
        return 2;
    }
    char escaped_workspace[CUBICLE_NAME_MAX * 2];
    if (cubicle_json_escape(escaped_workspace, sizeof(escaped_workspace),
                            workspace) < 0) {
        fprintf(stderr, "cube: workspace name is too long\n");
        return 2;
    }

    char params[2048];
    if (strcmp(method, "workspace.delete") == 0) {
        snprintf(params, sizeof(params),
                 "{\"workspace_id\":\"%s\",\"stop_running_processes\":false,\"remove_retained_processes\":true}",
                 escaped_workspace);
    } else {
        snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\"}",
                 escaped_workspace);
    }

    cube_rpc_response_t response;
    if (call_manager(manager_socket, method, params, &response) < 0) {
        return print_rpc_error(&response);
    }
    if (options->json) {
        printf("%s\n", response.result_json);
    } else {
        printf("Workspace %s %s\n", workspace,
               strcmp(method, "workspace.stop") == 0 ? "stopped" : "deleted");
    }
    cleanup_rpc_response(&response);
    return 0;
}

static int resolve_workspace_selection(const cube_options_t *options,
                                       char *workspace,
                                       size_t workspace_size,
                                       int *from_selected_workspace)
{
    if (options->workspace != NULL && options->workspace[0] != '\0') {
        int length = snprintf(workspace, workspace_size, "%s",
                              options->workspace);
        if (from_selected_workspace != NULL) {
            *from_selected_workspace = 0;
        }
        return length < 0 || (size_t)length >= workspace_size ? -1 : 0;
    }
    if (from_selected_workspace != NULL) {
        *from_selected_workspace = 1;
    }
    return cubeui_read_selected_workspace(workspace, workspace_size);
}

static int resolve_workspace_argument(const cube_options_t *options,
                                      char *workspace,
                                      size_t workspace_size)
{
    return resolve_workspace_selection(options, workspace, workspace_size,
                                       NULL);
}

static int print_workspace_rpc_error(const cube_rpc_response_t *response,
                                     const char *workspace,
                                     int from_selected_workspace)
{
    if (response->code == CUBICLE_ERR_NOT_FOUND && from_selected_workspace) {
        cubeui_clear_selected_workspace_if_matches(workspace);
        fprintf(stderr,
                "cube: selected workspace '%s' was not found by the manager\n",
                workspace);
        fprintf(stderr,
                "hint: cleared the stale selection; run `cube workspace list` and then `cube workspace NAME` to select an existing workspace\n");
        return 1;
    }
    return print_rpc_error(response);
}

static int fetch_workspace_directory(const char *manager_socket,
                                     const char *workspace,
                                     char *directory,
                                     size_t directory_size,
                                     int from_selected_workspace)
{
    cubicle_json_builder_t params = {0};
    if (cubicle_json_builder_append(&params, "{\"workspace\":") < 0 ||
        cubicle_json_builder_append_string(&params, workspace) < 0 ||
        cubicle_json_builder_append(&params, "}") < 0) {
        cubicle_json_builder_cleanup(&params);
        fprintf(stderr, "cube: failed to encode workspace lookup request\n");
        return 2;
    }

    cube_rpc_response_t response;
    if (call_manager(manager_socket, "workspace.get", params.data,
                     &response) < 0) {
        cubicle_json_builder_cleanup(&params);
        return print_workspace_rpc_error(&response, workspace,
                                         from_selected_workspace);
    }
    cubicle_json_builder_cleanup(&params);

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid workspace response\n");
        return 2;
    }
    if (json_string_field(document.root, "directory", directory,
                          directory_size) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid workspace response\n");
        return 2;
    }
    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int command_workspace(const char *manager_socket,
                             const cube_options_t *options,
                             int argc,
                             char **argv,
                             int command_index)
{
    int remaining = argc - command_index - 1;
    char **arguments = &argv[command_index + 1];
    if (remaining == 0) {
        return workspace_show_current(options);
    }
    if (remaining == 1 && strcmp(arguments[0], "list") == 0) {
        return workspace_list(manager_socket, options);
    }
    if (remaining >= 2 && strcmp(arguments[0], "create") == 0) {
        const char *name = NULL;
        const char *directory = NULL;
        for (int i = 1; i < remaining; ++i) {
            if (strcmp(arguments[i], "--dir") == 0 && i + 1 < remaining) {
                directory = arguments[++i];
            } else if (name == NULL) {
                name = arguments[i];
            } else {
                fprintf(stderr, "cube: invalid workspace create command\n");
                return 2;
            }
        }
        if (name == NULL) {
            fprintf(stderr, "cube: workspace create requires a name\n");
            return 2;
        }
        return workspace_create_or_select(manager_socket, options, name,
                                          directory);
    }
    if (remaining == 2 && strcmp(arguments[0], "select") == 0) {
        return workspace_create_or_select(manager_socket, options,
                                          arguments[1], NULL);
    }
    if (remaining == 2 && strcmp(arguments[0], "stop") == 0) {
        return workspace_simple_action(manager_socket, options,
                                       "workspace.stop",
                                       arguments[1]);
    }
    if (remaining == 2 && strcmp(arguments[0], "delete") == 0) {
        return workspace_simple_action(manager_socket, options,
                                       "workspace.delete",
                                       arguments[1]);
    }
    if (remaining == 1) {
        return workspace_create_or_select(manager_socket, options,
                                          arguments[0], NULL);
    }

    fprintf(stderr, "cube: invalid workspace command\n");
    return 2;
}

static cubicle_capability_mask_t cube_access_role_mask(const char *role)
{
    cubicle_capability_mask_t observer =
        CUBICLE_CAP_WORKSPACE_READ |
        CUBICLE_CAP_PROCESS_READ |
        CUBICLE_CAP_PROCESS_OBSERVE |
        CUBICLE_CAP_EVENTS_READ;
    cubicle_capability_mask_t operator_mask =
        observer |
        CUBICLE_CAP_WORKSPACE_STOP |
        CUBICLE_CAP_PROCESS_START |
        CUBICLE_CAP_PROCESS_INPUT |
        CUBICLE_CAP_PROCESS_SIGNAL |
        CUBICLE_CAP_PROCESS_REMOVE;
    cubicle_capability_mask_t owner =
        operator_mask |
        CUBICLE_CAP_WORKSPACE_RENAME |
        CUBICLE_CAP_WORKSPACE_DELETE |
        CUBICLE_CAP_WORKSPACE_MANAGE_KEYS;

    if (strcmp(role, "observer") == 0) {
        return observer;
    }
    if (strcmp(role, "operator") == 0) {
        return operator_mask;
    }
    if (strcmp(role, "owner") == 0) {
        return owner;
    }
    return 0;
}

static int read_public_key_hex(const char *path_or_hex,
                               char public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH])
{
    FILE *file = fopen(path_or_hex, "r");
    if (file != NULL) {
        if (fgets(public_key_hex, CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH, file) ==
            NULL) {
            fclose(file);
            return -1;
        }
        fclose(file);
        public_key_hex[strcspn(public_key_hex, "\r\n")] = '\0';
    } else {
        int length = snprintf(public_key_hex,
                              CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH, "%s",
                              path_or_hex);
        if (length < 0 ||
            (size_t)length >= CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH) {
            return -1;
        }
    }

    unsigned char decoded[CUBICLE_AUTH_PUBLIC_KEY_BYTES];
    return cubicle_auth_hex_decode(public_key_hex, decoded, sizeof(decoded));
}

static int command_access(const char *manager_socket,
                          const cube_options_t *options,
                          int argc,
                          char **argv,
                          int command_index)
{
    int remaining = argc - command_index - 1;
    char **arguments = &argv[command_index + 1];
    char workspace[CUBICLE_NAME_MAX];
    int from_selected_workspace = 0;
    if (resolve_workspace_selection(options, workspace, sizeof(workspace),
                                    &from_selected_workspace) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }
    char escaped_workspace[CUBICLE_NAME_MAX * 2];
    if (cubicle_json_escape(escaped_workspace, sizeof(escaped_workspace),
                            workspace) < 0) {
        fprintf(stderr, "cube: workspace name is too long\n");
        return 2;
    }

    if (remaining == 1 && strcmp(arguments[0], "list") == 0) {
        char params[1024];
        snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\"}",
                 escaped_workspace);
        cube_rpc_response_t response;
        if (call_manager(manager_socket, "workspace.key.list", params,
                         &response) < 0) {
            return print_rpc_error(&response);
        }
        if (options->json) {
            printf("%s\n", response.result_json);
            cleanup_rpc_response(&response);
            return 0;
        }
        cubicle_json_doc_t document;
        if (cubicle_json_parse(&document, response.result_json) < 0) {
            cleanup_rpc_response(&response);
            fprintf(stderr, "cube: invalid access list response\n");
            return 2;
        }
        yyjson_val *keys = yyjson_obj_get(document.root, "keys");
        printf("KEY ID\tLABEL\tCAPABILITIES\tREVOKED\n");
        if (yyjson_is_arr(keys)) {
            size_t index;
            size_t max;
            yyjson_val *item;
            yyjson_arr_foreach(keys, index, max, item) {
                char key_id[CUBICLE_ID_STRING_LENGTH];
                char label[CUBICLE_KEY_LABEL_MAX];
                uint64_t capabilities = 0;
                uint64_t revoked_at_ms = 0;
                if (json_string_field(item, "key_id", key_id,
                                      sizeof(key_id)) == 0 &&
                    json_string_field(item, "label", label,
                                      sizeof(label)) == 0 &&
                    json_u64_field(item, "capabilities",
                                   &capabilities) == 0) {
                    (void)json_u64_field(item, "revoked_at_ms",
                                         &revoked_at_ms);
                    printf("%s\t%s\t%llu\t%s\n", key_id, label,
                           (unsigned long long)capabilities,
                           revoked_at_ms == 0 ? "no" : "yes");
                }
            }
        }
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        return 0;
    }

    if (remaining >= 2 && strcmp(arguments[0], "add") == 0) {
        const char *role = "operator";
        const char *label = "";
        int index = 2;
        while (index < remaining) {
            if (strcmp(arguments[index], "--role") == 0 &&
                index + 1 < remaining) {
                role = arguments[index + 1];
                index += 2;
                continue;
            }
            if (strcmp(arguments[index], "--label") == 0 &&
                index + 1 < remaining) {
                label = arguments[index + 1];
                index += 2;
                continue;
            }
            fprintf(stderr, "cube: invalid access add option '%s'\n",
                    arguments[index]);
            return 2;
        }
        cubicle_capability_mask_t capabilities = cube_access_role_mask(role);
        if (capabilities == 0) {
            fprintf(stderr, "cube: role must be observer, operator, or owner\n");
            return 2;
        }
        char public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
        if (read_public_key_hex(arguments[1], public_key_hex) < 0) {
            fprintf(stderr, "cube: invalid public key\n");
            return 2;
        }
        char escaped_label[CUBICLE_KEY_LABEL_MAX * 2];
        if (cubicle_json_escape(escaped_label, sizeof(escaped_label),
                                label) < 0) {
            fprintf(stderr, "cube: label is too long\n");
            return 2;
        }
        char params[2048];
        snprintf(params, sizeof(params),
                 "{\"workspace_id\":\"%s\",\"public_key\":\"%s\",\"label\":\"%s\",\"capabilities\":%llu}",
                 escaped_workspace, public_key_hex, escaped_label,
                 (unsigned long long)capabilities);
        cube_rpc_response_t response;
        if (call_manager(manager_socket, "workspace.key.add", params,
                         &response) < 0) {
            return print_rpc_error(&response);
        }
        if (options->json) {
            printf("%s\n", response.result_json);
        } else {
            cubicle_json_doc_t document;
            char key_id[CUBICLE_ID_STRING_LENGTH] = "";
            if (cubicle_json_parse(&document, response.result_json) == 0) {
                (void)json_string_field(document.root, "key_id", key_id,
                                        sizeof(key_id));
                cubicle_json_cleanup(&document);
            }
            printf("Access added%s%s\n", key_id[0] ? ": " : "", key_id);
        }
        cleanup_rpc_response(&response);
        return 0;
    }

    if (remaining == 3 && strcmp(arguments[0], "set-role") == 0) {
        cubicle_capability_mask_t capabilities =
            cube_access_role_mask(arguments[2]);
        if (capabilities == 0) {
            fprintf(stderr, "cube: role must be observer, operator, or owner\n");
            return 2;
        }
        char escaped_key[CUBICLE_ID_STRING_LENGTH * 2];
        if (cubicle_json_escape(escaped_key, sizeof(escaped_key),
                                arguments[1]) < 0) {
            fprintf(stderr, "cube: key id is too long\n");
            return 2;
        }
        char params[1024];
        snprintf(params, sizeof(params),
                 "{\"workspace_id\":\"%s\",\"key_id\":\"%s\",\"capabilities\":%llu}",
                 escaped_workspace, escaped_key,
                 (unsigned long long)capabilities);
        cube_rpc_response_t response;
        if (call_manager(manager_socket, "workspace.key.update", params,
                         &response) < 0) {
            return print_rpc_error(&response);
        }
        if (options->json) {
            printf("%s\n", response.result_json);
        } else {
            printf("Access updated\n");
        }
        cleanup_rpc_response(&response);
        return 0;
    }

    if (remaining == 2 &&
        (strcmp(arguments[0], "remove") == 0 ||
         strcmp(arguments[0], "revoke") == 0)) {
        char escaped_key[CUBICLE_ID_STRING_LENGTH * 2];
        if (cubicle_json_escape(escaped_key, sizeof(escaped_key),
                                arguments[1]) < 0) {
            fprintf(stderr, "cube: key id is too long\n");
            return 2;
        }
        char params[1024];
        snprintf(params, sizeof(params),
                 "{\"workspace_id\":\"%s\",\"key_id\":\"%s\"}",
                 escaped_workspace, escaped_key);
        cube_rpc_response_t response;
        if (call_manager(manager_socket, "workspace.key.revoke", params,
                         &response) < 0) {
            return print_rpc_error(&response);
        }
        if (options->json) {
            printf("%s\n", response.result_json);
        } else {
            printf("Access removed\n");
        }
        cleanup_rpc_response(&response);
        return 0;
    }

    fprintf(stderr, "cube: invalid access command\n");
    return 2;
}

typedef struct process_list_options {
    int all_workspaces;
} process_list_options_t;

static int parse_process_list_options(int argc,
                                      char **argv,
                                      int command_index,
                                      process_list_options_t *list_options)
{
    memset(list_options, 0, sizeof(*list_options));
    for (int i = command_index + 1; i < argc; ++i) {
        if (strcmp(argv[i], "-a") == 0 ||
            strcmp(argv[i], "--all-workspaces") == 0) {
            list_options->all_workspaces = 1;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_command_usage("ps", stdout);
            return 1;
        }
        fprintf(stderr, "cube: unknown ps option '%s'\n", argv[i]);
        return -1;
    }
    return 0;
}

static int print_process_list_result(const char *workspace,
                                     const cube_rpc_response_t *response)
{
    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response->result_json) < 0) {
        fprintf(stderr, "cube: invalid process list response\n");
        return 2;
    }

    yyjson_val *processes = yyjson_obj_get(document.root, "processes");
    if (!yyjson_is_arr(processes)) {
        cubicle_json_cleanup(&document);
        fprintf(stderr, "cube: invalid process list response\n");
        return 2;
    }

    printf("Workspace %s\n\n", workspace);
    printf("NAME\tMODE\tSTATE\n");
    size_t index;
    size_t max;
    yyjson_val *item;
    yyjson_arr_foreach(processes, index, max, item) {
        char name[CUBICLE_NAME_MAX];
        char mode[32];
        char state[32];
        if (json_string_field(item, "friendly_name", name, sizeof(name)) == 0 &&
            json_string_field(item, "mode", mode, sizeof(mode)) == 0 &&
            json_string_field(item, "state", state, sizeof(state)) == 0) {
            printf("%s\t%s\t%s\n", name, mode, state);
        }
    }

    cubicle_json_cleanup(&document);
    return 0;
}

static int process_list_for_workspace(const char *manager_socket,
                                      const char *workspace,
                                      cube_rpc_response_t *response_out)
{
    memset(response_out, 0, sizeof(*response_out));
    cubicle_json_builder_t params = {0};
    if (cubicle_json_builder_append(&params, "{\"workspace_id\":") < 0 ||
        cubicle_json_builder_append_string(&params, workspace) < 0 ||
        cubicle_json_builder_append(&params, "}") < 0) {
        cubicle_json_builder_cleanup(&params);
        response_out->code = CUBICLE_ERR_RESOURCE_LIMIT;
        snprintf(response_out->error_message, sizeof(response_out->error_message),
                 "workspace name is too long");
        return -1;
    }

    int result = call_manager(manager_socket, "process.list", params.data,
                              response_out);
    cubicle_json_builder_cleanup(&params);
    return result;
}

static int process_list_selected_workspace(const char *manager_socket,
                                           const cube_options_t *options)
{
    char workspace[CUBICLE_NAME_MAX];
    int from_selected_workspace = 0;
    if (resolve_workspace_selection(options, workspace, sizeof(workspace),
                                    &from_selected_workspace) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }

    cube_rpc_response_t response;
    if (process_list_for_workspace(manager_socket, workspace, &response) < 0) {
        return print_workspace_rpc_error(&response, workspace,
                                         from_selected_workspace);
    }

    if (options->json) {
        printf("%s\n", response.result_json);
        cleanup_rpc_response(&response);
        return 0;
    }

    int result = print_process_list_result(workspace, &response);
    cleanup_rpc_response(&response);
    return result;
}

static int append_all_workspace_processes_json(cubicle_json_builder_t *output,
                                               const char *id,
                                               const char *name,
                                               cube_rpc_response_t *response)
{
    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response->result_json) < 0) {
        fprintf(stderr, "cube: invalid process list response\n");
        return -1;
    }
    yyjson_val *processes = yyjson_obj_get(document.root, "processes");
    yyjson_val *count = yyjson_obj_get(document.root, "count");
    if (!yyjson_is_arr(processes) || !yyjson_is_uint(count)) {
        cubicle_json_cleanup(&document);
        fprintf(stderr, "cube: invalid process list response\n");
        return -1;
    }

    char *processes_json = cubicle_json_copy_value(processes);
    if (processes_json == NULL) {
        cubicle_json_cleanup(&document);
        fprintf(stderr, "cube: failed to format process list response\n");
        return -1;
    }

    int result =
        cubicle_json_builder_append(output, "{\"id\":") < 0 ||
                cubicle_json_builder_append_string(output, id) < 0 ||
                cubicle_json_builder_append(output, ",\"name\":") < 0 ||
                cubicle_json_builder_append_string(output, name) < 0 ||
                cubicle_json_builder_append(output, ",\"count\":") < 0 ||
                cubicle_json_builder_appendf(output, "%llu",
                                             (unsigned long long)
                                                 yyjson_get_uint(count)) < 0 ||
                cubicle_json_builder_append(output, ",\"processes\":") < 0 ||
                cubicle_json_builder_append(output, processes_json) < 0 ||
                cubicle_json_builder_append(output, "}") < 0
            ? -1
            : 0;
    free(processes_json);
    cubicle_json_cleanup(&document);
    if (result < 0) {
        fprintf(stderr, "cube: failed to format process list response\n");
    }
    return result;
}

static int process_list_all_workspaces(const char *manager_socket,
                                       const cube_options_t *options)
{
    cube_rpc_response_t workspace_response;
    if (call_manager(manager_socket, "workspace.list", "{}",
                     &workspace_response) < 0) {
        return print_rpc_error(&workspace_response);
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, workspace_response.result_json) < 0) {
        cleanup_rpc_response(&workspace_response);
        fprintf(stderr, "cube: invalid workspace list response\n");
        return 2;
    }

    yyjson_val *workspaces = yyjson_obj_get(document.root, "workspaces");
    if (!yyjson_is_arr(workspaces)) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&workspace_response);
        fprintf(stderr, "cube: invalid workspace list response\n");
        return 2;
    }

    cubicle_json_builder_t json_output = {0};
    if (options->json &&
        cubicle_json_builder_append(&json_output, "{\"workspaces\":[") < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&workspace_response);
        fprintf(stderr, "cube: failed to format process list response\n");
        return 2;
    }

    int result = 0;
    int first_text_block = 1;
    int first_json_item = 1;
    size_t index;
    size_t max;
    yyjson_val *item;
    yyjson_arr_foreach(workspaces, index, max, item) {
        char id[CUBICLE_ID_STRING_LENGTH];
        char name[CUBICLE_NAME_MAX];
        if (json_string_field(item, "id", id, sizeof(id)) < 0 ||
            json_string_field(item, "name", name, sizeof(name)) < 0) {
            fprintf(stderr, "cube: invalid workspace list response\n");
            result = 2;
            break;
        }

        cube_rpc_response_t process_response;
        if (process_list_for_workspace(manager_socket, id,
                                       &process_response) < 0) {
            result = print_rpc_error(&process_response);
            break;
        }

        if (options->json) {
            if ((!first_json_item &&
                 cubicle_json_builder_append(&json_output, ",") < 0) ||
                append_all_workspace_processes_json(&json_output, id, name,
                                                    &process_response) < 0) {
                cleanup_rpc_response(&process_response);
                result = 2;
                break;
            }
            first_json_item = 0;
        } else {
            if (!first_text_block) {
                printf("\n");
            }
            result = print_process_list_result(name, &process_response);
            first_text_block = 0;
        }
        cleanup_rpc_response(&process_response);
        if (result != 0) {
            break;
        }
    }

    if (result == 0 && options->json) {
        if (cubicle_json_builder_append(&json_output, "]}") < 0) {
            fprintf(stderr, "cube: failed to format process list response\n");
            result = 2;
        } else {
            printf("%s\n", json_output.data);
        }
    }

    cubicle_json_builder_cleanup(&json_output);
    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&workspace_response);
    return result;
}

static int process_list(const char *manager_socket,
                        const cube_options_t *options,
                        int argc,
                        char **argv,
                        int command_index)
{
    process_list_options_t list_options;
    int parse_result =
        parse_process_list_options(argc, argv, command_index, &list_options);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        return 2;
    }

    if (list_options.all_workspaces) {
        return process_list_all_workspaces(manager_socket, options);
    }
    return process_list_selected_workspace(manager_socket, options);
}

typedef struct cleanup_counts {
    uint64_t removed_count;
    uint64_t skipped_live_count;
    uint64_t skipped_saved_count;
} cleanup_counts_t;

static int manager_cleanup_workspace(const char *manager_socket,
                                     const char *workspace,
                                     int json_output,
                                     cleanup_counts_t *counts)
{
    char escaped_workspace[CUBICLE_NAME_MAX * 2];
    if (cubicle_json_escape(escaped_workspace, sizeof(escaped_workspace),
                            workspace) < 0) {
        fprintf(stderr, "cube: workspace name is too long\n");
        return 2;
    }

    char params[1024];
    snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\"}",
             escaped_workspace);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "manager.cleanup", params,
                     &response) < 0) {
        return print_rpc_error(&response);
    }

    if (json_output) {
        printf("%s\n", response.result_json);
        cleanup_rpc_response(&response);
        return 0;
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid cleanup response\n");
        return 2;
    }

    uint64_t removed_count = 0;
    uint64_t skipped_live_count = 0;
    uint64_t skipped_saved_count = 0;
    if (json_u64_field(document.root, "removed_count",
                       &removed_count) < 0 ||
        json_u64_field(document.root, "skipped_live_count",
                       &skipped_live_count) < 0 ||
        json_u64_field(document.root, "skipped_saved_count",
                       &skipped_saved_count) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid cleanup response\n");
        return 2;
    }

    if (counts) {
        counts->removed_count = removed_count;
        counts->skipped_live_count = skipped_live_count;
        counts->skipped_saved_count = skipped_saved_count;
    }

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int process_cleanup(const char *manager_socket,
                           const cube_options_t *options)
{
    char workspace[CUBICLE_NAME_MAX];
    if (resolve_workspace_argument(options, workspace, sizeof(workspace)) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }

    cleanup_counts_t counts = {0};
    int result = manager_cleanup_workspace(manager_socket, workspace,
                                           options->json, &counts);
    if (result != 0 || options->json) {
        return result;
    }

    printf("Removed %llu processes\n",
           (unsigned long long)counts.removed_count);
    printf("Skipped %llu live processes\n",
           (unsigned long long)counts.skipped_live_count);
    printf("Skipped %llu saved processes\n",
           (unsigned long long)counts.skipped_saved_count);
    return 0;
}

static int process_inspect(const char *manager_socket,
                           const cube_options_t *options,
                           const char *process_name)
{
    char workspace[CUBICLE_NAME_MAX];
    if (resolve_workspace_argument(options, workspace, sizeof(workspace)) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }
    if (!valid_name(process_name)) {
        fprintf(stderr, "cube: invalid process name\n");
        return 2;
    }

    char escaped_workspace[CUBICLE_NAME_MAX * 2];
    char escaped_process[CUBICLE_NAME_MAX * 2];
    if (cubicle_json_escape(escaped_workspace, sizeof(escaped_workspace),
                            workspace) < 0 ||
        cubicle_json_escape(escaped_process, sizeof(escaped_process),
                            process_name) < 0) {
        fprintf(stderr, "cube: name is too long\n");
        return 2;
    }

    char params[2048];
    snprintf(params, sizeof(params),
             "{\"process\":\"%s\",\"workspace_id\":\"%s\"}",
             escaped_process, escaped_workspace);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "process.get", params, &response) < 0) {
        return print_rpc_error(&response);
    }

    if (options->json) {
        printf("%s\n", response.result_json);
        cleanup_rpc_response(&response);
        return 0;
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }

    char id[CUBICLE_ID_STRING_LENGTH];
    char name[CUBICLE_NAME_MAX];
    char mode[32];
    char state[32];
    int saved = 0;
    if (json_string_field(document.root, "id", id, sizeof(id)) < 0 ||
        json_string_field(document.root, "friendly_name", name,
                          sizeof(name)) < 0 ||
        json_string_field(document.root, "mode", mode, sizeof(mode)) < 0 ||
        json_string_field(document.root, "state", state, sizeof(state)) < 0 ||
        json_bool_field(document.root, "saved", &saved) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }

    printf("Name:        %s\n", name);
    printf("Workspace:   %s\n", workspace);
    printf("Mode:        %s\n", mode);
    printf("State:       %s\n", state);
    printf("Saved:       %s\n", saved ? "yes" : "no");
    printf("Process ID:  %s\n", id);

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int signal_number_for_name(const char *signal_name, int *signal_number)
{
    struct signal_alias {
        const char *name;
        int number;
    };
    static const struct signal_alias aliases[] = {
        {"HUP", SIGHUP},   {"SIGHUP", SIGHUP},
        {"INT", SIGINT},   {"SIGINT", SIGINT},
        {"QUIT", SIGQUIT}, {"SIGQUIT", SIGQUIT},
        {"TERM", SIGTERM}, {"SIGTERM", SIGTERM},
        {"KILL", SIGKILL}, {"SIGKILL", SIGKILL},
        {"USR1", SIGUSR1}, {"SIGUSR1", SIGUSR1},
        {"USR2", SIGUSR2}, {"SIGUSR2", SIGUSR2},
    };

    char *end = NULL;
    errno = 0;
    long parsed = strtol(signal_name, &end, 10);
    if (errno == 0 && end != signal_name && *end == '\0' &&
        parsed > 0 && parsed < 128) {
        *signal_number = (int)parsed;
        return 0;
    }

    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (strcmp(signal_name, aliases[i].name) == 0) {
            *signal_number = aliases[i].number;
            return 0;
        }
    }
    return -1;
}

static int resolve_process_metadata(const char *manager_socket,
                                    const cube_options_t *options,
                                    const char *process_name,
                                    char *process_id,
                                    size_t process_id_size,
                                    char *mode,
                                    size_t mode_size)
{
    char workspace[CUBICLE_NAME_MAX];
    int from_selected_workspace = 0;
    int has_workspace = resolve_workspace_selection(
        options, workspace, sizeof(workspace), &from_selected_workspace) == 0;
    if (!valid_name(process_name)) {
        fprintf(stderr, "cube: invalid process name\n");
        return 2;
    }

    char escaped_process[CUBICLE_NAME_MAX * 2];
    if (cubicle_json_escape(escaped_process, sizeof(escaped_process),
                            process_name) < 0) {
        fprintf(stderr, "cube: process name is too long\n");
        return 2;
    }

    char params[2048];
    if (has_workspace) {
        char escaped_workspace[CUBICLE_NAME_MAX * 2];
        if (cubicle_json_escape(escaped_workspace, sizeof(escaped_workspace),
                                workspace) < 0) {
            fprintf(stderr, "cube: workspace name is too long\n");
            return 2;
        }
        snprintf(params, sizeof(params),
                 "{\"process\":\"%s\",\"workspace_id\":\"%s\"}",
                 escaped_process, escaped_workspace);
    } else {
        snprintf(params, sizeof(params), "{\"process\":\"%s\"}",
                 escaped_process);
    }

    cube_rpc_response_t response;
    if (call_manager(manager_socket, "process.get", params, &response) < 0) {
        if (has_workspace) {
            return print_workspace_rpc_error(&response, workspace,
                                             from_selected_workspace);
        }
        return print_rpc_error(&response);
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }
    if (json_string_field(document.root, "id", process_id,
                          process_id_size) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }
    if (mode != NULL &&
        json_string_field(document.root, "mode", mode, mode_size) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int resolve_process_id(const char *manager_socket,
                              const cube_options_t *options,
                              const char *process_name,
                              char *process_id,
                              size_t process_id_size)
{
    return resolve_process_metadata(manager_socket, options, process_name,
                                    process_id, process_id_size, NULL, 0);
}

static int process_lifecycle_action(const char *manager_socket,
                                    const cube_options_t *options,
                                    const char *process_name,
                                    const char *method,
                                    const char *message,
                                    int signal_number)
{
    char process_id[CUBICLE_ID_STRING_LENGTH];
    int resolve_result = resolve_process_id(manager_socket, options,
                                            process_name, process_id,
                                            sizeof(process_id));
    if (resolve_result != 0) {
        return resolve_result;
    }

    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    if (strcmp(method, "process.signal") == 0) {
        snprintf(params, sizeof(params),
                 "{\"process_id\":\"%s\",\"signal_number\":%d}",
                 escaped_process_id, signal_number);
    } else {
        snprintf(params, sizeof(params), "{\"process_id\":\"%s\"}",
                 escaped_process_id);
    }

    cube_rpc_response_t response;
    if (call_manager(manager_socket, method, params, &response) < 0) {
        return print_rpc_error(&response);
    }

    if (options->json) {
        printf("%s\n", response.result_json);
    } else {
        printf("Process %s %s\n", process_name, message);
    }
    cleanup_rpc_response(&response);
    return 0;
}

static int process_action_by_id(const char *manager_socket,
                                const char *process_id,
                                const char *method,
                                int signal_number)
{
    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    if (strcmp(method, "process.signal") == 0) {
        snprintf(params, sizeof(params),
                 "{\"process_id\":\"%s\",\"signal_number\":%d}",
                 escaped_process_id, signal_number);
    } else {
        snprintf(params, sizeof(params), "{\"process_id\":\"%s\"}",
                 escaped_process_id);
    }

    cube_rpc_response_t response;
    if (call_manager(manager_socket, method, params, &response) < 0) {
        return print_rpc_error(&response);
    }
    cleanup_rpc_response(&response);
    return 0;
}

static int wait_for_process_timeout(const char *manager_socket,
                                    const char *process_id,
                                    uint64_t timeout_ms)
{
    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    snprintf(params, sizeof(params),
             "{\"process_id\":\"%s\",\"timeout_ms\":%llu}",
             escaped_process_id, (unsigned long long)timeout_ms);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "process.wait", params, &response) < 0) {
        return print_rpc_error(&response);
    }
    cleanup_rpc_response(&response);
    return 0;
}

static int remove_process_by_id(const char *manager_socket,
                                const char *process_id)
{
    return process_action_by_id(manager_socket, process_id, "process.remove",
                                0);
}

static int process_saved_by_id(const char *manager_socket,
                               const char *process_id,
                               int *saved)
{
    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    snprintf(params, sizeof(params), "{\"process\":\"%s\"}",
             escaped_process_id);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "process.get", params, &response) < 0) {
        return print_rpc_error(&response);
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }
    if (json_bool_field(document.root, "saved", saved) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int process_save_command(const char *manager_socket,
                                const cube_options_t *options,
                                const char *process_name,
                                int saved)
{
    char process_id[CUBICLE_ID_STRING_LENGTH];
    int resolve_result = resolve_process_id(manager_socket, options,
                                            process_name, process_id,
                                            sizeof(process_id));
    if (resolve_result != 0) {
        return resolve_result;
    }

    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    snprintf(params, sizeof(params), "{\"process_id\":\"%s\"}",
             escaped_process_id);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, saved ? "process.save" : "process.unsave",
                     params, &response) < 0) {
        return print_rpc_error(&response);
    }

    if (options->json) {
        printf("%s\n", response.result_json);
    } else {
        printf("Process %s %s\n", process_name,
               saved ? "saved" : "unsaved");
    }
    cleanup_rpc_response(&response);
    return 0;
}

static int process_kill_single(const char *manager_socket,
                               const cube_options_t *options,
                               const char *process_name,
                               int cleanup_after_kill)
{
    char process_id[CUBICLE_ID_STRING_LENGTH];
    int resolve_result = resolve_process_id(manager_socket, options,
                                            process_name, process_id,
                                            sizeof(process_id));
    if (resolve_result != 0) {
        return resolve_result;
    }

    int result = process_action_by_id(manager_socket, process_id,
                                      "process.kill", 0);
    if (result != 0) {
        return result;
    }

    int removed_after_kill = 0;
    int skipped_saved = 0;
    if (cleanup_after_kill) {
        result = wait_for_process_timeout(manager_socket, process_id, 5000);
        if (result != 0) {
            return result;
        }
        int saved = 0;
        result = process_saved_by_id(manager_socket, process_id, &saved);
        if (result != 0) {
            return result;
        }
        if (saved) {
            skipped_saved = 1;
        } else {
            result = remove_process_by_id(manager_socket, process_id);
            if (result != 0) {
                return result;
            }
            removed_after_kill = 1;
        }
    }

    if (options->json) {
        if (cleanup_after_kill) {
            printf("{\"removed\":%s,\"skipped_saved\":%s}\n",
                   removed_after_kill ? "true" : "false",
                   skipped_saved ? "true" : "false");
        } else {
            printf("{}\n");
        }
    } else {
        printf("Process %s killed\n", process_name);
        if (removed_after_kill) {
            printf("Process %s removed\n", process_name);
        } else if (skipped_saved) {
            printf("Process %s saved; cleanup skipped\n", process_name);
        }
    }
    return 0;
}

static int process_kill_all(const char *manager_socket,
                            const cube_options_t *options,
                            int cleanup_after_kill)
{
    char workspace[CUBICLE_NAME_MAX];
    if (resolve_workspace_argument(options, workspace, sizeof(workspace)) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }

    char escaped_workspace[CUBICLE_NAME_MAX * 2];
    if (cubicle_json_escape(escaped_workspace, sizeof(escaped_workspace),
                            workspace) < 0) {
        fprintf(stderr, "cube: workspace name is too long\n");
        return 2;
    }

    char params[1024];
    snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\"}",
             escaped_workspace);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "process.list", params, &response) < 0) {
        return print_rpc_error(&response);
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process list response\n");
        return 2;
    }

    yyjson_val *processes = yyjson_obj_get(document.root, "processes");
    if (!yyjson_is_arr(processes)) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process list response\n");
        return 2;
    }

    typedef struct kill_target {
        char id[CUBICLE_ID_STRING_LENGTH];
        char name[CUBICLE_NAME_MAX];
    } kill_target_t;
    kill_target_t targets[256];
    size_t target_count = 0;
    size_t index;
    size_t max;
    yyjson_val *item;
    yyjson_arr_foreach(processes, index, max, item) {
        char id[CUBICLE_ID_STRING_LENGTH];
        char name[CUBICLE_NAME_MAX];
        char state[32];
        if (json_string_field(item, "id", id, sizeof(id)) == 0 &&
            json_string_field(item, "friendly_name", name, sizeof(name)) == 0 &&
            json_string_field(item, "state", state, sizeof(state)) == 0 &&
            strcmp(state, "running") == 0) {
            if (target_count >= sizeof(targets) / sizeof(targets[0])) {
                cubicle_json_cleanup(&document);
                cleanup_rpc_response(&response);
                fprintf(stderr, "cube: too many running processes to kill\n");
                return 2;
            }
            snprintf(targets[target_count].id, sizeof(targets[target_count].id),
                     "%s", id);
            snprintf(targets[target_count].name,
                     sizeof(targets[target_count].name), "%s", name);
            ++target_count;
        }
    }

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);

    size_t killed_count = 0;
    size_t removed_count = 0;
    size_t skipped_saved_count = 0;
    cleanup_counts_t cleanup_counts = {0};
    for (size_t i = 0; i < target_count; ++i) {
        int result = process_action_by_id(manager_socket, targets[i].id,
                                          "process.kill", 0);
        if (result != 0) {
            return result;
        }
        ++killed_count;
    }

    if (cleanup_after_kill) {
        for (size_t i = 0; i < target_count; ++i) {
            int result = wait_for_process_timeout(manager_socket,
                                                  targets[i].id, 5000);
            if (result != 0) {
                return result;
            }
        }
        int result = manager_cleanup_workspace(manager_socket, workspace, 0,
                                               &cleanup_counts);
        if (result != 0) {
            return result;
        }
        removed_count = (size_t)cleanup_counts.removed_count;
        skipped_saved_count = (size_t)cleanup_counts.skipped_saved_count;
    }

    if (options->json) {
        printf("{\"killed_count\":%zu,\"removed_count\":%zu,\"skipped_saved_count\":%zu}\n",
               killed_count, removed_count, skipped_saved_count);
    } else {
        printf("Killed %zu processes\n", killed_count);
        if (cleanup_after_kill) {
            printf("Removed %zu processes\n", removed_count);
            printf("Skipped %zu saved processes\n", skipped_saved_count);
        }
    }
    return 0;
}

static int process_kill_command(const char *manager_socket,
                                const cube_options_t *options,
                                const cubicle_config_t *config,
                                int argc,
                                char **argv,
                                int command_index)
{
    int all = 0;
    int cleanup_after_kill = config->default_kill_cleanup;
    const char *process_name = NULL;

    for (int i = command_index + 1; i < argc; ++i) {
        if (strcmp(argv[i], "--all") == 0) {
            all = 1;
            continue;
        }
        if (strcmp(argv[i], "--cleanup") == 0) {
            cleanup_after_kill = 1;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "cube: unknown kill option '%s'\n", argv[i]);
            return 2;
        }
        if (process_name != NULL) {
            fprintf(stderr, "cube: kill takes only one process name\n");
            return 2;
        }
        process_name = argv[i];
    }

    if (all && process_name != NULL) {
        fprintf(stderr, "cube: kill --all does not take a process name\n");
        return 2;
    }
    if (all) {
        return process_kill_all(manager_socket, options, cleanup_after_kill);
    }
    if (process_name == NULL) {
        fprintf(stderr, "cube: kill requires a process name or --all\n");
        return 2;
    }
    return process_kill_single(manager_socket, options, process_name,
                               cleanup_after_kill);
}

static int generated_process_name(char *buffer,
                                  size_t buffer_size,
                                  const char *command)
{
    const char *base = strrchr(command, '/');
    base = base == NULL ? command : base + 1;
    if (base[0] == '\0') {
        return -1;
    }

    size_t used = 0;
    for (const char *cursor = base; *cursor != '\0'; ++cursor) {
        char c = *cursor;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-') {
            if (used + 1 >= buffer_size) {
                break;
            }
            buffer[used++] = c;
        } else if (used > 0 && buffer[used - 1] != '-') {
            if (used + 1 >= buffer_size) {
                break;
            }
            buffer[used++] = '-';
        }
    }
    while (used > 0 && buffer[used - 1] == '-') {
        --used;
    }
    if (used == 0) {
        return -1;
    }
    buffer[used] = '\0';
    return 0;
}

static int generated_process_name_with_suffix(char *buffer,
                                              size_t buffer_size,
                                              const char *base_name,
                                              int suffix)
{
    int length = 0;
    if (suffix == 0) {
        length = snprintf(buffer, buffer_size, "%s", base_name);
    } else {
        length = snprintf(buffer, buffer_size, "%s-%d", base_name, suffix);
    }
    return length < 0 || (size_t)length >= buffer_size ? -1 : 0;
}

static int build_process_start_params(cubicle_json_builder_t *params,
                                      const char *workspace,
                                      const cube_run_options_t *run_options,
                                      int command_argc,
                                      char **command_argv)
{
    if (cubicle_json_builder_append(params, "{\"workspace_id\":") < 0 ||
        cubicle_json_builder_append_string(params, workspace) < 0 ||
        cubicle_json_builder_append(params, ",\"friendly_name\":") < 0 ||
        cubicle_json_builder_append_string(params, run_options->name) < 0 ||
        cubicle_json_builder_append(params, ",\"mode\":") < 0 ||
        cubicle_json_builder_append_string(params, run_options->mode) < 0 ||
        cubicle_json_builder_append(params, ",\"stdin_policy\":") < 0 ||
        cubicle_json_builder_append_string(
            params,
            run_options->background ||
                    process_mode_uses_terminal(run_options->mode)
                ? "open"
                : "eof") < 0 ||
        cubicle_json_builder_append(params, ",\"cwd\":") < 0 ||
        cubicle_json_builder_append_string(params, run_options->directory) < 0 ||
        cubicle_json_builder_append(params, ",\"argv\":[") < 0) {
        return -1;
    }

    for (int i = 0; i < command_argc; ++i) {
        if ((i > 0 && cubicle_json_builder_append(params, ",") < 0) ||
            cubicle_json_builder_append_string(params, command_argv[i]) < 0) {
            return -1;
        }
    }

    return cubicle_json_builder_append(params, "]}");
}

static int read_process_output_once(const char *manager_socket,
                                    const char *process_id,
                                    const char *stream,
                                    uint64_t *offset,
                                    uint64_t maximum_length,
                                    FILE *output,
                                    int *end_of_stream,
                                    int *advanced)
{
    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    snprintf(params, sizeof(params),
             "{\"process_id\":\"%s\",\"stream\":\"%s\",\"offset\":%llu,\"maximum_length\":%llu}",
             escaped_process_id, stream,
             (unsigned long long)*offset,
             (unsigned long long)maximum_length);

    cube_rpc_response_t response;
    if (call_manager(manager_socket, "process.read_output", params,
                     &response) < 0) {
        return print_rpc_error(&response);
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid output response\n");
        return 2;
    }

    yyjson_val *data = yyjson_obj_get(document.root, "data");
    uint64_t next_offset = 0;
    if (!yyjson_is_str(data) ||
        json_u64_field(document.root, "next_offset",
                       &next_offset) < 0 ||
        json_bool_field(document.root, "end_of_stream",
                        end_of_stream) < 0 ||
        next_offset < *offset) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid output response\n");
        return 2;
    }
    fputs(yyjson_get_str(data), output);
    fflush(output);
    *advanced = next_offset > *offset ? 1 : 0;
    *offset = next_offset;

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int read_process_output(const char *manager_socket,
                               const char *process_id,
                               const char *stream,
                               uint64_t start,
                               uint64_t end,
                               int has_end,
                               FILE *output)
{
    uint64_t offset = start;
    for (;;) {
        uint64_t maximum_length = 8192;
        if (has_end) {
            if (offset >= end) {
                return 0;
            }
            if (end - offset < maximum_length) {
                maximum_length = end - offset;
            }
        }
        int end_of_stream = 0;
        int advanced = 0;
        int result = read_process_output_once(manager_socket, process_id,
                                              stream, &offset, maximum_length,
                                              output, &end_of_stream,
                                              &advanced);
        if (result != 0 || end_of_stream) {
            return result;
        }
        if (!advanced) {
            fprintf(stderr, "cube: output stream did not advance\n");
            return 2;
        }
    }
}

static int process_is_terminal_name(const char *state)
{
    return strcmp(state, "completed") == 0 ||
           strcmp(state, "failed") == 0 ||
           strcmp(state, "lost") == 0 ||
           strcmp(state, "removed") == 0 ||
           strcmp(state, "exited") == 0;
}

static int manager_process_is_terminal(const char *manager_socket,
                                       const char *process_id,
                                       int *terminal)
{
    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    snprintf(params, sizeof(params), "{\"process\":\"%s\"}",
             escaped_process_id);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "process.get", params, &response) < 0) {
        return print_rpc_error(&response);
    }

    cubicle_json_doc_t document;
    char state[32];
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }
    if (json_string_field(document.root, "state", state, sizeof(state)) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }
    *terminal = process_is_terminal_name(state);
    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int follow_process_output(const char *manager_socket,
                                 const char *process_id,
                                 const char *mode,
                                 const cube_log_options_t *log_options)
{
    uint64_t stdout_offset = log_options->start;
    uint64_t stderr_offset = log_options->start;
    uint64_t tty_offset = log_options->start;
    int stdout_end = 0;
    int stderr_end = 0;
    int tty_end = 0;

    for (;;) {
        int advanced = 0;
        int result = 0;
        if (process_mode_uses_terminal(mode)) {
            if (!log_options->stderr_only) {
                result = read_process_output_once(manager_socket, process_id,
                                                  "tty", &tty_offset, 8192,
                                                  stdout, &tty_end,
                                                  &advanced);
            }
            if (result == 0 && strcmp(mode, "term") == 0 &&
                !log_options->stdout_only) {
                int stream_advanced = 0;
                result = read_process_output_once(manager_socket, process_id,
                                                  "stderr", &stderr_offset,
                                                  8192,
                                                  stderr, &stderr_end,
                                                  &stream_advanced);
                advanced = advanced || stream_advanced;
            }
        } else {
            int stream_advanced = 0;
            if (!log_options->stderr_only) {
                result = read_process_output_once(manager_socket, process_id,
                                                  "stdout", &stdout_offset,
                                                  8192, stdout, &stdout_end,
                                                  &stream_advanced);
                advanced = stream_advanced;
            }
            if (result == 0 && !log_options->stdout_only) {
                stream_advanced = 0;
                result = read_process_output_once(manager_socket, process_id,
                                                  "stderr", &stderr_offset,
                                                  8192,
                                                  stderr, &stderr_end,
                                                  &stream_advanced);
                advanced = advanced || stream_advanced;
            }
        }
        if (result != 0) {
            return result;
        }

        int terminal = 0;
        result = manager_process_is_terminal(manager_socket, process_id,
                                             &terminal);
        if (result != 0) {
            return result;
        }
        if (terminal &&
            ((strcmp(mode, "tty") == 0 &&
              (log_options->stderr_only || tty_end)) ||
             (strcmp(mode, "term") == 0 &&
              (log_options->stderr_only || tty_end) &&
              (log_options->stdout_only || stderr_end)) ||
             (!process_mode_uses_terminal(mode) &&
              (log_options->stderr_only || stdout_end) &&
              (log_options->stdout_only || stderr_end)))) {
            return 0;
        }
        if (!advanced) {
            cube_sleep_poll_interval();
        }
    }
}

static int process_logs(const char *manager_socket,
                        const cube_options_t *options,
                        int argc,
                        char **argv,
                        int command_index)
{
    int argument_index = command_index + 1;
    cube_log_options_t log_options = {0};
    while (argument_index < argc) {
        if (strcmp(argv[argument_index], "--follow") == 0) {
            log_options.follow = 1;
            ++argument_index;
            continue;
        }
        if (strcmp(argv[argument_index], "--stdout") == 0) {
            log_options.stdout_only = 1;
            ++argument_index;
            continue;
        }
        if (strcmp(argv[argument_index], "--stderr") == 0) {
            log_options.stderr_only = 1;
            ++argument_index;
            continue;
        }
        if (strcmp(argv[argument_index], "--start") == 0) {
            if (argument_index + 1 >= argc ||
                parse_u64_arg(argv[argument_index + 1],
                              &log_options.start) < 0) {
                fprintf(stderr, "cube: --start requires a byte offset\n");
                return 2;
            }
            argument_index += 2;
            continue;
        }
        if (strcmp(argv[argument_index], "--end") == 0) {
            if (argument_index + 1 >= argc ||
                parse_u64_arg(argv[argument_index + 1],
                              &log_options.end) < 0) {
                fprintf(stderr, "cube: --end requires a byte offset\n");
                return 2;
            }
            log_options.has_end = 1;
            argument_index += 2;
            continue;
        }
        if (strncmp(argv[argument_index], "--start=", 8) == 0) {
            if (parse_u64_arg(argv[argument_index] + 8,
                              &log_options.start) < 0) {
                fprintf(stderr, "cube: --start requires a byte offset\n");
                return 2;
            }
            ++argument_index;
            continue;
        }
        if (strncmp(argv[argument_index], "--end=", 6) == 0) {
            if (parse_u64_arg(argv[argument_index] + 6,
                              &log_options.end) < 0) {
                fprintf(stderr, "cube: --end requires a byte offset\n");
                return 2;
            }
            log_options.has_end = 1;
            ++argument_index;
            continue;
        }
        if (argv[argument_index][0] == '-' &&
            argv[argument_index][1] == '-') {
            fprintf(stderr, "cube: unknown logs option '%s'\n",
                    argv[argument_index]);
            return 2;
        }
        break;
    }
    if (log_options.stdout_only && log_options.stderr_only) {
        fprintf(stderr, "cube: --stdout and --stderr are mutually exclusive\n");
        return 2;
    }
    if (log_options.has_end && log_options.end < log_options.start) {
        fprintf(stderr, "cube: --end must be greater than or equal to --start\n");
        return 2;
    }
    if (log_options.follow && log_options.has_end) {
        fprintf(stderr, "cube: --end cannot be used with --follow\n");
        return 2;
    }
    if (argument_index + 1 != argc) {
        fprintf(stderr, "cube: logs requires a process name\n");
        return 2;
    }

    char process_id[CUBICLE_ID_STRING_LENGTH];
    char mode[32];
    int resolve_result = resolve_process_metadata(
        manager_socket, options, argv[argument_index], process_id,
        sizeof(process_id), mode, sizeof(mode));
    if (resolve_result != 0) {
        return resolve_result;
    }

    if (strcmp(mode, "tty") == 0 && log_options.stderr_only) {
        fprintf(stderr, "cube: tty processes do not retain stderr separately\n");
        return 2;
    }

    if (log_options.follow) {
        return follow_process_output(manager_socket, process_id, mode,
                                     &log_options);
    }

    if (strcmp(mode, "tty") == 0) {
        return read_process_output(manager_socket, process_id, "tty",
                                   log_options.start, log_options.end,
                                   log_options.has_end, stdout);
    }
    if (strcmp(mode, "term") == 0) {
        int tty_result = 0;
        int stderr_result = 0;
        if (!log_options.stderr_only) {
            tty_result = read_process_output(
                manager_socket, process_id, "tty", log_options.start,
                log_options.end, log_options.has_end, stdout);
        }
        if (!log_options.stdout_only) {
            stderr_result = read_process_output(
                manager_socket, process_id, "stderr", log_options.start,
                log_options.end, log_options.has_end, stderr);
        }
        return tty_result != 0 ? tty_result : stderr_result;
    }

    int stdout_result = 0;
    int stderr_result = 0;
    if (!log_options.stderr_only) {
        stdout_result = read_process_output(
            manager_socket, process_id, "stdout", log_options.start,
            log_options.end, log_options.has_end, stdout);
    }
    if (!log_options.stdout_only) {
        stderr_result = read_process_output(
            manager_socket, process_id, "stderr", log_options.start,
            log_options.end, log_options.has_end, stderr);
    }
    return stdout_result != 0 ? stdout_result : stderr_result;
}

static int print_events_response(const char *result_json,
                                 int json,
                                 uint64_t *after_sequence,
                                 size_t *event_count,
                                 int *has_more)
{
    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, result_json) < 0) {
        fprintf(stderr, "cube: invalid events response\n");
        return 2;
    }

    yyjson_val *events = yyjson_obj_get(document.root, "events");
    if (!yyjson_is_arr(events)) {
        cubicle_json_cleanup(&document);
        fprintf(stderr, "cube: invalid events response\n");
        return 2;
    }

    *event_count = yyjson_arr_size(events);
    int more = 0;
    (void)json_bool_field(document.root, "has_more", &more);
    if (has_more != NULL) {
        *has_more = more;
    }
    if (json) {
        if (*event_count > 0) {
            printf("%s\n", result_json);
            fflush(stdout);
        }
        yyjson_arr_iter iter = yyjson_arr_iter_with(events);
        yyjson_val *item = NULL;
        while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
            uint64_t sequence = 0;
            if (json_u64_field(item, "global_sequence", &sequence) == 0 &&
                sequence > *after_sequence) {
                *after_sequence = sequence;
            }
        }
        cubicle_json_cleanup(&document);
        return 0;
    }

    size_t index;
    size_t max;
    yyjson_val *item;
    yyjson_arr_foreach(events, index, max, item) {
        uint64_t sequence = 0;
        char process_id[CUBICLE_ID_STRING_LENGTH];
        char type[64];
        char payload[CUBICLE_EVENT_PAYLOAD_MAX];
        if (json_u64_field(item, "global_sequence", &sequence) == 0 &&
            json_string_field(item, "process_id", process_id,
                              sizeof(process_id)) == 0 &&
            json_string_field(item, "type", type, sizeof(type)) == 0 &&
            json_string_field(item, "payload", payload,
                              sizeof(payload)) == 0) {
            printf("%llu\t%s\t%s\t%s\n",
                   (unsigned long long)sequence, process_id, type, payload);
            if (sequence > *after_sequence) {
                *after_sequence = sequence;
            }
        }
    }
    fflush(stdout);

    cubicle_json_cleanup(&document);
    return 0;
}

static int process_events(const char *manager_socket,
                          const cube_options_t *options,
                          int argc,
                          char **argv,
                          int command_index)
{
    int argument_index = command_index + 1;
    int follow = 0;
    int iterations = -1;
    while (argument_index < argc) {
        if (strcmp(argv[argument_index], "--follow") == 0) {
            follow = 1;
            ++argument_index;
            continue;
        }
        if (strcmp(argv[argument_index], "--iterations") == 0) {
            char *end = NULL;
            long parsed = 0;
            if (argument_index + 1 >= argc) {
                fprintf(stderr, "cube: --iterations requires a count\n");
                return 2;
            }
            errno = 0;
            parsed = strtol(argv[argument_index + 1], &end, 10);
            if (errno != 0 || end == argv[argument_index + 1] ||
                *end != '\0' || parsed < 0 || parsed > INT_MAX) {
                fprintf(stderr, "cube: invalid --iterations count\n");
                return 2;
            }
            iterations = (int)parsed;
            argument_index += 2;
            continue;
        }
        fprintf(stderr, "cube: unknown events option '%s'\n",
                argv[argument_index]);
        return 2;
    }
    if (iterations >= 0 && !follow) {
        fprintf(stderr, "cube: --iterations requires --follow\n");
        return 2;
    }
    char workspace[CUBICLE_NAME_MAX];
    if (resolve_workspace_argument(options, workspace, sizeof(workspace)) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }
    char escaped_workspace[CUBICLE_NAME_MAX * 2];
    if (cubicle_json_escape(escaped_workspace, sizeof(escaped_workspace),
                            workspace) < 0) {
        fprintf(stderr, "cube: workspace name is too long\n");
        return 2;
    }

    if (!options->json) {
        printf("Workspace %s\n\n", workspace);
        printf("SEQ\tPROCESS\tTYPE\tPAYLOAD\n");
        fflush(stdout);
    }

    uint64_t after_sequence = 0;

    int completed_iterations = 0;
    do {
        char params[1024];
        snprintf(params, sizeof(params),
                 "{\"workspace_id\":\"%s\",\"after_sequence\":%llu,\"limit\":100}",
                 escaped_workspace, (unsigned long long)after_sequence);
        cube_rpc_response_t response;
        if (call_manager(manager_socket, "events.list", params,
                         &response) < 0) {
            return print_rpc_error(&response);
        }

        size_t event_count = 0;
        int has_more = 0;
        int result = print_events_response(response.result_json,
                                          options->json, &after_sequence,
                                          &event_count, &has_more);
        cleanup_rpc_response(&response);
        if (result != 0) {
            return result;
        }

        if (!follow && !has_more) {
            return 0;
        }
        ++completed_iterations;
        if (iterations >= 0 && completed_iterations >= iterations) {
            return 0;
        }
        if (event_count == 0) {
            cube_sleep_poll_interval();
        }
    } while (1);
}

static int attachment_write_all(cubicle_attachment_t *attachment,
                                const char *buffer,
                                size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t result = cubicle_attachment_write(attachment, buffer + written,
                                                  length - written);
        if (result < 0) {
            const cubicle_error_t *error =
                cubicle_attachment_last_error(attachment);
            fprintf(stderr, "cube: %s\n",
                    error != NULL && error->message[0] != '\0'
                        ? error->message
                        : "failed to write attachment input");
            return 2;
        }
        if (result == 0) {
            fprintf(stderr, "cube: failed to write attachment input\n");
            return 2;
        }
        written += (size_t)result;
    }
    return 0;
}

static int process_input_chunk(cubicle_attachment_t *attachment,
                               const char *buffer,
                               size_t length,
                               int *escape_pending,
                               int *detach_requested)
{
    char output[4096];
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)buffer[i];
        if (*escape_pending) {
            *escape_pending = 0;
            if (byte == 'd') {
                *detach_requested = 1;
                continue;
            }
            output[used++] = '\034';
        } else if (byte == '\034') {
            *escape_pending = 1;
            continue;
        }
        output[used++] = (char)byte;
        if (used >= sizeof(output)) {
            int result = attachment_write_all(attachment, output, used);
            if (result != 0) {
                return result;
            }
            used = 0;
        }
    }
    if (used > 0) {
        return attachment_write_all(attachment, output, used);
    }
    return 0;
}

static int attachment_read_stream(cubicle_attachment_t *attachment,
                                  cubicle_stream_kind_t stream,
                                  FILE *output,
                                  int *end_of_stream)
{
    char buffer[8192];
    bool end = false;
    ssize_t nread = cubicle_attachment_read_stream(
        attachment, stream, buffer, sizeof(buffer), &end);
    if (nread < 0) {
        const cubicle_error_t *error = cubicle_attachment_last_error(attachment);
        if (error != NULL &&
            (error->code == CUBICLE_ERR_MANAGER_UNAVAILABLE ||
             error->code == CUBICLE_ERR_IO)) {
            return 1;
        }
        fprintf(stderr, "cube: %s\n",
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "attachment read failed");
        return 2;
    }
    if (nread > 0) {
        fwrite(buffer, 1, (size_t)nread, output);
        fflush(output);
    }
    *end_of_stream = end ? 1 : 0;
    return 0;
}

static int attachment_resize_tty(cubicle_attachment_t *attachment)
{
    if (!isatty(STDOUT_FILENO)) {
        return 0;
    }
    struct winsize size;
    memset(&size, 0, sizeof(size));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0 ||
        size.ws_row == 0 || size.ws_col == 0) {
        return 0;
    }
    cubicle_error_code_t code = cubicle_attachment_resize(
        attachment, (unsigned int)size.ws_row, (unsigned int)size.ws_col);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *error = cubicle_attachment_last_error(attachment);
        fprintf(stderr, "cube: %s\n",
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "attachment resize failed");
        return 2;
    }
    return 0;
}

static void drain_terminal_input_once(void)
{
    if (!isatty(STDIN_FILENO)) {
        return;
    }

    for (;;) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        int ready = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);
        if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) {
            break;
        }

        char discard[512];
        ssize_t nread = read(STDIN_FILENO, discard, sizeof(discard));
        if (nread <= 0) {
            break;
        }
    }
}

static int attachment_is_completed(cubicle_attachment_t *attachment,
                                   int *completed)
{
    cubicle_attachment_status_t status;
    memset(&status, 0, sizeof(status));
    cubicle_error_code_t code = cubicle_attachment_status(attachment, &status);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *error = cubicle_attachment_last_error(attachment);
        if (error != NULL &&
            (error->code == CUBICLE_ERR_MANAGER_UNAVAILABLE ||
             error->code == CUBICLE_ERR_IO)) {
            return 1;
        }
        fprintf(stderr, "cube: %s\n",
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "attachment status failed");
        return 2;
    }
    *completed = status.state == CUBICLE_PROCESS_COMPLETED ||
                 status.state == CUBICLE_PROCESS_FAILED ||
                 status.state == CUBICLE_PROCESS_LOST;
    return 0;
}

static int replay_terminal_output(cubicle_attachment_t *attachment,
                                  uint64_t replay_bytes)
{
    if (replay_bytes == 0) {
        return 0;
    }

    cubicle_attachment_status_t status;
    memset(&status, 0, sizeof(status));
    cubicle_error_code_t code = cubicle_attachment_status(attachment, &status);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *error = cubicle_attachment_last_error(attachment);
        fprintf(stderr, "cube: %s\n",
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "attachment status failed");
        return 2;
    }

    uint64_t replay_start = status.tty_offset > replay_bytes
                                ? status.tty_offset - replay_bytes
                                : 0;
    uint64_t remaining = status.tty_offset - replay_start;
    cubicle_attachment_replay(attachment, replay_bytes);

    while (remaining > 0) {
        char buffer[8192];
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer)
                                                  : (size_t)remaining;
        bool end_of_stream = false;
        ssize_t nread = cubicle_attachment_read_stream(
            attachment, CUBICLE_STREAM_TTY, buffer, chunk, &end_of_stream);
        if (nread < 0) {
            const cubicle_error_t *error = cubicle_attachment_last_error(attachment);
            fprintf(stderr, "cube: %s\n",
                    error != NULL && error->message[0] != '\0'
                        ? error->message
                        : "attachment replay failed");
            return 2;
        }
        if (nread == 0) {
            break;
        }
        fwrite(buffer, 1, (size_t)nread, stdout);
        fflush(stdout);
        remaining -= (uint64_t)nread;
        drain_terminal_input_once();
        if (end_of_stream) {
            break;
        }
    }

    drain_terminal_input_once();
    return 0;
}

static int render_terminal_snapshot(cubicle_attachment_t *attachment)
{
    cubicle_terminal_snapshot_t snapshot;
    cubicle_error_code_t code = cubicle_attachment_snapshot(attachment,
                                                            &snapshot);
    if (code != CUBICLE_OK) {
        return 1;
    }

    fputs("\x1b[H\x1b[2J", stdout);
    char active_sgr[96] = "";
    for (unsigned int row = 0; row < snapshot.rows; ++row) {
        fprintf(stdout, "\x1b[%u;1H", row + 1);
        for (unsigned int col = 0; col < snapshot.cols; ++col) {
            const cubicle_terminal_cell_t *cell =
                &snapshot.cells[(size_t)row * snapshot.cols + col];
            const char *sgr = cell->sgr;
            if (strcmp(active_sgr, sgr) != 0) {
                fputs(sgr[0] == '\0' ? "\x1b[0m" : sgr, stdout);
                snprintf(active_sgr, sizeof(active_sgr), "%s", sgr);
            }
            fputs(cell->text[0] == '\0' ? " " : cell->text, stdout);
        }
        if (active_sgr[0] != '\0') {
            fputs("\x1b[0m", stdout);
            active_sgr[0] = '\0';
        }
    }
    fprintf(stdout, "\x1b[0m\x1b[%u;%uH",
            snapshot.cursor_row + 1,
            snapshot.cursor_col + 1);
    if (snapshot.cursor_visible) {
        fputs("\x1b[?25h", stdout);
    } else {
        fputs("\x1b[?25l", stdout);
    }
    fflush(stdout);
    cubicle_terminal_snapshot_cleanup(&snapshot);
    return 0;
}

static int attachment_loop(cubicle_attachment_t *attachment,
                           unsigned int channels,
                           const char *process_name,
                           const char *mode,
                           int read_only,
                           uint64_t replay_bytes)
{
    int stdin_open = !read_only && (channels & CUBE_CHANNEL_STDIN) != 0;
    int stdin_is_tty = isatty(STDIN_FILENO);
    int detach_requested = 0;
    int escape_pending = 0;
    struct termios original;
    int raw_enabled = 0;
    int terminal_mode = process_mode_uses_terminal(mode);
    int interactive_tty = stdin_open && isatty(STDIN_FILENO) &&
                          isatty(STDOUT_FILENO);
    struct sigaction old_int;
    struct sigaction old_term;
    struct sigaction old_hup;
    struct sigaction old_quit;
    int handlers_installed = 0;

    if ((terminal_mode || interactive_tty) &&
        isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &original) < 0) {
            fprintf(stderr, "cube: failed to read terminal mode: %s\n",
                    strerror(errno));
            return 2;
        }
        struct termios raw = original;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        if (terminal_mode) {
            raw.c_oflag &= ~OPOST;
        }
        raw.c_cflag |= CS8;
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
            fprintf(stderr, "cube: failed to set terminal raw mode: %s\n",
                    strerror(errno));
            return 2;
        }
        cube_saved_terminal = original;
        cube_terminal_restore_active = 1;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = cube_attach_signal_handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &old_int) == 0 &&
            sigaction(SIGTERM, &action, &old_term) == 0 &&
            sigaction(SIGHUP, &action, &old_hup) == 0 &&
            sigaction(SIGQUIT, &action, &old_quit) == 0) {
            handlers_installed = 1;
        }
        raw_enabled = 1;
    }

    fprintf(stderr, "Connected to [%s]. Detach with Ctrl-\\ d\n",
            process_name);
    int result = 0;
    if (terminal_mode) {
        if (strcmp(mode, "term") == 0 &&
            (channels & CUBE_CHANNEL_STDERR) != 0) {
            cubicle_attachment_replay(attachment, replay_bytes);
        }
        result = render_terminal_snapshot(attachment);
        if (result != 0) {
            result = replay_terminal_output(attachment, replay_bytes);
        }
    } else {
        cubicle_attachment_replay(attachment, replay_bytes);
    }
    if (result == 0) {
        result = attachment_resize_tty(attachment);
    }

    while (result == 0 && !detach_requested) {
        int completed = 0;
        int stdout_end = 1;
        int stderr_end = 1;
        int tty_end = 1;

        if (terminal_mode &&
            (channels & (CUBE_CHANNEL_TTY | CUBE_CHANNEL_STDOUT)) != 0) {
            result = attachment_read_stream(attachment, CUBICLE_STREAM_TTY,
                                            stdout, &tty_end);
            if (result == 0 && strcmp(mode, "term") == 0 &&
                (channels & CUBE_CHANNEL_STDERR) != 0) {
                result = attachment_read_stream(attachment,
                                                CUBICLE_STREAM_STDERR,
                                                stderr, &stderr_end);
            }
        } else {
            if ((channels & CUBE_CHANNEL_STDOUT) != 0) {
                result = attachment_read_stream(attachment,
                                                CUBICLE_STREAM_STDOUT,
                                                stdout, &stdout_end);
            }
            if (result == 0 && (channels & CUBE_CHANNEL_STDERR) != 0) {
                result = attachment_read_stream(attachment,
                                                CUBICLE_STREAM_STDERR,
                                                stderr, &stderr_end);
            }
        }
        if (result == 1 && (!stdin_open || terminal_mode)) {
            result = 0;
            break;
        }
        if (result != 0) {
            if (result == 1) {
                fprintf(stderr, "cube: controller connection lost\n");
                result = 2;
            }
            break;
        }

        int status_result = attachment_is_completed(attachment, &completed);
        if (status_result == 1 &&
            ((stdout_end && stderr_end && tty_end && !stdin_open) ||
             terminal_mode)) {
            break;
        }
        if (status_result != 0) {
            if (status_result == 1) {
                fprintf(stderr, "cube: controller connection lost\n");
                result = 2;
            } else {
                result = status_result;
            }
            break;
        }
        if (completed && stdout_end && stderr_end && tty_end) {
            break;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &read_fds);
            max_fd = STDIN_FILENO;
        }
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = CUBE_ATTACH_POLL_MS * 1000;
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "cube: failed while waiting for attachment input: %s\n",
                    strerror(errno));
            result = 2;
            break;
        }
        if (stdin_open && ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
            char buffer[4096];
            ssize_t nread = read(STDIN_FILENO, buffer, sizeof(buffer) - 1);
            if (nread < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "cube: failed to read stdin: %s\n",
                        strerror(errno));
                result = 2;
                break;
            }
            if (nread == 0) {
                stdin_open = 0;
            } else {
                result = process_input_chunk(attachment, buffer,
                                             (size_t)nread, &escape_pending,
                                             &detach_requested);
                if (!stdin_is_tty) {
                    stdin_open = 0;
                }
            }
        }
    }

    if (raw_enabled) {
        cube_restore_terminal();
    }
    if (handlers_installed) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGHUP, &old_hup, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
    }
    return result;
}

static int attach_to_process_id(const char *manager_socket,
                                const char *process_id,
                                const char *process_name,
                                const char *mode,
                                int read_only,
                                uint64_t replay_bytes)
{
    unsigned int requested_channels = CUBE_CHANNEL_STDOUT | CUBE_CHANNEL_STDERR;
    if (process_mode_uses_terminal(mode)) {
        requested_channels = CUBE_CHANNEL_TTY | CUBE_CHANNEL_STDOUT;
        if (strcmp(mode, "term") == 0) {
            requested_channels |= CUBE_CHANNEL_STDERR;
        }
    }
    if (!read_only) {
        requested_channels |= CUBE_CHANNEL_STDIN;
    }

    cubicle_client_t *client = NULL;
    cubicle_error_code_t code = cubicle_client_connect_uri(manager_socket, NULL,
                                                           &client);
    if (code != CUBICLE_OK) {
        fprintf(stderr, "cube: failed to connect to manager\n");
        return 2;
    }

    cubicle_attachment_request_t request;
    memset(&request, 0, sizeof(request));
    request.process_id = process_id;
    request.channels = (cubicle_channel_mask_t)requested_channels;
    request.mode = read_only ? CUBICLE_ATTACHMENT_OBSERVER
                             : CUBICLE_ATTACHMENT_INTERACTIVE;

    cubicle_attachment_grant_t grant;
    memset(&grant, 0, sizeof(grant));
    code = cubicle_attachment_request(client, &request, &grant);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *error = cubicle_client_last_error(client);
        fprintf(stderr, "cube: %s\n",
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "attachment request failed");
        cubicle_client_disconnect(client);
        return 2;
    }
    cubicle_client_disconnect(client);

    cubicle_attachment_t *attachment = NULL;
    cubicle_attachment_options_t attachment_options;
    memset(&attachment_options, 0, sizeof(attachment_options));
    code = cubicle_attachment_connect(&grant, &attachment_options,
                                      &attachment);
    if (code != CUBICLE_OK) {
        fprintf(stderr, "cube: failed to attach to process\n");
        return 2;
    }

    unsigned int accepted_channels =
        (unsigned int)cubicle_attachment_channels(attachment);
    if (accepted_channels == 0) {
        fprintf(stderr,
                "cube: attachment grant did not include requested channels\n");
        cubicle_attachment_disconnect(attachment);
        return 2;
    }

    int result = attachment_loop(attachment, accepted_channels, process_name,
                                 mode, read_only, replay_bytes);
    (void)cubicle_attachment_detach(attachment);
    cubicle_attachment_disconnect(attachment);
    return result;
}

static int process_connect(const char *manager_socket,
                           const cube_options_t *options,
                           int argc,
                           char **argv,
                           int command_index)
{
    int read_only = 0;
    int argument_index = command_index + 1;
    while (argument_index < argc) {
        if (strcmp(argv[argument_index], "--ro") == 0) {
            read_only = 1;
            ++argument_index;
            continue;
        }
        if (argv[argument_index][0] == '-' &&
            argv[argument_index][1] == '-') {
            fprintf(stderr, "cube: unknown connect option '%s'\n",
                    argv[argument_index]);
            return 2;
        }
        break;
    }
    if (argument_index + 1 != argc) {
        fprintf(stderr, "cube: connect requires a process name\n");
        return 2;
    }

    char process_id[CUBICLE_ID_STRING_LENGTH];
    char mode[32];
    int resolve_result = resolve_process_metadata(
        manager_socket, options, argv[argument_index], process_id,
        sizeof(process_id), mode, sizeof(mode));
    if (resolve_result != 0) {
        return resolve_result;
    }

    return attach_to_process_id(manager_socket, process_id,
                                argv[argument_index], mode, read_only,
                                CUBE_CONNECT_REPLAY_BYTES);
}

static int wait_for_process(const char *manager_socket,
                            const char *process_id,
                            cube_rpc_response_t *response)
{
    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    snprintf(params, sizeof(params),
             "{\"process_id\":\"%s\",\"timeout_ms\":86400000}",
             escaped_process_id);
    if (call_manager(manager_socket, "process.wait", params, response) < 0) {
        return print_rpc_error(response);
    }
    return 0;
}

static int process_run(const char *manager_socket,
                       const cube_options_t *options,
                       const cubicle_config_t *config,
                       int argc,
                       char **argv,
                       int command_index)
{
    cube_run_options_t run_options = {
        .name = NULL,
        .mode = cube_mode_name(config->default_mode),
        .directory = NULL,
        .background = config->default_launch == CUBICLE_LAUNCH_BACKGROUND,
        .generated_name = 0,
    };

    int argument_index = command_index + 1;
    while (argument_index < argc) {
        const char *argument = argv[argument_index];
        if (strcmp(argument, "--") == 0) {
            ++argument_index;
            break;
        }
        if (strcmp(argument, "--fg") == 0) {
            run_options.background = 0;
            ++argument_index;
            continue;
        }
        if (strcmp(argument, "--bg") == 0) {
            run_options.background = 1;
            ++argument_index;
            continue;
        }
        if (strcmp(argument, "--stream") == 0) {
            run_options.mode = "stream";
            ++argument_index;
            continue;
        }
        if (strcmp(argument, "--tty") == 0) {
            run_options.mode = "tty";
            ++argument_index;
            continue;
        }
        if (strcmp(argument, "--term") == 0) {
            run_options.mode = "term";
            ++argument_index;
            continue;
        }
        if (strcmp(argument, "--name") == 0) {
            if (argument_index + 1 >= argc) {
                fprintf(stderr, "cube: --name requires a process name\n");
                return 2;
            }
            run_options.name = argv[argument_index + 1];
            argument_index += 2;
            continue;
        }
        if (strcmp(argument, "--dir") == 0) {
            if (argument_index + 1 >= argc) {
                fprintf(stderr, "cube: --dir requires a directory\n");
                return 2;
            }
            run_options.directory = argv[argument_index + 1];
            argument_index += 2;
            continue;
        }
        if (argument[0] == '-' && argument[1] == '-') {
            fprintf(stderr, "cube: unknown run option '%s'\n", argument);
            return 2;
        }
        break;
    }

    if (argument_index >= argc) {
        fprintf(stderr, "cube: run requires a command\n");
        return 2;
    }

    char generated_name[CUBICLE_NAME_MAX];
    if (run_options.name == NULL) {
        if (generated_process_name(generated_name, sizeof(generated_name),
                                   argv[argument_index]) < 0) {
            fprintf(stderr, "cube: failed to generate process name\n");
            return 2;
        }
        run_options.name = generated_name;
        run_options.generated_name = 1;
    }
    if (!valid_name(run_options.name)) {
        fprintf(stderr, "cube: invalid process name\n");
        return 2;
    }
    char workspace[CUBICLE_NAME_MAX];
    int from_selected_workspace = 0;
    if (resolve_workspace_selection(options, workspace, sizeof(workspace),
                                    &from_selected_workspace) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }

    char workspace_directory[CUBICLE_PATH_MAX];
    int workspace_result = fetch_workspace_directory(manager_socket, workspace,
                                                     workspace_directory,
                                                     sizeof(workspace_directory),
                                                     from_selected_workspace);
    if (workspace_result != 0) {
        return workspace_result;
    }

    char run_directory[CUBICLE_PATH_MAX];
    if (resolve_directory_path(run_options.directory != NULL ?
                               run_options.directory : workspace_directory,
                               run_directory) < 0 ||
        !valid_directory_field(run_directory)) {
        fprintf(stderr, "cube: invalid process directory: %s\n",
                run_options.directory == NULL ? workspace_directory :
                                                run_options.directory);
        return 2;
    }
    run_options.directory = run_directory;

    int command_argc = argc - argument_index;

    cube_rpc_response_t start_response;
    memset(&start_response, 0, sizeof(start_response));
    char base_name[CUBICLE_NAME_MAX];
    int base_name_length = snprintf(base_name, sizeof(base_name), "%s",
                                    run_options.name);
    if (base_name_length < 0 ||
        (size_t)base_name_length >= sizeof(base_name)) {
        fprintf(stderr, "cube: process name is too long\n");
        return 2;
    }
    int started = 0;
    for (int suffix = 0; suffix < 1000; ++suffix) {
        char candidate_name[CUBICLE_NAME_MAX];
        if (run_options.generated_name &&
            generated_process_name_with_suffix(candidate_name,
                                               sizeof(candidate_name),
                                               base_name, suffix) < 0) {
            continue;
        }
        if (run_options.generated_name) {
            run_options.name = candidate_name;
        }

        cubicle_json_builder_t params = {0};
        if (build_process_start_params(&params, workspace, &run_options,
                                       command_argc,
                                       &argv[argument_index]) < 0) {
            cubicle_json_builder_cleanup(&params);
            fprintf(stderr, "cube: failed to encode process start request\n");
            return 2;
        }

        if (call_manager(manager_socket, "process.start", params.data,
                         &start_response) == 0) {
            cubicle_json_builder_cleanup(&params);
            started = 1;
            break;
        }
        cubicle_json_builder_cleanup(&params);
        if (!run_options.generated_name ||
            start_response.code != CUBICLE_ERR_ALREADY_EXISTS) {
            return print_rpc_error(&start_response);
        }
    }
    if (!started) {
        fprintf(stderr, "cube: failed to allocate a unique process name\n");
        return 2;
    }

    cubicle_json_doc_t start_document;
    if (cubicle_json_parse(&start_document, start_response.result_json) < 0) {
        cleanup_rpc_response(&start_response);
        fprintf(stderr, "cube: invalid process start response\n");
        return 2;
    }

    char process_id[CUBICLE_ID_STRING_LENGTH];
    char process_name[CUBICLE_NAME_MAX];
    char mode[32];
    if (json_string_field(start_document.root, "id", process_id,
                          sizeof(process_id)) < 0 ||
        json_string_field(start_document.root, "friendly_name", process_name,
                          sizeof(process_name)) < 0 ||
        json_string_field(start_document.root, "mode", mode, sizeof(mode)) < 0) {
        cubicle_json_cleanup(&start_document);
        cleanup_rpc_response(&start_response);
        fprintf(stderr, "cube: invalid process start response\n");
        return 2;
    }
    cubicle_json_cleanup(&start_document);

    if (run_options.background) {
        if (options->json) {
            printf("%s\n", start_response.result_json);
        } else {
            printf("[%s] started in %s mode\n", process_name, mode);
        }
        cleanup_rpc_response(&start_response);
        return 0;
    }
    cleanup_rpc_response(&start_response);

    if (process_mode_uses_terminal(mode)) {
        return attach_to_process_id(manager_socket, process_id, process_name,
                                    mode, 0, UINT64_MAX);
    }

    cube_rpc_response_t wait_response;
    int wait_result = wait_for_process(manager_socket, process_id,
                                       &wait_response);
    if (wait_result != 0) {
        return wait_result;
    }

    int stdout_result = read_process_output(manager_socket, process_id,
                                            "stdout", 0, 0, 0, stdout);
    int stderr_result = read_process_output(manager_socket, process_id,
                                            "stderr", 0, 0, 0, stderr);
    if (stdout_result != 0 || stderr_result != 0) {
        cleanup_rpc_response(&wait_response);
        return stdout_result != 0 ? stdout_result : stderr_result;
    }

    int exit_code = 0;
    int has_exit_code = 0;
    int has_exit_status = 0;
    cubicle_json_doc_t wait_document;
    if (cubicle_json_parse(&wait_document, wait_response.result_json) < 0) {
        cleanup_rpc_response(&wait_response);
        fprintf(stderr, "cube: invalid process wait response\n");
        return 2;
    }
    has_exit_code = json_int_field(wait_document.root, "exit_code",
                                   &exit_code) == 0;
    if (json_bool_field(wait_document.root, "has_exit_status",
                        &has_exit_status) == 0 &&
        has_exit_status && has_exit_code) {
        cubicle_json_cleanup(&wait_document);
        cleanup_rpc_response(&wait_response);
        return exit_code >= 0 && exit_code <= 255 ? exit_code : 1;
    }
    cubicle_json_cleanup(&wait_document);
    cleanup_rpc_response(&wait_response);
    return 0;
}

int main(int argc, char **argv)
{
    cube_options_t options;
    int command_index = 0;
    int parse_result = parse_global_options(argc, argv, &options,
                                            &command_index);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        return 2;
    }

    if (command_index >= argc) {
        print_usage(stderr);
        return 2;
    }

    const char *command = argv[command_index];
    if (strcmp(command, "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (command_index + 2 == argc &&
        (strcmp(argv[command_index + 1], "--help") == 0 ||
         strcmp(argv[command_index + 1], "-h") == 0)) {
        if (print_command_usage(command, stdout) == 0) {
            return 0;
        }
        fprintf(stderr, "cube: unknown command '%s'\n", command);
        return 2;
    }

    cubicle_config_t config;
    char config_error[512] = "";
    int config_loaded = cubicle_config_load_client(&config, config_error,
                                                   sizeof(config_error)) == 0;
    if (!config_loaded) {
        cubicle_config_defaults(&config);
    }

    if (strcmp(command, "config") == 0) {
        return command_config(&config, config_error, argc, argv,
                              command_index);
    }

    if (strcmp(command, "defaults") == 0) {
        if (!config_loaded) {
            fprintf(stderr, "cube: configuration error: %s\n", config_error);
            return 2;
        }
        return command_defaults(&options, &config, argc, argv, command_index);
    }

    if (!config_loaded) {
        fprintf(stderr, "cube: configuration error: %s\n", config_error);
        return 2;
    }
    configure_library_debug(&config, "cube");

    if (!command_requires_manager(command)) {
        fprintf(stderr, "cube: unknown command '%s'\n", command);
        return 2;
    }

    char configured_endpoint[CUBICLE_ENDPOINT_URI_MAX];
    const char *manager_endpoint = cubeui_resolve_manager_endpoint(
        options.manager_socket, &config, configured_endpoint,
        sizeof(configured_endpoint));
    if (manager_endpoint == NULL) {
        fprintf(stderr, "cube: manager endpoint is not configured\n");
        fprintf(stderr,
                "hint: pass --manager-socket PATH, set CUBICLE_MANAGER_SOCKET, or configure client.manager\n");
        return 2;
    }

    if (strcmp(command, "workspace") == 0) {
        return command_workspace(manager_endpoint, &options, argc, argv,
                                 command_index);
    }

    if (strcmp(command, "run") == 0) {
        return process_run(manager_endpoint, &options, &config, argc, argv,
                           command_index);
    }

    if (strcmp(command, "ps") == 0) {
        return process_list(manager_endpoint, &options, argc, argv,
                            command_index);
    }

    if (strcmp(command, "cleanup") == 0) {
        if (command_index + 1 != argc) {
            fprintf(stderr, "cube: cleanup does not take arguments\n");
            return 2;
        }
        return process_cleanup(manager_endpoint, &options);
    }

    if (strcmp(command, "access") == 0) {
        return command_access(manager_endpoint, &options, argc, argv,
                              command_index);
    }

    if (strcmp(command, "inspect") == 0) {
        if (command_index + 2 != argc) {
            fprintf(stderr, "cube: inspect requires a process name\n");
            return 2;
        }
        return process_inspect(manager_endpoint, &options,
                               argv[command_index + 1]);
    }

    if (strcmp(command, "logs") == 0) {
        return process_logs(manager_endpoint, &options, argc, argv,
                            command_index);
    }

    if (strcmp(command, "connect") == 0) {
        return process_connect(manager_endpoint, &options, argc, argv,
                               command_index);
    }

    if (strcmp(command, "events") == 0) {
        return process_events(manager_endpoint, &options, argc, argv,
                              command_index);
    }

    if (strcmp(command, "signal") == 0) {
        if (command_index + 3 != argc) {
            fprintf(stderr, "cube: signal requires a process name and signal\n");
            return 2;
        }
        int signal_number = 0;
        if (signal_number_for_name(argv[command_index + 2],
                                   &signal_number) < 0) {
            fprintf(stderr, "cube: invalid signal\n");
            return 2;
        }
        return process_lifecycle_action(manager_endpoint, &options,
                                        argv[command_index + 1],
                                        "process.signal", "signaled",
                                        signal_number);
    }

    if (strcmp(command, "stop") == 0) {
        if (command_index + 2 != argc) {
            fprintf(stderr, "cube: stop requires a process name\n");
            return 2;
        }
        return process_lifecycle_action(manager_endpoint, &options,
                                        argv[command_index + 1],
                                        "process.terminate", "stopped", 0);
    }

    if (strcmp(command, "kill") == 0) {
        return process_kill_command(manager_endpoint, &options, &config, argc,
                                    argv, command_index);
    }

    if (strcmp(command, "save") == 0 || strcmp(command, "unsave") == 0) {
        if (command_index + 2 != argc) {
            fprintf(stderr, "cube: %s requires a process name\n", command);
            return 2;
        }
        return process_save_command(manager_endpoint, &options,
                                    argv[command_index + 1],
                                    strcmp(command, "save") == 0);
    }

    if (strcmp(command, "remove") == 0) {
        if (command_index + 2 != argc) {
            fprintf(stderr, "cube: remove requires a process name\n");
            return 2;
        }
        return process_lifecycle_action(manager_endpoint, &options,
                                        argv[command_index + 1],
                                        "process.remove", "removed", 0);
    }

    (void)options.json;
    fprintf(stderr, "cube: command '%s' is not implemented yet\n", command);
    return 2;
}
