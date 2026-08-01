#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cube_options {
    const char *manager_socket;
    const char *workspace;
    int json;
} cube_options_t;

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  cube [--manager-socket PATH] [--workspace NAME] [--json] COMMAND [ARG...]\n"
            "  cube workspace NAME\n"
            "  cube ps\n"
            "  cube connect [--ro] NAME\n"
            "  cube stop NAME\n"
            "\n"
            "Run and reconnect to persistent processes inside Cubicle workspaces.\n");
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
           strcmp(command, "ps") == 0 ||
           strcmp(command, "inspect") == 0 ||
           strcmp(command, "connect") == 0 ||
           strcmp(command, "signal") == 0 ||
           strcmp(command, "stop") == 0 ||
           strcmp(command, "kill") == 0 ||
           strcmp(command, "remove") == 0 ||
           strcmp(command, "logs") == 0 ||
           strcmp(command, "events") == 0 ||
           strcmp(command, "defaults") == 0;
}

static const char *resolve_manager_socket(const cube_options_t *options)
{
    if (options->manager_socket != NULL &&
        options->manager_socket[0] != '\0') {
        return options->manager_socket;
    }
    const char *environment = getenv("CUBICLE_MANAGER_SOCKET");
    return environment != NULL && environment[0] != '\0' ? environment : NULL;
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

    if (!command_requires_manager(command)) {
        fprintf(stderr, "cube: unknown command '%s'\n", command);
        return 2;
    }

    const char *manager_socket = resolve_manager_socket(&options);
    if (manager_socket == NULL) {
        fprintf(stderr, "cube: manager socket is not configured\n");
        fprintf(stderr,
                "hint: pass --manager-socket PATH or set CUBICLE_MANAGER_SOCKET\n");
        return 2;
    }

    (void)manager_socket;
    (void)options.workspace;
    (void)options.json;
    fprintf(stderr, "cube: command '%s' is not implemented yet\n", command);
    return 2;
}
