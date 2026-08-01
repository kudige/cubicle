#define _POSIX_C_SOURCE 200809L

#include "../common/json.h"
#include "../common/rpc_internal.h"

#include "cubicle/rpc.h"
#include "cubicle/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX CUBICLE_PATH_MAX
#endif

#define CUBE_MAX_FRAME 65536
#define CUBE_CHANNEL_STDIN 1U
#define CUBE_CHANNEL_STDOUT 2U
#define CUBE_CHANNEL_STDERR 4U
#define CUBE_CHANNEL_TTY 8U
#define CUBE_ATTACH_POLL_MS 50

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

typedef struct cube_attach_grant {
    char endpoint_uri[CUBICLE_ENDPOINT_URI_MAX];
    char token[CUBICLE_TOKEN_MAX];
    unsigned int channels;
} cube_attach_grant_t;

typedef struct cube_attach_offsets {
    uint64_t stdout_offset;
    uint64_t stderr_offset;
    uint64_t tty_offset;
} cube_attach_offsets_t;

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  cube [--manager-socket PATH] [--workspace NAME] [--json] COMMAND [ARG...]\n"
            "  cube workspace NAME\n"
            "  cube run [--fg|--bg] [--stream|--tty] [--name NAME] COMMAND [ARG...]\n"
            "  cube ps\n"
            "  cube logs NAME\n"
            "  cube events\n"
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

static int call_rpc_peer(const char *peer_name,
                         const char *socket_path,
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
                 "%s socket path is too long", peer_name);
        response->code = CUBICLE_ERR_INVALID_ARGUMENT;
        return -1;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to connect to %s", peer_name);
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
                 "failed to write %s request", peer_name);
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
                 "failed to read %s response from %s: %s",
                 peer_name, socket_path, strerror(saved_errno));
        response->code = CUBICLE_ERR_IO;
        return -1;
    }
    uint32_t response_length = ntohl(response_length_network);
    if (response_length == 0 || response_length > CUBE_MAX_FRAME) {
        close(fd);
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid %s response length", peer_name);
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
                 "failed to read %s response from %s: %s",
                 peer_name, socket_path, strerror(saved_errno));
        response->code = CUBICLE_ERR_IO;
        return -1;
    }
    close(fd);

    cubicle_rpc_response_envelope_t envelope;
    if (cubicle_rpc_decode_response(&envelope, response_json, "cube-1") < 0) {
        free(response_json);
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid %s response", peer_name);
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
                     "invalid %s error response", peer_name);
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

static int call_manager(const char *socket_path,
                        const char *method,
                        const char *params,
                        cube_rpc_response_t *response)
{
    return call_rpc_peer("manager", socket_path, method, params, response);
}

