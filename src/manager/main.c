#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "cubicle/log.h"
#include "cubicle/config.h"
#include "cubicle/manager_registry.h"
#include "cubicle/attachment.h"
#include "cubicle/auth.h"
#include "cubicle/process.h"
#include "cubicle/rpc.h"
#include "cubicle/util.h"
#include "cubicle/workspace.h"

#include "../common/auth_crypto.h"
#include "../common/auth_protocol.h"
#include "../common/rpc_internal.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX CUBICLE_PATH_MAX
#endif

typedef struct manager_state {
    char dir[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char log_dir[PATH_MAX];
    char listen_uri[CUBICLE_ENDPOINT_URI_MAX];
    mode_t socket_mode;
    char socket_group[256];
    char controller_bin[PATH_MAX];
    char controller_debug[64];
    cubicle_auth_identity_t identity;
} manager_state_t;

#define CUBICLE_MANAGER_MAX_SESSIONS 128
#define CUBICLE_MANAGER_DEFAULT_MAX_CLIENTS 64
#define CUBICLE_MANAGER_MAX_CLIENTS 1024

typedef struct manager_session_record {
    int active;
    cubicle_session_info_t session;
    unsigned char resume_secret[CUBICLE_AUTH_SECRET_BYTES];
    uid_t peer_uid;
    gid_t peer_gid;
    uint64_t manager_generation;
    char client_public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
} manager_session_record_t;

typedef struct manager_session_store {
    manager_session_record_t records[CUBICLE_MANAGER_MAX_SESSIONS];
} manager_session_store_t;

typedef struct manager_runtime manager_runtime_t;

typedef struct manager_worker_slot {
    int active;
    int fd;
    manager_runtime_t *runtime;
} manager_worker_slot_t;

struct manager_runtime {
    const manager_state_t *state;
    manager_session_store_t sessions;
    pthread_mutex_t sessions_mutex;
    pthread_mutex_t workers_mutex;
    pthread_cond_t workers_cond;
    int shutdown_requested;
    uint64_t started_at_ms;
    int max_clients;
    int active_workers;
    manager_worker_slot_t workers[CUBICLE_MANAGER_MAX_CLIENTS];
};

typedef struct manager_connection {
    int has_peer_credentials;
    uid_t peer_uid;
    gid_t peer_gid;
    pid_t peer_pid;
    int has_pending_auth;
    cubicle_auth_transcript_t pending_auth;
    int authenticated;
    cubicle_session_info_t session;
    unsigned char resume_secret[CUBICLE_AUTH_SECRET_BYTES];
    char client_public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
    manager_runtime_t *runtime;
} manager_connection_t;

typedef struct workspace_key_record {
    char workspace_id[CUBICLE_ID_STRING_LENGTH];
    char key_id[CUBICLE_ID_STRING_LENGTH];
    char fingerprint[CUBICLE_NAME_MAX];
    char label[CUBICLE_KEY_LABEL_MAX];
    cubicle_capability_mask_t capabilities;
    uint64_t created_at_ms;
    uint64_t revoked_at_ms;
    char public_key_hex[512];
} workspace_key_record_t;

#define CUBICLE_API_MAX_FRAME 65536
#define CUBICLE_MANAGER_MAX_SIGNAL_NUMBER 128
#define CUBICLE_API_CAPABILITIES \
    (CUBICLE_PROTOCOL_CAP_TRANSPORT_UNIX | CUBICLE_PROTOCOL_CAP_PROCESS_STREAM | \
     CUBICLE_PROTOCOL_CAP_PROCESS_TTY | CUBICLE_PROTOCOL_CAP_ATTACHMENT_DIRECT)

static _Thread_local char manager_error_detail[PATH_MAX + 128];

static void clear_manager_error_detail(void)
{
    manager_error_detail[0] = '\0';
}

static int set_manager_path_error(const char *operation,
                                  const char *path,
                                  int error_number)
{
    snprintf(manager_error_detail, sizeof(manager_error_detail),
             "%s %s: %s", operation, path, strerror(error_number));
    errno = error_number;
    return -1;
}

static const char *manager_error_message(int error_number)
{
    return manager_error_detail[0] == '\0' ? strerror(error_number)
                                           : manager_error_detail;
}

static void manager_log_error(int error_number)
{
    cubicle_log(CUBICLE_LOG_ERROR, "manager",
                manager_error_message(error_number));
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] workspace create [--dir DIR] NAME\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] workspace list\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] process register --workspace NAME_OR_ID --friendly-name NAME --mode MODE --controller-id ID --control-socket PATH [--process-id ID]\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] [--controller-bin PATH] process start --workspace NAME_OR_ID --friendly-name NAME --mode stream|tty|term [--stdin-policy open|eof] [--dir DIR] -- COMMAND [ARGS...]\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] process resolve PROCESS_ID_OR_NAME [--workspace NAME_OR_ID]\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] events poll [--workspace NAME_OR_ID]\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] events list [--workspace NAME_OR_ID]\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] events follow [--iterations N] [--interval-ms N] [--workspace NAME_OR_ID]\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] daemon [--foreground] [--control-socket PATH] [--listen URI] [--allow-insecure] [--event-interval-ms N] [--max-clients N]\n"
            "       %s [--state-dir dir] [--runtime-dir dir] [--log-dir dir] process list [--workspace NAME_OR_ID]\n",
            program, program, program, program, program, program, program,
            program, program, program);
    fprintf(stderr, "Global options: --config PATH, --state-dir dir, --runtime-dir dir, --log-dir dir\n");
}

static int apply_config_option_prescan(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "cubicle-manager: --config requires a path\n");
                return -1;
            }
            if (setenv("CUBICLE_CONFIG", argv[i + 1], 1) < 0) {
                fprintf(stderr,
                        "cubicle-manager: failed to set config override: %s\n",
                        strerror(errno));
                return -1;
            }
            ++i;
        }
    }
    return 0;
}

static int validate_field(const char *value, const char *name)
{
    if (value == NULL || value[0] == '\0') {
        fprintf(stderr, "Missing %s\n", name);
        return -1;
    }

    if (strchr(value, '\t') != NULL || strchr(value, '\n') != NULL) {
        fprintf(stderr, "%s may not contain tabs or newlines\n", name);
        return -1;
    }

    return 0;
}

static int resolve_directory_path(const char *directory,
                                  char resolved[CUBICLE_PATH_MAX])
{
    if (directory == NULL || directory[0] == '\0') {
        if (getcwd(resolved, CUBICLE_PATH_MAX) == NULL) {
            return -1;
        }
        return 0;
    }

    char real_path[CUBICLE_PATH_MAX];
    if (realpath(directory, real_path) == NULL) {
        return -1;
    }

    int length = snprintf(resolved, CUBICLE_PATH_MAX, "%s", real_path);
    return length < 0 || length >= CUBICLE_PATH_MAX ? -1 : 0;
}

