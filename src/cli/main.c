#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "../common/auth_crypto.h"
#include "../common/auth_protocol.h"
#include "../common/json.h"
#include "../common/rpc_internal.h"
#include "../cubeui/cubeui.h"

#include "cubicle/auth.h"
#include "cubicle/config.h"
#include "cubicle/rpc.h"
#include "cubicle/util.h"
#include "cubicle/workspace.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
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

typedef struct cube_log_options {
    int follow;
    int stdout_only;
    int stderr_only;
    uint64_t start;
    uint64_t end;
    int has_end;
} cube_log_options_t;

typedef struct cube_cached_session {
    cubicle_session_info_t session;
    unsigned char resume_secret[CUBICLE_AUTH_SECRET_BYTES];
    uint64_t manager_generation;
    uid_t peer_uid;
    gid_t peer_gid;
} cube_cached_session_t;

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
            "  cube [--manager-socket PATH] [--workspace NAME] [--json] COMMAND [ARG...]\n"
            "  cube workspace [NAME]\n"
            "  cube workspace list|create|select|stop|delete ...\n"
            "  cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] [--dir DIR] COMMAND [ARG...]\n"
            "  cube ps\n"
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
            "  cube config show|paths|validate\n"
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
        fprintf(stream, "Usage:\n  cube ps\n");
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
        fprintf(stream, "Usage:\n  cube config show|paths|validate\n");
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
           strcmp(command, "events") == 0 ||
           strcmp(command, "defaults") == 0;
}

static int command_config(const cubicle_config_t *config,
                          const char *config_error,
                          int argc,
                          char **argv,
                          int command_index)
{
    if (command_index + 1 >= argc) {
        fprintf(stderr, "cube: config requires show, paths, or validate\n");
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
        printf("client.manager=%s\n", config->client_manager_uri);
        printf("defaults.launch=%s\n",
               cubicle_launch_default_name(config->default_launch));
        printf("defaults.mode=%s\n", cube_mode_name(config->default_mode));
        printf("defaults.kill_cleanup=%s\n",
               config->default_kill_cleanup ? "true" : "false");
        return 0;
    }

    fprintf(stderr, "cube: unknown config command '%s'\n", subcommand);
    return 2;
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

static int split_tcp_endpoint(const char *endpoint, char *host,
                              size_t host_size, char *port,
                              size_t port_size)
{
    const char prefix[] = "tcp://";
    if (strncmp(endpoint, prefix, strlen(prefix)) != 0) {
        errno = EINVAL;
        return -1;
    }

    const char *authority = endpoint + strlen(prefix);
    const char *port_start = NULL;
    size_t host_length = 0;
    if (authority[0] == '[') {
        const char *end = strchr(authority, ']');
        if (end == NULL || end[1] != ':') {
            errno = EINVAL;
            return -1;
        }
        host_length = (size_t)(end - authority - 1);
        authority += 1;
        port_start = end + 2;
    } else {
        const char *colon = strrchr(authority, ':');
        if (colon == NULL) {
            errno = EINVAL;
            return -1;
        }
        host_length = (size_t)(colon - authority);
        port_start = colon + 1;
    }
    if (host_length == 0 || port_start == NULL || port_start[0] == '\0' ||
        host_length >= host_size || strlen(port_start) >= port_size) {
        errno = EINVAL;
        return -1;
    }
    memcpy(host, authority, host_length);
    host[host_length] = '\0';
    snprintf(port, port_size, "%s", port_start);
    return 0;
}

static int connect_unix_endpoint(const char *peer_name,
                                 const char *endpoint,
                                 cube_rpc_response_t *response)
{
    const char *socket_path = endpoint;
    if (strncmp(endpoint, "unix://", 7) == 0) {
        socket_path = endpoint + 7;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "%s socket path is too long", peer_name);
        response->code = CUBICLE_ERR_INVALID_ARGUMENT;
        return -1;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to create socket");
        response->code = CUBICLE_ERR_IO;
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to connect to %s", peer_name);
        response->code = CUBICLE_ERR_MANAGER_UNAVAILABLE;
        return -1;
    }
    return fd;
}

