#define _POSIX_C_SOURCE 200809L

#include "../common/json.h"
#include "../common/rpc_internal.h"

#include "cubicle/rpc.h"
#include "cubicle/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX CUBICLE_PATH_MAX
#endif

#define CUBE_MAX_FRAME 65536

typedef struct cube_options {
    const char *manager_socket;
    const char *workspace;
    int json;
} cube_options_t;

typedef struct cube_run_options {
    const char *name;
    const char *mode;
    int background;
    int generated_name;
} cube_run_options_t;

typedef struct cube_rpc_response {
    cubicle_error_code_t code;
    char *result_json;
    char error_message[CUBICLE_ERROR_MESSAGE_MAX];
} cube_rpc_response_t;

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  cube [--manager-socket PATH] [--workspace NAME] [--json] COMMAND [ARG...]\n"
            "  cube workspace NAME\n"
            "  cube run [--fg|--bg] [--stream|--tty] [--name NAME] COMMAND [ARG...]\n"
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
           strcmp(command, "run") == 0 ||
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

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t written = send(fd, cursor, length, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            errno = EPIPE;
            return -1;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t nread = recv(fd, cursor, length, 0);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nread == 0) {
            errno = ECONNRESET;
            return -1;
        }
        cursor += (size_t)nread;
        length -= (size_t)nread;
    }
    return 0;
}