static int state_path(char path[PATH_MAX], const manager_state_t *state,
                      const char *name)
{
    int result = snprintf(path, PATH_MAX, "%s/%s", state->dir, name);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int runtime_path(char path[PATH_MAX], const manager_state_t *state,
                        const char *name)
{
    int result = snprintf(path, PATH_MAX, "%s/%s", state->runtime_dir, name);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int controller_state_path(char path[PATH_MAX],
                                 const manager_state_t *state,
                                 const char *process_id)
{
    int result = snprintf(path, PATH_MAX, "%s/controllers/%s", state->dir,
                          process_id);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int controller_socket_path(char path[PATH_MAX],
                                  const manager_state_t *state,
                                  const char *process_id)
{
    int result = snprintf(path, PATH_MAX, "%s/controllers/%s/control.sock",
                          state->runtime_dir, process_id);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int controller_log_path(char path[PATH_MAX],
                               const manager_state_t *state,
                               const char *process_id)
{
    int result = snprintf(path, PATH_MAX, "%s/controllers/%s", state->log_dir,
                          process_id);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int process_output_path(char path[PATH_MAX],
                               const manager_state_t *state,
                               const char *process_id,
                               const char *file_name);

static int ensure_parent_directory(const char *path)
{
    char parent[PATH_MAX];
    int result = snprintf(parent, sizeof(parent), "%s", path);
    if (result < 0 || (size_t)result >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char *slash = strrchr(parent, '/');
    if (slash == NULL) {
        return 0;
    }
    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    return cubicle_mkdir_p(parent);
}

static void log_manager_directory_error(const char *label,
                                        const char *path,
                                        const char *detail)
{
    char message[PATH_MAX + 256];
    snprintf(message, sizeof(message), "%s (%s): %s", label, path, detail);
    cubicle_log(CUBICLE_LOG_ERROR, "manager", message);
}

static int verify_manager_directory(const char *label, const char *path)
{
    if (cubicle_mkdir_p(path) < 0) {
        char detail[256];
        snprintf(detail, sizeof(detail), "failed to create directory: %s",
                 strerror(errno));
        log_manager_directory_error(label, path, detail);
        return -1;
    }

    struct stat status;
    if (lstat(path, &status) < 0) {
        char detail[256];
        snprintf(detail, sizeof(detail), "failed to stat directory: %s",
                 strerror(errno));
        log_manager_directory_error(label, path, detail);
        return -1;
    }
    if (S_ISLNK(status.st_mode)) {
        log_manager_directory_error(label, path,
                                    "must not be a symbolic link");
        errno = ELOOP;
        return -1;
    }
    if (!S_ISDIR(status.st_mode)) {
        log_manager_directory_error(label, path, "must be a directory");
        errno = ENOTDIR;
        return -1;
    }
    if (status.st_uid != geteuid()) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "owner uid %ld does not match manager uid %ld",
                 (long)status.st_uid, (long)geteuid());
        log_manager_directory_error(label, path, detail);
        errno = EACCES;
        return -1;
    }
    if ((status.st_mode & 0022) != 0) {
        log_manager_directory_error(
            label, path, "must not be writable by group or other");
        errno = EACCES;
        return -1;
    }
    return 0;
}

static void log_manager_setting(const char *key, const char *value)
{
    char message[PATH_MAX + 128];
    snprintf(message, sizeof(message), "%s=%s", key,
             value != NULL && value[0] != '\0' ? value : "(none)");
    cubicle_log(CUBICLE_LOG_INFO, "manager.config", message);
}

static void log_manager_effective_config(const manager_state_t *state)
{
    char socket_mode[16];
    snprintf(socket_mode, sizeof(socket_mode), "%04o",
             (unsigned int)state->socket_mode);
    log_manager_setting("manager.state_dir", state->dir);
    log_manager_setting("manager.runtime_dir", state->runtime_dir);
    log_manager_setting("manager.log_dir", state->log_dir);
    log_manager_setting("manager.listen", state->listen_uri);
    log_manager_setting("manager.socket_mode", socket_mode);
    log_manager_setting("manager.socket_group", state->socket_group);
    log_manager_setting("manager.controller_binary", state->controller_bin);
    log_manager_setting("controller.debug", state->controller_debug);
}

static int append_line(const manager_state_t *state, const char *file_name,
                       const char *line)
{
    clear_manager_error_detail();
    char path[PATH_MAX];
    if (state_path(path, state, file_name) < 0) {
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return set_manager_path_error("open", path, errno);
    }

    size_t length = strlen(line);
    int result = cubicle_write_all(fd, line, length);
    if (result < 0) {
        int saved_errno = errno;
        close(fd);
        return set_manager_path_error("write", path, saved_errno);
    }
    if (close(fd) < 0) {
        return set_manager_path_error("close", path, errno);
    }
    return 0;
}

static int set_fd_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_fd_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int lock_state_file(const manager_state_t *state, const char *file_name)
{
    char path[PATH_MAX];
    if (state_path(path, state, file_name) < 0) {
        return -1;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }

    if (flock(fd, LOCK_EX) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int lock_state(const manager_state_t *state)
{
    return lock_state_file(state, "manager.lock");
}

static int lock_daemon(const manager_state_t *state)
{
    char path[PATH_MAX];
    if (runtime_path(path, state, "manager.daemon.lock") < 0) {
        return -1;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void unlock_state(int lock_fd)
{
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

static FILE *open_state_file_for_read(const manager_state_t *state,
                                      const char *file_name)
{
    char path[PATH_MAX];
    if (state_path(path, state, file_name) < 0) {
        return NULL;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL && errno == ENOENT) {
        return NULL;
    }

    return file;
}

static int manager_listen_uri(char uri[CUBICLE_ENDPOINT_URI_MAX],
                              const manager_state_t *state,
                              const char *requested_socket,
                              const char *requested_uri)
{
    if (requested_socket != NULL && requested_uri != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (requested_uri != NULL) {
        int result = snprintf(uri, CUBICLE_ENDPOINT_URI_MAX, "%s",
                              requested_uri);
        if (result < 0 || (size_t)result >= CUBICLE_ENDPOINT_URI_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    if (requested_socket != NULL) {
        int result = snprintf(uri, CUBICLE_ENDPOINT_URI_MAX, "unix://%s",
                              requested_socket);
        if (result < 0 || (size_t)result >= CUBICLE_ENDPOINT_URI_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    int result = snprintf(uri, CUBICLE_ENDPOINT_URI_MAX, "%s",
                          state->listen_uri);
    if (result < 0 || (size_t)result >= CUBICLE_ENDPOINT_URI_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int is_live_unix_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (set_fd_cloexec(fd) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    int live = connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
    close(fd);
    return live;
}

static int prepare_manager_socket_path(const char *path)
{
    struct stat existing;
    if (lstat(path, &existing) < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (!S_ISSOCK(existing.st_mode)) {
        errno = EEXIST;
        return -1;
    }

    int live = is_live_unix_socket(path);
    if (live != 0) {
        errno = live > 0 ? EADDRINUSE : errno;
        return -1;
    }

    return unlink(path);
}

static int open_manager_socket(const char *path, mode_t socket_mode,
                               const char *socket_group)
{
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (prepare_manager_socket_path(path) < 0) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (set_fd_cloexec(fd) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    if (socket_group != NULL && socket_group[0] != '\0') {
        struct group *group = getgrnam(socket_group);
        if (group == NULL || chown(path, (uid_t)-1, group->gr_gid) < 0) {
            close(fd);
            return -1;
        }
    }
    if (chmod(path, socket_mode) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int split_tcp_uri(const char *uri, char *host, size_t host_size,
                         char *port, size_t port_size)
{
    const char prefix[] = "tcp://";
    if (uri == NULL || strncmp(uri, prefix, strlen(prefix)) != 0) {
        errno = EINVAL;
        return -1;
    }

    const char *authority = uri + strlen(prefix);
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

static int open_manager_tcp_listener(const char *uri)
{
    char host[256];
    char port[32];
    if (split_tcp_uri(uri, host, sizeof(host), port, sizeof(port)) < 0) {
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *addresses = NULL;
    int gai_result = getaddrinfo(host, port, &hints, &addresses);
    if (gai_result != 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = -1;
    int saved_errno = EADDRNOTAVAIL;
    for (struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (set_fd_cloexec(fd) < 0) {
            saved_errno = errno;
            close(fd);
            fd = -1;
            continue;
        }

        int reuse = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(fd, 16) == 0) {
            break;
        }

        saved_errno = errno;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(addresses);
    if (fd < 0) {
        errno = saved_errno;
    }
    return fd;
}

static int open_manager_listener(const manager_state_t *state, const char *uri,
                                 int allow_insecure,
                                 char cleanup_path[PATH_MAX])
{
    cleanup_path[0] = '\0';
    if (strncmp(uri, "unix://", 7) == 0) {
        char path[PATH_MAX];
        if (cubicle_config_unix_uri_path(uri, path, sizeof(path)) < 0) {
            return -1;
        }
        int result = snprintf(cleanup_path, PATH_MAX, "%s", path);
        if (result < 0 || result >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return open_manager_socket(path, state->socket_mode,
                                   state->socket_group);
    }

    if (strncmp(uri, "tcp://", 6) == 0) {
        if (!allow_insecure) {
            errno = EACCES;
            return -1;
        }
        return open_manager_tcp_listener(uri);
    }

    errno = EINVAL;
    return -1;
}

static int manager_id(const manager_state_t *state, char id[CUBICLE_MANAGER_ID_LENGTH + 1])
{
    FILE *file = open_state_file_for_read(state, "manager-id");
    if (file != NULL) {
        if (fgets(id, CUBICLE_MANAGER_ID_LENGTH + 1, file) == NULL) {
            fclose(file);
            errno = EINVAL;
            return -1;
        }
        id[strcspn(id, "\n")] = '\0';
        fclose(file);
        return 0;
    }

    if (cubicle_generate_hex_id(id, CUBICLE_MANAGER_ID_LENGTH + 1) < 0) {
        return -1;
    }

    char line[CUBICLE_MANAGER_ID_LENGTH + 2];
    snprintf(line, sizeof(line), "%s\n", id);
    return append_line(state, "manager-id", line);
}

static int count_workspaces(const manager_state_t *state, size_t *count)
{
    *count = 0;

    FILE *file = open_state_file_for_read(state, "workspaces.tsv");
    if (file == NULL) {
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_workspace_record_t record;
        if (cubicle_parse_workspace_record(line, &record) == 0) {
            ++(*count);
        }
    }

    fclose(file);
    return 0;
}

static int count_processes(const manager_state_t *state, size_t *count)
{
    *count = 0;

    FILE *file = open_state_file_for_read(state, "processes.tsv");
    if (file == NULL) {
        return 0;
    }

    char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_process_record_t record;
        if (cubicle_parse_process_record(line, &record) == 0) {
            ++(*count);
        }
    }

    fclose(file);
    return 0;
}

static int find_workspace(const manager_state_t *state, const char *name_or_id,
                          cubicle_workspace_record_t *record)
{
    FILE *file = open_state_file_for_read(state, "workspaces.tsv");
    if (file == NULL) {
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_workspace_record_t candidate;
        if (cubicle_parse_workspace_record(line, &candidate) == 0 &&
            (strcmp(candidate.id, name_or_id) == 0 ||
             strcmp(candidate.name, name_or_id) == 0)) {
            *record = candidate;
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    return -1;
}

static int workspace_name_exists(const manager_state_t *state, const char *name)
{
    cubicle_workspace_record_t record;
    return find_workspace(state, name, &record) == 0;
}

static int append_workspace_record_with_id(const manager_state_t *state,
                                           const char *workspace_id,
                                           const char *name,
                                           const char *directory)
{
    char line[CUBICLE_PATH_MAX + 256];
    int length = snprintf(line, sizeof(line), "%s\t%s\t%s\n", workspace_id,
                          name, directory == NULL ? "" : directory);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return append_line(state, "workspaces.tsv", line);
}

static int rewrite_workspace_records(const manager_state_t *state,
                                     const char *workspace_id,
                                     const char *new_name,
                                     int remove_record,
                                     int *found)
{
    *found = 0;
    char path[PATH_MAX];
    char temp_path[PATH_MAX];
    if (state_path(path, state, "workspaces.tsv") < 0) {
        return -1;
    }
    int length = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *input = fopen(path, "r");
    FILE *output = fopen(temp_path, "w");
    if (output == NULL) {
        if (input != NULL) {
            fclose(input);
        }
        return -1;
    }

    if (input != NULL) {
        char line[512];
        while (fgets(line, sizeof(line), input) != NULL) {
            cubicle_workspace_record_t record;
            if (cubicle_parse_workspace_record(line, &record) != 0) {
                continue;
            }
            if (strcmp(record.id, workspace_id) == 0) {
                *found = 1;
                if (remove_record) {
                    continue;
                }
                if (new_name != NULL) {
                    snprintf(record.name, sizeof(record.name), "%s",
                             new_name);
                }
            }
            if (fprintf(output, "%s\t%s\t%s\n", record.id, record.name,
                        record.directory) < 0) {
                fclose(input);
                fclose(output);
                return -1;
            }
        }
        fclose(input);
    }

    if (fclose(output) != 0) {
        return -1;
    }
    return rename(temp_path, path);
}

static int count_processes_for_workspace(const manager_state_t *state,
                                         const char *workspace_id,
                                         size_t *process_count,
                                         size_t *running_count)
{
    *process_count = 0;
    *running_count = 0;

    FILE *file = open_state_file_for_read(state, "processes.tsv");
    if (file == NULL) {
        return 0;
    }

    char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_process_record_t record;
        if (cubicle_parse_process_record(line, &record) == 0 &&
            strcmp(record.workspace_id, workspace_id) == 0) {
            ++(*process_count);
            if (strcmp(record.state, "running") == 0) {
                ++(*running_count);
            }
        }
    }

    fclose(file);
    return 0;
}

static int workspace_info_json(const manager_state_t *state,
                               const char *manager_id_value,
                               const cubicle_workspace_record_t *workspace,
                               char *buffer,
                               size_t buffer_size)
{
    size_t process_count = 0;
    size_t running_count = 0;
    char escaped_name[256];
    char escaped_directory[CUBICLE_PATH_MAX * 2];
    if (count_processes_for_workspace(state, workspace->id, &process_count,
                                      &running_count) < 0 ||
        cubicle_json_escape(escaped_name, sizeof(escaped_name),
                            workspace->name) < 0 ||
        cubicle_json_escape(escaped_directory, sizeof(escaped_directory),
                            workspace->directory) < 0) {
        return -1;
    }

    int length = snprintf(buffer, buffer_size,
                          "{\"manager_id\":\"%s\",\"id\":\"%s\",\"name\":\"%s\",\"directory\":\"%s\",\"created_at_ms\":0,\"updated_at_ms\":0,\"process_count\":%zu,\"running_process_count\":%zu}",
                          manager_id_value, workspace->id, escaped_name,
                          escaped_directory, process_count, running_count);
    if (length < 0 || (size_t)length >= buffer_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static const char *api_process_state_name(const char *record_state)
{
    if (strcmp(record_state, "running") == 0) {
        return "running";
    }
    if (strcmp(record_state, "completed") == 0) {
        return "completed";
    }
    if (strcmp(record_state, "failed") == 0) {
        return "failed";
    }
    return "lost";
}

static int process_output_range(const manager_state_t *state,
                                const char *process_id,
                                const char *file_name,
                                uint64_t *start_offset,
                                uint64_t *end_offset)
{
    char path[PATH_MAX];
    if (process_output_path(path, state, process_id, file_name) < 0) {
        return -1;
    }

    struct stat status;
    if (stat(path, &status) < 0) {
        if (errno == ENOENT) {
            *start_offset = 0;
            *end_offset = 0;
            return 0;
        }
        return -1;
    }

    *start_offset = 0;
    *end_offset = status.st_size < 0 ? 0 : (uint64_t)status.st_size;
    return 0;
}

static int process_info_json(const manager_state_t *state,
                             const char *manager_id_value,
                             const cubicle_process_record_t *process,
                             char *buffer,
                             size_t buffer_size)
{
    char escaped_name[256];
    char escaped_cwd[CUBICLE_PATH_MAX * 2];
    uint64_t stdout_start_offset = 0;
    uint64_t stdout_offset = 0;
    uint64_t stderr_start_offset = 0;
    uint64_t stderr_offset = 0;
    if (cubicle_json_escape(escaped_name, sizeof(escaped_name),
                            process->friendly_name) < 0 ||
        cubicle_json_escape(escaped_cwd, sizeof(escaped_cwd),
                            process->cwd) < 0 ||
        process_output_range(state, process->process_id, "stdout.log",
                             &stdout_start_offset, &stdout_offset) < 0 ||
        process_output_range(state, process->process_id, "stderr.log",
                             &stderr_start_offset, &stderr_offset) < 0) {
        return -1;
    }

    int length = snprintf(buffer, buffer_size,
                          "{\"manager_id\":\"%s\",\"workspace_id\":\"%s\",\"id\":\"%s\",\"friendly_name\":\"%s\",\"mode\":\"%s\",\"state\":\"%s\",\"cwd\":\"%s\",\"argv\":%s,\"saved\":%s,\"restart\":%s,\"exit_code\":0,\"termination_signal\":0,\"has_exit_status\":false,\"stdout_start_offset\":%llu,\"stdout_offset\":%llu,\"stderr_start_offset\":%llu,\"stderr_offset\":%llu,\"tty_offset\":%llu,\"created_at_ms\":0,\"started_at_ms\":0,\"exited_at_ms\":0,\"local_pid\":0,\"local_pgid\":0}",
                          manager_id_value, process->workspace_id,
                          process->process_id, escaped_name, process->mode,
                          api_process_state_name(process->state), escaped_cwd,
                          process->argv_json[0] == '\0' ? "[]" :
                                                           process->argv_json,
                          process->saved ? "true" : "false",
                          process->restart ? "true" : "false",
                          (unsigned long long)stdout_start_offset,
                          (unsigned long long)stdout_offset,
                          (unsigned long long)stderr_start_offset,
                          (unsigned long long)stderr_offset,
                          (unsigned long long)stdout_offset);
    if (length < 0 || (size_t)length >= buffer_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static const char *api_event_type_name(const char *payload)
{
    if (strstr(payload, "type=process_started") != NULL) {
        return "process_started";
    }
    if (strstr(payload, "type=process_exited") != NULL) {
        return "process_exited";
    }
    if (strstr(payload, "type=output ") != NULL) {
        return "output_available";
    }
    if (strstr(payload, "type=client_attached") != NULL) {
        return "client_attached";
    }
    if (strstr(payload, "type=client_detached") != NULL) {
        return "client_detached";
    }
    return "process_state_changed";
}

static int event_info_json(uint64_t global_sequence,
                           const char *workspace_id,
                           const char *process_id,
                           const char *payload,
                           char *buffer,
                           size_t buffer_size)
{
    long long workspace_sequence = 0;
    (void)cubicle_parse_event_sequence(payload, &workspace_sequence);

    char escaped_payload[CUBICLE_EVENT_PAYLOAD_MAX * 2];
    if (cubicle_json_escape(escaped_payload, sizeof(escaped_payload),
                            payload) < 0) {
        return -1;
    }

    int length = snprintf(buffer, buffer_size,
                          "{\"global_sequence\":%llu,\"workspace_sequence\":%lld,\"timestamp_ms\":0,\"type\":\"%s\",\"workspace_id\":\"%s\",\"process_id\":\"%s\",\"payload\":\"%s\"}",
                          (unsigned long long)global_sequence,
                          workspace_sequence,
                          api_event_type_name(payload), workspace_id,
                          process_id, escaped_payload);
    if (length < 0 || (size_t)length >= buffer_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static const char *api_stream_file_name(const char *stream)
{
    if (strcmp(stream, "stdout") == 0) {
        return "stdout.log";
    }
    if (strcmp(stream, "stderr") == 0) {
        return "stderr.log";
    }
    if (strcmp(stream, "tty") == 0) {
        return "stdout.log";
    }
    return NULL;
}

static int find_process_record(const manager_state_t *state,
                               const char *process_id_or_name,
                               const char *workspace_id,
                               cubicle_process_record_t *record,
                               int *ambiguous)
{
    *ambiguous = 0;
    FILE *file = open_state_file_for_read(state, "processes.tsv");
    if (file == NULL) {
        return -1;
    }

    int found = 0;
    char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_process_record_t candidate;
        if (cubicle_parse_process_record(line, &candidate) != 0) {
            continue;
        }

        int matches = strcmp(candidate.process_id, process_id_or_name) == 0;
        if (!matches && workspace_id != NULL &&
            strcmp(candidate.workspace_id, workspace_id) == 0 &&
            strcmp(candidate.friendly_name, process_id_or_name) == 0) {
            matches = 1;
        }

        if (!matches) {
            continue;
        }
        if (found) {
            *ambiguous = 1;
            fclose(file);
            return -1;
        }
        *record = candidate;
        found = 1;
    }

    fclose(file);
    return found ? 0 : -1;
}

static int process_conflict_exists(const manager_state_t *state,
                                   const char *workspace_id,
                                   const char *process_id,
                                   const char *friendly_name,
                                   int *process_id_conflict,
                                   int *friendly_name_conflict)
{
    *process_id_conflict = 0;
    *friendly_name_conflict = 0;

    FILE *file = open_state_file_for_read(state, "processes.tsv");
    if (file == NULL) {
        return 0;
    }

    char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_process_record_t record;
        if (cubicle_parse_process_record(line, &record) != 0) {
            continue;
        }

        if (strcmp(record.process_id, process_id) == 0) {
            *process_id_conflict = 1;
        }

        if (strcmp(record.workspace_id, workspace_id) == 0 &&
            strcmp(record.friendly_name, friendly_name) == 0) {
            *friendly_name_conflict = 1;
        }
    }

    fclose(file);
    return 0;
}

static int process_friendly_name_conflicts(const manager_state_t *state,
                                           const char *workspace_id,
                                           const char *process_id,
                                           const char *friendly_name,
                                           int *conflict)
{
    *conflict = 0;
    FILE *file = open_state_file_for_read(state, "processes.tsv");
    if (file == NULL) {
        return 0;
    }

    char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_process_record_t record;
        if (cubicle_parse_process_record(line, &record) != 0) {
            continue;
        }
        if (strcmp(record.workspace_id, workspace_id) == 0 &&
            strcmp(record.process_id, process_id) != 0 &&
            strcmp(record.friendly_name, friendly_name) == 0) {
            *conflict = 1;
            break;
        }
    }

    fclose(file);
    return 0;
}

static int parse_workspace_key_record(const char *line,
                                      workspace_key_record_t *record)
{
    char copy[1024];
    int length = snprintf(copy, sizeof(copy), "%s", line);
    if (length < 0 || (size_t)length >= sizeof(copy)) {
        return -1;
    }

    char *fields[8];
    char *cursor = copy;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        fields[i] = cursor;
        char *separator = strpbrk(cursor, "\t\n");
        if (separator == NULL && i + 1 < sizeof(fields) / sizeof(fields[0])) {
            return -1;
        }
        if (separator != NULL) {
            *separator = '\0';
            cursor = separator + 1;
        }
    }

    memset(record, 0, sizeof(*record));
    snprintf(record->workspace_id, sizeof(record->workspace_id), "%s",
             fields[0]);
    snprintf(record->key_id, sizeof(record->key_id), "%s", fields[1]);
    snprintf(record->fingerprint, sizeof(record->fingerprint), "%s",
             fields[2]);
    snprintf(record->label, sizeof(record->label), "%s", fields[3]);
    record->capabilities = (cubicle_capability_mask_t)strtoull(fields[4],
                                                               NULL, 10);
    record->created_at_ms = strtoull(fields[5], NULL, 10);
    record->revoked_at_ms = strtoull(fields[6], NULL, 10);
    snprintf(record->public_key_hex, sizeof(record->public_key_hex), "%s",
             fields[7]);
    return record->workspace_id[0] != '\0' && record->key_id[0] != '\0'
               ? 0
               : -1;
}

static int write_workspace_key_record(FILE *file,
                                      const workspace_key_record_t *record)
{
    return fprintf(file, "%s\t%s\t%s\t%s\t%llu\t%llu\t%llu\t%s\n",
                   record->workspace_id, record->key_id, record->fingerprint,
                   record->label, (unsigned long long)record->capabilities,
                   (unsigned long long)record->created_at_ms,
                   (unsigned long long)record->revoked_at_ms,
                   record->public_key_hex) < 0
               ? -1
               : 0;
}

static int workspace_key_info_json(const workspace_key_record_t *record,
                                   char *buffer,
                                   size_t buffer_size)
{
    char escaped_fingerprint[CUBICLE_NAME_MAX * 2];
    char escaped_label[CUBICLE_KEY_LABEL_MAX * 2];
    if (cubicle_json_escape(escaped_fingerprint, sizeof(escaped_fingerprint),
                            record->fingerprint) < 0 ||
        cubicle_json_escape(escaped_label, sizeof(escaped_label),
                            record->label) < 0) {
        return -1;
    }

    int length = snprintf(buffer, buffer_size,
                          "{\"key_id\":\"%s\",\"fingerprint\":\"%s\",\"label\":\"%s\",\"capabilities\":%llu,\"created_at_ms\":%llu,\"revoked_at_ms\":%llu}",
                          record->key_id, escaped_fingerprint, escaped_label,
                          (unsigned long long)record->capabilities,
                          (unsigned long long)record->created_at_ms,
                          (unsigned long long)record->revoked_at_ms);
    if (length < 0 || (size_t)length >= buffer_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static cubicle_capability_mask_t role_owner_capabilities(void)
{
    return CUBICLE_CAP_WORKSPACE_READ |
           CUBICLE_CAP_WORKSPACE_RENAME |
           CUBICLE_CAP_WORKSPACE_STOP |
           CUBICLE_CAP_WORKSPACE_DELETE |
           CUBICLE_CAP_WORKSPACE_MANAGE_KEYS |
           CUBICLE_CAP_PROCESS_START |
           CUBICLE_CAP_PROCESS_READ |
           CUBICLE_CAP_PROCESS_OBSERVE |
           CUBICLE_CAP_PROCESS_INPUT |
           CUBICLE_CAP_PROCESS_SIGNAL |
           CUBICLE_CAP_PROCESS_REMOVE |
           CUBICLE_CAP_EVENTS_READ;
}

static int connection_is_same_uid(const manager_connection_t *connection)
{
    return connection != NULL && connection->has_peer_credentials &&
           connection->peer_uid == geteuid();
}

static int connection_is_manager_owner(const manager_connection_t *connection)
{
    return connection_is_same_uid(connection);
}

static int connection_has_workspace_capability(const manager_state_t *state,
                                               const char *workspace_id,
                                               const manager_connection_t *connection,
                                               cubicle_capability_mask_t required)
{
    if (connection_is_manager_owner(connection)) {
        return 1;
    }
    if (state == NULL || workspace_id == NULL || connection == NULL ||
        !connection->authenticated ||
        connection->session.client_key_id[0] == '\0') {
        return 0;
    }
    FILE *file = open_state_file_for_read(state, "workspace-keys.tsv");
    if (file == NULL) {
        return 0;
    }
    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        workspace_key_record_t record;
        if (parse_workspace_key_record(line, &record) == 0 &&
            strcmp(record.workspace_id, workspace_id) == 0 &&
            strcmp(record.key_id, connection->session.client_key_id) == 0 &&
            record.revoked_at_ms == 0) {
            int allowed = (record.capabilities & required) == required;
            fclose(file);
            return allowed;
        }
    }
    fclose(file);
    return 0;
}

static int append_workspace_key_record(const manager_state_t *state,
                                       const workspace_key_record_t *record)
{
    char line[1024];
    int line_length = snprintf(
        line, sizeof(line), "%s\t%s\t%s\t%s\t%llu\t%llu\t%llu\t%s\n",
        record->workspace_id, record->key_id, record->fingerprint,
        record->label, (unsigned long long)record->capabilities,
        (unsigned long long)record->created_at_ms,
        (unsigned long long)record->revoked_at_ms,
        record->public_key_hex);
    if (line_length < 0 || (size_t)line_length >= sizeof(line)) {
        errno = ENOSPC;
        return -1;
    }
    return append_line(state, "workspace-keys.tsv", line);
}

static int build_workspace_key_record_from_public_hex(
    workspace_key_record_t *record,
    const char *workspace_id,
    const char *public_key_hex,
    const char *label,
    cubicle_capability_mask_t capabilities,
    uint64_t created_at_ms)
{
    size_t public_key_hex_length = strlen(public_key_hex);
    if (public_key_hex_length == 0 ||
        (public_key_hex_length % 2) != 0 ||
        public_key_hex_length >= sizeof(record->public_key_hex)) {
        errno = EINVAL;
        return -1;
    }
    unsigned char public_key[sizeof(record->public_key_hex) / 2];
    size_t public_key_length = public_key_hex_length / 2;
    char fingerprint[CUBICLE_AUTH_FINGERPRINT_LENGTH];
    char key_id[CUBICLE_ID_STRING_LENGTH];
    for (size_t i = 0; i < public_key_length; ++i) {
        char byte_hex[3] = {public_key_hex[i * 2], public_key_hex[i * 2 + 1],
                            '\0'};
        char *end = NULL;
        errno = 0;
        unsigned long value = strtoul(byte_hex, &end, 16);
        if (errno != 0 || end == byte_hex || *end != '\0' || value > 0xff) {
            errno = EINVAL;
            return -1;
        }
        public_key[i] = (unsigned char)value;
    }
    if (cubicle_auth_key_fingerprint(public_key, public_key_length, key_id,
                                     fingerprint) < 0) {
        return -1;
    }

    memset(record, 0, sizeof(*record));
    snprintf(record->workspace_id, sizeof(record->workspace_id), "%s",
             workspace_id);
    snprintf(record->key_id, sizeof(record->key_id), "%s", key_id);
    snprintf(record->fingerprint, sizeof(record->fingerprint), "%s",
             fingerprint);
    snprintf(record->label, sizeof(record->label), "%s",
             label == NULL ? "" : label);
    snprintf(record->public_key_hex, sizeof(record->public_key_hex), "%s",
             public_key_hex);
    record->capabilities = capabilities;
    record->created_at_ms = created_at_ms;
    record->revoked_at_ms = 0;
    return 0;
}

static int update_workspace_key_capabilities(const manager_state_t *state,
                                             const char *workspace_id,
                                             const char *key_id,
                                             cubicle_capability_mask_t capabilities,
                                             int *found)
{
    *found = 0;
    char path[PATH_MAX];
    char temp_path[PATH_MAX];
    if (state_path(path, state, "workspace-keys.tsv") < 0) {
        return -1;
    }
    int length = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *input = fopen(path, "r");
    FILE *output = fopen(temp_path, "w");
    if (output == NULL) {
        if (input != NULL) {
            fclose(input);
        }
        return -1;
    }
    if (input != NULL) {
        char line[1024];
        while (fgets(line, sizeof(line), input) != NULL) {
            workspace_key_record_t record;
            if (parse_workspace_key_record(line, &record) == 0) {
                if (strcmp(record.workspace_id, workspace_id) == 0 &&
                    strcmp(record.key_id, key_id) == 0) {
                    record.capabilities = capabilities;
                    *found = 1;
                }
                if (write_workspace_key_record(output, &record) < 0) {
                    fclose(input);
                    fclose(output);
                    return -1;
                }
            }
        }
        fclose(input);
    }
    if (fclose(output) != 0) {
        return -1;
    }
    return rename(temp_path, path);
}

static int update_workspace_key_revocation(const manager_state_t *state,
                                           const char *workspace_id,
                                           const char *key_id,
                                           uint64_t revoked_at_ms,
                                           int *found)
{
    *found = 0;
    char path[PATH_MAX];
    char temp_path[PATH_MAX];
    if (state_path(path, state, "workspace-keys.tsv") < 0) {
        return -1;
    }
    int length = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *input = fopen(path, "r");
    FILE *output = fopen(temp_path, "w");
    if (output == NULL) {
        if (input != NULL) {
            fclose(input);
        }
        return -1;
    }
    if (input != NULL) {
        char line[1024];
        while (fgets(line, sizeof(line), input) != NULL) {
            workspace_key_record_t record;
            if (parse_workspace_key_record(line, &record) == 0) {
                if (strcmp(record.workspace_id, workspace_id) == 0 &&
                    strcmp(record.key_id, key_id) == 0) {
                    record.revoked_at_ms = revoked_at_ms;
                    *found = 1;
                }
                if (write_workspace_key_record(output, &record) < 0) {
                    fclose(input);
                    fclose(output);
                    return -1;
                }
            }
        }
        fclose(input);
    }
    if (fclose(output) != 0) {
        return -1;
    }
    return rename(temp_path, path);
}

static int controller_line_request(const char *socket_path,
                                   const char *command,
                                   char *response,
                                   size_t response_size)
{
    if (response == NULL || response_size == 0 ||
        strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = EINVAL;
        return -1;
    }
    response[0] = '\0';

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }

    if (cubicle_write_all(fd, command, strlen(command)) < 0 ||
        cubicle_write_all(fd, "\n", 1) < 0 ||
        shutdown(fd, SHUT_WR) < 0) {
        close(fd);
        return -1;
    }

    size_t used = 0;
    while (used + 1 < response_size) {
        ssize_t nread = read(fd, response + used, response_size - used - 1);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (nread == 0) {
            break;
        }
        used += (size_t)nread;
    }
    response[used] = '\0';
    close(fd);
    return strncmp(response, "ok", 2) == 0 ? 0 : -1;
}

static int signal_controller(const cubicle_process_record_t *process,
                             int signal_number)
{
    char command[64];
    int length = snprintf(command, sizeof(command), "signal %d",
                          signal_number);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char response[256];
    return controller_line_request(process->control_socket, command, response,
                                   sizeof(response));
}

static int terminate_controller(const cubicle_process_record_t *process)
{
    char response[256];
    return controller_line_request(process->control_socket, "terminate",
                                   response, sizeof(response));
}

static int controller_status_state(const cubicle_process_record_t *process,
                                   char *state,
                                   size_t state_size)
{
    char response[512];
    if (controller_line_request(process->control_socket, "status", response,
                                sizeof(response)) < 0) {
        return -1;
    }
    const char *state_start = strstr(response, "state=");
    if (state_start == NULL) {
        errno = EPROTO;
        return -1;
    }
    state_start += strlen("state=");
    size_t length = strcspn(state_start, " \n");
    if (length == 0 || length >= state_size) {
        errno = EPROTO;
        return -1;
    }
    memcpy(state, state_start, length);
    state[length] = '\0';
    if (strcmp(state, "exited") == 0) {
        snprintf(state, state_size, "completed");
    }
    return 0;
}

static int process_is_terminal_state(const char *state)
{
    return strcmp(state, "completed") == 0 ||
           strcmp(state, "failed") == 0 ||
           strcmp(state, "lost") == 0 ||
           strcmp(state, "removed") == 0 ||
           strcmp(state, "exited") == 0;
}

static int channel_mask_string(uint64_t channels, char *buffer,
                               size_t buffer_size)
{
    const struct {
        uint64_t bit;
        const char *name;
    } entries[] = {
        {CUBICLE_CHANNEL_STDIN, "stdin"},
        {CUBICLE_CHANNEL_STDOUT, "stdout"},
        {CUBICLE_CHANNEL_STDERR, "stderr"},
        {CUBICLE_CHANNEL_TTY, "tty"},
    };

    size_t used = 0;
    buffer[0] = '\0';
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if ((channels & entries[i].bit) == 0) {
            continue;
        }
        int written = snprintf(buffer + used, buffer_size - used, "%s%s",
                               used == 0 ? "" : ",", entries[i].name);
        if (written < 0 || (size_t)written >= buffer_size - used) {
            errno = ENOSPC;
            return -1;
        }
        used += (size_t)written;
    }
    return used == 0 ? -1 : 0;
}

static int command_workspace_create(const manager_state_t *state, int argc,
                                    char **argv)
{
    const char *name = NULL;
    const char *directory_arg = NULL;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            directory_arg = argv[++i];
        } else if (name == NULL) {
            name = argv[i];
        } else {
            fprintf(stderr, "Unknown workspace create option: %s\n", argv[i]);
            return 2;
        }
    }

    if (validate_field(name, "workspace name") < 0) {
        return 2;
    }

    char directory[CUBICLE_PATH_MAX];
    if (resolve_directory_path(directory_arg, directory) < 0 ||
        validate_field(directory, "workspace directory") < 0) {
        fprintf(stderr, "Invalid workspace directory: %s\n",
                directory_arg == NULL ? "." : directory_arg);
        return 2;
    }

    int lock_fd = lock_state(state);
    if (lock_fd < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    if (workspace_name_exists(state, name)) {
        fprintf(stderr, "Workspace already exists: %s\n", name);
        unlock_state(lock_fd);
        return 1;
    }

    char id[CUBICLE_MANAGER_ID_LENGTH + 1];
    if (cubicle_generate_hex_id(id, sizeof(id)) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    char line[CUBICLE_PATH_MAX + 256];
    int length = snprintf(line, sizeof(line), "%s\t%s\t%s\n", id, name,
                          directory);
    if (length < 0 || (size_t)length >= sizeof(line) ||
        append_line(state, "workspaces.tsv", line) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    unlock_state(lock_fd);
    printf("workspace id=%s name=%s directory=%s\n", id, name, directory);
    return 0;
}

static int command_workspace_list(const manager_state_t *state)
{
    FILE *file = open_state_file_for_read(state, "workspaces.tsv");
    if (file == NULL) {
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_workspace_record_t record;
        if (cubicle_parse_workspace_record(line, &record) == 0) {
            printf("%s\t%s\t%s\n", record.id, record.name,
                   record.directory);
        }
    }

    fclose(file);
    return 0;
}

static int build_argv_json(char *buffer, size_t buffer_size, char **argv)
{
    cubicle_json_builder_t builder = {0};
    if (cubicle_json_builder_append(&builder, "[") < 0) {
        cubicle_json_builder_cleanup(&builder);
        return -1;
    }
    for (size_t i = 0; argv != NULL && argv[i] != NULL; ++i) {
        if ((i > 0 && cubicle_json_builder_append(&builder, ",") < 0) ||
            cubicle_json_builder_append_string(&builder, argv[i]) < 0) {
            cubicle_json_builder_cleanup(&builder);
            return -1;
        }
    }
    if (cubicle_json_builder_append(&builder, "]") < 0 ||
        builder.length >= buffer_size) {
        cubicle_json_builder_cleanup(&builder);
        errno = ENOSPC;
        return -1;
    }
    snprintf(buffer, buffer_size, "%s", builder.data);
    cubicle_json_builder_cleanup(&builder);
    return 0;
}

static void free_argv_copy(char **argv)
{
    if (argv == NULL) {
        return;
    }
    for (size_t i = 0; argv[i] != NULL; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static char **argv_copy_from_json(const char *argv_json)
{
    cubicle_json_doc_t document;
    if (argv_json == NULL || argv_json[0] == '\0' ||
        cubicle_json_parse(&document, argv_json) < 0 ||
        !yyjson_is_arr(document.root)) {
        return NULL;
    }

    size_t argc = yyjson_arr_size(document.root);
    if (argc == 0 || argc > CUBICLE_JSON_MAX_ARGC) {
        cubicle_json_cleanup(&document);
        return NULL;
    }

    char **argv = calloc(argc + 1, sizeof(*argv));
    if (argv == NULL) {
        cubicle_json_cleanup(&document);
        return NULL;
    }

    for (size_t i = 0; i < argc; ++i) {
        yyjson_val *item = yyjson_arr_get(document.root, i);
        if (!yyjson_is_str(item) || yyjson_get_len(item) == 0 ||
            yyjson_get_len(item) >= PATH_MAX) {
            free_argv_copy(argv);
            cubicle_json_cleanup(&document);
            return NULL;
        }
        argv[i] = strdup(yyjson_get_str(item));
        if (argv[i] == NULL) {
            free_argv_copy(argv);
            cubicle_json_cleanup(&document);
            return NULL;
        }
    }

    cubicle_json_cleanup(&document);
    return argv;
}

static int append_process_record(const manager_state_t *state,
                                 const char *process_id,
                                 const char *workspace_id,
                                 const char *friendly_name,
                                 const char *mode,
                                 const char *process_state,
                                 const char *controller_id,
                                 const char *control_socket,
                                 const char *cwd,
                                 int saved,
                                 const char *argv_json,
                                 int restart,
                                 const char *stdin_policy,
                                 const char *workspace_name,
                                 const char *workspace_directory)
{
    char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    int length = snprintf(line, sizeof(line),
                          "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%d\t%s\t%s\t%s\n",
                          process_id, workspace_id, friendly_name, mode,
                          process_state, controller_id, control_socket,
                          cwd == NULL ? "" : cwd, saved ? 1 : 0,
                          argv_json == NULL ? "" : argv_json,
                          restart ? 1 : 0,
                          stdin_policy == NULL ? "open" : stdin_policy,
                          workspace_name == NULL ? "" : workspace_name,
                          workspace_directory == NULL ? "" :
                                                         workspace_directory);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return append_line(state, "processes.tsv", line);
}

typedef struct process_record_update {
    const char *state;
    int has_saved;
    int saved;
    const char *friendly_name;
    int has_restart;
    int restart;
} process_record_update_t;

static int write_process_record(FILE *output,
                                const cubicle_process_record_t *record)
{
    return fprintf(output, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%d\t%s\t%s\t%s\n",
                   record->process_id, record->workspace_id,
                   record->friendly_name, record->mode, record->state,
                   record->controller_id, record->control_socket,
                   record->cwd, record->saved ? 1 : 0,
                   record->argv_json, record->restart ? 1 : 0,
                   record->stdin_policy, record->workspace_name,
                   record->workspace_directory) < 0
               ? -1
               : 0;
}

static int rewrite_process_records(const manager_state_t *state,
                                   const char *process_id,
                                   const process_record_update_t *update,
                                   int remove_record,
                                   int *found)
{
    *found = 0;
    char path[PATH_MAX];
    char temp_path[PATH_MAX];
    if (state_path(path, state, "processes.tsv") < 0) {
        return -1;
    }
    int length = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *input = fopen(path, "r");
    FILE *output = fopen(temp_path, "w");
    if (output == NULL) {
        if (input != NULL) {
            fclose(input);
        }
        return -1;
    }

    if (input != NULL) {
        char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
        while (fgets(line, sizeof(line), input) != NULL) {
            cubicle_process_record_t record;
            if (cubicle_parse_process_record(line, &record) != 0) {
                continue;
            }
            if (strcmp(record.process_id, process_id) == 0) {
                *found = 1;
                if (remove_record) {
                    continue;
                }
                if (update != NULL && update->state != NULL) {
                    snprintf(record.state, sizeof(record.state), "%s",
                             update->state);
                }
                if (update != NULL && update->has_saved) {
                    record.saved = update->saved;
                }
                if (update != NULL && update->friendly_name != NULL) {
                    snprintf(record.friendly_name, sizeof(record.friendly_name),
                             "%s", update->friendly_name);
                }
                if (update != NULL && update->has_restart) {
                    record.restart = update->restart;
                }
            }
            if (write_process_record(output, &record) < 0) {
                fclose(input);
                fclose(output);
                return -1;
            }
        }
        fclose(input);
    }

    if (fclose(output) != 0) {
        return -1;
    }
    return rename(temp_path, path);
}

static int remove_tree(const char *path)
{
    struct stat status;
    if (lstat(path, &status) < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (!S_ISDIR(status.st_mode)) {
        return unlink(path);
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        int length = snprintf(child, sizeof(child), "%s/%s", path,
                              entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(child) ||
            remove_tree(child) < 0) {
            int saved_errno = errno;
            closedir(dir);
            errno = saved_errno;
            return -1;
        }
    }

    if (closedir(dir) != 0) {
        return -1;
    }
    return rmdir(path);
}

static int remove_tree_if_exists(const char *path)
{
    if (remove_tree(path) == 0) {
        return 0;
    }
    return errno == ENOENT ? 0 : -1;
}

static int process_events_path(char path[PATH_MAX], const manager_state_t *state,
                               const char *process_id)
{
    int result = snprintf(path, PATH_MAX, "%s/controllers/%s/events.log",
                          state->log_dir, process_id);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (access(path, F_OK) == 0) {
        return 0;
    }

    result = snprintf(path, PATH_MAX, "%s/controllers/%s/events.log",
                      state->dir, process_id);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int process_output_path(char path[PATH_MAX],
                               const manager_state_t *state,
                               const char *process_id,
                               const char *file_name)
{
    int result = snprintf(path, PATH_MAX, "%s/controllers/%s/%s",
                          state->log_dir, process_id, file_name);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (access(path, F_OK) == 0) {
        return 0;
    }

    result = snprintf(path, PATH_MAX, "%s/controllers/%s/%s",
                      state->dir, process_id, file_name);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static cubicle_cursor_record_t *cursor_for_process(cubicle_cursor_record_t *cursors,
                                                   size_t cursor_count,
                                                   const char *process_id)
{
    for (size_t i = 0; i < cursor_count; ++i) {
        if (strcmp(cursors[i].process_id, process_id) == 0) {
            return &cursors[i];
        }
    }

    return NULL;
}

static int update_cursor(cubicle_cursor_record_t *cursors, size_t *cursor_count,
                         const char *process_id, long long sequence,
                         long long offset)
{
    for (size_t i = 0; i < *cursor_count; ++i) {
        if (strcmp(cursors[i].process_id, process_id) == 0) {
            if (sequence > cursors[i].sequence) {
                cursors[i].sequence = sequence;
            }
            if (offset > cursors[i].offset) {
                cursors[i].offset = offset;
            }
            return 0;
        }
    }

    if (*cursor_count >= 256) {
        errno = ENOSPC;
        return -1;
    }

    snprintf(cursors[*cursor_count].process_id,
             sizeof(cursors[*cursor_count].process_id), "%s", process_id);
    cursors[*cursor_count].sequence = sequence;
    cursors[*cursor_count].offset = offset;
    ++(*cursor_count);
    return 0;
}

static int load_cursors(const manager_state_t *state,
                        cubicle_cursor_record_t *cursors,
                        size_t *cursor_count)
{
    *cursor_count = 0;

    FILE *file = open_state_file_for_read(state, "cursors.tsv");
    if (file == NULL) {
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), file) != NULL && *cursor_count < 256) {
        cubicle_cursor_record_t record;
        if (cubicle_parse_cursor_record(line, &record) == 0) {
            cursors[*cursor_count] = record;
            ++(*cursor_count);
        }
    }

    fclose(file);
    return 0;
}

static int save_cursors(const manager_state_t *state,
                        cubicle_cursor_record_t *cursors,
                        size_t cursor_count)
{
    clear_manager_error_detail();
    char path[PATH_MAX];
    char temp_path[PATH_MAX];
    if (state_path(path, state, "cursors.tsv") < 0) {
        return -1;
    }

    int result = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (result < 0 || (size_t)result >= sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *file = fopen(temp_path, "w");
    if (file == NULL) {
        return set_manager_path_error("open", temp_path, errno);
    }

    for (size_t i = 0; i < cursor_count; ++i) {
        if (fprintf(file, "%s\t%lld\t%lld\n", cursors[i].process_id,
                    cursors[i].sequence, cursors[i].offset) < 0) {
            int saved_errno = errno;
            fclose(file);
            return set_manager_path_error("write", temp_path, saved_errno);
        }
    }

    if (fclose(file) != 0) {
        return set_manager_path_error("close", temp_path, errno);
    }

    if (rename(temp_path, path) < 0) {
        return set_manager_path_error("rename", temp_path, errno);
    }
    return 0;
}

static int command_process_register(const manager_state_t *state, int argc,
                                    char **argv)
{
    const char *workspace = NULL;
    const char *friendly_name = NULL;
    const char *mode = NULL;
    const char *controller_id = NULL;
    const char *control_socket = NULL;
    const char *requested_process_id = NULL;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            workspace = argv[++i];
        } else if (strcmp(argv[i], "--friendly-name") == 0 && i + 1 < argc) {
            friendly_name = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "--controller-id") == 0 && i + 1 < argc) {
            controller_id = argv[++i];
        } else if (strcmp(argv[i], "--control-socket") == 0 && i + 1 < argc) {
            control_socket = argv[++i];
        } else if (strcmp(argv[i], "--process-id") == 0 && i + 1 < argc) {
            requested_process_id = argv[++i];
        } else {
            fprintf(stderr, "Unknown process register option: %s\n", argv[i]);
            return 2;
        }
    }

    if (validate_field(workspace, "workspace") < 0 ||
        validate_field(friendly_name, "friendly name") < 0 ||
        validate_field(mode, "mode") < 0 ||
        validate_field(controller_id, "controller id") < 0 ||
        validate_field(control_socket, "control socket") < 0) {
        return 2;
    }

    if (strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_STREAM)) != 0 &&
        strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_TTY)) != 0 &&
        strcmp(mode, cubicle_process_mode_name(
                         CUBICLE_PROCESS_TTY_CAPTURED_STDERR)) != 0) {
        fprintf(stderr, "Only stream, tty, and term modes can be registered currently\n");
        return 2;
    }

    int lock_fd = lock_state(state);
    if (lock_fd < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    cubicle_workspace_record_t workspace_record;
    if (find_workspace(state, workspace, &workspace_record) < 0) {
        fprintf(stderr, "Unknown workspace: %s\n", workspace);
        unlock_state(lock_fd);
        return 1;
    }

    char process_id[CUBICLE_MANAGER_ID_LENGTH + 1];
    if (requested_process_id != NULL) {
        if (validate_field(requested_process_id, "process id") < 0 ||
            strlen(requested_process_id) > CUBICLE_MANAGER_ID_LENGTH) {
            unlock_state(lock_fd);
            return 2;
        }
        snprintf(process_id, sizeof(process_id), "%s", requested_process_id);
    } else if (cubicle_generate_hex_id(process_id, sizeof(process_id)) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    int process_id_conflict = 0;
    int friendly_name_conflict = 0;
    if (process_conflict_exists(state, workspace_record.id, process_id,
                                friendly_name, &process_id_conflict,
                                &friendly_name_conflict) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    if (process_id_conflict) {
        fprintf(stderr, "Process already exists: %s\n", process_id);
        unlock_state(lock_fd);
        return 1;
    }

    if (friendly_name_conflict) {
        fprintf(stderr, "Process friendly name already exists in workspace: %s\n",
                friendly_name);
        unlock_state(lock_fd);
        return 1;
    }

    if (append_process_record(state, process_id, workspace_record.id,
                              friendly_name, mode, "running", controller_id,
                              control_socket, workspace_record.directory, 0,
                              "", 0, "open", workspace_record.name,
                              workspace_record.directory) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    unlock_state(lock_fd);
    printf("process id=%s workspace_id=%s friendly_name=%s controller_id=%s control_socket=%s\n",
           process_id, workspace_record.id, friendly_name, controller_id,
           control_socket);
    return 0;
}

static int event_log_has_exit(const char *events_path)
{
    FILE *file = fopen(events_path, "r");
    if (file == NULL) {
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "type=process_exited") != NULL) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static int process_event_log_has_exit(const manager_state_t *state,
                                      const char *process_id)
{
    char events_path[PATH_MAX];
    if (process_events_path(events_path, state, process_id) < 0) {
        return 0;
    }

    FILE *file = fopen(events_path, "r");
    if (file == NULL) {
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "type=process_exited") != NULL) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static int process_observed_state(const manager_state_t *state,
                                  const cubicle_process_record_t *process,
                                  char *state_out,
                                  size_t state_out_size)
{
    snprintf(state_out, state_out_size, "%s", process->state);
    if (process_is_terminal_state(state_out)) {
        return 0;
    }
    if (controller_status_state(process, state_out, state_out_size) == 0) {
        return 0;
    }
    if (process_event_log_has_exit(state, process->process_id)) {
        snprintf(state_out, state_out_size, "completed");
    } else {
        snprintf(state_out, state_out_size, "lost");
    }
    return 0;
}

static int process_observed_terminal_state(const manager_state_t *state,
                                           const cubicle_process_record_t *process,
                                           char *state_out,
                                           size_t state_out_size)
{
    if (process_observed_state(state, process, state_out, state_out_size) < 0) {
        return 0;
    }
    return process_is_terminal_state(state_out);
}

static void refresh_observed_process_state(const manager_state_t *state,
                                           cubicle_process_record_t *process)
{
    char latest_state[32];
    if (process_observed_state(state, process, latest_state,
                               sizeof(latest_state)) == 0) {
        snprintf(process->state, sizeof(process->state), "%s", latest_state);
    }
}

typedef struct {
    char process_id[128];
    char state[32];
} process_state_update_t;

static int read_metadata_field(const char *metadata_path, const char *field,
                               char *value, size_t value_size);
static int launch_controller(const manager_state_t *state,
                             const char *controller_state,
                             const char *controller_log,
                             const char *control_socket,
                             const char *mode,
                             const char *stdin_policy,
                             const char *cwd,
                             char **command);

static int reconcile_process_records(const manager_state_t *state)
{
    int lock_fd = lock_state(state);
    if (lock_fd < 0) {
        return -1;
    }

    FILE *file = open_state_file_for_read(state, "processes.tsv");
    process_state_update_t *updates = NULL;
    size_t update_count = 0;
    size_t update_capacity = 0;
    int result = 0;

    if (file != NULL) {
        char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
        while (fgets(line, sizeof(line), file) != NULL) {
            cubicle_process_record_t process;
            char latest_state[32];
            if (cubicle_parse_process_record(line, &process) != 0 ||
                process_is_terminal_state(process.state) ||
                process_observed_state(state, &process, latest_state,
                                       sizeof(latest_state)) < 0 ||
                strcmp(latest_state, process.state) == 0 ||
                !process_is_terminal_state(latest_state)) {
                continue;
            }

            if (update_count == update_capacity) {
                size_t new_capacity = update_capacity == 0
                                          ? 8
                                          : update_capacity * 2;
                process_state_update_t *new_updates =
                    realloc(updates, new_capacity * sizeof(*updates));
                if (new_updates == NULL) {
                    result = -1;
                    break;
                }
                updates = new_updates;
                update_capacity = new_capacity;
            }
            snprintf(updates[update_count].process_id,
                     sizeof(updates[update_count].process_id), "%s",
                     process.process_id);
            snprintf(updates[update_count].state,
                     sizeof(updates[update_count].state), "%s",
                     latest_state);
            ++update_count;
        }
        if (fclose(file) != 0 && result == 0) {
            result = -1;
        }
    }

    for (size_t i = 0; result == 0 && i < update_count; ++i) {
        int found = 0;
        process_record_update_t update = {.state = updates[i].state};
        if (rewrite_process_records(state, updates[i].process_id,
                                    &update, 0, &found) < 0) {
            result = -1;
            break;
        }
    }

    free(updates);
    unlock_state(lock_fd);
    return result;
}

static int wait_for_controller_ready(const char *control_socket,
                                     const char *metadata_path,
                                     const char *events_path,
                                     char *process_state,
                                     size_t process_state_size)
{
    for (int i = 0; i < 100; ++i) {
        struct stat socket_stat;
        struct stat metadata_stat;
        if (stat(control_socket, &socket_stat) == 0 &&
            S_ISSOCK(socket_stat.st_mode) &&
            stat(metadata_path, &metadata_stat) == 0 &&
            metadata_stat.st_size > 0) {
            snprintf(process_state, process_state_size, "running");
            return 0;
        }

        if (stat(metadata_path, &metadata_stat) == 0 &&
            metadata_stat.st_size > 0 &&
            event_log_has_exit(events_path)) {
            snprintf(process_state, process_state_size, "exited");
            return 0;
        }

        struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&delay, NULL);
    }

    errno = ETIMEDOUT;
    return -1;
}

static int autostart_restart_processes(const manager_state_t *state)
{
    int lock_fd = lock_state(state);
    if (lock_fd < 0) {
        return -1;
    }

    FILE *file = open_state_file_for_read(state, "processes.tsv");
    cubicle_process_record_t *records = NULL;
    size_t record_count = 0;
    size_t record_capacity = 0;
    int result = 0;

    if (file != NULL) {
        char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
        while (fgets(line, sizeof(line), file) != NULL) {
            cubicle_process_record_t process;
            if (cubicle_parse_process_record(line, &process) != 0 ||
                !process.restart) {
                continue;
            }
            char latest_state[32];
            if (process_observed_state(state, &process, latest_state,
                                       sizeof(latest_state)) == 0 &&
                !process_is_terminal_state(latest_state)) {
                continue;
            }
            if (record_count == record_capacity) {
                size_t new_capacity = record_capacity == 0
                                          ? 8
                                          : record_capacity * 2;
                cubicle_process_record_t *new_records =
                    realloc(records, new_capacity * sizeof(*records));
                if (new_records == NULL) {
                    result = -1;
                    break;
                }
                records = new_records;
                record_capacity = new_capacity;
            }
            records[record_count++] = process;
        }
        if (fclose(file) != 0 && result == 0) {
            result = -1;
        }
    }

    for (size_t i = 0; result == 0 && i < record_count; ++i) {
        cubicle_process_record_t *old = &records[i];
        cubicle_workspace_record_t workspace;
        if (find_workspace(state, old->workspace_id, &workspace) < 0) {
            const char *workspace_name = old->workspace_name[0] == '\0'
                                             ? old->workspace_id
                                             : old->workspace_name;
            const char *workspace_directory =
                old->workspace_directory[0] == '\0'
                    ? old->cwd
                    : old->workspace_directory;
            if (validate_field(workspace_name, "workspace name") < 0 ||
                validate_field(workspace_directory, "workspace directory") <
                    0 ||
                append_workspace_record_with_id(state, old->workspace_id,
                                                workspace_name,
                                                workspace_directory) < 0 ||
                find_workspace(state, old->workspace_id, &workspace) < 0) {
                result = -1;
                break;
            }
        }

        char **command_argv = argv_copy_from_json(old->argv_json);
        if (command_argv == NULL) {
            process_record_update_t update = {.state = "lost"};
            int found = 0;
            if (rewrite_process_records(state, old->process_id, &update, 0,
                                        &found) < 0) {
                result = -1;
                break;
            }
            continue;
        }

        char process_id[CUBICLE_MANAGER_ID_LENGTH + 1];
        char controller_state[PATH_MAX];
        char controller_log[PATH_MAX];
        char control_socket[PATH_MAX];
        char metadata_path[PATH_MAX];
        char events_path[PATH_MAX];
        if (cubicle_generate_hex_id(process_id, sizeof(process_id)) < 0 ||
            controller_state_path(controller_state, state, process_id) < 0 ||
            controller_log_path(controller_log, state, process_id) < 0 ||
            controller_socket_path(control_socket, state, process_id) < 0) {
            free_argv_copy(command_argv);
            result = -1;
            break;
        }

        int metadata_length = snprintf(metadata_path, sizeof(metadata_path),
                                       "%s/metadata", controller_state);
        int events_length = snprintf(events_path, sizeof(events_path),
                                     "%s/events.log", controller_log);
        if (metadata_length < 0 ||
            (size_t)metadata_length >= sizeof(metadata_path) ||
            events_length < 0 ||
            (size_t)events_length >= sizeof(events_path) ||
            ensure_parent_directory(control_socket) < 0 ||
            cubicle_mkdir_p(controller_log) < 0) {
            free_argv_copy(command_argv);
            result = -1;
            break;
        }

        const char *cwd = old->cwd[0] == '\0' ? workspace.directory : old->cwd;
        const char *stdin_policy =
            old->stdin_policy[0] == '\0' ? "open" : old->stdin_policy;
        char process_state[32];
        if (launch_controller(state, controller_state, controller_log,
                              control_socket, old->mode, stdin_policy, cwd,
                              command_argv) < 0 ||
            wait_for_controller_ready(control_socket, metadata_path,
                                      events_path, process_state,
                                      sizeof(process_state)) < 0) {
            free_argv_copy(command_argv);
            result = -1;
            break;
        }
        free_argv_copy(command_argv);

        char controller_id[CUBICLE_MANAGER_ID_LENGTH + 1];
        int found = 0;
        if (read_metadata_field(metadata_path, "controller_id", controller_id,
                                sizeof(controller_id)) < 0 ||
            rewrite_process_records(state, old->process_id, NULL, 1,
                                    &found) < 0 ||
            append_process_record(state, process_id, old->workspace_id,
                                  old->friendly_name, old->mode,
                                  process_state, controller_id,
                                  control_socket, cwd, old->saved,
                                  old->argv_json, old->restart, stdin_policy,
                                  workspace.name,
                                  workspace.directory) < 0) {
            result = -1;
            break;
        }

        char old_controller_state[PATH_MAX];
        char old_controller_log[PATH_MAX];
        if (controller_state_path(old_controller_state, state,
                                  old->process_id) == 0) {
            (void)remove_tree_if_exists(old_controller_state);
        }
        if (controller_log_path(old_controller_log, state,
                                old->process_id) == 0) {
            (void)remove_tree_if_exists(old_controller_log);
        }
    }

    free(records);
    unlock_state(lock_fd);
    return result;
}

static int read_metadata_field(const char *metadata_path, const char *field,
                               char *value, size_t value_size)
{
    FILE *file = fopen(metadata_path, "r");
    if (file == NULL) {
        return -1;
    }

    char prefix[64];
    int prefix_length = snprintf(prefix, sizeof(prefix), "%s=", field);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix)) {
        fclose(file);
        errno = ENAMETOOLONG;
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, prefix, (size_t)prefix_length) != 0) {
            continue;
        }

        char *start = line + prefix_length;
        start[strcspn(start, "\n")] = '\0';
        int result = snprintf(value, value_size, "%s", start);
        fclose(file);
        if (result < 0 || (size_t)result >= value_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    fclose(file);
    errno = ENOENT;
    return -1;
}

static int launch_controller(const manager_state_t *state,
                             const char *controller_state,
                             const char *controller_log,
                             const char *control_socket,
                             const char *mode,
                             const char *stdin_policy,
                             const char *cwd,
                             char **command)
{
    size_t command_count = 0;
    while (command[command_count] != NULL) {
        ++command_count;
    }

    int controller_debug_enabled =
        state->controller_debug[0] != '\0' &&
        strcmp(state->controller_debug, "none") != 0;
    size_t argv_count = 15 + (controller_debug_enabled ? 2 : 0) +
                        command_count + 1;
    char **controller_argv = calloc(argv_count, sizeof(char *));
    if (controller_argv == NULL) {
        return -1;
    }

    size_t index = 0;
    controller_argv[index++] = (char *)state->controller_bin;
    controller_argv[index++] = "--daemon";
    controller_argv[index++] = "--stdin-policy";
    controller_argv[index++] = (char *)stdin_policy;
    controller_argv[index++] = "--state-dir";
    controller_argv[index++] = (char *)controller_state;
    controller_argv[index++] = "--log-dir";
    controller_argv[index++] = (char *)controller_log;
    controller_argv[index++] = "--control-socket";
    controller_argv[index++] = (char *)control_socket;
    controller_argv[index++] = "--mode";
    controller_argv[index++] = (char *)mode;
    controller_argv[index++] = "--cwd";
    controller_argv[index++] = (char *)cwd;
    if (controller_debug_enabled) {
        controller_argv[index++] = "--debug";
        controller_argv[index++] = (char *)state->controller_debug;
    }
    controller_argv[index++] = "--";
    for (size_t i = 0; i < command_count; ++i) {
        controller_argv[index++] = command[i];
    }
    controller_argv[index] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        free(controller_argv);
        return -1;
    }

    if (pid == 0) {
        execvp(state->controller_bin, controller_argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    free(controller_argv);

    int status = 0;
    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = ECHILD;
        return -1;
    }

    return 0;
}

static int command_process_start(const manager_state_t *state, int argc,
                                 char **argv)
{
    const char *workspace = NULL;
    const char *friendly_name = NULL;
    const char *mode = "stream";
    const char *stdin_policy = "open";
    const char *directory_arg = NULL;
    int command_index = -1;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            workspace = argv[++i];
        } else if (strcmp(argv[i], "--friendly-name") == 0 && i + 1 < argc) {
            friendly_name = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "--stdin-policy") == 0 && i + 1 < argc) {
            stdin_policy = argv[++i];
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            directory_arg = argv[++i];
        } else if (strcmp(argv[i], "--") == 0 && i + 1 < argc) {
            command_index = i + 1;
            break;
        } else {
            fprintf(stderr, "Unknown process start option: %s\n", argv[i]);
            return 2;
        }
    }

    if (command_index < 0 ||
        validate_field(workspace, "workspace") < 0 ||
        validate_field(friendly_name, "friendly name") < 0 ||
        validate_field(mode, "mode") < 0 ||
        validate_field(stdin_policy, "stdin policy") < 0) {
        return 2;
    }

    if (strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_STREAM)) != 0 &&
        strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_TTY)) != 0 &&
        strcmp(mode, cubicle_process_mode_name(
                         CUBICLE_PROCESS_TTY_CAPTURED_STDERR)) != 0) {
        fprintf(stderr, "Only stream, tty, and term modes can be started currently\n");
        return 2;
    }

    if (strcmp(stdin_policy, "open") != 0 && strcmp(stdin_policy, "eof") != 0) {
        fprintf(stderr, "Unknown stdin policy: %s\n", stdin_policy);
        return 2;
    }

    int lock_fd = lock_state(state);
    if (lock_fd < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    cubicle_workspace_record_t workspace_record;
    if (find_workspace(state, workspace, &workspace_record) < 0) {
        fprintf(stderr, "Unknown workspace: %s\n", workspace);
        unlock_state(lock_fd);
        return 1;
    }

    char cwd[CUBICLE_PATH_MAX];
    if (resolve_directory_path(directory_arg != NULL ? directory_arg :
                               workspace_record.directory, cwd) < 0 ||
        validate_field(cwd, "process directory") < 0) {
        fprintf(stderr, "Invalid process directory: %s\n",
                directory_arg == NULL ? workspace_record.directory : directory_arg);
        unlock_state(lock_fd);
        return 2;
    }

    char process_id[CUBICLE_MANAGER_ID_LENGTH + 1];
    if (cubicle_generate_hex_id(process_id, sizeof(process_id)) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    int process_id_conflict = 0;
    int friendly_name_conflict = 0;
    if (process_conflict_exists(state, workspace_record.id, process_id,
                                friendly_name, &process_id_conflict,
                                &friendly_name_conflict) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    if (process_id_conflict) {
        fprintf(stderr, "Process already exists: %s\n", process_id);
        unlock_state(lock_fd);
        return 1;
    }

    if (friendly_name_conflict) {
        fprintf(stderr, "Process friendly name already exists in workspace: %s\n",
                friendly_name);
        unlock_state(lock_fd);
        return 1;
    }

    char controller_state[PATH_MAX];
    char controller_log[PATH_MAX];
    char control_socket[PATH_MAX];
    char metadata_path[PATH_MAX];
    char events_path[PATH_MAX];
    if (controller_state_path(controller_state, state, process_id) < 0 ||
        controller_log_path(controller_log, state, process_id) < 0 ||
        controller_socket_path(control_socket, state, process_id) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    if (ensure_parent_directory(control_socket) < 0 ||
        cubicle_mkdir_p(controller_log) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    int result = snprintf(metadata_path, sizeof(metadata_path), "%s/metadata",
                          controller_state);
    int events_result = snprintf(events_path, sizeof(events_path),
                                 "%s/events.log", controller_log);
    if (result < 0 || (size_t)result >= sizeof(metadata_path) ||
        events_result < 0 || (size_t)events_result >= sizeof(events_path)) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", "metadata path too long");
        unlock_state(lock_fd);
        return 1;
    }

    char process_state[32];
    if (launch_controller(state, controller_state, controller_log, control_socket, mode,
                          stdin_policy, cwd, &argv[command_index]) < 0 ||
        wait_for_controller_ready(control_socket, metadata_path, events_path,
                                  process_state,
                                  sizeof(process_state)) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    char controller_id[CUBICLE_MANAGER_ID_LENGTH + 1];
    if (read_metadata_field(metadata_path, "controller_id", controller_id,
                            sizeof(controller_id)) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    char argv_json[CUBICLE_PROCESS_ARGV_JSON_MAX];
    if (build_argv_json(argv_json, sizeof(argv_json),
                        &argv[command_index]) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    if (append_process_record(state, process_id, workspace_record.id,
                              friendly_name, mode, process_state, controller_id,
                              control_socket, cwd, 0, argv_json, 0,
                              stdin_policy, workspace_record.name,
                              workspace_record.directory) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    unlock_state(lock_fd);
    printf("process id=%s workspace_id=%s friendly_name=%s controller_id=%s control_socket=%s\n",
           process_id, workspace_record.id, friendly_name, controller_id,
           control_socket);
    return 0;
}

static int command_process_list(const manager_state_t *state, int argc, char **argv)
{
    const char *workspace = NULL;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            workspace = argv[++i];
        } else {
            fprintf(stderr, "Unknown process list option: %s\n", argv[i]);
            return 2;
        }
    }

    cubicle_workspace_record_t workspace_record;
    if (workspace != NULL && find_workspace(state, workspace, &workspace_record) < 0) {
        fprintf(stderr, "Unknown workspace: %s\n", workspace);
        return 1;
    }

    FILE *file = open_state_file_for_read(state, "processes.tsv");
    if (file == NULL) {
        return 0;
    }

    char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_process_record_t record;
        if (cubicle_parse_process_record(line, &record) != 0) {
            continue;
        }

        if (workspace != NULL &&
            strcmp(record.workspace_id, workspace_record.id) != 0) {
            continue;
        }

        printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
               record.process_id, record.workspace_id, record.friendly_name,
               record.mode, record.state, record.controller_id,
               record.control_socket, record.cwd, record.saved ? 1 : 0);
    }

    fclose(file);
    return 0;
}

static void print_process_record(const cubicle_process_record_t *record)
{
    printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
           record->process_id, record->workspace_id, record->friendly_name,
           record->mode, record->state, record->controller_id,
           record->control_socket, record->cwd, record->saved ? 1 : 0);
}

static int command_process_resolve(const manager_state_t *state, int argc,
                                   char **argv)
{
    if (argc < 1) {
        return 2;
    }

    const char *target = argv[0];
    const char *workspace = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            workspace = argv[++i];
        } else {
            fprintf(stderr, "Unknown process resolve option: %s\n", argv[i]);
            return 2;
        }
    }

    cubicle_workspace_record_t workspace_record;
    if (workspace != NULL && find_workspace(state, workspace, &workspace_record) < 0) {
        fprintf(stderr, "Unknown workspace: %s\n", workspace);
        return 1;
    }

    FILE *file = open_state_file_for_read(state, "processes.tsv");
    if (file == NULL) {
        fprintf(stderr, "Unknown process: %s\n", target);
        return 1;
    }

    cubicle_process_record_t found;
    int match_count = 0;
    char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_process_record_t record;
        if (cubicle_parse_process_record(line, &record) != 0) {
            continue;
        }

        int matches = strcmp(record.process_id, target) == 0;
        if (!matches && workspace != NULL &&
            strcmp(record.workspace_id, workspace_record.id) == 0 &&
            strcmp(record.friendly_name, target) == 0) {
            matches = 1;
        }

        if (!matches) {
            continue;
        }

        found = record;
        ++match_count;
    }

    fclose(file);

    if (match_count == 0) {
        if (workspace == NULL) {
            fprintf(stderr,
                    "Unknown process: %s (friendly names require --workspace)\n",
                    target);
        } else {
            fprintf(stderr, "Unknown process: %s\n", target);
        }
        return 1;
    }

    if (match_count > 1) {
        fprintf(stderr, "Ambiguous process: %s\n", target);
        return 1;
    }

    print_process_record(&found);
    return 0;
}

static int poll_workspace_events(const manager_state_t *state,
                                 const char *workspace,
                                 FILE *output)
{
    cubicle_workspace_record_t workspace_record;
    if (workspace != NULL && find_workspace(state, workspace, &workspace_record) < 0) {
        fprintf(stderr, "Unknown workspace: %s\n", workspace);
        return 1;
    }

    int lock_fd = lock_state(state);
    if (lock_fd < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    cubicle_cursor_record_t cursors[256];
    size_t cursor_count = 0;
    if (load_cursors(state, cursors, &cursor_count) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    FILE *processes = open_state_file_for_read(state, "processes.tsv");
    if (processes == NULL) {
        unlock_state(lock_fd);
        return 0;
    }

    char process_line[CUBICLE_PROCESS_RECORD_LINE_MAX];
    while (fgets(process_line, sizeof(process_line), processes) != NULL) {
        cubicle_process_record_t process;
        if (cubicle_parse_process_record(process_line, &process) != 0) {
            continue;
        }

        if (workspace != NULL &&
            strcmp(process.workspace_id, workspace_record.id) != 0) {
            continue;
        }

        char events_path[PATH_MAX];
        if (process_events_path(events_path, state, process.process_id) < 0) {
            continue;
        }

        FILE *events = fopen(events_path, "r");
        if (events == NULL) {
            continue;
        }

        cubicle_cursor_record_t *cursor_record =
            cursor_for_process(cursors, cursor_count, process.process_id);
        long long cursor = cursor_record == NULL ? 0 : cursor_record->sequence;
        long long cursor_offset = cursor_record == NULL ? 0 : cursor_record->offset;
        long long max_sequence = cursor;
        long long max_offset = cursor_offset;

        struct stat events_stat;
        if (fstat(fileno(events), &events_stat) == 0 &&
            cursor_offset > events_stat.st_size) {
            cursor = 0;
            cursor_offset = 0;
            max_sequence = 0;
            max_offset = 0;
        }
        if (cursor_offset > 0 &&
            fseeko(events, (off_t)cursor_offset, SEEK_SET) < 0) {
            cursor_offset = 0;
            max_offset = 0;
            rewind(events);
        }

        char event_line[1024];
        while (fgets(event_line, sizeof(event_line), events) != NULL) {
            off_t line_end_offset = ftello(events);
            long long sequence = 0;
            if (cubicle_parse_event_sequence(event_line, &sequence) < 0 ||
                sequence <= cursor) {
                if (line_end_offset >= 0 &&
                    (long long)line_end_offset > max_offset) {
                    max_offset = (long long)line_end_offset;
                }
                continue;
            }

            event_line[strcspn(event_line, "\n")] = '\0';

            char workspace_event[PATH_MAX + 1400];
            int length = snprintf(workspace_event, sizeof(workspace_event),
                                  "%s\t%s\t%s\t%s\n",
                                  process.workspace_id, process.process_id,
                                  process.friendly_name, event_line);
            if (length < 0 || (size_t)length >= sizeof(workspace_event) ||
                append_line(state, "workspace-events.log", workspace_event) < 0) {
                int saved_errno = errno;
                fclose(events);
                fclose(processes);
                manager_log_error(saved_errno);
                unlock_state(lock_fd);
                errno = saved_errno;
                return 1;
            }

            if (output != NULL) {
                fprintf(output, "%s", workspace_event);
            }
            if (sequence > max_sequence) {
                max_sequence = sequence;
            }
            if (line_end_offset >= 0 &&
                (long long)line_end_offset > max_offset) {
                max_offset = (long long)line_end_offset;
            }
        }

        fclose(events);

        if ((max_sequence > cursor || max_offset > cursor_offset) &&
            update_cursor(cursors, &cursor_count, process.process_id,
                          max_sequence, max_offset) < 0) {
            int saved_errno = errno;
            fclose(processes);
            manager_log_error(saved_errno);
            unlock_state(lock_fd);
            errno = saved_errno;
            return 1;
        }
    }

    fclose(processes);

    if (save_cursors(state, cursors, cursor_count) < 0) {
        int saved_errno = errno;
        manager_log_error(saved_errno);
        unlock_state(lock_fd);
        errno = saved_errno;
        return 1;
    }

    unlock_state(lock_fd);
    return 0;
}

static int command_events_poll(const manager_state_t *state, int argc, char **argv)
{
    const char *workspace = NULL;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            workspace = argv[++i];
        } else {
            fprintf(stderr, "Unknown events poll option: %s\n", argv[i]);
            return 2;
        }
    }

    if (poll_workspace_events(state, workspace, stdout) != 0) {
        return 1;
    }
    if (reconcile_process_records(state) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }
    return 0;
}

static int command_events_list(const manager_state_t *state, int argc, char **argv)
{
    const char *workspace = NULL;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            workspace = argv[++i];
        } else {
            fprintf(stderr, "Unknown events list option: %s\n", argv[i]);
            return 2;
        }
    }

    cubicle_workspace_record_t workspace_record;
    if (workspace != NULL && find_workspace(state, workspace, &workspace_record) < 0) {
        fprintf(stderr, "Unknown workspace: %s\n", workspace);
        return 1;
    }

    FILE *file = open_state_file_for_read(state, "workspace-events.log");
    if (file == NULL) {
        return 0;
    }

    char line[PATH_MAX + 1400];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (workspace != NULL) {
            char copy[PATH_MAX + 1400];
            snprintf(copy, sizeof(copy), "%s", line);
            char *workspace_id = strtok(copy, "\t\n");
            if (workspace_id == NULL ||
                strcmp(workspace_id, workspace_record.id) != 0) {
                continue;
            }
        }

        printf("%s", line);
    }

    fclose(file);
    return 0;
}

static int command_events_follow(const manager_state_t *state, int argc, char **argv)
{
    const char *workspace = NULL;
    int iterations = -1;
    int has_iterations = 0;
    int interval_ms = 250;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            workspace = argv[++i];
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            has_iterations = 1;
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            interval_ms = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown events follow option: %s\n", argv[i]);
            return 2;
        }
    }

    if ((has_iterations && iterations < 0) || interval_ms < 0) {
        fprintf(stderr, "events follow requires nonnegative --iterations and --interval-ms\n");
        return 2;
    }

    char *poll_args[2];
    int poll_argc = 0;
    if (workspace != NULL) {
        poll_args[poll_argc++] = "--workspace";
        poll_args[poll_argc++] = (char *)workspace;
    }

    for (int i = 0; iterations < 0 || i < iterations; ++i) {
        int result = command_events_poll(state, poll_argc, poll_args);
        if (result != 0) {
            return result;
        }

        fflush(stdout);

        if ((iterations < 0 || i + 1 < iterations) && interval_ms > 0) {
            struct timespec delay;
            delay.tv_sec = interval_ms / 1000;
            delay.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
            nanosleep(&delay, NULL);
        }
    }

    return 0;
}

static uint64_t manager_time_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static int read_all_fd(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = read(fd, cursor, length);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = ECONNRESET;
            return -1;
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int read_api_frame(int client_fd, char *request, size_t request_size)
{
    uint32_t length_network = 0;
    if (read_all_fd(client_fd, &length_network, sizeof(length_network)) < 0) {
        return -1;
    }

    uint32_t length = ntohl(length_network);
    if (length == 0 || length >= request_size ||
        length > CUBICLE_API_MAX_FRAME) {
        errno = EMSGSIZE;
        return -1;
    }

    if (read_all_fd(client_fd, request, length) < 0) {
        return -1;
    }
    request[length] = '\0';
    return 0;
}

static int write_api_frame(int client_fd, const char *response)
{
    size_t length = strlen(response);
    if (length > CUBICLE_API_MAX_FRAME) {
        errno = EMSGSIZE;
        return -1;
    }

    uint32_t length_network = htonl((uint32_t)length);
    return cubicle_write_all(client_fd, (const char *)&length_network,
                             sizeof(length_network)) == 0 &&
                   cubicle_write_all(client_fd, response, length) == 0
               ? 0
               : -1;
}

static int manager_api_error(int client_fd, const char *request_id,
                             cubicle_error_code_t code, const char *message,
                             int retryable, int system_errno)
{
    char response[2048];
    if (cubicle_rpc_error(response, sizeof(response), request_id, code,
                          message, retryable, system_errno) < 0) {
        return -1;
    }
    return write_api_frame(client_fd, response);
}

static int manager_api_success(int client_fd, const char *request_id,
                               const char *result)
{
    char response[131072];
    if (cubicle_rpc_success(response, sizeof(response), request_id,
                            result) < 0) {
        return -1;
    }
    return write_api_frame(client_fd, response);
}

static void load_peer_credentials(int client_fd, manager_connection_t *connection)
{
    memset(connection, 0, sizeof(*connection));
#ifdef SO_PEERCRED
    struct ucred credentials;
    socklen_t credentials_length = sizeof(credentials);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                   &credentials_length) == 0) {
        connection->has_peer_credentials = 1;
        connection->peer_uid = credentials.uid;
        connection->peer_gid = credentials.gid;
        connection->peer_pid = credentials.pid;
    }
#else
    (void)client_fd;
#endif
}

static int session_info_json(const manager_state_t *state,
                             const cubicle_session_info_t *session,
                             const unsigned char *resume_secret,
                             char *result,
                             size_t result_size)
{
    char resume_secret_field[CUBICLE_AUTH_SECRET_BYTES * 2 + 32] = "";
    if (resume_secret != NULL) {
        char resume_secret_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
        if (cubicle_auth_hex_encode(resume_secret,
                                    CUBICLE_AUTH_SECRET_BYTES,
                                    resume_secret_hex,
                                    sizeof(resume_secret_hex)) < 0) {
            return -1;
        }
        int field_length = snprintf(resume_secret_field,
                                    sizeof(resume_secret_field),
                                    ",\"resume_secret\":\"%s\"",
                                    resume_secret_hex);
        if (field_length < 0 ||
            (size_t)field_length >= sizeof(resume_secret_field)) {
            errno = ENOSPC;
            return -1;
        }
    }

    int length = snprintf(
        result, result_size,
        "{\"session_id\":\"%s\",\"manager_id\":\"%s\",\"client_key_id\":\"%s\",\"protocol_major\":%u,\"protocol_minor\":%u,\"negotiated_capabilities\":%llu,\"authenticated_at_ms\":%llu,\"expires_at_ms\":%llu,\"manager_public_key\":\"%s\"%s}",
        session->session_id, session->manager_id, session->client_key_id,
        session->protocol_major, session->protocol_minor,
        (unsigned long long)session->negotiated_capabilities,
        (unsigned long long)session->authenticated_at_ms,
        (unsigned long long)session->expires_at_ms,
        state->identity.public_key_hex, resume_secret_field);
    if (length < 0 || (size_t)length >= result_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static manager_session_record_t *find_session_unlocked(
    manager_session_store_t *store,
    const char *session_id)
{
    if (store == NULL || session_id == NULL || session_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < CUBICLE_MANAGER_MAX_SESSIONS; ++i) {
        if (store->records[i].active &&
            strcmp(store->records[i].session.session_id, session_id) == 0) {
            return &store->records[i];
        }
    }
    return NULL;
}

static size_t active_session_count_unlocked(const manager_session_store_t *store)
{
    size_t count = 0;
    if (store == NULL) {
        return 0;
    }
    for (size_t i = 0; i < CUBICLE_MANAGER_MAX_SESSIONS; ++i) {
        if (store->records[i].active) {
            ++count;
        }
    }
    return count;
}

static int save_session_unlocked(manager_session_store_t *store,
                                 const manager_connection_t *connection,
                                 uint64_t manager_generation)
{
    if (store == NULL || connection == NULL || !connection->authenticated) {
        errno = EINVAL;
        return -1;
    }

    manager_session_record_t *record = find_session_unlocked(
        store, connection->session.session_id);
    if (record == NULL) {
        for (size_t i = 0; i < CUBICLE_MANAGER_MAX_SESSIONS; ++i) {
            if (!store->records[i].active) {
                record = &store->records[i];
                break;
            }
        }
    }
    if (record == NULL) {
        record = &store->records[0];
        for (size_t i = 1; i < CUBICLE_MANAGER_MAX_SESSIONS; ++i) {
            if (store->records[i].session.authenticated_at_ms <
                record->session.authenticated_at_ms) {
                record = &store->records[i];
            }
        }
    }

    memset(record, 0, sizeof(*record));
    record->active = 1;
    record->session = connection->session;
    memcpy(record->resume_secret, connection->resume_secret,
           sizeof(record->resume_secret));
    record->peer_uid = connection->peer_uid;
    record->peer_gid = connection->peer_gid;
    record->manager_generation = manager_generation;
    snprintf(record->client_public_key_hex, sizeof(record->client_public_key_hex),
             "%s", connection->client_public_key_hex);
    return 0;
}

static int runtime_save_session(manager_runtime_t *runtime,
                                const manager_connection_t *connection)
{
    if (runtime == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&runtime->sessions_mutex);
    int result = save_session_unlocked(&runtime->sessions, connection,
                                       runtime->started_at_ms);
    int saved_errno = errno;
    pthread_mutex_unlock(&runtime->sessions_mutex);
    errno = saved_errno;
    return result;
}

static int runtime_find_session_copy(manager_runtime_t *runtime,
                                     const char *session_id,
                                     manager_session_record_t *record)
{
    if (runtime == NULL || record == NULL) {
        errno = EINVAL;
        return -1;
    }

    int found = 0;
    pthread_mutex_lock(&runtime->sessions_mutex);
    manager_session_record_t *source = find_session_unlocked(
        &runtime->sessions, session_id);
    if (source != NULL) {
        *record = *source;
        found = 1;
    }
    pthread_mutex_unlock(&runtime->sessions_mutex);
    return found;
}

static size_t runtime_active_session_count(manager_runtime_t *runtime)
{
    if (runtime == NULL) {
        return 0;
    }
    pthread_mutex_lock(&runtime->sessions_mutex);
    size_t count = active_session_count_unlocked(&runtime->sessions);
    pthread_mutex_unlock(&runtime->sessions_mutex);
    return count;
}

static int runtime_shutdown_requested(manager_runtime_t *runtime)
{
    if (runtime == NULL) {
        return 1;
    }
    pthread_mutex_lock(&runtime->workers_mutex);
    int requested = runtime->shutdown_requested;
    pthread_mutex_unlock(&runtime->workers_mutex);
    return requested;
}

static void runtime_request_shutdown(manager_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    pthread_mutex_lock(&runtime->workers_mutex);
    runtime->shutdown_requested = 1;
    pthread_cond_broadcast(&runtime->workers_cond);
    pthread_mutex_unlock(&runtime->workers_mutex);
}

static int create_authenticated_session(const manager_state_t *state,
                                        manager_connection_t *connection,
                                        const unsigned char *client_public_key,
                                        uint64_t now_ms)
{
    memset(&connection->session, 0, sizeof(connection->session));
    char client_fingerprint[CUBICLE_AUTH_FINGERPRINT_LENGTH];
    if (cubicle_generate_hex_id(connection->session.session_id,
                                sizeof(connection->session.session_id)) < 0 ||
        cubicle_auth_key_fingerprint(client_public_key,
                                     CUBICLE_AUTH_PUBLIC_KEY_BYTES,
                                     connection->session.client_key_id,
                                     client_fingerprint) < 0 ||
        cubicle_auth_random_bytes(connection->resume_secret,
                                  sizeof(connection->resume_secret)) < 0) {
        return -1;
    }

    snprintf(connection->session.manager_id,
             sizeof(connection->session.manager_id), "%s",
             state->identity.key_id);
    connection->session.protocol_major = CUBICLE_PROTOCOL_MAJOR;
    connection->session.protocol_minor = CUBICLE_PROTOCOL_MINOR;
    connection->session.negotiated_capabilities =
        CUBICLE_API_CAPABILITIES | CUBICLE_PROTOCOL_CAP_AUTH_ED25519;
    connection->session.authenticated_at_ms = now_ms;
    connection->session.expires_at_ms = now_ms + 12ULL * 60ULL * 60ULL * 1000ULL;
    connection->authenticated = 1;
    if (cubicle_auth_hex_encode(client_public_key, CUBICLE_AUTH_PUBLIC_KEY_BYTES,
                                connection->client_public_key_hex,
                                sizeof(connection->client_public_key_hex)) < 0) {
        return -1;
    }
    return 0;
}

static int handle_manager_client(const manager_state_t *state, int client_fd,
                                 manager_connection_t *connection)
{
    char request[8192];
    if (read_api_frame(client_fd, request, sizeof(request)) < 0) {
        return -1;
    }

    cubicle_rpc_request_envelope_t envelope;
    char request_id[64] = "";
    char method[128] = "";
    if (cubicle_rpc_decode_request(&envelope, request) < 0) {
        return manager_api_error(client_fd, request_id,
                                 CUBICLE_ERR_PROTOCOL,
                                 "invalid request envelope", false, 0);
    }
    snprintf(request_id, sizeof(request_id), "%s", envelope.request_id);
    snprintf(method, sizeof(method), "%s", envelope.method);
    yyjson_val *params = envelope.params;

#define MANAGER_RETURN(expression) do { \
        int cubicle_manager_result__ = (expression); \
        cubicle_rpc_request_envelope_cleanup(&envelope); \
        return cubicle_manager_result__; \
    } while (0)

    char id[CUBICLE_MANAGER_ID_LENGTH + 1];
    if (manager_id(state, id) < 0) {
        MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                         CUBICLE_ERR_INTERNAL,
                                         "failed to read manager id", false,
                                         errno));
    }

    uint64_t now_ms = manager_time_ms();
    if (strcmp(method, "auth.challenge") == 0) {
        if (!connection->has_peer_credentials) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_UNSUPPORTED,
                                             "authenticated bootstrap requires Unix peer credentials",
                                             false, 0));
        }

        char client_public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
        char client_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
        char workspace_ref[CUBICLE_NAME_MAX] = "";
        int has_workspace_ref = 0;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "client_public_key",
                                             client_public_key_hex,
                                             sizeof(client_public_key_hex),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "client_nonce",
                                             client_nonce_hex,
                                             sizeof(client_nonce_hex),
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "workspace",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &has_workspace_ref,
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid auth challenge request",
                                             false, 0));
        }

        cubicle_auth_transcript_t transcript;
        memset(&transcript, 0, sizeof(transcript));
        transcript.protocol_major = CUBICLE_PROTOCOL_MAJOR;
        transcript.protocol_minor = CUBICLE_PROTOCOL_MINOR;
        memcpy(transcript.manager_public_key, state->identity.public_key,
               sizeof(transcript.manager_public_key));
        if (cubicle_auth_hex_decode(client_public_key_hex,
                                    transcript.client_public_key,
                                    sizeof(transcript.client_public_key)) < 0 ||
            cubicle_auth_hex_decode(client_nonce_hex,
                                    transcript.client_nonce,
                                    sizeof(transcript.client_nonce)) < 0 ||
            cubicle_auth_random_bytes(transcript.manager_nonce,
                                      sizeof(transcript.manager_nonce)) < 0 ||
            cubicle_auth_random_bytes(transcript.connection_id,
                                      sizeof(transcript.connection_id)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid auth challenge material",
                                             false, 0));
        }
        transcript.capabilities =
            CUBICLE_API_CAPABILITIES | CUBICLE_PROTOCOL_CAP_AUTH_ED25519;
        transcript.manager_generation = connection->runtime->started_at_ms;
        transcript.peer_uid = connection->peer_uid;
        transcript.peer_gid = connection->peer_gid;
        if (has_workspace_ref) {
            snprintf(transcript.workspace_ref, sizeof(transcript.workspace_ref),
                     "%s", workspace_ref);
        }
        connection->pending_auth = transcript;
        connection->has_pending_auth = 1;

        char manager_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
        char connection_id_hex[CUBICLE_AUTH_CONNECTION_ID_BYTES * 2 + 1];
        if (cubicle_auth_hex_encode(transcript.manager_nonce,
                                    sizeof(transcript.manager_nonce),
                                    manager_nonce_hex,
                                    sizeof(manager_nonce_hex)) < 0 ||
            cubicle_auth_hex_encode(transcript.connection_id,
                                    sizeof(transcript.connection_id),
                                    connection_id_hex,
                                    sizeof(connection_id_hex)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode auth challenge",
                                             false, errno));
        }
        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        int length = snprintf(
            result, sizeof(result),
            "{\"manager_id\":\"%s\",\"manager_public_key\":\"%s\",\"manager_nonce\":\"%s\",\"connection_id\":\"%s\",\"protocol_major\":%u,\"protocol_minor\":%u,\"capabilities\":%llu,\"manager_generation\":%llu,\"peer_uid\":%llu,\"peer_gid\":%llu}",
            state->identity.key_id, state->identity.public_key_hex,
            manager_nonce_hex, connection_id_hex, CUBICLE_PROTOCOL_MAJOR,
            CUBICLE_PROTOCOL_MINOR,
            (unsigned long long)transcript.capabilities,
            (unsigned long long)transcript.manager_generation,
            (unsigned long long)transcript.peer_uid,
            (unsigned long long)transcript.peer_gid);
        if (length < 0 || (size_t)length >= sizeof(result)) {
            MANAGER_RETURN(-1);
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "auth.authenticate") == 0) {
        if (!connection->has_pending_auth) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_STATE,
                                             "auth challenge is required",
                                             false, 0));
        }

        char signature_hex[CUBICLE_AUTH_SIGNATURE_BYTES * 2 + 1];
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "signature",
                                             signature_hex,
                                             sizeof(signature_hex),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "missing auth signature",
                                             false, 0));
        }

        unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES];
        unsigned char transcript_bytes[512];
        size_t transcript_length = 0;
        if (cubicle_auth_hex_decode(signature_hex, signature,
                                    sizeof(signature)) < 0 ||
            cubicle_auth_encode_transcript(&connection->pending_auth,
                                           transcript_bytes,
                                           sizeof(transcript_bytes),
                                           &transcript_length) < 0 ||
            cubicle_auth_verify(connection->pending_auth.client_public_key,
                                transcript_bytes, transcript_length,
                                signature) < 0) {
            connection->has_pending_auth = 0;
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "invalid auth signature",
                                             false, 0));
        }
        if (create_authenticated_session(
                state, connection, connection->pending_auth.client_public_key,
                now_ms) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to create session",
                                             false, errno));
        }
        if (runtime_save_session(connection->runtime, connection) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "failed to save session",
                                             false, errno));
        }
        connection->has_pending_auth = 0;

        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        if (session_info_json(state, &connection->session,
                              connection->resume_secret, result,
                              sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode session",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "auth.resume") == 0) {
        if (!connection->has_peer_credentials) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_UNSUPPORTED,
                                             "session resume requires Unix peer credentials",
                                             false, 0));
        }

        char session_id[CUBICLE_ID_STRING_LENGTH];
        char client_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
        char connection_id_hex[CUBICLE_AUTH_CONNECTION_ID_BYTES * 2 + 1];
        char authenticator_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "session_id",
                                             session_id,
                                             sizeof(session_id),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "client_nonce",
                                             client_nonce_hex,
                                             sizeof(client_nonce_hex),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "connection_id",
                                             connection_id_hex,
                                             sizeof(connection_id_hex),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "authenticator",
                                             authenticator_hex,
                                             sizeof(authenticator_hex),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid session resume request",
                                             false, 0));
        }

        manager_session_record_t record;
        if (!runtime_find_session_copy(connection->runtime, session_id,
                                       &record) ||
            record.manager_generation != connection->runtime->started_at_ms ||
            record.peer_uid != connection->peer_uid ||
            record.peer_gid != connection->peer_gid ||
            (record.session.expires_at_ms > 0 &&
             record.session.expires_at_ms < now_ms)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_SESSION_EXPIRED,
                                             "session cannot be resumed",
                                             false, 0));
        }

        cubicle_auth_resume_t resume;
        memset(&resume, 0, sizeof(resume));
        snprintf(resume.manager_key_id, sizeof(resume.manager_key_id), "%s",
                 state->identity.key_id);
        snprintf(resume.session_id, sizeof(resume.session_id), "%s",
                 session_id);
        if (cubicle_auth_hex_decode(client_nonce_hex, resume.client_nonce,
                                    sizeof(resume.client_nonce)) < 0 ||
            cubicle_auth_hex_decode(connection_id_hex, resume.connection_id,
                                    sizeof(resume.connection_id)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid session resume material",
                                             false, 0));
        }
        resume.manager_generation = connection->runtime->started_at_ms;
        resume.peer_uid = connection->peer_uid;
        resume.peer_gid = connection->peer_gid;

        unsigned char resume_bytes[512];
        size_t resume_length = 0;
        unsigned char expected_authenticator[CUBICLE_AUTH_SECRET_BYTES];
        unsigned char received_authenticator[CUBICLE_AUTH_SECRET_BYTES];
        if (cubicle_auth_encode_resume(&resume, resume_bytes,
                                       sizeof(resume_bytes),
                                       &resume_length) < 0 ||
            cubicle_auth_hmac_sha256(record.resume_secret,
                                     sizeof(record.resume_secret),
                                     resume_bytes, resume_length,
                                     expected_authenticator) < 0 ||
            cubicle_auth_hex_decode(authenticator_hex,
                                    received_authenticator,
                                    sizeof(received_authenticator)) < 0 ||
            memcmp(expected_authenticator, received_authenticator,
                   sizeof(expected_authenticator)) != 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_AUTHENTICATION_FAILED,
                                             "invalid session resume authenticator",
                                             false, 0));
        }

        connection->authenticated = 1;
        connection->session = record.session;
        memcpy(connection->resume_secret, record.resume_secret,
               sizeof(connection->resume_secret));
        snprintf(connection->client_public_key_hex,
                 sizeof(connection->client_public_key_hex), "%s",
                 record.client_public_key_hex);

        unsigned char server_nonce[CUBICLE_AUTH_NONCE_BYTES];
        char server_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
        if (cubicle_auth_random_bytes(server_nonce, sizeof(server_nonce)) < 0 ||
            cubicle_auth_hex_encode(server_nonce, sizeof(server_nonce),
                                    server_nonce_hex,
                                    sizeof(server_nonce_hex)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to create resume response",
                                             false, errno));
        }

        char session_json[2048];
        if (session_info_json(state, &connection->session, NULL,
                              session_json, sizeof(session_json)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode resumed session",
                                             false, errno));
        }
        char result[2300];
        size_t prefix = strlen(session_json);
        if (prefix == 0 || session_json[prefix - 1] != '}') {
            MANAGER_RETURN(-1);
        }
        session_json[prefix - 1] = '\0';
        int length = snprintf(result, sizeof(result),
                              "%s,\"server_nonce\":\"%s\"}",
                              session_json, server_nonce_hex);
        if (length < 0 || (size_t)length >= sizeof(result)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "session resume response too large",
                                             false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "session.local_bootstrap") == 0) {
        char result[1024];
        if (connection->authenticated) {
            if (session_info_json(state, &connection->session, NULL, result,
                                  sizeof(result)) < 0) {
                MANAGER_RETURN(-1);
            }
        } else {
            int length = snprintf(
                result, sizeof(result),
                "{\"session_id\":\"local-session\",\"manager_id\":\"%s\",\"client_key_id\":\"local-bootstrap\",\"protocol_major\":%u,\"protocol_minor\":%u,\"negotiated_capabilities\":%llu,\"authenticated_at_ms\":%llu,\"expires_at_ms\":0,\"manager_public_key\":\"%s\"}",
                id, CUBICLE_PROTOCOL_MAJOR, CUBICLE_PROTOCOL_MINOR,
                (unsigned long long)CUBICLE_API_CAPABILITIES,
                (unsigned long long)now_ms, state->identity.public_key_hex);
            if (length < 0 || (size_t)length >= sizeof(result)) {
                MANAGER_RETURN(-1);
            }
        }
        if (result[0] == '\0') {
            MANAGER_RETURN(-1);
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "manager.ping") == 0) {
        char result[512];
        int length = snprintf(result, sizeof(result),
                              "{\"manager_id\":\"%s\",\"protocol_major\":%u,\"protocol_minor\":%u,\"server_time_ms\":%llu,\"uptime_ms\":%llu}",
                              id, CUBICLE_PROTOCOL_MAJOR,
                              CUBICLE_PROTOCOL_MINOR,
                              (unsigned long long)now_ms,
                              (unsigned long long)(now_ms -
                                  connection->runtime->started_at_ms));
        if (length < 0 || (size_t)length >= sizeof(result)) {
            MANAGER_RETURN(-1);
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "manager.status") == 0) {
        size_t workspace_count = 0;
        size_t process_count = 0;
        if (count_workspaces(state, &workspace_count) < 0 ||
            count_processes(state, &process_count) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to count manager state",
                                             false, errno));
        }

        char result[1024];
        int length = snprintf(result, sizeof(result),
                              "{\"manager_id\":\"%s\",\"protocol_major\":%u,\"protocol_minor\":%u,\"capabilities\":%llu,\"started_at_ms\":%llu,\"server_time_ms\":%llu,\"workspace_count\":%zu,\"process_count\":%zu,\"controller_count\":%zu,\"active_client_sessions\":%zu}",
                              id, CUBICLE_PROTOCOL_MAJOR,
                              CUBICLE_PROTOCOL_MINOR,
                              (unsigned long long)CUBICLE_API_CAPABILITIES,
                              (unsigned long long)connection->runtime->started_at_ms,
                              (unsigned long long)now_ms, workspace_count,
                              process_count, process_count,
                              runtime_active_session_count(connection->runtime));
        if (length < 0 || (size_t)length >= sizeof(result)) {
            MANAGER_RETURN(-1);
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "manager.shutdown") == 0) {
        runtime_request_shutdown(connection->runtime);
        MANAGER_RETURN(manager_api_success(client_fd, request_id, "{}"));
    }

    if (strcmp(method, "manager.reconcile") == 0) {
        if (reconcile_process_records(state) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to reconcile processes",
                                             true, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, "{}"));
    }

    if (strcmp(method, "workspace.create") == 0) {
        char name[128];
        char requested_directory[CUBICLE_PATH_MAX] = "";
        char initial_owner_label[CUBICLE_KEY_LABEL_MAX] = "owner";
        cubicle_validation_error_t validation_error;
        int has_directory = 0;
        if (cubicle_json_get_required_string(params, "name", name,
                                             sizeof(name),
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "directory",
                                             requested_directory,
                                             sizeof(requested_directory),
                                             &has_directory,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "initial_owner_label",
                                             initial_owner_label,
                                             sizeof(initial_owner_label), NULL,
                                             &validation_error) < 0 ||
            validate_field(name, "workspace name") < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid workspace name", false,
                                             0));
        }
        char directory[CUBICLE_PATH_MAX];
        if (resolve_directory_path(has_directory ? requested_directory : NULL,
                                   directory) < 0 ||
            validate_field(directory, "workspace directory") < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid workspace directory",
                                             false, 0));
        }
        if (!connection_is_manager_owner(connection)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "workspace creation requires local owner access",
                                             false, 0));
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }

        if (workspace_name_exists(state, name)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_ALREADY_EXISTS,
                                             "workspace already exists",
                                             false, 0));
        }

        cubicle_workspace_record_t workspace;
        if (cubicle_generate_hex_id(workspace.id, sizeof(workspace.id)) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to allocate workspace id",
                                             false, saved_errno));
        }
        snprintf(workspace.name, sizeof(workspace.name), "%s", name);
        snprintf(workspace.directory, sizeof(workspace.directory), "%s",
                 directory);

        char line[CUBICLE_PATH_MAX + 256];
        int line_length = snprintf(line, sizeof(line), "%s\t%s\t%s\n",
                                   workspace.id, workspace.name,
                                   workspace.directory);
        if (line_length < 0 || (size_t)line_length >= sizeof(line) ||
            append_line(state, "workspaces.tsv", line) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to persist workspace",
                                             true, saved_errno));
        }
        if (connection->authenticated &&
            connection->client_public_key_hex[0] != '\0') {
            workspace_key_record_t owner_key;
            if (build_workspace_key_record_from_public_hex(
                    &owner_key, workspace.id, connection->client_public_key_hex,
                    initial_owner_label, role_owner_capabilities(), now_ms) < 0 ||
                append_workspace_key_record(state, &owner_key) < 0) {
                int saved_errno = errno;
                unlock_state(lock_fd);
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_IO,
                                                 "failed to persist workspace owner key",
                                                 true, saved_errno));
            }
        }
        unlock_state(lock_fd);

        char result[1024];
        if (workspace_info_json(state, id, &workspace, result,
                                sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode workspace",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "workspace.get") == 0) {
        char name_or_id[128];
        cubicle_workspace_record_t workspace;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "workspace", name_or_id,
                                             sizeof(name_or_id),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "missing workspace reference",
                                             false, 0));
        }
        if (find_workspace(state, name_or_id, &workspace) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false,
                                             0));
        }
        if (!connection_has_workspace_capability(
                state, workspace.id, connection, CUBICLE_CAP_WORKSPACE_READ)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "workspace read access is required",
                                             false, 0));
        }
        char result[1024];
        if (workspace_info_json(state, id, &workspace, result,
                                sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode workspace",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "workspace.list") == 0) {
        FILE *file = open_state_file_for_read(state, "workspaces.tsv");
        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        size_t used = 0;
        int written = snprintf(result, sizeof(result),
                               "{\"workspaces\":[");
        if (written < 0 || (size_t)written >= sizeof(result)) {
            MANAGER_RETURN(-1);
        }
        used = (size_t)written;

        size_t count = 0;
        if (file != NULL) {
            char line[512];
            while (fgets(line, sizeof(line), file) != NULL) {
                cubicle_workspace_record_t workspace;
                char item[1024];
                if (cubicle_parse_workspace_record(line, &workspace) != 0 ||
                    !connection_has_workspace_capability(
                        state, workspace.id, connection,
                        CUBICLE_CAP_WORKSPACE_READ) ||
                    workspace_info_json(state, id, &workspace, item,
                                        sizeof(item)) < 0) {
                    continue;
                }
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s%s", count == 0 ? "" : ",", item);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    fclose(file);
                    MANAGER_RETURN(manager_api_error(
                        client_fd, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                        "workspace list response too large", false, 0));
                }
                used += (size_t)written;
                ++count;
            }
            fclose(file);
        }

        written = snprintf(result + used, sizeof(result) - used,
                           "],\"count\":%zu,\"has_more\":false}", count);
        if (written < 0 || (size_t)written >= sizeof(result) - used) {
            MANAGER_RETURN(manager_api_error(
                client_fd, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                "workspace list response too large", false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "workspace.rename") == 0) {
        char workspace_ref[128];
        char new_name[128];
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "new_name", new_name,
                                             sizeof(new_name),
                                             &validation_error) < 0 ||
            validate_field(new_name, "workspace name") < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid workspace rename request",
                                             false, 0));
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }
        cubicle_workspace_record_t workspace;
        if (find_workspace(state, workspace_ref, &workspace) < 0) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false,
                                             0));
        }
        if (!connection_has_workspace_capability(
                state, workspace.id, connection,
                CUBICLE_CAP_WORKSPACE_RENAME)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "workspace rename access is required",
                                             false, 0));
        }
        cubicle_workspace_record_t existing;
        if (find_workspace(state, new_name, &existing) == 0 &&
            strcmp(existing.id, workspace.id) != 0) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_ALREADY_EXISTS,
                                             "workspace name already exists",
                                             false, 0));
        }
        int found = 0;
        if (rewrite_workspace_records(state, workspace.id, new_name, 0,
                                      &found) < 0 || !found) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to rename workspace",
                                             true, saved_errno));
        }
        unlock_state(lock_fd);
        MANAGER_RETURN(manager_api_success(client_fd, request_id, "{}"));
    }

    if (strcmp(method, "workspace.stop") == 0 ||
        strcmp(method, "workspace.delete") == 0) {
        char workspace_ref[128];
        bool stop_running_processes = false;
        bool remove_retained_processes = false;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_bool(params, "stop_running_processes",
                                           &stop_running_processes, NULL,
                                           &validation_error) < 0 ||
            cubicle_json_get_optional_bool(params, "remove_retained_processes",
                                           &remove_retained_processes, NULL,
                                           &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid workspace lifecycle request",
                                             false, 0));
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }
        cubicle_workspace_record_t workspace;
        if (find_workspace(state, workspace_ref, &workspace) < 0) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false,
                                             0));
        }
        cubicle_capability_mask_t lifecycle_capability =
            strcmp(method, "workspace.delete") == 0
                ? CUBICLE_CAP_WORKSPACE_DELETE
                : CUBICLE_CAP_WORKSPACE_STOP;
        if (!connection_has_workspace_capability(
                state, workspace.id, connection, lifecycle_capability)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "workspace lifecycle access is required",
                                             false, 0));
        }

        FILE *processes = open_state_file_for_read(state, "processes.tsv");
        char process_ids_to_remove[256][CUBICLE_ID_STRING_LENGTH];
        size_t remove_count = 0;
        int live_count = 0;
        int action_failed = 0;
        if (processes != NULL) {
            char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
            while (fgets(line, sizeof(line), processes) != NULL) {
                cubicle_process_record_t process;
                if (cubicle_parse_process_record(line, &process) != 0 ||
                    strcmp(process.workspace_id, workspace.id) != 0) {
                    continue;
                }
                char latest_state[32];
                snprintf(latest_state, sizeof(latest_state), "%s",
                         process.state);
                if (!process_is_terminal_state(latest_state) &&
                    controller_status_state(&process, latest_state,
                                            sizeof(latest_state)) < 0 &&
                    process_event_log_has_exit(state, process.process_id)) {
                    snprintf(latest_state, sizeof(latest_state),
                             "completed");
                }
                if (!process_is_terminal_state(latest_state)) {
                    ++live_count;
                    if (strcmp(method, "workspace.stop") == 0 ||
                        stop_running_processes) {
                        if (terminate_controller(&process) < 0) {
                            action_failed = 1;
                        }
                    }
                }
                if (remove_retained_processes && remove_count < 256) {
                    snprintf(process_ids_to_remove[remove_count],
                             sizeof(process_ids_to_remove[remove_count]), "%s",
                             process.process_id);
                    ++remove_count;
                }
            }
            fclose(processes);
        }

        if (action_failed) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_CONTROLLER_UNAVAILABLE,
                                             "failed to stop workspace process",
                                             true, errno));
        }
        if (strcmp(method, "workspace.delete") == 0 &&
            live_count > 0 && !stop_running_processes) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_STATE,
                                             "workspace has live processes",
                                             false, 0));
        }

        if (strcmp(method, "workspace.delete") == 0) {
            int found = 0;
            if (rewrite_workspace_records(state, workspace.id, NULL, 1,
                                          &found) < 0 || !found) {
                int saved_errno = errno;
                unlock_state(lock_fd);
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_IO,
                                                 "failed to delete workspace",
                                                 true, saved_errno));
            }
            if (remove_retained_processes) {
                for (size_t i = 0; i < remove_count; ++i) {
                    int process_found = 0;
                    if (rewrite_process_records(state, process_ids_to_remove[i],
                                                NULL, 1, &process_found) < 0) {
                        int saved_errno = errno;
                        unlock_state(lock_fd);
                        MANAGER_RETURN(manager_api_error(
                            client_fd, request_id, CUBICLE_ERR_IO,
                            "failed to delete workspace processes", true,
                            saved_errno));
                    }
                    char controller_state[PATH_MAX];
                    char controller_log[PATH_MAX];
                    if (controller_state_path(controller_state, state,
                                              process_ids_to_remove[i]) < 0 ||
                        controller_log_path(controller_log, state,
                                            process_ids_to_remove[i]) < 0 ||
                        remove_tree_if_exists(controller_state) < 0 ||
                        remove_tree_if_exists(controller_log) < 0) {
                        int saved_errno = errno;
                        unlock_state(lock_fd);
                        MANAGER_RETURN(manager_api_error(
                            client_fd, request_id, CUBICLE_ERR_IO,
                            "failed to delete controller state", true,
                            saved_errno));
                    }
                }
            }
        }
        unlock_state(lock_fd);
        MANAGER_RETURN(manager_api_success(client_fd, request_id, "{}"));
    }

    if (strcmp(method, "process.get") == 0) {
        char process_ref[128];
        char workspace_ref[128];
        char workspace_id[128];
        const char *workspace_id_ptr = NULL;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "process", process_ref,
                                             sizeof(process_ref),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "missing process reference",
                                             false, 0));
        }
        int has_workspace_ref = 0;
        if (cubicle_json_get_optional_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &has_workspace_ref,
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid workspace reference",
                                             false, 0));
        }
        if (has_workspace_ref && workspace_ref[0] != '\0') {
            cubicle_workspace_record_t workspace;
            if (find_workspace(state, workspace_ref, &workspace) < 0) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_NOT_FOUND,
                                                 "workspace not found",
                                                 false, 0));
            }
            snprintf(workspace_id, sizeof(workspace_id), "%s",
                     workspace.id);
            workspace_id_ptr = workspace_id;
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_ref, workspace_id_ptr,
                                &process, &ambiguous) < 0) {
            MANAGER_RETURN(manager_api_error(
                client_fd, request_id,
                ambiguous ? CUBICLE_ERR_AMBIGUOUS_NAME
                          : CUBICLE_ERR_NOT_FOUND,
                ambiguous ? "ambiguous process name" : "process not found",
                false, 0));
        }
        refresh_observed_process_state(state, &process);
        if (!connection_has_workspace_capability(
                state, process.workspace_id, connection,
                CUBICLE_CAP_PROCESS_READ)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "process read access is required",
                                             false, 0));
        }

        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        if (process_info_json(state, id, &process, result, sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode process",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "process.list") == 0) {
        char workspace_ref[128];
        char workspace_id[128];
        const char *workspace_id_ptr = NULL;
        cubicle_validation_error_t validation_error;
        int has_workspace_ref = 0;
        if (cubicle_json_get_optional_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &has_workspace_ref,
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid workspace reference",
                                             false, 0));
        }
        if (has_workspace_ref && workspace_ref[0] != '\0') {
            cubicle_workspace_record_t workspace;
            if (find_workspace(state, workspace_ref, &workspace) < 0) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_NOT_FOUND,
                                                 "workspace not found",
                                                 false, 0));
            }
            snprintf(workspace_id, sizeof(workspace_id), "%s",
                     workspace.id);
            workspace_id_ptr = workspace_id;
        }

        FILE *file = open_state_file_for_read(state, "processes.tsv");
        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        size_t used = 0;
        int written = snprintf(result, sizeof(result), "{\"processes\":[");
        if (written < 0 || (size_t)written >= sizeof(result)) {
            MANAGER_RETURN(-1);
        }
        used = (size_t)written;

        size_t count = 0;
        if (file != NULL) {
            char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
            while (fgets(line, sizeof(line), file) != NULL) {
                cubicle_process_record_t process;
                char item[CUBICLE_PROCESS_RECORD_LINE_MAX];
                if (cubicle_parse_process_record(line, &process) != 0 ||
                    (workspace_id_ptr != NULL &&
                     strcmp(process.workspace_id, workspace_id_ptr) != 0) ||
                    !connection_has_workspace_capability(
                        state, process.workspace_id, connection,
                        CUBICLE_CAP_PROCESS_READ)) {
                    continue;
                }
                refresh_observed_process_state(state, &process);
                if (process_info_json(state, id, &process, item, sizeof(item)) < 0) {
                    continue;
                }
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s%s", count == 0 ? "" : ",", item);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    fclose(file);
                    MANAGER_RETURN(manager_api_error(
                        client_fd, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                        "process list response too large", false, 0));
                }
                used += (size_t)written;
                ++count;
            }
            fclose(file);
        }

        written = snprintf(result + used, sizeof(result) - used,
                           "],\"count\":%zu,\"has_more\":false}", count);
        if (written < 0 || (size_t)written >= sizeof(result) - used) {
            MANAGER_RETURN(manager_api_error(
                client_fd, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                "process list response too large", false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "process.start") == 0) {
        char workspace_ref[128];
        char friendly_name[128];
        char mode[32];
        char stdin_policy[32] = "open";
        char requested_cwd[CUBICLE_PATH_MAX] = "";
        yyjson_val *argv_array = NULL;
        cubicle_validation_error_t validation_error;
        int has_friendly_name = 0;
        int has_stdin_policy = 0;
        int has_cwd = 0;
        bool restart = false;
        if (cubicle_json_get_required_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "friendly_name",
                                             friendly_name,
                                             sizeof(friendly_name),
                                             &has_friendly_name,
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "mode", mode,
                                             sizeof(mode),
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "stdin_policy",
                                             stdin_policy,
                                             sizeof(stdin_policy),
                                             &has_stdin_policy,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "cwd",
                                             requested_cwd,
                                             sizeof(requested_cwd),
                                             &has_cwd,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_bool(params, "restart", &restart, NULL,
                                           &validation_error) < 0 ||
            cubicle_json_get_required_array(params, "argv", &argv_array,
                                            &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid process start request",
                                             false, 0));
        }
        (void)has_stdin_policy;
        size_t argc = cubicle_json_array_size(argv_array);
        if (argc == 0 || argc > CUBICLE_JSON_MAX_ARGC ||
            validate_field(workspace_ref, "workspace") < 0 ||
            validate_field(mode, "mode") < 0 ||
            (!has_friendly_name &&
             cubicle_generate_hex_id(friendly_name, sizeof(friendly_name)) < 0) ||
            validate_field(friendly_name, "friendly name") < 0 ||
            (strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_STREAM)) != 0 &&
             strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_TTY)) != 0 &&
             strcmp(mode, cubicle_process_mode_name(
                              CUBICLE_PROCESS_TTY_CAPTURED_STDERR)) != 0) ||
            (strcmp(stdin_policy, "open") != 0 &&
             strcmp(stdin_policy, "eof") != 0)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid process start request",
                                             false, 0));
        }

        char **command_argv = calloc(argc + 1, sizeof(*command_argv));
        if (command_argv == NULL) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to allocate argv",
                                             false, ENOMEM));
        }
        for (size_t i = 0; i < argc; ++i) {
            yyjson_val *item = cubicle_json_array_get(argv_array, i);
            if (!yyjson_is_str(item) ||
                yyjson_get_len(item) == 0 ||
                yyjson_get_len(item) >= PATH_MAX) {
                free(command_argv);
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_INVALID_ARGUMENT,
                                                 "invalid process argv",
                                                 false, 0));
            }
            command_argv[i] = (char *)(void *)yyjson_get_str(item);
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }

        cubicle_workspace_record_t workspace;
        if (find_workspace(state, workspace_ref, &workspace) < 0) {
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false,
                                             0));
        }
        if (!connection_has_workspace_capability(
                state, workspace.id, connection, CUBICLE_CAP_PROCESS_START)) {
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "process start access is required",
                                             false, 0));
        }

        char cwd[CUBICLE_PATH_MAX];
        if (resolve_directory_path(has_cwd ? requested_cwd :
                                   workspace.directory, cwd) < 0 ||
            validate_field(cwd, "process directory") < 0) {
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid process directory",
                                             false, 0));
        }

        char process_id[CUBICLE_MANAGER_ID_LENGTH + 1];
        if (cubicle_generate_hex_id(process_id, sizeof(process_id)) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to allocate process id",
                                             false, saved_errno));
        }

        int process_id_conflict = 0;
        int friendly_name_conflict = 0;
        if (process_conflict_exists(state, workspace.id, process_id,
                                    friendly_name, &process_id_conflict,
                                    &friendly_name_conflict) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to check process conflicts",
                                             true, saved_errno));
        }
        if (friendly_name_conflict) {
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_ALREADY_EXISTS,
                                             "process friendly name already exists",
                                             false, 0));
        }

        char controller_state[PATH_MAX];
        char controller_log[PATH_MAX];
        char control_socket[PATH_MAX];
        char metadata_path[PATH_MAX];
        char events_path[PATH_MAX];
        if (controller_state_path(controller_state, state, process_id) < 0 ||
            controller_log_path(controller_log, state, process_id) < 0 ||
            controller_socket_path(control_socket, state, process_id) < 0) {
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "controller path too long",
                                             false, 0));
        }

        int path_result = snprintf(metadata_path, sizeof(metadata_path),
                                   "%s/metadata", controller_state);
        int events_result = snprintf(events_path, sizeof(events_path),
                                     "%s/events.log", controller_log);
        if (path_result < 0 ||
            (size_t)path_result >= sizeof(metadata_path) ||
            events_result < 0 ||
            (size_t)events_result >= sizeof(events_path) ||
            ensure_parent_directory(control_socket) < 0 ||
            cubicle_mkdir_p(controller_log) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to prepare controller paths",
                                             true, saved_errno));
        }

        char process_state[32];
        if (launch_controller(state, controller_state, controller_log, control_socket, mode,
                              stdin_policy, cwd, command_argv) < 0 ||
            wait_for_controller_ready(control_socket, metadata_path,
                                      events_path,
                                      process_state,
                                      sizeof(process_state)) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to start controller",
                                             false, saved_errno));
        }

        char argv_json[CUBICLE_PROCESS_ARGV_JSON_MAX];
        if (build_argv_json(argv_json, sizeof(argv_json), command_argv) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            free(command_argv);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "process argv is too large",
                                             false, saved_errno));
        }
        free(command_argv);

        char controller_id[CUBICLE_MANAGER_ID_LENGTH + 1];
        if (read_metadata_field(metadata_path, "controller_id",
                                controller_id,
                                sizeof(controller_id)) < 0 ||
            append_process_record(state, process_id, workspace.id,
                                  friendly_name, mode, process_state,
                                  controller_id, control_socket, cwd,
                                  0, argv_json, restart ? 1 : 0, stdin_policy,
                                  workspace.name, workspace.directory) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to persist process",
                                             true, saved_errno));
        }
        unlock_state(lock_fd);

        cubicle_process_record_t process;
        snprintf(process.process_id, sizeof(process.process_id), "%s",
                 process_id);
        snprintf(process.workspace_id, sizeof(process.workspace_id), "%s",
                 workspace.id);
        snprintf(process.friendly_name, sizeof(process.friendly_name), "%s",
                 friendly_name);
        snprintf(process.mode, sizeof(process.mode), "%s", mode);
        snprintf(process.state, sizeof(process.state), "%s", process_state);
        snprintf(process.controller_id, sizeof(process.controller_id), "%s",
                 controller_id);
        snprintf(process.control_socket, sizeof(process.control_socket), "%s",
                 control_socket);
        snprintf(process.cwd, sizeof(process.cwd), "%s", cwd);
        snprintf(process.argv_json, sizeof(process.argv_json), "%s",
                 argv_json);
        process.saved = 0;
        process.restart = restart ? 1 : 0;
        snprintf(process.stdin_policy, sizeof(process.stdin_policy), "%s",
                 stdin_policy);
        snprintf(process.workspace_name, sizeof(process.workspace_name), "%s",
                 workspace.name);
        snprintf(process.workspace_directory,
                 sizeof(process.workspace_directory), "%s",
                 workspace.directory);

        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        if (process_info_json(state, id, &process, result, sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode process",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "process.signal") == 0 ||
        strcmp(method, "process.terminate") == 0 ||
        strcmp(method, "process.kill") == 0) {
        char process_id[128];
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "process_id", process_id,
                                             sizeof(process_id),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "missing process id", false,
                                             0));
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_id, NULL, &process,
                                &ambiguous) < 0) {
            (void)ambiguous;
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "process not found", false, 0));
        }
        if (!connection_has_workspace_capability(
                state, process.workspace_id, connection,
                CUBICLE_CAP_PROCESS_SIGNAL)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "process signal access is required",
                                             false, 0));
        }

        int action_result = 0;
        if (strcmp(method, "process.signal") == 0) {
            uint64_t signal_number = 0;
            if (cubicle_json_get_required_u64(params, "signal_number",
                                              &signal_number,
                                              &validation_error) < 0 ||
                signal_number == 0 ||
                signal_number >= CUBICLE_MANAGER_MAX_SIGNAL_NUMBER) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_INVALID_ARGUMENT,
                                                 "invalid signal number",
                                                 false, 0));
            }
            action_result = signal_controller(&process, (int)signal_number);
        } else if (strcmp(method, "process.terminate") == 0) {
            action_result = terminate_controller(&process);
        } else {
            action_result = signal_controller(&process, SIGKILL);
        }

        if (action_result < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_CONTROLLER_UNAVAILABLE,
                                             "controller request failed",
                                             true, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, "{}"));
    }

    if (strcmp(method, "process.wait") == 0) {
        char process_id[128];
        uint64_t timeout_ms = 0;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "process_id", process_id,
                                             sizeof(process_id),
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_u64(params, "timeout_ms", &timeout_ms,
                                          NULL,
                                          &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid process wait request",
                                             false, 0));
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_id, NULL, &process,
                                &ambiguous) < 0) {
            (void)ambiguous;
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "process not found", false, 0));
        }
        if (!connection_has_workspace_capability(
                state, process.workspace_id, connection,
                CUBICLE_CAP_PROCESS_READ)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "process wait access is required",
                                             false, 0));
        }

        uint64_t waited_ms = 0;
        char latest_state[32];
        snprintf(latest_state, sizeof(latest_state), "%s", process.state);
        while (!process_is_terminal_state(latest_state)) {
            if (process_observed_state(state, &process, latest_state,
                                       sizeof(latest_state)) < 0 ||
                process_is_terminal_state(latest_state)) {
                break;
            }
            if (waited_ms >= timeout_ms) {
                break;
            }
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000L};
            nanosleep(&delay, NULL);
            waited_ms += 50;
        }

        if (strcmp(latest_state, process.state) != 0 &&
            process_is_terminal_state(latest_state)) {
            int found = 0;
            int lock_fd = lock_state(state);
            if (lock_fd >= 0) {
                process_record_update_t update = {.state = latest_state};
                if (rewrite_process_records(state, process.process_id,
                                            &update, 0, &found) == 0 &&
                    found) {
                    snprintf(process.state, sizeof(process.state), "%s",
                             latest_state);
                }
                unlock_state(lock_fd);
            }
        }
        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        if (process_info_json(state, id, &process, result, sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode process",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "process.update") == 0) {
        char process_ref[128];
        char workspace_ref[128];
        char friendly_name[128];
        bool restart = false;
        int has_workspace_ref = 0;
        int has_friendly_name = 0;
        int has_restart = 0;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "process", process_ref,
                                             sizeof(process_ref),
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &has_workspace_ref,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "friendly_name",
                                             friendly_name,
                                             sizeof(friendly_name),
                                             &has_friendly_name,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_bool(params, "restart", &restart,
                                           &has_restart,
                                           &validation_error) < 0 ||
            (has_friendly_name &&
             validate_field(friendly_name, "friendly name") < 0)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid process update request",
                                             false, 0));
        }
        if (!has_friendly_name && !has_restart) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "process update has no changes",
                                             false, 0));
        }

        char workspace_id[128];
        const char *workspace_id_ptr = NULL;
        if (has_workspace_ref && workspace_ref[0] != '\0') {
            cubicle_workspace_record_t workspace;
            if (find_workspace(state, workspace_ref, &workspace) < 0) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_NOT_FOUND,
                                                 "workspace not found",
                                                 false, 0));
            }
            snprintf(workspace_id, sizeof(workspace_id), "%s", workspace.id);
            workspace_id_ptr = workspace_id;
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_ref, workspace_id_ptr, &process,
                                &ambiguous) < 0) {
            (void)ambiguous;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "process not found", false, 0));
        }
        if (!connection_has_workspace_capability(
                state, process.workspace_id, connection,
                CUBICLE_CAP_PROCESS_SIGNAL)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "process update access is required",
                                             false, 0));
        }

        if (has_friendly_name) {
            int conflict = 0;
            if (process_friendly_name_conflicts(
                    state, process.workspace_id, process.process_id,
                    friendly_name, &conflict) < 0) {
                int saved_errno = errno;
                unlock_state(lock_fd);
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_IO,
                                                 "failed to check process name",
                                                 true, saved_errno));
            }
            if (conflict) {
                unlock_state(lock_fd);
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_CONFLICT,
                                                 "friendly name already exists",
                                                 false, 0));
            }
        }

        process_record_update_t update = {
            .friendly_name = has_friendly_name ? friendly_name : NULL,
            .has_restart = has_restart,
            .restart = restart ? 1 : 0,
        };
        int found = 0;
        if (rewrite_process_records(state, process.process_id, &update, 0,
                                    &found) < 0 || !found) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to update process",
                                             true, saved_errno));
        }
        if (has_friendly_name) {
            snprintf(process.friendly_name, sizeof(process.friendly_name), "%s",
                     friendly_name);
        }
        if (has_restart) {
            process.restart = restart ? 1 : 0;
        }
        unlock_state(lock_fd);

        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        if (process_info_json(state, id, &process, result, sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode process",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "process.save") == 0 ||
        strcmp(method, "process.unsave") == 0) {
        char process_id[128];
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "process_id", process_id,
                                             sizeof(process_id),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "missing process id", false,
                                             0));
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_id, NULL, &process,
                                &ambiguous) < 0) {
            (void)ambiguous;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "process not found", false, 0));
        }
        if (!connection_has_workspace_capability(
                state, process.workspace_id, connection,
                CUBICLE_CAP_PROCESS_REMOVE)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "process save access is required",
                                             false, 0));
        }

        process_record_update_t update = {
            .has_saved = 1,
            .saved = strcmp(method, "process.save") == 0 ? 1 : 0,
        };
        int found = 0;
        if (rewrite_process_records(state, process.process_id, &update, 0,
                                    &found) < 0 || !found) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to update process",
                                             true, saved_errno));
        }
        process.saved = update.saved;
        unlock_state(lock_fd);

        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        if (process_info_json(state, id, &process, result, sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode process",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "process.remove") == 0) {
        char process_id[128];
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "process_id", process_id,
                                             sizeof(process_id),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "missing process id", false,
                                             0));
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_id, NULL, &process,
                                &ambiguous) < 0) {
            (void)ambiguous;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "process not found", false, 0));
        }
        if (!connection_has_workspace_capability(
                state, process.workspace_id, connection,
                CUBICLE_CAP_PROCESS_REMOVE)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "process remove access is required",
                                             false, 0));
        }

        char latest_state[32];
        (void)process_observed_state(state, &process, latest_state,
                                     sizeof(latest_state));
        if (!process_is_terminal_state(process.state) &&
            process_is_terminal_state(latest_state)) {
            int found = 0;
            process_record_update_t update = {.state = latest_state};
            if (rewrite_process_records(state, process.process_id,
                                        &update, 0, &found) == 0 &&
                found) {
                snprintf(process.state, sizeof(process.state), "%s",
                         latest_state);
            }
        }

        if (!process_is_terminal_state(process.state)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_STATE,
                                             "process is still live", false,
                                             0));
        }

        int found = 0;
        if (rewrite_process_records(state, process.process_id, NULL, 1,
                                    &found) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to remove process",
                                             true, saved_errno));
        }
        char controller_state[PATH_MAX];
        char controller_log[PATH_MAX];
        if (controller_state_path(controller_state, state,
                                  process.process_id) < 0 ||
            controller_log_path(controller_log, state,
                                process.process_id) < 0 ||
            remove_tree_if_exists(controller_state) < 0 ||
            remove_tree_if_exists(controller_log) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to remove process state",
                                             true, saved_errno));
        }
        unlock_state(lock_fd);
        MANAGER_RETURN(manager_api_success(client_fd, request_id, "{}"));
    }

    if (strcmp(method, "manager.cleanup") == 0) {
        char workspace_ref[128];
        char workspace_id[CUBICLE_ID_STRING_LENGTH] = "";
        int has_workspace_ref = 0;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_optional_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &has_workspace_ref,
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid cleanup request",
                                             false, 0));
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }

        if (has_workspace_ref && workspace_ref[0] != '\0') {
            cubicle_workspace_record_t workspace;
            if (find_workspace(state, workspace_ref, &workspace) < 0) {
                unlock_state(lock_fd);
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_NOT_FOUND,
                                                 "workspace not found",
                                                 false, 0));
            }
            snprintf(workspace_id, sizeof(workspace_id), "%s",
                     workspace.id);
            if (!connection_has_workspace_capability(
                    state, workspace_id, connection,
                    CUBICLE_CAP_PROCESS_REMOVE)) {
                unlock_state(lock_fd);
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_PERMISSION_DENIED,
                                                 "cleanup access is required",
                                                 false, 0));
            }
        }
        if (workspace_id[0] == '\0' &&
            !connection_is_manager_owner(connection)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "manager cleanup requires local owner access",
                                             false, 0));
        }

        typedef struct cleanup_candidate {
            char process_id[CUBICLE_ID_STRING_LENGTH];
        } cleanup_candidate_t;

        cleanup_candidate_t *candidates = NULL;
        size_t candidate_count = 0;
        size_t candidate_capacity = 0;
        size_t skipped_live_count = 0;
        size_t skipped_saved_count = 0;
        FILE *file = open_state_file_for_read(state, "processes.tsv");
        if (file != NULL) {
            char line[CUBICLE_PROCESS_RECORD_LINE_MAX];
            while (fgets(line, sizeof(line), file) != NULL) {
                cubicle_process_record_t process;
                if (cubicle_parse_process_record(line, &process) != 0 ||
                    (workspace_id[0] != '\0' &&
                     strcmp(process.workspace_id, workspace_id) != 0)) {
                    continue;
                }

                char latest_state[32];
                if (!process_observed_terminal_state(state, &process,
                                                     latest_state,
                                                     sizeof(latest_state))) {
                    ++skipped_live_count;
                    continue;
                }
                if (process.saved) {
                    ++skipped_saved_count;
                    continue;
                }

                if (candidate_count == candidate_capacity) {
                    size_t new_capacity = candidate_capacity == 0
                                              ? 16
                                              : candidate_capacity * 2;
                    cleanup_candidate_t *new_candidates = realloc(
                        candidates,
                        new_capacity * sizeof(*new_candidates));
                    if (new_candidates == NULL) {
                        int saved_errno = errno;
                        fclose(file);
                        free(candidates);
                        unlock_state(lock_fd);
                        MANAGER_RETURN(manager_api_error(
                            client_fd, request_id, CUBICLE_ERR_INTERNAL,
                            "failed to allocate cleanup candidates",
                            false, saved_errno));
                    }
                    candidates = new_candidates;
                    candidate_capacity = new_capacity;
                }
                snprintf(candidates[candidate_count].process_id,
                         sizeof(candidates[candidate_count].process_id),
                         "%s", process.process_id);
                ++candidate_count;
            }
            fclose(file);
        }

        size_t removed_count = 0;
        for (size_t i = 0; i < candidate_count; ++i) {
            int found = 0;
            if (rewrite_process_records(state, candidates[i].process_id,
                                        NULL, 1, &found) < 0) {
                int saved_errno = errno;
                free(candidates);
                unlock_state(lock_fd);
                MANAGER_RETURN(manager_api_error(
                    client_fd, request_id, CUBICLE_ERR_IO,
                    "failed to remove cleanup process", true,
                    saved_errno));
            }
            char controller_state[PATH_MAX];
            char controller_log[PATH_MAX];
            if (controller_state_path(controller_state, state,
                                      candidates[i].process_id) < 0 ||
                controller_log_path(controller_log, state,
                                    candidates[i].process_id) < 0 ||
                remove_tree_if_exists(controller_state) < 0 ||
                remove_tree_if_exists(controller_log) < 0) {
                int saved_errno = errno;
                free(candidates);
                unlock_state(lock_fd);
                MANAGER_RETURN(manager_api_error(
                    client_fd, request_id, CUBICLE_ERR_IO,
                    "failed to remove cleanup process state", true,
                    saved_errno));
            }
            if (found) {
                ++removed_count;
            }
        }
        free(candidates);
        unlock_state(lock_fd);

        char result[256];
        int length = snprintf(
            result, sizeof(result),
            "{\"removed_count\":%zu,\"skipped_live_count\":%zu,\"skipped_saved_count\":%zu,\"failed_count\":0}",
            removed_count, skipped_live_count, skipped_saved_count);
        if (length < 0 || (size_t)length >= sizeof(result)) {
            MANAGER_RETURN(-1);
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "workspace.key.add") == 0) {
        char workspace_ref[128];
        char public_key_hex[512];
        char label[CUBICLE_KEY_LABEL_MAX] = "";
        uint64_t capabilities = 0;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "public_key",
                                             public_key_hex,
                                             sizeof(public_key_hex),
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "label", label,
                                             sizeof(label), NULL,
                                             &validation_error) < 0 ||
            cubicle_json_get_required_u64(params, "capabilities",
                                          &capabilities,
                                          &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid key add request",
                                             false, 0));
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }
        cubicle_workspace_record_t workspace;
        if (find_workspace(state, workspace_ref, &workspace) < 0) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false,
                                             0));
        }
        if (!connection_has_workspace_capability(
                state, workspace.id, connection,
                CUBICLE_CAP_WORKSPACE_MANAGE_KEYS)) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "workspace key management requires owner access",
                                             false, 0));
        }

        workspace_key_record_t record;
        if (build_workspace_key_record_from_public_hex(
                &record, workspace.id, public_key_hex, label,
                (cubicle_capability_mask_t)capabilities, now_ms) < 0) {
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid public key", false, 0));
        }

        if (append_workspace_key_record(state, &record) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to persist key",
                                             true, saved_errno));
        }
        unlock_state(lock_fd);

        char result[1024];
        if (workspace_key_info_json(&record, result, sizeof(result)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode key",
                                             false, errno));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "workspace.key.list") == 0) {
        char workspace_ref[128];
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "missing workspace id", false,
                                             0));
        }
        cubicle_workspace_record_t workspace;
        if (find_workspace(state, workspace_ref, &workspace) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false,
                                             0));
        }
        if (!connection_has_workspace_capability(
                state, workspace.id, connection, CUBICLE_CAP_WORKSPACE_READ)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "workspace key list requires read access",
                                             false, 0));
        }

        FILE *file = open_state_file_for_read(state, "workspace-keys.tsv");
        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        size_t used = 0;
        int written = snprintf(result, sizeof(result), "{\"keys\":[");
        if (written < 0 || (size_t)written >= sizeof(result)) {
            MANAGER_RETURN(-1);
        }
        used = (size_t)written;
        size_t count = 0;
        if (file != NULL) {
            char line[1024];
            while (fgets(line, sizeof(line), file) != NULL) {
                workspace_key_record_t record;
                char item[1024];
                if (parse_workspace_key_record(line, &record) != 0 ||
                    strcmp(record.workspace_id, workspace.id) != 0 ||
                    workspace_key_info_json(&record, item,
                                            sizeof(item)) < 0) {
                    continue;
                }
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s%s", count == 0 ? "" : ",", item);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    fclose(file);
                    MANAGER_RETURN(manager_api_error(
                        client_fd, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                        "key list response too large", false, 0));
                }
                used += (size_t)written;
                ++count;
            }
            fclose(file);
        }
        written = snprintf(result + used, sizeof(result) - used, "]}");
        if (written < 0 || (size_t)written >= sizeof(result) - used) {
            MANAGER_RETURN(manager_api_error(
                client_fd, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                "key list response too large", false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "workspace.key.update") == 0) {
        char workspace_ref[128];
        char key_id[128];
        uint64_t capabilities = 0;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "key_id", key_id,
                                             sizeof(key_id),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_u64(params, "capabilities",
                                          &capabilities,
                                          &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid key update request",
                                             false, 0));
        }
        cubicle_workspace_record_t workspace;
        if (find_workspace(state, workspace_ref, &workspace) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false,
                                             0));
        }
        if (!connection_has_workspace_capability(
                state, workspace.id, connection,
                CUBICLE_CAP_WORKSPACE_MANAGE_KEYS)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "workspace key update requires owner access",
                                             false, 0));
        }
        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }
        int found = 0;
        if (update_workspace_key_capabilities(
                state, workspace.id, key_id,
                (cubicle_capability_mask_t)capabilities, &found) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to update key", true,
                                             saved_errno));
        }
        unlock_state(lock_fd);
        if (!found) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "key not found", false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, "{}"));
    }

    if (strcmp(method, "workspace.key.revoke") == 0) {
        char workspace_ref[128];
        char key_id[128];
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "key_id", key_id,
                                             sizeof(key_id),
                                             &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid key revoke request",
                                             false, 0));
        }
        cubicle_workspace_record_t workspace;
        if (find_workspace(state, workspace_ref, &workspace) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false,
                                             0));
        }
        if (!connection_has_workspace_capability(
                state, workspace.id, connection,
                CUBICLE_CAP_WORKSPACE_MANAGE_KEYS)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "workspace key revoke requires owner access",
                                             false, 0));
        }
        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to lock manager state",
                                             true, errno));
        }
        int found = 0;
        if (update_workspace_key_revocation(state, workspace.id, key_id,
                                            now_ms, &found) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to revoke key", true,
                                             saved_errno));
        }
        unlock_state(lock_fd);
        if (!found) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "key not found", false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, "{}"));
    }

    if (strcmp(method, "attachment.request") == 0) {
        char process_id[128];
        char mode[32] = "observer";
        uint64_t channels = 0;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "process_id",
                                             process_id,
                                             sizeof(process_id),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_u64(params, "channels", &channels,
                                          &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "mode", mode,
                                             sizeof(mode),
                                             &validation_error) < 0 ||
            channels == 0 ||
            (channels & ~(uint64_t)(CUBICLE_CHANNEL_STDIN |
                                    CUBICLE_CHANNEL_STDOUT |
                                    CUBICLE_CHANNEL_STDERR |
                                    CUBICLE_CHANNEL_TTY)) != 0 ||
            (strcmp(mode, "observer") != 0 &&
             strcmp(mode, "interactive") != 0)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid attachment request",
                                             false, 0));
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_id, NULL, &process,
                                &ambiguous) < 0) {
            (void)ambiguous;
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "process not found", false, 0));
        }
        cubicle_capability_mask_t attachment_capability =
            strcmp(mode, "interactive") == 0 ||
                    (channels & CUBICLE_CHANNEL_STDIN) != 0
                ? (CUBICLE_CAP_PROCESS_OBSERVE | CUBICLE_CAP_PROCESS_INPUT)
                : CUBICLE_CAP_PROCESS_OBSERVE;
        if (!connection_has_workspace_capability(
                state, process.workspace_id, connection,
                attachment_capability)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "attachment access is required",
                                             false, 0));
        }

        uint64_t granted_channels = channels;
        if (strcmp(process.mode,
                   cubicle_process_mode_name(CUBICLE_PROCESS_TTY)) == 0 ||
            strcmp(process.mode,
                   cubicle_process_mode_name(
                       CUBICLE_PROCESS_TTY_CAPTURED_STDERR)) == 0) {
            if ((granted_channels & CUBICLE_CHANNEL_STDOUT) != 0) {
                granted_channels |= CUBICLE_CHANNEL_TTY;
            }
        } else {
            granted_channels &= ~((uint64_t)CUBICLE_CHANNEL_TTY);
        }
        if (granted_channels == 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "no requested channels are available",
                                             false, 0));
        }

        char grant_id[CUBICLE_ID_STRING_LENGTH];
        if (cubicle_generate_hex_id(grant_id, sizeof(grant_id)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to allocate grant id",
                                             false, errno));
        }
        char granted_channels_text[64];
        if (channel_mask_string(granted_channels, granted_channels_text,
                                sizeof(granted_channels_text)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to encode channels",
                                             false, errno));
        }

        char endpoint_uri[CUBICLE_ENDPOINT_URI_MAX];
        int uri_length = snprintf(endpoint_uri, sizeof(endpoint_uri),
                                  "unix://%s", process.control_socket);
        if (uri_length < 0 || (size_t)uri_length >= sizeof(endpoint_uri)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "controller endpoint too long",
                                             false, 0));
        }

        uint64_t expires_at_ms = now_ms + 60000;
        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        int length = snprintf(
            result, sizeof(result),
            "{\"grant_id\":\"%s\",\"manager_id\":\"%s\",\"workspace_id\":\"%s\",\"process_id\":\"%s\",\"client_key_id\":\"%s\",\"endpoint\":{\"uri\":\"%s\",\"server_identity\":\"local-controller\"},\"token\":\"local:%s:%s\",\"issued_at_ms\":%llu,\"expires_at_ms\":%llu,\"connection_limit\":1,\"granted_channels\":\"%s\",\"mode\":\"%s\"}",
            grant_id, id, process.workspace_id, process.process_id,
            connection->authenticated ? connection->session.client_key_id
                                      : "local-bootstrap",
            endpoint_uri, grant_id, process.process_id,
            (unsigned long long)now_ms,
            (unsigned long long)expires_at_ms, granted_channels_text, mode);
        if (length < 0 || (size_t)length >= sizeof(result)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "attachment grant too large",
                                             false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "events.subscribe") == 0) {
        char workspace_ref[128];
        char process_ref[128];
        char workspace_id[128];
        const char *workspace_id_ptr = NULL;
        uint64_t after_sequence = 0;
        uint64_t limit = 100;
        cubicle_validation_error_t validation_error;
        int has_workspace_ref = 0;
        int has_process_ref = 0;

        if (cubicle_json_get_optional_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &has_workspace_ref,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "process_id",
                                             process_ref,
                                             sizeof(process_ref),
                                             &has_process_ref,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_u64(params, "after_sequence",
                                          &after_sequence, NULL,
                                          &validation_error) < 0 ||
            cubicle_json_get_optional_u64(params, "limit", &limit, NULL,
                                          &validation_error) < 0 ||
            limit > 1024) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid events subscribe request",
                                             false, 0));
        }
        if (has_workspace_ref && workspace_ref[0] != '\0') {
            cubicle_workspace_record_t workspace;
            if (find_workspace(state, workspace_ref, &workspace) < 0) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_NOT_FOUND,
                                                 "workspace not found",
                                                 false, 0));
            }
            snprintf(workspace_id, sizeof(workspace_id), "%s",
                     workspace.id);
            workspace_id_ptr = workspace_id;
            if (!connection_has_workspace_capability(
                    state, workspace_id, connection,
                    CUBICLE_CAP_EVENTS_READ)) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_PERMISSION_DENIED,
                                                 "event access is required",
                                                 false, 0));
            }
        }
        if (has_process_ref && process_ref[0] != '\0') {
            cubicle_process_record_t process;
            int ambiguous = 0;
            if (find_process_record(state, process_ref, workspace_id_ptr,
                                    &process, &ambiguous) < 0) {
                (void)ambiguous;
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_NOT_FOUND,
                                                 "process not found",
                                                 false, 0));
            }
            if (!connection_has_workspace_capability(
                    state, process.workspace_id, connection,
                    CUBICLE_CAP_EVENTS_READ)) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_PERMISSION_DENIED,
                                                 "event access is required",
                                                 false, 0));
            }
        }

        char subscription_id[CUBICLE_ID_STRING_LENGTH];
        if (cubicle_generate_hex_id(subscription_id,
                                    sizeof(subscription_id)) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INTERNAL,
                                             "failed to allocate subscription",
                                             false, errno));
        }
        char result[512];
        int length = snprintf(
            result, sizeof(result),
            "{\"subscription_id\":\"%s\",\"after_sequence\":%llu,\"limit\":%llu}",
            subscription_id, (unsigned long long)after_sequence,
            (unsigned long long)(limit == 0 ? 100 : limit));
        if (length < 0 || (size_t)length >= sizeof(result)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "subscription response too large",
                                             false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "events.list") == 0) {
        char workspace_ref[128];
        char process_ref[128];
        char workspace_id[128];
        const char *workspace_id_ptr = NULL;
        const char *process_id_ptr = NULL;
        uint64_t after_sequence = 0;
        uint64_t limit = 100;
        cubicle_validation_error_t validation_error;
        int has_workspace_ref = 0;
        int has_process_ref = 0;
        int has_after_sequence = 0;
        int has_limit = 0;

        if (cubicle_json_get_optional_string(params, "workspace_id",
                                             workspace_ref,
                                             sizeof(workspace_ref),
                                             &has_workspace_ref,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_string(params, "process_id",
                                             process_ref,
                                             sizeof(process_ref),
                                             &has_process_ref,
                                             &validation_error) < 0 ||
            cubicle_json_get_optional_u64(params, "after_sequence",
                                          &after_sequence,
                                          &has_after_sequence,
                                          &validation_error) < 0 ||
            cubicle_json_get_optional_u64(params, "limit", &limit,
                                          &has_limit,
                                          &validation_error) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid events list request",
                                             false, 0));
        }
        (void)has_after_sequence;
        (void)has_limit;
        if (has_workspace_ref && workspace_ref[0] != '\0') {
            cubicle_workspace_record_t workspace;
            if (find_workspace(state, workspace_ref, &workspace) < 0) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_NOT_FOUND,
                                                 "workspace not found",
                                                 false, 0));
            }
            snprintf(workspace_id, sizeof(workspace_id), "%s",
                     workspace.id);
            workspace_id_ptr = workspace_id;
            if (!connection_has_workspace_capability(
                    state, workspace_id, connection,
                    CUBICLE_CAP_EVENTS_READ)) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_PERMISSION_DENIED,
                                                 "event access is required",
                                                 false, 0));
            }
        }
        if (has_process_ref && process_ref[0] != '\0') {
            cubicle_process_record_t process;
            int ambiguous = 0;
            if (find_process_record(state, process_ref, workspace_id_ptr,
                                    &process, &ambiguous) < 0) {
                (void)ambiguous;
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_NOT_FOUND,
                                                 "process not found",
                                                 false, 0));
            }
            if (!connection_has_workspace_capability(
                    state, process.workspace_id, connection,
                    CUBICLE_CAP_EVENTS_READ)) {
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_PERMISSION_DENIED,
                                                 "event access is required",
                                                 false, 0));
            }
            process_id_ptr = process_ref;
        }

        FILE *file = open_state_file_for_read(state, "workspace-events.log");
        char result[CUBICLE_PROCESS_RECORD_LINE_MAX];
        size_t used = 0;
        int written = snprintf(result, sizeof(result), "{\"events\":[");
        if (written < 0 || (size_t)written >= sizeof(result)) {
            MANAGER_RETURN(-1);
        }
        used = (size_t)written;

        size_t count = 0;
        uint64_t global_sequence = 0;
        int has_more = 0;
        if (file != NULL) {
            char line[PATH_MAX + 1400];
            while (fgets(line, sizeof(line), file) != NULL) {
                ++global_sequence;
                if (global_sequence <= after_sequence) {
                    continue;
                }
                if (limit > 0 && count >= limit) {
                    has_more = 1;
                    break;
                }

                char copy[PATH_MAX + 1400];
                snprintf(copy, sizeof(copy), "%s", line);
                char *event_workspace_id = strtok(copy, "\t\n");
                char *event_process_id = strtok(NULL, "\t\n");
                (void)strtok(NULL, "\t\n");
                char *payload = strtok(NULL, "\n");
                if (event_workspace_id == NULL || event_process_id == NULL ||
                    payload == NULL ||
                    (workspace_id_ptr != NULL &&
                     strcmp(event_workspace_id, workspace_id_ptr) != 0) ||
                    (process_id_ptr != NULL &&
                     strcmp(event_process_id, process_id_ptr) != 0)) {
                    continue;
                }

                char item[CUBICLE_PROCESS_RECORD_LINE_MAX];
                if (event_info_json(global_sequence, event_workspace_id,
                                    event_process_id, payload, item,
                                    sizeof(item)) < 0) {
                    continue;
                }
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s%s", count == 0 ? "" : ",", item);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    if (count == 0) {
                        fclose(file);
                        MANAGER_RETURN(manager_api_error(
                            client_fd, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                            "events response too large", false, 0));
                    }
                    has_more = 1;
                    break;
                }
                used += (size_t)written;
                ++count;
            }
            fclose(file);
        }

        written = snprintf(result + used, sizeof(result) - used,
                           "],\"count\":%zu,\"has_more\":%s}", count,
                           has_more ? "true" : "false");
        if (written < 0 || (size_t)written >= sizeof(result) - used) {
            MANAGER_RETURN(manager_api_error(
                client_fd, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                "events response too large", false, 0));
        }
        MANAGER_RETURN(manager_api_success(client_fd, request_id, result));
    }

    if (strcmp(method, "process.read_output") == 0) {
        char process_id[128];
        char stream[32];
        uint64_t offset = 0;
        uint64_t maximum_length = 0;
        cubicle_validation_error_t validation_error;
        if (cubicle_json_get_required_string(params, "process_id", process_id,
                                             sizeof(process_id),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_string(params, "stream", stream,
                                             sizeof(stream),
                                             &validation_error) < 0 ||
            cubicle_json_get_required_u64(params, "offset", &offset,
                                          &validation_error) < 0 ||
            cubicle_json_get_required_u64(params, "maximum_length",
                                          &maximum_length,
                                          &validation_error) < 0 ||
            maximum_length > 8192) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "invalid read_output request",
                                             false, 0));
        }

        const char *file_name = api_stream_file_name(stream);
        if (file_name == NULL) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "unknown output stream", false,
                                             0));
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_id, NULL, &process,
                                &ambiguous) < 0) {
            (void)ambiguous;
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "process not found", false, 0));
        }
        if (!connection_has_workspace_capability(
                state, process.workspace_id, connection,
                CUBICLE_CAP_PROCESS_READ)) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_PERMISSION_DENIED,
                                             "process output access is required",
                                             false, 0));
        }

        char path[PATH_MAX];
        if (process_output_path(path, state, process.process_id,
                                file_name) < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "output path too long", false,
                                             0));
        }

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to open output", true,
                                             errno));
        }

        struct stat status;
        if (fstat(fd, &status) < 0) {
            int saved_errno = errno;
            close(fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_IO,
                                             "failed to stat output", true,
                                             saved_errno));
        }
        if (offset > (uint64_t)status.st_size) {
            close(fd);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_INVALID_ARGUMENT,
                                             "offset is past end of output",
                                             false, 0));
        }

        size_t available = (size_t)((uint64_t)status.st_size - offset);
        size_t read_length = available < maximum_length ? available
                                                        : (size_t)maximum_length;
        char data[8193];
        size_t total = 0;
        while (total < read_length) {
            ssize_t nread = pread(fd, data + total, read_length - total,
                                  (off_t)(offset + total));
            if (nread < 0) {
                if (errno == EINTR) {
                    continue;
                }
                int saved_errno = errno;
                close(fd);
                MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                                 CUBICLE_ERR_IO,
                                                 "failed to read output",
                                                 true, saved_errno));
            }
            if (nread == 0) {
                break;
            }
            total += (size_t)nread;
        }
        close(fd);

        size_t safe_total = cubicle_json_safe_utf8_prefix_length(data, total);
        if (safe_total > 0 && safe_total < total) {
            total = safe_total;
        }

        cubicle_json_builder_t result;
        cubicle_json_builder_init(&result);
        if (cubicle_json_builder_appendf(
                &result,
                "{\"start_offset\":%llu,\"next_offset\":%llu,\"end_of_stream\":%s,\"data\":",
                (unsigned long long)offset,
                (unsigned long long)(offset + total),
                offset + total >= (uint64_t)status.st_size ? "true"
                                                           : "false") < 0 ||
            cubicle_json_builder_append_string_n(&result, data, total) < 0 ||
            cubicle_json_builder_appendf(&result, ",\"length\":%zu}",
                                         total) < 0) {
            cubicle_json_builder_cleanup(&result);
            MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "read_output response too large",
                                             false, 0));
        }
        int read_output_result = manager_api_success(client_fd, request_id,
                                                     result.data);
        cubicle_json_builder_cleanup(&result);
        MANAGER_RETURN(read_output_result);
    }

    MANAGER_RETURN(manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_UNSUPPORTED,
                                     "method is not implemented", false, 0));