static int connect_tcp_endpoint(const char *endpoint,
                                cube_rpc_response_t *response)
{
    char host[256];
    char port[32];
    if (split_tcp_endpoint(endpoint, host, sizeof(host), port,
                           sizeof(port)) < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid manager TCP endpoint");
        response->code = CUBICLE_ERR_INVALID_ARGUMENT;
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *addresses = NULL;
    int gai_result = getaddrinfo(host, port, &hints, &addresses);
    if (gai_result != 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to resolve manager TCP endpoint");
        response->code = CUBICLE_ERR_INVALID_ARGUMENT;
        return -1;
    }

    int fd = -1;
    int saved_errno = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);

    if (fd < 0) {
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to connect to manager");
        response->code = CUBICLE_ERR_MANAGER_UNAVAILABLE;
    }
    return fd;
}

static int connect_rpc_endpoint(const char *peer_name,
                                const char *endpoint,
                                cube_rpc_response_t *response)
{
    if (strncmp(endpoint, "tcp://", 6) == 0) {
        return connect_tcp_endpoint(endpoint, response);
    }
    if (strncmp(endpoint, "unix://", 7) == 0 || endpoint[0] == '/') {
        return connect_unix_endpoint(peer_name, endpoint, response);
    }

    snprintf(response->error_message, sizeof(response->error_message),
             "%s endpoint must be a Unix path, unix:// URI, or tcp:// URI",
             peer_name);
    response->code = CUBICLE_ERR_INVALID_ARGUMENT;
    return -1;
}

static int endpoint_is_unix(const char *endpoint)
{
    return strncmp(endpoint, "unix://", 7) == 0 ||
           strncmp(endpoint, "tcp://", 6) != 0;
}