static int call_controller(const char *socket_path,
                           const char *method,
                           const char *params,
                           cube_rpc_response_t *response)
{
    return call_rpc_peer("controller", socket_path, method, params, response);
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
                                      const cube_options_t *options,
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

    if (!selected_existing) {
        cleanup_rpc_response(&response);
        snprintf(params, sizeof(params), "{\"name\":\"%s\"}", escaped_name);
        if (call_manager(manager_socket, "workspace.create", params,
                         &response) < 0) {
            return print_rpc_error(&response);
        }
    }

    if (store_selected_workspace(name) < 0) {
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
    if (read_selected_workspace(workspace, sizeof(workspace)) < 0) {
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
    if (remaining == 2 && strcmp(arguments[0], "create") == 0) {
        return workspace_create_or_select(manager_socket, options,
                                          arguments[1]);
    }
    if (remaining == 2 && strcmp(arguments[0], "select") == 0) {
        return workspace_create_or_select(manager_socket, options,
                                          arguments[1]);
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
                                          arguments[0]);
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

static int resolve_process_metadata(const char *manager_socket,
                                    const cube_options_t *options,
                                    const char *process_name,
                                    char *process_id,
                                    size_t process_id_size,
                                    char *mode,
                                    size_t mode_size)
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
                    strcmp(run_options->mode, "tty") == 0
                ? "open"
                : "eof") < 0 ||
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

    uint64_t offset = 0;
    for (;;) {
        char params[512];
        snprintf(params, sizeof(params),
                 "{\"process_id\":\"%s\",\"stream\":\"%s\",\"offset\":%llu,\"maximum_length\":8192}",
                 escaped_process_id, stream,
                 (unsigned long long)offset);

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
        int end_of_stream = 0;
        if (!yyjson_is_str(data) ||
            json_u64_field(document.root, "next_offset",
                           &next_offset) < 0 ||
            json_bool_field(document.root, "end_of_stream",
                            &end_of_stream) < 0 ||
            next_offset < offset) {
            cubicle_json_cleanup(&document);
            cleanup_rpc_response(&response);
            fprintf(stderr, "cube: invalid output response\n");
            return 2;
        }
        fputs(yyjson_get_str(data), output);

        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        if (end_of_stream) {
            return 0;
        }
        if (next_offset == offset) {
            fprintf(stderr, "cube: output stream did not advance\n");
            return 2;
        }
        offset = next_offset;
    }
}

static int process_logs(const char *manager_socket,
                        const cube_options_t *options,
                        int argc,
                        char **argv,
                        int command_index)
{
    int argument_index = command_index + 1;
    int follow = 0;
    while (argument_index < argc) {
        if (strcmp(argv[argument_index], "--follow") == 0) {
            follow = 1;
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
    if (follow) {
        fprintf(stderr, "cube: logs --follow is not implemented yet\n");
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

    if (strcmp(mode, "tty") == 0) {
        return read_process_output(manager_socket, process_id, "tty", stdout);
    }

    int stdout_result = read_process_output(manager_socket, process_id,
                                            "stdout", stdout);
    int stderr_result = read_process_output(manager_socket, process_id,
                                            "stderr", stderr);
    return stdout_result != 0 ? stdout_result : stderr_result;
}

static int process_events(const char *manager_socket,
                          const cube_options_t *options,
                          int argc,
                          char **argv,
                          int command_index)
{
    int argument_index = command_index + 1;
    int follow = 0;
    while (argument_index < argc) {
        if (strcmp(argv[argument_index], "--follow") == 0) {
            follow = 1;
            ++argument_index;
            continue;
        }
        fprintf(stderr, "cube: unknown events option '%s'\n",
                argv[argument_index]);
        return 2;
    }
    if (follow) {
        fprintf(stderr, "cube: events --follow is not implemented yet\n");
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

    char params[1024];
    snprintf(params, sizeof(params),
             "{\"workspace_id\":\"%s\",\"after_sequence\":0,\"limit\":100}",
             escaped_workspace);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "events.list", params, &response) < 0) {
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
        fprintf(stderr, "cube: invalid events response\n");
        return 2;
    }

    yyjson_val *events = yyjson_obj_get(document.root, "events");
    if (!yyjson_is_arr(events)) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid events response\n");
        return 2;
    }

    printf("Workspace %s\n\n", workspace);
    printf("SEQ\tPROCESS\tTYPE\tPAYLOAD\n");
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
        }
    }

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int unix_socket_path_from_uri(const char *uri,
                                     char *path,
                                     size_t path_size)
{
    const char *prefix = "unix://";
    size_t prefix_length = strlen(prefix);
    if (strncmp(uri, prefix, prefix_length) != 0 ||
        uri[prefix_length] == '\0') {
        return -1;
    }
    int length = snprintf(path, path_size, "%s", uri + prefix_length);
    return length < 0 || (size_t)length >= path_size ? -1 : 0;
}

static int channel_mask_from_string(const char *text, unsigned int *channels)
{
    unsigned int mask = 0;
    if (strstr(text, "stdin") != NULL) {
        mask |= CUBE_CHANNEL_STDIN;
    }
    if (strstr(text, "stdout") != NULL) {
        mask |= CUBE_CHANNEL_STDOUT;
    }
    if (strstr(text, "stderr") != NULL) {
        mask |= CUBE_CHANNEL_STDERR;
    }
    if (strstr(text, "tty") != NULL) {
        mask |= CUBE_CHANNEL_TTY;
    }
    *channels = mask;
    return mask == 0 ? -1 : 0;
}

static int request_attachment_grant(const char *manager_socket,
                                    const char *process_id,
                                    unsigned int channels,
                                    const char *mode,
                                    cube_attach_grant_t *grant)
{
    char escaped_process_id[CUBICLE_ID_STRING_LENGTH * 2];
    if (cubicle_json_escape(escaped_process_id, sizeof(escaped_process_id),
                            process_id) < 0) {
        fprintf(stderr, "cube: process id is too long\n");
        return 2;
    }

    char params[1024];
    snprintf(params, sizeof(params),
             "{\"process_id\":\"%s\",\"channels\":%u,\"mode\":\"%s\",\"stdout_offset\":0,\"stderr_offset\":0,\"tty_offset\":0,\"rows\":0,\"cols\":0}",
             escaped_process_id, channels, mode);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "attachment.request", params,
                     &response) < 0) {
        return print_rpc_error(&response);
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid attachment grant response\n");
        return 2;
    }

    yyjson_val *endpoint = yyjson_obj_get(document.root, "endpoint");
    char granted_channels[128];
    if (!yyjson_is_obj(endpoint) ||
        json_string_field(endpoint, "uri", grant->endpoint_uri,
                          sizeof(grant->endpoint_uri)) < 0 ||
        json_string_field(document.root, "token", grant->token,
                          sizeof(grant->token)) < 0 ||
        json_string_field(document.root, "granted_channels",
                          granted_channels, sizeof(granted_channels)) < 0 ||
        channel_mask_from_string(granted_channels, &grant->channels) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid attachment grant response\n");
        return 2;
    }

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int controller_attach(const char *controller_socket,
                             const cube_attach_grant_t *grant,
                             unsigned int requested_channels,
                             cube_attach_offsets_t *offsets)
{
    char escaped_token[CUBICLE_TOKEN_MAX * 2];
    if (cubicle_json_escape(escaped_token, sizeof(escaped_token),
                            grant->token) < 0) {
        fprintf(stderr, "cube: attachment token is too long\n");
        return 2;
    }

    char params[2048];
    snprintf(params, sizeof(params),
             "{\"token\":\"%s\",\"channels\":%u,\"mode\":\"%s\"}",
             escaped_token, requested_channels,
             (requested_channels & CUBE_CHANNEL_STDIN) != 0 ? "interactive"
                                                            : "observer");
    cube_rpc_response_t response;
    if (call_controller(controller_socket, "controller.attach", params,
                        &response) < 0) {
        return print_rpc_error(&response);
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid controller attach response\n");
        return 2;
    }
    if (json_u64_field(document.root, "stdout_offset",
                       &offsets->stdout_offset) < 0 ||
        json_u64_field(document.root, "stderr_offset",
                       &offsets->stderr_offset) < 0 ||
        json_u64_field(document.root, "tty_offset",
                       &offsets->tty_offset) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid controller attach response\n");
        return 2;
    }

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int controller_read_stream(const char *controller_socket,
                                  const char *stream,
                                  uint64_t *offset,
                                  FILE *output,
                                  int *end_of_stream)
{
    char params[512];
    snprintf(params, sizeof(params),
             "{\"stream\":\"%s\",\"offset\":%llu,\"maximum_length\":8192}",
             stream, (unsigned long long)*offset);
    cube_rpc_response_t response;
    if (call_controller(controller_socket, "controller.read", params,
                        &response) < 0) {
        if (response.code == CUBICLE_ERR_MANAGER_UNAVAILABLE ||
            response.code == CUBICLE_ERR_IO) {
            return 1;
        }
        return print_rpc_error(&response);
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid controller read response\n");
        return 2;
    }

    yyjson_val *data = yyjson_obj_get(document.root, "data");
    uint64_t next_offset = 0;
    if (!yyjson_is_str(data) ||
        json_u64_field(document.root, "next_offset", &next_offset) < 0 ||
        json_bool_field(document.root, "end_of_stream", end_of_stream) < 0 ||
        next_offset < *offset) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid controller read response\n");
        return 2;
    }
    fputs(yyjson_get_str(data), output);
    fflush(output);
    *offset = next_offset;

    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int controller_write_stdin(const char *controller_socket,
                                  const char *data)
{
    char escaped_data[8192];
    if (cubicle_json_escape(escaped_data, sizeof(escaped_data), data) < 0) {
        fprintf(stderr, "cube: input chunk is too large\n");
        return 2;
    }
    char params[16384];
    snprintf(params, sizeof(params), "{\"data\":\"%s\"}", escaped_data);
    cube_rpc_response_t response;
    if (call_controller(controller_socket, "controller.write", params,
                        &response) < 0) {
        return print_rpc_error(&response);
    }
    cleanup_rpc_response(&response);
    return 0;
}

static int controller_resize_tty(const char *controller_socket)
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
    char params[128];
    snprintf(params, sizeof(params), "{\"rows\":%u,\"columns\":%u}",
             (unsigned int)size.ws_row, (unsigned int)size.ws_col);
    cube_rpc_response_t response;
    if (call_controller(controller_socket, "controller.resize", params,
                        &response) < 0) {
        return print_rpc_error(&response);
    }
    cleanup_rpc_response(&response);
    return 0;
}