#undef MANAGER_RETURN
}

static int handle_manager_connection(const manager_state_t *state,
                                     int client_fd,
                                     manager_runtime_t *runtime)
{
    manager_connection_t connection;
    load_peer_credentials(client_fd, &connection);
    connection.runtime = runtime;
    while (!runtime_shutdown_requested(runtime)) {
        if (handle_manager_client(state, client_fd, &connection) == 0) {
            continue;
        }
        return errno == ECONNRESET ? 0 : -1;
    }
    return 0;
}

static int manager_runtime_init(manager_runtime_t *runtime,
                                const manager_state_t *state,
                                int max_clients)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->state = state;
    runtime->started_at_ms = manager_time_ms();
    runtime->max_clients = max_clients;
    for (int i = 0; i < CUBICLE_MANAGER_MAX_CLIENTS; ++i) {
        runtime->workers[i].fd = -1;
    }

    int result = pthread_mutex_init(&runtime->sessions_mutex, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    result = pthread_mutex_init(&runtime->workers_mutex, NULL);
    if (result != 0) {
        pthread_mutex_destroy(&runtime->sessions_mutex);
        errno = result;
        return -1;
    }
    result = pthread_cond_init(&runtime->workers_cond, NULL);
    if (result != 0) {
        pthread_mutex_destroy(&runtime->workers_mutex);
        pthread_mutex_destroy(&runtime->sessions_mutex);
        errno = result;
        return -1;
    }
    return 0;
}

