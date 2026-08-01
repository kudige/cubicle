#define _POSIX_C_SOURCE 200809L

#include "../common/json.h"
#include "../common/rpc_internal.h"

#include "cubicle/rpc.h"
#include "cubicle/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

    (void)options.json;
    fprintf(stderr, "cube: command '%s' is not implemented yet\n", command);
    return 2;
}