static int cube_client_key_dir(char path[PATH_MAX])
{
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (config_home != NULL && config_home[0] != '\0') {
        int length = snprintf(path, PATH_MAX, "%s/cubicle/keys",
                              config_home);
        return length < 0 || length >= PATH_MAX ? -1 : 0;
    }

    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        int length = snprintf(path, PATH_MAX, "%s/.config/cubicle/keys",
                              home);
        return length < 0 || length >= PATH_MAX ? -1 : 0;
    }

    int length = snprintf(path, PATH_MAX, ".cubicle/keys");
    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static int cube_client_private_key_path(char path[PATH_MAX],
                                        const char *key_dir)
{
    int length = snprintf(path, PATH_MAX, "%s/client.key", key_dir);
    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static uint64_t cube_endpoint_hash(const char *text)
{
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        hash ^= (uint64_t)*cursor;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t cube_now_ms(void)
{
    time_t now = time(NULL);
    if (now < 0) {
        return 0;
    }
    return (uint64_t)now * 1000ULL;
}

static int cube_session_cache_path(const char *endpoint,
                                   char directory[PATH_MAX],
                                   char path[PATH_MAX])
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == NULL || runtime_dir[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    int length = snprintf(directory, PATH_MAX,
                          "%s/cubicle/sessions/by-endpoint", runtime_dir);
    if (length < 0 || length >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    length = snprintf(path, PATH_MAX, "%s/%016llx.session", directory,
                      (unsigned long long)cube_endpoint_hash(endpoint));
    if (length < 0 || length >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int cube_cache_field(const char *data,
                            const char *key,
                            char *buffer,
                            size_t buffer_size)
{
    size_t key_length = strlen(key);
    const char *cursor = data;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_length =
            line_end == NULL ? strlen(cursor) : (size_t)(line_end - cursor);
        if (line_length > key_length && cursor[key_length] == '=' &&
            strncmp(cursor, key, key_length) == 0) {
            size_t value_length = line_length - key_length - 1;
            if (value_length >= buffer_size) {
                errno = ENOSPC;
                return -1;
            }
            memcpy(buffer, cursor + key_length + 1, value_length);
            buffer[value_length] = '\0';
            return 0;
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }
    errno = ENOENT;
    return -1;
}

static int cube_cache_field_u64(const char *data,
                                const char *key,
                                uint64_t *value_out)
{
    char value[32];
    char *end = NULL;
    if (cube_cache_field(data, key, value, sizeof(value)) < 0) {
        return -1;
    }
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        errno = EINVAL;
        return -1;
    }
    *value_out = (uint64_t)parsed;
    return 0;
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

static int cube_load_cached_session(const char *endpoint,
                                    cube_cached_session_t *cached)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];
    if (cube_session_cache_path(endpoint, directory, path) < 0) {
        return -1;
    }

    struct stat status;
    if (stat(path, &status) < 0) {
        return -1;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != getuid() ||
        (status.st_mode & 0077) != 0 || status.st_size <= 0 ||
        status.st_size > 4096) {
        errno = EACCES;
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    char data[4097];
    size_t length = fread(data, 1, sizeof(data) - 1, file);
    int read_failed = ferror(file);
    fclose(file);
    if (read_failed) {
        errno = EIO;
        return -1;
    }
    data[length] = '\0';

    memset(cached, 0, sizeof(*cached));
    char resume_secret_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
    uint64_t value = 0;
    if (cube_cache_field(data, "session_id", cached->session.session_id,
                         sizeof(cached->session.session_id)) < 0 ||
        cube_cache_field(data, "manager_id", cached->session.manager_id,
                         sizeof(cached->session.manager_id)) < 0 ||
        cube_cache_field(data, "client_key_id", cached->session.client_key_id,
                         sizeof(cached->session.client_key_id)) < 0 ||
        cube_cache_field(data, "resume_secret", resume_secret_hex,
                         sizeof(resume_secret_hex)) < 0 ||
        cubicle_auth_hex_decode(resume_secret_hex, cached->resume_secret,
                                sizeof(cached->resume_secret)) < 0 ||
        cube_cache_field_u64(data, "protocol_major", &value) < 0) {
        return -1;
    }
    cached->session.protocol_major = (uint32_t)value;
    if (cube_cache_field_u64(data, "protocol_minor", &value) == 0) {
        cached->session.protocol_minor = (uint32_t)value;
    }
    (void)cube_cache_field_u64(data, "negotiated_capabilities",
                               &cached->session.negotiated_capabilities);
    (void)cube_cache_field_u64(data, "authenticated_at_ms",
                               &cached->session.authenticated_at_ms);
    (void)cube_cache_field_u64(data, "expires_at_ms",
                               &cached->session.expires_at_ms);
    (void)cube_cache_field_u64(data, "manager_generation",
                               &cached->manager_generation);
    if (cube_cache_field_u64(data, "peer_uid", &value) == 0) {
        cached->peer_uid = (uid_t)value;
    }
    if (cube_cache_field_u64(data, "peer_gid", &value) == 0) {
        cached->peer_gid = (gid_t)value;
    }
    if (cached->session.expires_at_ms > 0 &&
        cached->session.expires_at_ms <= cube_now_ms()) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static void cube_save_cached_session(const char *endpoint,
                                     const cubicle_session_info_t *session,
                                     const unsigned char *resume_secret,
                                     uint64_t manager_generation,
                                     uid_t peer_uid,
                                     gid_t peer_gid)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];
    if (cube_session_cache_path(endpoint, directory, path) < 0 ||
        cubicle_mkdir_p(directory) < 0 ||
        chmod(directory, 0700) < 0) {
        return;
    }

    char resume_secret_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
    if (cubicle_auth_hex_encode(resume_secret, CUBICLE_AUTH_SECRET_BYTES,
                                resume_secret_hex,
                                sizeof(resume_secret_hex)) < 0) {
        return;
    }

    char temporary[PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                          (long)getpid());
    if (length < 0 || length >= PATH_MAX) {
        return;
    }

    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return;
    }
    char content[1024];
    length = snprintf(
        content, sizeof(content),
        "session_id=%s\nmanager_id=%s\nclient_key_id=%s\nprotocol_major=%u\nprotocol_minor=%u\nnegotiated_capabilities=%llu\nauthenticated_at_ms=%llu\nexpires_at_ms=%llu\nresume_secret=%s\nmanager_generation=%llu\npeer_uid=%llu\npeer_gid=%llu\n",
        session->session_id, session->manager_id, session->client_key_id,
        session->protocol_major, session->protocol_minor,
        (unsigned long long)session->negotiated_capabilities,
        (unsigned long long)session->authenticated_at_ms,
        (unsigned long long)session->expires_at_ms, resume_secret_hex,
        (unsigned long long)manager_generation,
        (unsigned long long)peer_uid, (unsigned long long)peer_gid);
    int write_result =
        length < 0 || (size_t)length >= sizeof(content) ||
        cubicle_write_all(fd, content, (size_t)length) < 0 ||
        fsync(fd) < 0;
    int close_result = close(fd);
    if (write_result || close_result < 0) {
        unlink(temporary);
        return;
    }
    if (rename(temporary, path) < 0) {
        unlink(temporary);
    }
}

static int call_rpc_peer_fd(const char *peer_name,
                            const char *endpoint,
                            int fd,
                            const char *request_id,
                            const char *session_id,
                            const char *method,
                            const char *params,
                            cube_rpc_response_t *response)
{
    cleanup_rpc_response(response);
    response->code = CUBICLE_OK;
    response->error_message[0] = '\0';

    char request[8192];
    if (cubicle_rpc_request(request, sizeof(request), request_id,
                            session_id, method, params) < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to encode request");
        response->code = CUBICLE_ERR_INTERNAL;
        return -1;
    }

    uint32_t request_length = htonl((uint32_t)strlen(request));
    if (write_all(fd, &request_length, sizeof(request_length)) < 0 ||
        write_all(fd, request, strlen(request)) < 0) {
        int saved_errno = errno;
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
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to read %s response from %s: %s",
                 peer_name, endpoint, strerror(saved_errno));
        response->code = CUBICLE_ERR_IO;
        return -1;
    }
    uint32_t response_length = ntohl(response_length_network);
    if (response_length == 0 || response_length > CUBE_MAX_FRAME) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid %s response length", peer_name);
        response->code = CUBICLE_ERR_PROTOCOL;
        return -1;
    }

    char *response_json = calloc((size_t)response_length + 1, 1);
    if (response_json == NULL) {
        response->code = CUBICLE_ERR_INTERNAL;
        return -1;
    }
    if (read_all(fd, response_json, response_length) < 0) {
        int saved_errno = errno;
        free(response_json);
        errno = saved_errno;
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to read %s response from %s: %s",
                 peer_name, endpoint, strerror(saved_errno));
        response->code = CUBICLE_ERR_IO;
        return -1;
    }

    cubicle_rpc_response_envelope_t envelope;
    if (cubicle_rpc_decode_response(&envelope, response_json,
                                    request_id) < 0) {
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

static int cube_parse_session_info(yyjson_val *object,
                                   cubicle_session_info_t *session)
{
    uint64_t value = 0;
    memset(session, 0, sizeof(*session));
    if (json_string_field(object, "session_id", session->session_id,
                          sizeof(session->session_id)) < 0 ||
        json_string_field(object, "manager_id", session->manager_id,
                          sizeof(session->manager_id)) < 0 ||
        json_string_field(object, "client_key_id", session->client_key_id,
                          sizeof(session->client_key_id)) < 0 ||
        cubicle_json_get_u64(object, "protocol_major", &value) < 0) {
        return -1;
    }
    session->protocol_major = (uint32_t)value;
    if (cubicle_json_get_u64(object, "protocol_minor", &value) == 0) {
        session->protocol_minor = (uint32_t)value;
    }
    (void)cubicle_json_get_u64(object, "negotiated_capabilities",
                               &session->negotiated_capabilities);
    (void)cubicle_json_get_u64(object, "authenticated_at_ms",
                               &session->authenticated_at_ms);
    (void)cubicle_json_get_u64(object, "expires_at_ms",
                               &session->expires_at_ms);
    return 0;
}

static int resume_manager_fd(const char *endpoint,
                             int fd,
                             char session_id[CUBICLE_ID_STRING_LENGTH],
                             cube_rpc_response_t *response)
{
    cube_cached_session_t cached;
    if (cube_load_cached_session(endpoint, &cached) < 0) {
        return 1;
    }

    cubicle_auth_resume_t resume;
    memset(&resume, 0, sizeof(resume));
    snprintf(resume.manager_key_id, sizeof(resume.manager_key_id), "%s",
             cached.session.manager_id);
    snprintf(resume.session_id, sizeof(resume.session_id), "%s",
             cached.session.session_id);
    if (cubicle_auth_random_bytes(resume.client_nonce,
                                  sizeof(resume.client_nonce)) < 0 ||
        cubicle_auth_random_bytes(resume.connection_id,
                                  sizeof(resume.connection_id)) < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to create resume nonce");
        response->code = CUBICLE_ERR_INTERNAL;
        return -1;
    }
    resume.manager_generation = cached.manager_generation;
    resume.peer_uid = cached.peer_uid;
    resume.peer_gid = cached.peer_gid;

    unsigned char resume_bytes[512];
    size_t resume_length = 0;
    unsigned char authenticator[CUBICLE_AUTH_SECRET_BYTES];
    char client_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
    char connection_id_hex[CUBICLE_AUTH_CONNECTION_ID_BYTES * 2 + 1];
    char authenticator_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
    if (cubicle_auth_encode_resume(&resume, resume_bytes,
                                   sizeof(resume_bytes),
                                   &resume_length) < 0 ||
        cubicle_auth_hmac_sha256(cached.resume_secret,
                                 sizeof(cached.resume_secret),
                                 resume_bytes, resume_length,
                                 authenticator) < 0 ||
        cubicle_auth_hex_encode(resume.client_nonce,
                                sizeof(resume.client_nonce),
                                client_nonce_hex,
                                sizeof(client_nonce_hex)) < 0 ||
        cubicle_auth_hex_encode(resume.connection_id,
                                sizeof(resume.connection_id),
                                connection_id_hex,
                                sizeof(connection_id_hex)) < 0 ||
        cubicle_auth_hex_encode(authenticator, sizeof(authenticator),
                                authenticator_hex,
                                sizeof(authenticator_hex)) < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to create resume authenticator");
        response->code = CUBICLE_ERR_INTERNAL;
        return -1;
    }

    char params[512];
    int length = snprintf(
        params, sizeof(params),
        "{\"session_id\":\"%s\",\"client_nonce\":\"%s\",\"connection_id\":\"%s\",\"authenticator\":\"%s\"}",
        cached.session.session_id, client_nonce_hex, connection_id_hex,
        authenticator_hex);
    if (length < 0 || (size_t)length >= sizeof(params)) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "auth resume request is too large");
        response->code = CUBICLE_ERR_RESOURCE_LIMIT;
        return -1;
    }

    if (call_rpc_peer_fd("manager", endpoint, fd, "cube-auth-resume", "",
                         "auth.resume", params, response) < 0) {
        cleanup_rpc_response(response);
        memset(response, 0, sizeof(*response));
        return 1;
    }

    cubicle_json_doc_t session_doc = {0};
    cubicle_session_info_t resumed;
    int parse_failed = cubicle_json_parse(&session_doc,
                                          response->result_json) < 0;
    if (!parse_failed) {
        parse_failed = cube_parse_session_info(session_doc.root,
                                               &resumed) < 0 ||
                       strcmp(resumed.session_id,
                              cached.session.session_id) != 0;
    }
    cubicle_json_cleanup(&session_doc);
    cleanup_rpc_response(response);
    memset(response, 0, sizeof(*response));
    if (parse_failed) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid auth resume response");
        response->code = CUBICLE_ERR_PROTOCOL;
        return -1;
    }

    snprintf(session_id, CUBICLE_ID_STRING_LENGTH, "%s",
             cached.session.session_id);
    return 0;
}