static void manager_runtime_destroy(manager_runtime_t *runtime)
{
    pthread_cond_destroy(&runtime->workers_cond);
    pthread_mutex_destroy(&runtime->workers_mutex);
    pthread_mutex_destroy(&runtime->sessions_mutex);
}

static void *manager_worker_main(void *argument)
{
    manager_worker_slot_t *slot = argument;
    manager_runtime_t *runtime = slot->runtime;
    int client_fd = slot->fd;

    if (handle_manager_connection(runtime->state, client_fd, runtime) < 0) {
        manager_log_error(errno);
    }
    close(client_fd);

    pthread_mutex_lock(&runtime->workers_mutex);
    slot->fd = -1;
    slot->active = 0;
    if (runtime->active_workers > 0) {
        --runtime->active_workers;
    }
    pthread_cond_broadcast(&runtime->workers_cond);
    pthread_mutex_unlock(&runtime->workers_mutex);
    return NULL;
}

static int manager_runtime_start_worker(manager_runtime_t *runtime,
                                        int client_fd)
{
    pthread_mutex_lock(&runtime->workers_mutex);
    if (runtime->shutdown_requested ||
        runtime->active_workers >= runtime->max_clients) {
        pthread_mutex_unlock(&runtime->workers_mutex);
        errno = EBUSY;
        return -1;
    }

    manager_worker_slot_t *slot = NULL;
    for (int i = 0; i < runtime->max_clients; ++i) {
        if (!runtime->workers[i].active) {
            slot = &runtime->workers[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&runtime->workers_mutex);
        errno = EBUSY;
        return -1;
    }

    slot->active = 1;
    slot->fd = client_fd;
    slot->runtime = runtime;
    ++runtime->active_workers;
    pthread_mutex_unlock(&runtime->workers_mutex);

    pthread_t thread;
    int result = pthread_create(&thread, NULL, manager_worker_main, slot);
    if (result != 0) {
        pthread_mutex_lock(&runtime->workers_mutex);
        slot->active = 0;
        slot->fd = -1;
        if (runtime->active_workers > 0) {
            --runtime->active_workers;
        }
        pthread_cond_broadcast(&runtime->workers_cond);
        pthread_mutex_unlock(&runtime->workers_mutex);
        errno = result;
        return -1;
    }
    result = pthread_detach(thread);
    if (result != 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager",
                    "failed to detach manager client worker");
    }
    return 0;
}