static int controller_is_completed(const char *controller_socket,
                                   int *completed)
{
    cube_rpc_response_t response;
    if (call_controller(controller_socket, "controller.status", "{}",
                        &response) < 0) {
        if (response.code == CUBICLE_ERR_MANAGER_UNAVAILABLE ||
            response.code == CUBICLE_ERR_IO) {
            return 1;
        }
        return print_rpc_error(&response);
    }
    cubicle_json_doc_t document;
    char state[32];
    if (cubicle_json_parse(&document, response.result_json) < 0) {
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid controller status response\n");
        return 2;
    }
    if (json_string_field(document.root, "state", state, sizeof(state)) < 0) {
        cubicle_json_cleanup(&document);
        cleanup_rpc_response(&response);
        fprintf(stderr, "cube: invalid controller status response\n");
        return 2;
    }
    *completed = strcmp(state, "completed") == 0 ? 1 : 0;
    cubicle_json_cleanup(&document);
    cleanup_rpc_response(&response);
    return 0;
}

static int process_input_chunk(const char *controller_socket,
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
        if (used + 1 >= sizeof(output)) {
            output[used] = '\0';
            int result = controller_write_stdin(controller_socket, output);
            if (result != 0) {
                return result;
            }
            used = 0;
        }
    }
    if (used > 0) {
        output[used] = '\0';
        return controller_write_stdin(controller_socket, output);
    }
    return 0;
}