static int authenticate_manager_fd(const char *endpoint,
                                   int fd,
                                   char session_id[CUBICLE_ID_STRING_LENGTH],
                                   cube_rpc_response_t *response)
{
    snprintf(session_id, CUBICLE_ID_STRING_LENGTH, "local-session");
    if (!endpoint_is_unix(endpoint)) {
        return 0;
    }

    int resume_result = resume_manager_fd(endpoint, fd, session_id, response);
    if (resume_result == 0) {
        return 0;
    }
    if (resume_result < 0) {
        return -1;
    }

    char key_dir[PATH_MAX];
    char private_key_path[PATH_MAX];
    cubicle_auth_identity_t identity;
    if (cube_client_key_dir(key_dir) < 0 ||
        cube_client_private_key_path(private_key_path, key_dir) < 0 ||
        cubicle_auth_ensure_identity(key_dir, "client.key", "client.pub",
                                     &identity) < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to initialize client identity: %s",
                 strerror(errno));
        response->code = CUBICLE_ERR_IO;
        return -1;
    }

    unsigned char client_nonce[CUBICLE_AUTH_NONCE_BYTES];
    char client_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
    if (cubicle_auth_random_bytes(client_nonce, sizeof(client_nonce)) < 0 ||
        cubicle_auth_hex_encode(client_nonce, sizeof(client_nonce),
                                client_nonce_hex,
                                sizeof(client_nonce_hex)) < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to create auth nonce");
        response->code = CUBICLE_ERR_INTERNAL;
        return -1;
    }

    char params[512];
    int length = snprintf(
        params, sizeof(params),
        "{\"client_public_key\":\"%s\",\"client_nonce\":\"%s\"}",
        identity.public_key_hex, client_nonce_hex);
    if (length < 0 || (size_t)length >= sizeof(params)) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "auth challenge request is too large");
        response->code = CUBICLE_ERR_RESOURCE_LIMIT;
        return -1;
    }

    if (call_rpc_peer_fd("manager", endpoint, fd, "cube-auth-1", "",
                         "auth.challenge", params, response) < 0) {
        if (response->code == CUBICLE_ERR_UNSUPPORTED) {
            cleanup_rpc_response(response);
            memset(response, 0, sizeof(*response));
            snprintf(session_id, CUBICLE_ID_STRING_LENGTH, "local-session");
            return 0;
        }
        return -1;
    }

    cubicle_json_doc_t challenge_doc;
    if (cubicle_json_parse(&challenge_doc, response->result_json) < 0) {
        cleanup_rpc_response(response);
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid auth challenge response");
        response->code = CUBICLE_ERR_PROTOCOL;
        return -1;
    }

    char manager_public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
    char manager_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
    char connection_id_hex[CUBICLE_AUTH_CONNECTION_ID_BYTES * 2 + 1];
    cubicle_auth_transcript_t transcript;
    memset(&transcript, 0, sizeof(transcript));
    uint64_t value = 0;
    int parse_failed =
        json_string_field(challenge_doc.root, "manager_public_key",
                          manager_public_key_hex,
                          sizeof(manager_public_key_hex)) < 0 ||
        json_string_field(challenge_doc.root, "manager_nonce",
                          manager_nonce_hex,
                          sizeof(manager_nonce_hex)) < 0 ||
        json_string_field(challenge_doc.root, "connection_id",
                          connection_id_hex,
                          sizeof(connection_id_hex)) < 0 ||
        cubicle_json_get_u64(challenge_doc.root, "protocol_major",
                             &value) < 0;
    if (!parse_failed) {
        transcript.protocol_major = (uint32_t)value;
        if (cubicle_json_get_u64(challenge_doc.root, "protocol_minor",
                                 &value) == 0) {
            transcript.protocol_minor = (uint32_t)value;
        }
        parse_failed =
            cubicle_auth_hex_decode(manager_public_key_hex,
                                    transcript.manager_public_key,
                                    sizeof(transcript.manager_public_key)) < 0 ||
            cubicle_auth_hex_decode(manager_nonce_hex,
                                    transcript.manager_nonce,
                                    sizeof(transcript.manager_nonce)) < 0 ||
            cubicle_auth_hex_decode(connection_id_hex,
                                    transcript.connection_id,
                                    sizeof(transcript.connection_id)) < 0;
    }
    if (!parse_failed) {
        memcpy(transcript.client_public_key, identity.public_key,
               sizeof(transcript.client_public_key));
        memcpy(transcript.client_nonce, client_nonce,
               sizeof(transcript.client_nonce));
        (void)cubicle_json_get_u64(challenge_doc.root, "capabilities",
                                   &transcript.capabilities);
        (void)cubicle_json_get_u64(challenge_doc.root, "manager_generation",
                                   &transcript.manager_generation);
        if (cubicle_json_get_u64(challenge_doc.root, "peer_uid",
                                 &value) == 0) {
            transcript.peer_uid = (uid_t)value;
        }
        if (cubicle_json_get_u64(challenge_doc.root, "peer_gid",
                                 &value) == 0) {
            transcript.peer_gid = (gid_t)value;
        }
    }
    cubicle_json_cleanup(&challenge_doc);
    cleanup_rpc_response(response);
    if (parse_failed) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid auth challenge response");
        response->code = CUBICLE_ERR_PROTOCOL;
        return -1;
    }

    unsigned char transcript_bytes[512];
    size_t transcript_length = 0;
    unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES];
    char signature_hex[CUBICLE_AUTH_SIGNATURE_BYTES * 2 + 1];
    if (cubicle_auth_encode_transcript(&transcript, transcript_bytes,
                                       sizeof(transcript_bytes),
                                       &transcript_length) < 0 ||
        cubicle_auth_sign_file_key(private_key_path, transcript_bytes,
                                   transcript_length, signature) < 0 ||
        cubicle_auth_hex_encode(signature, sizeof(signature), signature_hex,
                                sizeof(signature_hex)) < 0) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "failed to sign auth transcript");
        response->code = CUBICLE_ERR_AUTHENTICATION_FAILED;
        return -1;
    }

    length = snprintf(params, sizeof(params), "{\"signature\":\"%s\"}",
                      signature_hex);
    if (length < 0 || (size_t)length >= sizeof(params)) {
        snprintf(response->error_message, sizeof(response->error_message),
                 "auth signature request is too large");
        response->code = CUBICLE_ERR_RESOURCE_LIMIT;
        return -1;
    }
    if (call_rpc_peer_fd("manager", endpoint, fd, "cube-auth-2", "",
                         "auth.authenticate", params, response) < 0) {
        return -1;
    }

    cubicle_json_doc_t session_doc = {0};
    cubicle_session_info_t session;
    char resume_secret_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
    unsigned char resume_secret[CUBICLE_AUTH_SECRET_BYTES];
    int session_parse_failed =
        cubicle_json_parse(&session_doc, response->result_json) < 0;
    if (!session_parse_failed) {
        session_parse_failed =
            cube_parse_session_info(session_doc.root, &session) < 0 ||
            json_string_field(session_doc.root, "resume_secret",
                              resume_secret_hex,
                              sizeof(resume_secret_hex)) < 0 ||
            cubicle_auth_hex_decode(resume_secret_hex, resume_secret,
                                    sizeof(resume_secret)) < 0;
    }
    if (session_parse_failed) {
        cubicle_json_cleanup(&session_doc);
        cleanup_rpc_response(response);
        snprintf(response->error_message, sizeof(response->error_message),
                 "invalid auth session response");
        response->code = CUBICLE_ERR_PROTOCOL;
        return -1;
    }
    snprintf(session_id, CUBICLE_ID_STRING_LENGTH, "%s", session.session_id);
    cube_save_cached_session(endpoint, &session, resume_secret,
                             transcript.manager_generation,
                             transcript.peer_uid,
                             transcript.peer_gid);
    cubicle_json_cleanup(&session_doc);
    cleanup_rpc_response(response);
    memset(response, 0, sizeof(*response));
    return 0;
}