static void manager_runtime_shutdown_worker_sockets(manager_runtime_t *runtime)
{
    pthread_mutex_lock(&runtime->workers_mutex);
    for (int i = 0; i < runtime->max_clients; ++i) {
        if (runtime->workers[i].active && runtime->workers[i].fd >= 0) {
            shutdown(runtime->workers[i].fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&runtime->workers_mutex);
}

static void manager_runtime_wait_workers(manager_runtime_t *runtime)
{
    pthread_mutex_lock(&runtime->workers_mutex);
    while (runtime->active_workers > 0) {
        pthread_cond_wait(&runtime->workers_cond, &runtime->workers_mutex);
    }
    pthread_mutex_unlock(&runtime->workers_mutex);
}

static int daemonize_manager(void)
{
    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        _exit(0);
    }

    if (setsid() < 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        _exit(0);
    }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        return -1;
    }
    if (dup2(devnull, STDIN_FILENO) < 0 ||
        dup2(devnull, STDOUT_FILENO) < 0 ||
        dup2(devnull, STDERR_FILENO) < 0) {
        int saved_errno = errno;
        close(devnull);
        errno = saved_errno;
        return -1;
    }
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }
    return 0;
}

static int command_daemon(const manager_state_t *state, int argc, char **argv)
{
    const char *requested_socket = NULL;
    const char *requested_uri = NULL;
    int allow_insecure = 0;
    int foreground = 0;
    int poll_interval_ms = 250;
    int max_clients = CUBICLE_MANAGER_DEFAULT_MAX_CLIENTS;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--control-socket") == 0 && i + 1 < argc) {
            requested_socket = argv[++i];
        } else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            requested_uri = argv[++i];
        } else if (strcmp(argv[i], "--allow-insecure") == 0) {
            allow_insecure = 1;
        } else if (strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "--event-interval-ms") == 0 && i + 1 < argc) {
            poll_interval_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-clients") == 0 && i + 1 < argc) {
            max_clients = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown daemon option: %s\n", argv[i]);
            return 2;
        }
    }

    if (poll_interval_ms < 0) {
        fprintf(stderr, "daemon requires nonnegative --event-interval-ms\n");
        return 2;
    }
    if (max_clients < 1 || max_clients > CUBICLE_MANAGER_MAX_CLIENTS) {
        fprintf(stderr, "daemon requires --max-clients between 1 and %d\n",
                CUBICLE_MANAGER_MAX_CLIENTS);
        return 2;
    }
    if (requested_socket != NULL && requested_uri != NULL) {
        fprintf(stderr,
                "daemon accepts only one of --control-socket or --listen\n");
        return 2;
    }

    int daemon_lock_fd = lock_daemon(state);
    if (daemon_lock_fd < 0) {
        manager_log_error(errno);
        return 1;
    }

    char listen_uri[CUBICLE_ENDPOINT_URI_MAX];
    if (manager_listen_uri(listen_uri, state, requested_socket,
                           requested_uri) < 0) {
        manager_log_error(errno);
        unlock_state(daemon_lock_fd);
        return 1;
    }

    char cleanup_path[PATH_MAX];
    int listen_fd = open_manager_listener(state, listen_uri, allow_insecure,
                                          cleanup_path);
    if (listen_fd < 0) {
        if (errno == EACCES && strncmp(listen_uri, "tcp://", 6) == 0) {
            fprintf(stderr,
                    "manager: refusing unauthenticated TCP listener %s without --allow-insecure\n",
                    listen_uri);
        }
        manager_log_error(errno);
        unlock_state(daemon_lock_fd);
        return 1;
    }

    if (set_fd_nonblocking(listen_fd) < 0) {
        manager_log_error(errno);
        close(listen_fd);
        if (cleanup_path[0] != '\0') {
            unlink(cleanup_path);
        }
        unlock_state(daemon_lock_fd);
        return 1;
    }

    if (reconcile_process_records(state) < 0) {
        manager_log_error(errno);
        close(listen_fd);
        if (cleanup_path[0] != '\0') {
            unlink(cleanup_path);
        }
        unlock_state(daemon_lock_fd);
        return 1;
    }

    if (!foreground && daemonize_manager() < 0) {
        manager_log_error(errno);
        close(listen_fd);
        if (cleanup_path[0] != '\0') {
            unlink(cleanup_path);
        }
        unlock_state(daemon_lock_fd);
        return 1;
    }

    int result = 0;
    manager_runtime_t runtime;
    if (manager_runtime_init(&runtime, state, max_clients) < 0) {
        manager_log_error(errno);
        close(listen_fd);
        if (cleanup_path[0] != '\0') {
            unlink(cleanup_path);
        }
        unlock_state(daemon_lock_fd);
        return 1;
    }

    if (autostart_restart_processes(state) < 0) {
        manager_log_error(errno);
        manager_runtime_destroy(&runtime);
        close(listen_fd);
        if (cleanup_path[0] != '\0') {
            unlink(cleanup_path);
        }
        unlock_state(daemon_lock_fd);
        return 1;
    }

    while (!runtime_shutdown_requested(&runtime)) {
        if (poll_workspace_events(state, NULL, NULL) != 0) {
            manager_log_error(errno);
        }
        if (reconcile_process_records(state) < 0) {
            manager_log_error(errno);
        }

        struct pollfd daemon_fd = {.fd = listen_fd, .events = POLLIN};
        int ready = poll(&daemon_fd, 1, poll_interval_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            manager_log_error(errno);
            result = 1;
            break;
        }

        if (ready == 0 || (daemon_fd.revents & POLLIN) == 0) {
            continue;
        }

        for (;;) {
            int client_fd = accept(listen_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                manager_log_error(errno);
                result = 1;
                break;
            }
            if (set_fd_cloexec(client_fd) < 0) {
                manager_log_error(errno);
                close(client_fd);
                result = 1;
                break;
            }

            if (manager_runtime_start_worker(&runtime, client_fd) < 0) {
                manager_log_error(errno);
                shutdown(client_fd, SHUT_RDWR);
                close(client_fd);
            }
        }
    }

    close(listen_fd);
    manager_runtime_shutdown_worker_sockets(&runtime);
    manager_runtime_wait_workers(&runtime);
    manager_runtime_destroy(&runtime);
    if (cleanup_path[0] != '\0') {
        unlink(cleanup_path);
    }
    unlock_state(daemon_lock_fd);
    return result;
}