static int attachment_loop(const char *controller_socket,
                           unsigned int channels,
                           const char *process_name,
                           const char *mode,
                           int read_only)
{
    uint64_t stdout_offset = 0;
    uint64_t stderr_offset = 0;
    uint64_t tty_offset = 0;
    int stdin_open = !read_only && (channels & CUBE_CHANNEL_STDIN) != 0;
    int stdin_is_tty = isatty(STDIN_FILENO);
    int detach_requested = 0;
    int escape_pending = 0;
    struct termios original;
    int raw_enabled = 0;

    if (strcmp(mode, "tty") == 0 &&
        isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &original) < 0) {
            fprintf(stderr, "cube: failed to read terminal mode: %s\n",
                    strerror(errno));
            return 2;
        }
        struct termios raw = original;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~OPOST;
        raw.c_cflag |= CS8;
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
            fprintf(stderr, "cube: failed to set terminal raw mode: %s\n",
                    strerror(errno));
            return 2;
        }
        raw_enabled = 1;
    }

    fprintf(stderr, "Connected to [%s]. Detach with Ctrl-\\ d\n",
            process_name);
    int result = 0;
    result = controller_resize_tty(controller_socket);

    while (result == 0 && !detach_requested) {
        int completed = 0;
        int stdout_end = 1;
        int stderr_end = 1;
        int tty_end = 1;

        if (strcmp(mode, "tty") == 0 &&
            (channels & (CUBE_CHANNEL_TTY | CUBE_CHANNEL_STDOUT)) != 0) {
            result = controller_read_stream(controller_socket, "tty",
                                            &tty_offset, stdout, &tty_end);
        } else {
            if ((channels & CUBE_CHANNEL_STDOUT) != 0) {
                result = controller_read_stream(controller_socket, "stdout",
                                                &stdout_offset, stdout,
                                                &stdout_end);
            }
            if (result == 0 && (channels & CUBE_CHANNEL_STDERR) != 0) {
                result = controller_read_stream(controller_socket, "stderr",
                                                &stderr_offset, stderr,
                                                &stderr_end);
            }
        }
        if (result == 1 &&
            (!stdin_open || strcmp(mode, "tty") == 0)) {
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

        int status_result = controller_is_completed(controller_socket,
                                                   &completed);
        if (status_result == 1 &&
            ((stdout_end && stderr_end && tty_end && !stdin_open) ||
             strcmp(mode, "tty") == 0)) {
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
                buffer[nread] = '\0';
                result = process_input_chunk(controller_socket, buffer,
                                             (size_t)nread, &escape_pending,
                                             &detach_requested);
                if (!stdin_is_tty) {
                    stdin_open = 0;
                }
            }
        }
    }

    if (raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    }
    return result;
}