static int call_rpc_peer(const char *peer_name,
                         const char *endpoint,
                         const char *method,
                         const char *params,
                         int authenticate_manager,
                         cube_rpc_response_t *response)
{
    memset(response, 0, sizeof(*response));

    int fd = connect_rpc_endpoint(peer_name, endpoint, response);
    if (fd < 0) {
        return -1;
    }

    char session_id[CUBICLE_ID_STRING_LENGTH];
    snprintf(session_id, sizeof(session_id), "local-session");
    if (authenticate_manager &&
        authenticate_manager_fd(endpoint, fd, session_id, response) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    int result = call_rpc_peer_fd(peer_name, endpoint, fd, "cube-1",
                                  session_id, method, params, response);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result;
}

static int call_manager(const char *socket_path,
                        const char *method,
                        const char *params,
                        cube_rpc_response_t *response)
{
    return call_rpc_peer("manager", socket_path, method, params, 1, response);
}

static int call_controller(const char *socket_path,
                           const char *method,
                           const char *params,
                           cube_rpc_response_t *response)
{
    return call_rpc_peer("controller", socket_path, method, params, 0,
                         response);
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

static int process_list(const char *manager_socket,
                        const cube_options_t *options)
{
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

    char params[1024];
    snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\"}",
             escaped_workspace);
    cube_rpc_response_t response;
    if (call_manager(manager_socket, "process.list", params, &response) < 0) {
        return print_workspace_rpc_error(&response, workspace,
                                         from_selected_workspace);
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

static int process_cleanup(const char *manager_socket,
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
    if (call_manager(manager_socket, "manager.cleanup", params,
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

    printf("Removed %llu processes\n", (unsigned long long)removed_count);
    printf("Skipped %llu live processes\n",
           (unsigned long long)skipped_live_count);
    printf("Skipped %llu saved processes\n",
           (unsigned long long)skipped_saved_count);

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
            int saved = 0;
            result = process_saved_by_id(manager_socket, targets[i].id,
                                         &saved);
            if (result != 0) {
                return result;
            }
            if (saved) {
                ++skipped_saved_count;
                continue;
            }
            result = remove_process_by_id(manager_socket, targets[i].id);
            if (result != 0) {
                return result;
            }
            ++removed_count;
        }
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
                                 size_t *event_count)
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
        int result = print_events_response(response.result_json,
                                          options->json, &after_sequence,
                                          &event_count);
        cleanup_rpc_response(&response);
        if (result != 0) {
            return result;
        }

        if (!follow) {
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
                           int read_only,
                           const cube_attach_offsets_t *offsets,
                           uint64_t replay_bytes)
{
    uint64_t stdout_offset = offsets->stdout_offset > replay_bytes
                                 ? offsets->stdout_offset - replay_bytes
                                 : 0;
    uint64_t stderr_offset = offsets->stderr_offset > replay_bytes
                                 ? offsets->stderr_offset - replay_bytes
                                 : 0;
    uint64_t tty_offset = offsets->tty_offset > replay_bytes
                              ? offsets->tty_offset - replay_bytes
                              : 0;
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
    result = controller_resize_tty(controller_socket);

    while (result == 0 && !detach_requested) {
        int completed = 0;
        int stdout_end = 1;
        int stderr_end = 1;
        int tty_end = 1;

        if (terminal_mode &&
            (channels & (CUBE_CHANNEL_TTY | CUBE_CHANNEL_STDOUT)) != 0) {
            result = controller_read_stream(controller_socket, "tty",
                                            &tty_offset, stdout, &tty_end);
            if (result == 0 && strcmp(mode, "term") == 0 &&
                (channels & CUBE_CHANNEL_STDERR) != 0) {
                result = controller_read_stream(controller_socket, "stderr",
                                                &stderr_offset, stderr,
                                                &stderr_end);
            }
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

        int status_result = controller_is_completed(controller_socket,
                                                   &completed);
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

    return attachment_loop(controller_socket, accepted_channels, process_name,
                           mode, read_only, &offsets, replay_bytes);
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
    int config_loaded = cubicle_config_load(&config, config_error,
                                            sizeof(config_error)) == 0;
    if (!config_loaded) {
        cubicle_config_defaults(&config);
    }

    if (strcmp(command, "config") == 0) {
        return command_config(&config, config_error, argc, argv,
                              command_index);
    }

    if (!config_loaded) {
        fprintf(stderr, "cube: configuration error: %s\n", config_error);
        return 2;
    }

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
        return process_list(manager_endpoint, &options);
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