static int dispatch_command(const manager_state_t *state, int argc, char **argv)
{
    if (argc < 1) {
        return 2;
    }

    if (strcmp(argv[0], "daemon") == 0) {
        return command_daemon(state, argc - 1, &argv[1]);
    }

    if (argc < 2) {
        return 2;
    }

    if (strcmp(argv[0], "workspace") == 0 &&
        strcmp(argv[1], "create") == 0 && argc >= 3) {
        return command_workspace_create(state, argc - 2, &argv[2]);
    }

    if (strcmp(argv[0], "workspace") == 0 &&
        strcmp(argv[1], "list") == 0 && argc == 2) {
        return command_workspace_list(state);
    }

    if (strcmp(argv[0], "process") == 0 &&
        strcmp(argv[1], "register") == 0) {
        return command_process_register(state, argc - 2, &argv[2]);
    }

    if (strcmp(argv[0], "process") == 0 &&
        strcmp(argv[1], "start") == 0) {
        return command_process_start(state, argc - 2, &argv[2]);
    }

    if (strcmp(argv[0], "process") == 0 &&
        strcmp(argv[1], "list") == 0) {
        return command_process_list(state, argc - 2, &argv[2]);
    }

    if (strcmp(argv[0], "process") == 0 &&
        strcmp(argv[1], "resolve") == 0) {
        return command_process_resolve(state, argc - 2, &argv[2]);
    }

    if (strcmp(argv[0], "events") == 0 &&
        strcmp(argv[1], "poll") == 0) {
        return command_events_poll(state, argc - 2, &argv[2]);
    }

    if (strcmp(argv[0], "events") == 0 &&
        strcmp(argv[1], "list") == 0) {
        return command_events_list(state, argc - 2, &argv[2]);
    }

    if (strcmp(argv[0], "events") == 0 &&
        strcmp(argv[1], "follow") == 0) {
        return command_events_follow(state, argc - 2, &argv[2]);
    }

    return 2;
}