static int call_manager(const char *socket_path,
                        const char *method,
                        const char *params,
                        cube_rpc_response_t *response)
{
    memset(response, 0, sizeof(*response));

    char request[8192];
    if (cubicle_rpc_request(request, sizeof(request), "cube-1",
                            "local-session", method, params) < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to encode request");
        response->code = CUBICLE_ERR_INTERNAL;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to create socket");
        response->code = CUBICLE_ERR_IO;
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        close(fd);
        snprintf(response->error_message, sizeof(response->error_message),
                 "manager socket path is too long");
        response->code = CUBICLE_ERR_INVALID_ARGUMENT;
        return -1;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to connect to manager");
        response->code = CUBICLE_ERR_MANAGER_UNAVAILABLE;
        return -1;
    }

    uint32_t request_length = htonl((uint32_t)strlen(request));
    if (write_all(fd, &request_length, sizeof(request_length)) < 0 ||
        write_all(fd, request, strlen(request)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to write manager request");
        response->code = CUBICLE_ERR_IO;
        return -1;
    }

    uint32_t response_length_network = 0;
    if (read_all(fd, &response_length_network,
                 sizeof(response_length_network)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to read manager response");
        response->code = CUBICLE_ERR_IO;
        return -1;
    }
    uint32_t response_length = ntohl(response_length_network);
    if (response_length == 0 || response_length > CUBE_MAX_FRAME) {
        close(fd);
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid manager response length");
        response->code = CUBICLE_ERR_PROTOCOL;
        return -1;
    }

    char *response_json = calloc((size_t)response_length + 1, 1);
    if (response_json == NULL) {
        close(fd);
        response->code = CUBICLE_ERR_INTERNAL;
        return -1;
    }
    if (read_all(fd, response_json, response_length) < 0) {
        int saved_errno = errno;
        free(response_json);
        close(fd);
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to read manager response");
        response->code = CUBICLE_ERR_IO;
        return -1;
    }
    close(fd);

    cubicle_rpc_response_envelope_t envelope;
    if (cubicle_rpc_decode_response(&envelope, response_json, "cube-1") < 0) {
        free(response_json);
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid manager response");
        response->code = CUBICLE_ERR_PROTOCOL;
        return -1;
    }

    if (!envelope.success) {
        cubicle_error_t error;
        if (cubicle_rpc_decode_error_value(envelope.error, &error) == 0) {
            response->code = error.code;
            snprintf(response->error_message, sizeof(response->error_message),
                     "%s", error.message);
        } else {
            response->code = CUBICLE_ERR_PROTOCOL;
            snprintf(response->error_message, sizeof(response->error_message),
                     "invalid manager error response");
        }
        cubicle_rpc_response_envelope_cleanup(&envelope);
        free(response_json);
        return -1;
    }

    response->result_json = yyjson_val_write(envelope.result, 0, NULL);
    cubicle_rpc_response_envelope_cleanup(&envelope);
    free(response_json);
    if (response->result_json == NULL) {
        response->code = CUBICLE_ERR_INTERNAL;
        return -1;
    }
    response->code = CUBICLE_OK;
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
            response->error_message[0] == '\0' ? "manager request failed"
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

static int selected_workspace_path(char path[PATH_MAX])
{
    const char *state_home = getenv("XDG_STATE_HOME");
    if (state_home != NULL && state_home[0] != '\0') {
        int length = snprintf(path, PATH_MAX,
                              "%s/cubicle/current-workspace", state_home);
        return length < 0 || length >= PATH_MAX ? -1 : 0;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        int length = snprintf(path, PATH_MAX, ".cubicle/current-workspace");
        return length < 0 || length >= PATH_MAX ? -1 : 0;
    }
    int length = snprintf(path, PATH_MAX,
                          "%s/.local/state/cubicle/current-workspace", home);
    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static int ensure_parent_dir(const char *path)
{
    char parent[PATH_MAX];
    int length = snprintf(parent, sizeof(parent), "%s", path);
    if (length < 0 || (size_t)length >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (slash == NULL) {
        return 0;
    }
    *slash = '\0';
    return cubicle_mkdir_p(parent);
}

static int store_selected_workspace(const char *workspace_name)
{
    char path[PATH_MAX];
    if (selected_workspace_path(path) < 0 || ensure_parent_dir(path) < 0) {
        return -1;
    }

    char line[CUBICLE_NAME_MAX + 2];
    int length = snprintf(line, sizeof(line), "%s\n", workspace_name);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    int result = cubicle_write_all(fd, line, strlen(line));
    close(fd);
    return result;
}

static int read_selected_workspace(char *buffer, size_t buffer_size)
{
    char path[PATH_MAX];
    if (selected_workspace_path(path) < 0) {
        return -1;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    if (fgets(buffer, (int)buffer_size, file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);
    buffer[strcspn(buffer, "\n")] = '\0';
    return buffer[0] == '\0' ? -1 : 0;
}

static int valid_name(const char *name)
{
    return name != NULL && name[0] != '\0' &&
           strchr(name, '\t') == NULL && strchr(name, '\n') == NULL;
}

static int workspace_create_or_select(const char *manager_socket,
                                      const char *name)
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

    char params[2048];
    snprintf(params, sizeof(params), "{\"workspace\":\"%s\"}", escaped_name);
    cube_rpc_response_t response;
    int selected_existing = call_manager(manager_socket, "workspace.get",
                                         params, &response) == 0;
    cleanup_rpc_response(&response);

    if (!selected_existing) {
        snprintf(params, sizeof(params), "{\"name\":\"%s\"}", escaped_name);
        if (call_manager(manager_socket, "workspace.create", params,
                         &response) < 0) {
            return print_rpc_error(&response);
        }
        cleanup_rpc_response(&response);
    }

    if (store_selected_workspace(name) < 0) {
        fprintf(stderr, "cube: failed to persist selected workspace: %s\n",
                strerror(errno));
        return 2;
    }

    printf("Workspace %s %s\n", name,
           selected_existing ? "selected" : "created and selected");
    return 0;
}

static int workspace_list(const char *manager_socket)
{
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "workspace.list", "{}",
                     &response) < 0) {
        return print_rpc_error(&response);
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

static int workspace_show_current(void)
{
    char workspace[CUBICLE_NAME_MAX];
    if (read_selected_workspace(workspace, sizeof(workspace)) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }
    printf("Workspace %s selected\n", workspace);
    return 0;
}

static int workspace_simple_action(const char *manager_socket,
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

    char params[1024];
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
    cleanup_rpc_response(&response);
    printf("Workspace %s %s\n", workspace,
           strcmp(method, "workspace.stop") == 0 ? "stopped" : "deleted");
    return 0;
}

static int resolve_workspace_argument(const cube_options_t *options,
                                      char *workspace,
                                      size_t workspace_size)
{
    if (options->workspace != NULL && options->workspace[0] != '\0') {
        int length = snprintf(workspace, workspace_size, "%s",
                              options->workspace);
        return length < 0 || (size_t)length >= workspace_size ? -1 : 0;
    }
    return read_selected_workspace(workspace, workspace_size);
}

static int command_workspace(const char *manager_socket,
                             int argc,
                             char **argv,
                             int command_index)
{
    int remaining = argc - command_index - 1;
    char **arguments = &argv[command_index + 1];
    if (remaining == 0) {
        return workspace_show_current();
    }
    if (remaining == 1 && strcmp(arguments[0], "list") == 0) {
        return workspace_list(manager_socket);
    }
    if (remaining == 2 && strcmp(arguments[0], "create") == 0) {
        return workspace_create_or_select(manager_socket, arguments[1]);
    }
    if (remaining == 2 && strcmp(arguments[0], "select") == 0) {
        return workspace_create_or_select(manager_socket, arguments[1]);
    }
    if (remaining == 2 && strcmp(arguments[0], "stop") == 0) {
        return workspace_simple_action(manager_socket, "workspace.stop",
                                       arguments[1]);
    }
    if (remaining == 2 && strcmp(arguments[0], "delete") == 0) {
        return workspace_simple_action(manager_socket, "workspace.delete",
                                       arguments[1]);
    }
    if (remaining == 1) {
        return workspace_create_or_select(manager_socket, arguments[0]);
    }

    fprintf(stderr, "cube: invalid workspace command\n");
    return 2;
}

static int process_list(const char *manager_socket,
                        const cube_options_t *options)
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

    if (options->json) {
        printf("%s\n", response.result_json);
        cleanup_rpc_response(&response);
        return 0;
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
    cleanup_rpc_response(&response);
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
    if (json_string_field(document.root, "id", id, sizeof(id)) < 0 ||
        json_string_field(document.root, "friendly_name", name,
                          sizeof(name)) < 0 ||
        json_string_field(document.root, "mode", mode, sizeof(mode)) < 0 ||
        json_string_field(document.root, "state", state, sizeof(state)) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid process response\n");
        return 2;
    }

    printf("Name:        %s\n", name);
    printf("Workspace:   %s\n", workspace);
    printf("Mode:        %s\n", mode);
    printf("State:       %s\n", state);
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

static int resolve_process_id(const char *manager_socket,
                              const cube_options_t *options,
                              const char *process_name,
                              char *process_id,
                              size_t process_id_size)
{
    char workspace[CUBICLE_NAME_MAX];
    int has_workspace = resolve_workspace_argument(options, workspace,
                                                   sizeof(workspace)) == 0;
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

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
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
            params, run_options->background ? "open" : "eof") < 0 ||
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

static int read_process_output(const char *manager_socket,
                               const char *process_id,
                               const char *stream,
                               FILE *output)
{
    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[512];
    snprintf(params, sizeof(params),
             "{\"process_id\":\"%s\",\"stream\":\"%s\",\"offset\":0,\"maximum_length\":8192}",
             escaped_process_id, stream);

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
    if (!yyjson_is_str(data)) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid output response\n");
        return 2;
    }
    fputs(yyjson_get_str(data), output);

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
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
                       int argc,
                       char **argv,
                       int command_index)
{
    cube_run_options_t run_options = {
        .name = NULL,
        .mode = "stream",
        .background = 0,
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
            fprintf(stderr, "cube: term mode is not implemented yet\n");
            return 2;
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
    if (!run_options.background && strcmp(run_options.mode, "tty") == 0) {
        fprintf(stderr,
                "cube: foreground tty attach is not implemented yet; use --bg --tty or --stream\n");
        return 2;
    }

    char workspace[CUBICLE_NAME_MAX];
    if (resolve_workspace_argument(options, workspace, sizeof(workspace)) < 0) {
        fprintf(stderr, "cube: no workspace selected\n");
        return 1;
    }

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

    cube_rpc_response_t wait_response;
    int wait_result = wait_for_process(manager_socket, process_id,
                                       &wait_response);
    if (wait_result != 0) {
        return wait_result;
    }

    int stdout_result = read_process_output(manager_socket, process_id,
                                            "stdout", stdout);
    int stderr_result = read_process_output(manager_socket, process_id,
                                            "stderr", stderr);
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

    if (strcmp(command, "workspace") == 0) {
        return command_workspace(manager_socket, argc, argv, command_index);
    }

    if (strcmp(command, "run") == 0) {
        return process_run(manager_socket, &options, argc, argv,
                           command_index);
    }

    if (strcmp(command, "ps") == 0) {
        return process_list(manager_socket, &options);
    }

    if (strcmp(command, "inspect") == 0) {
        if (command_index + 2 != argc) {
            fprintf(stderr, "cube: inspect requires a process name\n");
            return 2;
        }
        return process_inspect(manager_socket, &options,
                               argv[command_index + 1]);
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
        return process_lifecycle_action(manager_socket, &options,
                                        argv[command_index + 1],
                                        "process.signal", "signaled",
                                        signal_number);
    }

    if (strcmp(command, "stop") == 0) {
        if (command_index + 2 != argc) {
            fprintf(stderr, "cube: stop requires a process name\n");
            return 2;
        }
        return process_lifecycle_action(manager_socket, &options,
                                        argv[command_index + 1],
                                        "process.terminate", "stopped", 0);
    }

    if (strcmp(command, "kill") == 0) {
        if (command_index + 2 != argc) {
            fprintf(stderr, "cube: kill requires a process name\n");
            return 2;
        }
        return process_lifecycle_action(manager_socket, &options,
                                        argv[command_index + 1],
                                        "process.kill", "killed", 0);
    }

    if (strcmp(command, "remove") == 0) {
        if (command_index + 2 != argc) {
            fprintf(stderr, "cube: remove requires a process name\n");
            return 2;
        }
        return process_lifecycle_action(manager_socket, &options,
                                        argv[command_index + 1],
                                        "process.remove", "removed", 0);
    }

    (void)options.json;
    fprintf(stderr, "cube: command '%s' is not implemented yet\n", command);
    return 2;
}