static int attach_to_process_id(const char *manager_socket,
                                const char *process_id,
                                const char *process_name,
                                const char *mode,
                                int read_only)
{
    unsigned int requested_channels = CUBE_CHANNEL_STDOUT | CUBE_CHANNEL_STDERR;
    if (strcmp(mode, "tty") == 0) {
        requested_channels = CUBE_CHANNEL_TTY | CUBE_CHANNEL_STDOUT;
    }
    if (!read_only) {
        requested_channels |= CUBE_CHANNEL_STDIN;
    }

    cube_attach_grant_t grant;
    memset(&grant, 0, sizeof(grant));
    int grant_result = request_attachment_grant(
        manager_socket, process_id, requested_channels,
        read_only ? "observer" : "interactive", &grant);
    if (grant_result != 0) {
        return grant_result;
    }

    char controller_socket[PATH_MAX];
    if (unix_socket_path_from_uri(grant.endpoint_uri, controller_socket,
                                  sizeof(controller_socket)) < 0) {
        fprintf(stderr, "cube: unsupported attachment endpoint '%s'\n",
                grant.endpoint_uri);
        return 2;
    }
    unsigned int accepted_channels = requested_channels & grant.channels;
    if (accepted_channels == 0) {
        fprintf(stderr, "cube: attachment grant did not include requested channels\n");
        return 2;
    }

    cube_attach_offsets_t offsets;
    memset(&offsets, 0, sizeof(offsets));
    int attach_result = controller_attach(controller_socket, &grant,
                                          accepted_channels, &offsets);
    if (attach_result != 0) {
        return attach_result;
    }
    (void)offsets;

    return attachment_loop(controller_socket, accepted_channels, process_name,
                           mode, read_only);
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
                                argv[argument_index], mode, read_only);
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

    if (strcmp(mode, "tty") == 0) {
        return attach_to_process_id(manager_socket, process_id, process_name,
                                    mode, 0);
    }

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
        return command_workspace(manager_socket, &options, argc, argv,
                                 command_index);
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

    if (strcmp(command, "logs") == 0) {
        return process_logs(manager_socket, &options, argc, argv,
                            command_index);
    }

    if (strcmp(command, "connect") == 0) {
        return process_connect(manager_socket, &options, argc, argv,
                               command_index);
    }

    if (strcmp(command, "events") == 0) {
        return process_events(manager_socket, &options, argc, argv,
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