int main(int argc, char **argv)
{
    int config_option_result = apply_config_option_prescan(argc, argv);
    if (config_option_result > 0) {
        return 0;
    }
    if (config_option_result < 0) {
        return 2;
    }

    cubicle_config_t config;
    char config_error[512];
    if (cubicle_config_load(&config, config_error, sizeof(config_error)) < 0) {
        fprintf(stderr, "manager configuration error: %s\n", config_error);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    manager_state_t state;
    snprintf(state.dir, sizeof(state.dir), "%s", config.manager_state_dir);
    snprintf(state.runtime_dir, sizeof(state.runtime_dir), "%s",
             config.manager_runtime_dir);
    snprintf(state.log_dir, sizeof(state.log_dir), "%s",
             config.manager_log_dir);
    snprintf(state.controller_bin, sizeof(state.controller_bin), "%s",
             config.controller_binary);
    state.controller_debug[0] = '\0';
    if (config.controller_debug_input) {
        snprintf(state.controller_debug + strlen(state.controller_debug),
                 sizeof(state.controller_debug) - strlen(state.controller_debug),
                 "%sinput", state.controller_debug[0] == '\0' ? "" : ",");
    }
    if (config.controller_debug_library) {
        snprintf(state.controller_debug + strlen(state.controller_debug),
                 sizeof(state.controller_debug) - strlen(state.controller_debug),
                 "%slibrary", state.controller_debug[0] == '\0' ? "" : ",");
    }
    if (config.controller_debug_terminal) {
        snprintf(state.controller_debug + strlen(state.controller_debug),
                 sizeof(state.controller_debug) - strlen(state.controller_debug),
                 "%sterminal", state.controller_debug[0] == '\0' ? "" : ",");
    }
    if (state.controller_debug[0] == '\0') {
        snprintf(state.controller_debug, sizeof(state.controller_debug), "none");
    }
    snprintf(state.listen_uri, sizeof(state.listen_uri), "%s",
             config.manager_listen_uri);
    state.socket_mode = (mode_t)config.manager_socket_mode;
    snprintf(state.socket_group, sizeof(state.socket_group), "%s",
             config.manager_socket_group);

    int command_index = -1;
    int state_dir_overridden = 0;
    int runtime_dir_overridden = 0;
    int log_dir_overridden = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            ++i;
            continue;
        }

        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            int result = snprintf(state.dir, sizeof(state.dir), "%s", argv[++i]);
            if (result < 0 || (size_t)result >= sizeof(state.dir)) {
                fprintf(stderr, "State directory path is too long\n");
                return 2;
            }
            state_dir_overridden = 1;
            continue;
        }

        if (strcmp(argv[i], "--runtime-dir") == 0 && i + 1 < argc) {
            int result = snprintf(state.runtime_dir,
                                  sizeof(state.runtime_dir), "%s", argv[++i]);
            if (result < 0 || (size_t)result >= sizeof(state.runtime_dir)) {
                fprintf(stderr, "Runtime directory path is too long\n");
                return 2;
            }
            runtime_dir_overridden = 1;
            continue;
        }

        if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            int result = snprintf(state.log_dir,
                                  sizeof(state.log_dir), "%s", argv[++i]);
            if (result < 0 || (size_t)result >= sizeof(state.log_dir)) {
                fprintf(stderr, "Log directory path is too long\n");
                return 2;
            }
            log_dir_overridden = 1;
            continue;
        }

        if (strcmp(argv[i], "--controller-bin") == 0 && i + 1 < argc) {
            int result = snprintf(state.controller_bin,
                                  sizeof(state.controller_bin), "%s", argv[++i]);
            if (result < 0 || (size_t)result >= sizeof(state.controller_bin)) {
                fprintf(stderr, "Controller binary path is too long\n");
                return 2;
            }
            continue;
        }

        command_index = i;
        break;
    }

    if (command_index < 0) {
        print_usage(argv[0]);
        return 2;
    }

    if (state_dir_overridden && !runtime_dir_overridden) {
        int result = snprintf(state.runtime_dir, sizeof(state.runtime_dir),
                              "%s", state.dir);
        if (result < 0 || (size_t)result >= sizeof(state.runtime_dir)) {
            fprintf(stderr, "Runtime directory path is too long\n");
            return 2;
        }
        result = snprintf(state.listen_uri, sizeof(state.listen_uri),
                          "unix://%s/manager.sock", state.runtime_dir);
        if (result < 0 || (size_t)result >= sizeof(state.listen_uri)) {
            fprintf(stderr, "Manager socket path is too long\n");
            return 2;
        }
    }
    if (state_dir_overridden && !log_dir_overridden) {
        int result = snprintf(state.log_dir, sizeof(state.log_dir),
                              "%s", state.dir);
        if (result < 0 || (size_t)result >= sizeof(state.log_dir)) {
            fprintf(stderr, "Log directory path is too long\n");
            return 2;
        }
    }

    log_manager_effective_config(&state);

    if (verify_manager_directory("manager.state_dir", state.dir) < 0 ||
        verify_manager_directory("manager.runtime_dir",
                                 state.runtime_dir) < 0 ||
        verify_manager_directory("manager.log_dir", state.log_dir) < 0) {
        return 1;
    }

    char manager_key_dir[PATH_MAX];
    int key_dir_length = snprintf(manager_key_dir, sizeof(manager_key_dir),
                                  "%s/keys", state.dir);
    if (key_dir_length < 0 ||
        (size_t)key_dir_length >= sizeof(manager_key_dir) ||
        cubicle_auth_ensure_identity(manager_key_dir, "manager.key",
                                     "manager.pub", &state.identity) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager",
                    "failed to initialize manager identity");
        return 1;
    }
    cubicle_log(CUBICLE_LOG_INFO, "manager", state.identity.fingerprint);

    int result = dispatch_command(&state, argc - command_index, &argv[command_index]);
    if (result == 2) {
        print_usage(argv[0]);
    }

    return result;
}
