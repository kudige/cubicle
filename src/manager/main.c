#define _POSIX_C_SOURCE 200809L

#include "cubicle/log.h"
#include "cubicle/manager_registry.h"
#include "cubicle/process.h"
#include "cubicle/rpc.h"
#include "cubicle/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
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
    char controller_bin[PATH_MAX];
} manager_state_t;

#define CUBICLE_API_MAX_FRAME 65536
#define CUBICLE_API_CAPABILITIES \
    (CUBICLE_PROTOCOL_CAP_TRANSPORT_UNIX | CUBICLE_PROTOCOL_CAP_PROCESS_STREAM | \
     CUBICLE_PROTOCOL_CAP_PROCESS_TTY | CUBICLE_PROTOCOL_CAP_ATTACHMENT_DIRECT)

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--state-dir dir] workspace create NAME\n"
            "       %s [--state-dir dir] workspace list\n"
            "       %s [--state-dir dir] process register --workspace NAME_OR_ID --friendly-name NAME --mode MODE --controller-id ID --control-socket PATH [--process-id ID]\n"
            "       %s [--state-dir dir] [--controller-bin PATH] process start --workspace NAME_OR_ID --friendly-name NAME --mode stream|tty [--stdin-policy open|eof] -- COMMAND [ARGS...]\n"
            "       %s [--state-dir dir] process resolve PROCESS_ID_OR_NAME [--workspace NAME_OR_ID]\n"
            "       %s [--state-dir dir] events poll [--workspace NAME_OR_ID]\n"
            "       %s [--state-dir dir] events list [--workspace NAME_OR_ID]\n"
            "       %s [--state-dir dir] events follow [--iterations N] [--interval-ms N] [--workspace NAME_OR_ID]\n"
            "       %s [--state-dir dir] daemon [--control-socket PATH] [--event-interval-ms N]\n"
            "       %s [--state-dir dir] process list [--workspace NAME_OR_ID]\n",
            program, program, program, program, program, program, program,
            program, program, program);
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
                          state->dir, process_id);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int append_line(const manager_state_t *state, const char *file_name,
                       const char *line)
{
    char path[PATH_MAX];
    if (state_path(path, state, file_name) < 0) {
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return -1;
    }

    size_t length = strlen(line);
    int result = cubicle_write_all(fd, line, length);
    close(fd);
    return result;
}

static int set_fd_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
    if (state_path(path, state, "manager.daemon.lock") < 0) {
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

static int manager_socket_path(char path[PATH_MAX], const manager_state_t *state,
                               const char *requested_socket)
{
    if (requested_socket != NULL) {
        int result = snprintf(path, PATH_MAX, "%s", requested_socket);
        if (result < 0 || result >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    return state_path(path, state, "manager.sock");
}

static int is_live_unix_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
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

static int open_manager_socket(const char *path)
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

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        chmod(path, 0600) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    return fd;
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

    char line[PATH_MAX + 512];
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

    char line[PATH_MAX + 512];
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
    if (count_processes_for_workspace(state, workspace->id, &process_count,
                                      &running_count) < 0 ||
        cubicle_json_escape(escaped_name, sizeof(escaped_name),
                            workspace->name) < 0) {
        return -1;
    }

    int length = snprintf(buffer, buffer_size,
                          "{\"manager_id\":\"%s\",\"id\":\"%s\",\"name\":\"%s\",\"created_at_ms\":0,\"updated_at_ms\":0,\"process_count\":%zu,\"running_process_count\":%zu}",
                          manager_id_value, workspace->id, escaped_name,
                          process_count, running_count);
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

static int process_info_json(const char *manager_id_value,
                             const cubicle_process_record_t *process,
                             char *buffer,
                             size_t buffer_size)
{
    char escaped_name[256];
    if (cubicle_json_escape(escaped_name, sizeof(escaped_name),
                            process->friendly_name) < 0) {
        return -1;
    }

    int length = snprintf(buffer, buffer_size,
                          "{\"manager_id\":\"%s\",\"workspace_id\":\"%s\",\"id\":\"%s\",\"friendly_name\":\"%s\",\"mode\":\"%s\",\"state\":\"%s\",\"exit_code\":0,\"termination_signal\":0,\"has_exit_status\":false,\"stdout_offset\":0,\"stderr_offset\":0,\"tty_offset\":0,\"created_at_ms\":0,\"started_at_ms\":0,\"exited_at_ms\":0,\"local_pid\":0,\"local_pgid\":0}",
                          manager_id_value, process->workspace_id,
                          process->process_id, escaped_name, process->mode,
                          api_process_state_name(process->state));
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
    char line[PATH_MAX + 512];
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

    char line[PATH_MAX + 512];
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

static int command_workspace_create(const manager_state_t *state, const char *name)
{
    if (validate_field(name, "workspace name") < 0) {
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

    char line[256];
    int length = snprintf(line, sizeof(line), "%s\t%s\n", id, name);
    if (length < 0 || (size_t)length >= sizeof(line) ||
        append_line(state, "workspaces.tsv", line) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    unlock_state(lock_fd);
    printf("workspace id=%s name=%s\n", id, name);
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
            printf("%s\t%s\n", record.id, record.name);
        }
    }

    fclose(file);
    return 0;
}

static int append_process_record(const manager_state_t *state,
                                 const char *process_id,
                                 const char *workspace_id,
                                 const char *friendly_name,
                                 const char *mode,
                                 const char *process_state,
                                 const char *controller_id,
                                 const char *control_socket)
{
    char line[PATH_MAX + 512];
    int length = snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                          process_id, workspace_id, friendly_name, mode,
                          process_state, controller_id, control_socket);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return append_line(state, "processes.tsv", line);
}

static int process_events_path(char path[PATH_MAX], const manager_state_t *state,
                               const char *process_id)
{
    int result = snprintf(path, PATH_MAX, "%s/controllers/%s/events.log",
                          state->dir, process_id);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static long long cursor_for_process(cubicle_cursor_record_t *cursors, size_t cursor_count,
                                    const char *process_id)
{
    for (size_t i = 0; i < cursor_count; ++i) {
        if (strcmp(cursors[i].process_id, process_id) == 0) {
            return cursors[i].sequence;
        }
    }

    return 0;
}

static int update_cursor(cubicle_cursor_record_t *cursors, size_t *cursor_count,
                         const char *process_id, long long sequence)
{
    for (size_t i = 0; i < *cursor_count; ++i) {
        if (strcmp(cursors[i].process_id, process_id) == 0) {
            if (sequence > cursors[i].sequence) {
                cursors[i].sequence = sequence;
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
        return -1;
    }

    for (size_t i = 0; i < cursor_count; ++i) {
        fprintf(file, "%s\t%lld\n", cursors[i].process_id,
                cursors[i].sequence);
    }

    if (fclose(file) != 0) {
        return -1;
    }

    return rename(temp_path, path);
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

    if (strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_STREAM)) != 0) {
        fprintf(stderr, "Only stream mode can be registered currently\n");
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
                              control_socket) < 0) {
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

static int event_log_has_exit(const char *metadata_path)
{
    char events_path[PATH_MAX];
    int result = snprintf(events_path, sizeof(events_path), "%s", metadata_path);
    if (result < 0 || (size_t)result >= sizeof(events_path)) {
        errno = ENAMETOOLONG;
        return 0;
    }

    char *last_slash = strrchr(events_path, '/');
    if (last_slash == NULL) {
        return 0;
    }
    snprintf(last_slash + 1, (size_t)(events_path + sizeof(events_path) - last_slash - 1),
             "events.log");

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

static int wait_for_controller_ready(const char *control_socket,
                                     const char *metadata_path,
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
            event_log_has_exit(metadata_path)) {
            snprintf(process_state, process_state_size, "exited");
            return 0;
        }

        struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&delay, NULL);
    }

    errno = ETIMEDOUT;
    return -1;
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
                             const char *control_socket,
                             const char *mode,
                             const char *stdin_policy,
                             char **command)
{
    size_t command_count = 0;
    while (command[command_count] != NULL) {
        ++command_count;
    }

    size_t argv_count = 11 + command_count + 1;
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
    controller_argv[index++] = "--control-socket";
    controller_argv[index++] = (char *)control_socket;
    controller_argv[index++] = "--mode";
    controller_argv[index++] = (char *)mode;
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
        strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_TTY)) != 0) {
        fprintf(stderr, "Only stream and tty modes can be started currently\n");
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
    char control_socket[PATH_MAX];
    char metadata_path[PATH_MAX];
    if (controller_state_path(controller_state, state, process_id) < 0 ||
        controller_socket_path(control_socket, state, process_id) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
        return 1;
    }

    int result = snprintf(metadata_path, sizeof(metadata_path), "%s/metadata",
                          controller_state);
    if (result < 0 || (size_t)result >= sizeof(metadata_path)) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", "metadata path too long");
        unlock_state(lock_fd);
        return 1;
    }

    char process_state[32];
    if (launch_controller(state, controller_state, control_socket, mode,
                          stdin_policy, &argv[command_index]) < 0 ||
        wait_for_controller_ready(control_socket, metadata_path, process_state,
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

    if (append_process_record(state, process_id, workspace_record.id,
                              friendly_name, mode, process_state, controller_id,
                              control_socket) < 0) {
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

    char line[PATH_MAX + 512];
    while (fgets(line, sizeof(line), file) != NULL) {
        cubicle_process_record_t record;
        if (cubicle_parse_process_record(line, &record) != 0) {
            continue;
        }

        if (workspace != NULL &&
            strcmp(record.workspace_id, workspace_record.id) != 0) {
            continue;
        }

        printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
               record.process_id, record.workspace_id, record.friendly_name,
               record.mode, record.state, record.controller_id,
               record.control_socket);
    }

    fclose(file);
    return 0;
}

static void print_process_record(const cubicle_process_record_t *record)
{
    printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
           record->process_id, record->workspace_id, record->friendly_name,
           record->mode, record->state, record->controller_id,
           record->control_socket);
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
    char line[PATH_MAX + 512];
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

    char process_line[PATH_MAX + 512];
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

        long long cursor = cursor_for_process(cursors, cursor_count,
                                              process.process_id);
        long long max_sequence = cursor;
        char event_line[1024];
        while (fgets(event_line, sizeof(event_line), events) != NULL) {
            long long sequence = 0;
            if (cubicle_parse_event_sequence(event_line, &sequence) < 0 ||
                sequence <= cursor) {
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
                fclose(events);
                fclose(processes);
                cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
                unlock_state(lock_fd);
                return 1;
            }

            if (output != NULL) {
                fprintf(output, "%s", workspace_event);
            }
            if (sequence > max_sequence) {
                max_sequence = sequence;
            }
        }

        fclose(events);

        if (max_sequence > cursor &&
            update_cursor(cursors, &cursor_count, process.process_id,
                          max_sequence) < 0) {
            fclose(processes);
            cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
            unlock_state(lock_fd);
            return 1;
        }
    }

    fclose(processes);

    if (save_cursors(state, cursors, cursor_count) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(lock_fd);
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

    return poll_workspace_events(state, workspace, stdout);
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
    char response[4096];
    if (cubicle_rpc_success(response, sizeof(response), request_id,
                            result) < 0) {
        return -1;
    }
    return write_api_frame(client_fd, response);
}

static int handle_manager_client(const manager_state_t *state, int client_fd,
                                 int *shutdown_requested,
                                 uint64_t started_at_ms)
{
    char request[8192];
    if (read_api_frame(client_fd, request, sizeof(request)) < 0) {
        return -1;
    }

    char request_id[64] = "";
    char method[128] = "";
    (void)cubicle_rpc_get_string(request, "request_id", request_id,
                                 sizeof(request_id));
    if (cubicle_rpc_get_string(request, "method", method,
                               sizeof(method)) < 0) {
        return manager_api_error(client_fd, request_id,
                                 CUBICLE_ERR_PROTOCOL,
                                 "request missing method", false, 0);
    }

    char id[CUBICLE_MANAGER_ID_LENGTH + 1];
    if (manager_id(state, id) < 0) {
        return manager_api_error(client_fd, request_id,
                                 CUBICLE_ERR_INTERNAL,
                                 "failed to read manager id", false, errno);
    }

    uint64_t now_ms = manager_time_ms();
    if (strcmp(method, "session.local_bootstrap") == 0) {
        char result[1024];
        int length = snprintf(result, sizeof(result),
                              "{\"session_id\":\"local-session\",\"manager_id\":\"%s\",\"client_key_id\":\"local-bootstrap\",\"protocol_major\":%u,\"protocol_minor\":%u,\"negotiated_capabilities\":%llu,\"authenticated_at_ms\":%llu,\"expires_at_ms\":0}",
                              id, CUBICLE_PROTOCOL_MAJOR,
                              CUBICLE_PROTOCOL_MINOR,
                              (unsigned long long)CUBICLE_API_CAPABILITIES,
                              (unsigned long long)now_ms);
        if (length < 0 || (size_t)length >= sizeof(result)) {
            return -1;
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "manager.ping") == 0) {
        char result[512];
        int length = snprintf(result, sizeof(result),
                              "{\"manager_id\":\"%s\",\"protocol_major\":%u,\"protocol_minor\":%u,\"server_time_ms\":%llu,\"uptime_ms\":%llu}",
                              id, CUBICLE_PROTOCOL_MAJOR,
                              CUBICLE_PROTOCOL_MINOR,
                              (unsigned long long)now_ms,
                              (unsigned long long)(now_ms - started_at_ms));
        if (length < 0 || (size_t)length >= sizeof(result)) {
            return -1;
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "manager.status") == 0) {
        size_t workspace_count = 0;
        size_t process_count = 0;
        if (count_workspaces(state, &workspace_count) < 0 ||
            count_processes(state, &process_count) < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INTERNAL,
                                     "failed to count manager state", false,
                                     errno);
        }

        char result[1024];
        int length = snprintf(result, sizeof(result),
                              "{\"manager_id\":\"%s\",\"protocol_major\":%u,\"protocol_minor\":%u,\"capabilities\":%llu,\"started_at_ms\":%llu,\"server_time_ms\":%llu,\"workspace_count\":%zu,\"process_count\":%zu,\"controller_count\":%zu,\"active_client_sessions\":1}",
                              id, CUBICLE_PROTOCOL_MAJOR,
                              CUBICLE_PROTOCOL_MINOR,
                              (unsigned long long)CUBICLE_API_CAPABILITIES,
                              (unsigned long long)started_at_ms,
                              (unsigned long long)now_ms, workspace_count,
                              process_count, process_count);
        if (length < 0 || (size_t)length >= sizeof(result)) {
            return -1;
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "manager.shutdown") == 0) {
        *shutdown_requested = 1;
        return manager_api_success(client_fd, request_id, "{}");
    }

    if (strcmp(method, "workspace.create") == 0) {
        char params[1024];
        char name[128];
        if (cubicle_rpc_get_object(request, "params", params,
                                   sizeof(params)) < 0 ||
            cubicle_rpc_get_string(params, "name", name, sizeof(name)) < 0 ||
            validate_field(name, "workspace name") < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INVALID_ARGUMENT,
                                     "invalid workspace name", false, 0);
        }

        int lock_fd = lock_state(state);
        if (lock_fd < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_IO,
                                     "failed to lock manager state", true,
                                     errno);
        }

        if (workspace_name_exists(state, name)) {
            unlock_state(lock_fd);
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_ALREADY_EXISTS,
                                     "workspace already exists", false, 0);
        }

        cubicle_workspace_record_t workspace;
        if (cubicle_generate_hex_id(workspace.id, sizeof(workspace.id)) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INTERNAL,
                                     "failed to allocate workspace id",
                                     false, saved_errno);
        }
        snprintf(workspace.name, sizeof(workspace.name), "%s", name);

        char line[256];
        int line_length = snprintf(line, sizeof(line), "%s\t%s\n",
                                   workspace.id, workspace.name);
        if (line_length < 0 || (size_t)line_length >= sizeof(line) ||
            append_line(state, "workspaces.tsv", line) < 0) {
            int saved_errno = errno;
            unlock_state(lock_fd);
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_IO,
                                     "failed to persist workspace", true,
                                     saved_errno);
        }
        unlock_state(lock_fd);

        char result[1024];
        if (workspace_info_json(state, id, &workspace, result,
                                sizeof(result)) < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INTERNAL,
                                     "failed to encode workspace", false,
                                     errno);
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "workspace.get") == 0) {
        char params[1024];
        char name_or_id[128];
        cubicle_workspace_record_t workspace;
        if (cubicle_rpc_get_object(request, "params", params,
                                   sizeof(params)) < 0 ||
            cubicle_rpc_get_string(params, "workspace", name_or_id,
                                   sizeof(name_or_id)) < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INVALID_ARGUMENT,
                                     "missing workspace reference", false, 0);
        }
        if (find_workspace(state, name_or_id, &workspace) < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_NOT_FOUND,
                                     "workspace not found", false, 0);
        }
        char result[1024];
        if (workspace_info_json(state, id, &workspace, result,
                                sizeof(result)) < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INTERNAL,
                                     "failed to encode workspace", false,
                                     errno);
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "workspace.list") == 0) {
        FILE *file = open_state_file_for_read(state, "workspaces.tsv");
        char result[8192];
        size_t used = 0;
        int written = snprintf(result, sizeof(result),
                               "{\"workspaces\":[");
        if (written < 0 || (size_t)written >= sizeof(result)) {
            return -1;
        }
        used = (size_t)written;

        size_t count = 0;
        if (file != NULL) {
            char line[512];
            while (fgets(line, sizeof(line), file) != NULL) {
                cubicle_workspace_record_t workspace;
                char item[1024];
                if (cubicle_parse_workspace_record(line, &workspace) != 0 ||
                    workspace_info_json(state, id, &workspace, item,
                                        sizeof(item)) < 0) {
                    continue;
                }
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s%s", count == 0 ? "" : ",", item);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    fclose(file);
                    return manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "workspace list response too large",
                                             false, 0);
                }
                used += (size_t)written;
                ++count;
            }
            fclose(file);
        }

        written = snprintf(result + used, sizeof(result) - used,
                           "],\"count\":%zu,\"has_more\":false}", count);
        if (written < 0 || (size_t)written >= sizeof(result) - used) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_RESOURCE_LIMIT,
                                     "workspace list response too large",
                                     false, 0);
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "process.get") == 0) {
        char params[1024];
        char process_ref[128];
        char workspace_ref[128];
        char workspace_id[128];
        const char *workspace_id_ptr = NULL;
        if (cubicle_rpc_get_object(request, "params", params,
                                   sizeof(params)) < 0 ||
            cubicle_rpc_get_string(params, "process", process_ref,
                                   sizeof(process_ref)) < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INVALID_ARGUMENT,
                                     "missing process reference", false, 0);
        }
        if (cubicle_rpc_get_string(params, "workspace_id", workspace_ref,
                                   sizeof(workspace_ref)) == 0 &&
            workspace_ref[0] != '\0') {
            cubicle_workspace_record_t workspace;
            if (find_workspace(state, workspace_ref, &workspace) < 0) {
                return manager_api_error(client_fd, request_id,
                                         CUBICLE_ERR_NOT_FOUND,
                                         "workspace not found", false, 0);
            }
            snprintf(workspace_id, sizeof(workspace_id), "%s",
                     workspace.id);
            workspace_id_ptr = workspace_id;
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_ref, workspace_id_ptr,
                                &process, &ambiguous) < 0) {
            return manager_api_error(client_fd, request_id,
                                     ambiguous ? CUBICLE_ERR_AMBIGUOUS_NAME
                                               : CUBICLE_ERR_NOT_FOUND,
                                     ambiguous ? "ambiguous process name"
                                               : "process not found",
                                     false, 0);
        }

        char result[2048];
        if (process_info_json(id, &process, result, sizeof(result)) < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INTERNAL,
                                     "failed to encode process", false,
                                     errno);
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "process.list") == 0) {
        char params[1024];
        char workspace_ref[128];
        char workspace_id[128];
        const char *workspace_id_ptr = NULL;
        if (cubicle_rpc_get_object(request, "params", params,
                                   sizeof(params)) == 0 &&
            cubicle_rpc_get_string(params, "workspace_id", workspace_ref,
                                   sizeof(workspace_ref)) == 0 &&
            workspace_ref[0] != '\0') {
            cubicle_workspace_record_t workspace;
            if (find_workspace(state, workspace_ref, &workspace) < 0) {
                return manager_api_error(client_fd, request_id,
                                         CUBICLE_ERR_NOT_FOUND,
                                         "workspace not found", false, 0);
            }
            snprintf(workspace_id, sizeof(workspace_id), "%s",
                     workspace.id);
            workspace_id_ptr = workspace_id;
        }

        FILE *file = open_state_file_for_read(state, "processes.tsv");
        char result[8192];
        size_t used = 0;
        int written = snprintf(result, sizeof(result), "{\"processes\":[");
        if (written < 0 || (size_t)written >= sizeof(result)) {
            return -1;
        }
        used = (size_t)written;

        size_t count = 0;
        if (file != NULL) {
            char line[PATH_MAX + 512];
            while (fgets(line, sizeof(line), file) != NULL) {
                cubicle_process_record_t process;
                char item[2048];
                if (cubicle_parse_process_record(line, &process) != 0 ||
                    (workspace_id_ptr != NULL &&
                     strcmp(process.workspace_id, workspace_id_ptr) != 0) ||
                    process_info_json(id, &process, item, sizeof(item)) < 0) {
                    continue;
                }
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s%s", count == 0 ? "" : ",", item);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    fclose(file);
                    return manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "process list response too large",
                                             false, 0);
                }
                used += (size_t)written;
                ++count;
            }
            fclose(file);
        }

        written = snprintf(result + used, sizeof(result) - used,
                           "],\"count\":%zu,\"has_more\":false}", count);
        if (written < 0 || (size_t)written >= sizeof(result) - used) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_RESOURCE_LIMIT,
                                     "process list response too large",
                                     false, 0);
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "events.list") == 0) {
        char params[1024];
        char workspace_ref[128];
        char process_ref[128];
        char workspace_id[128];
        const char *workspace_id_ptr = NULL;
        const char *process_id_ptr = NULL;
        uint64_t after_sequence = 0;
        uint64_t limit = 100;

        if (cubicle_rpc_get_object(request, "params", params,
                                   sizeof(params)) == 0) {
            if (cubicle_rpc_get_string(params, "workspace_id",
                                       workspace_ref,
                                       sizeof(workspace_ref)) == 0 &&
                workspace_ref[0] != '\0') {
                cubicle_workspace_record_t workspace;
                if (find_workspace(state, workspace_ref, &workspace) < 0) {
                    return manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_NOT_FOUND,
                                             "workspace not found", false, 0);
                }
                snprintf(workspace_id, sizeof(workspace_id), "%s",
                         workspace.id);
                workspace_id_ptr = workspace_id;
            }
            if (cubicle_rpc_get_string(params, "process_id", process_ref,
                                       sizeof(process_ref)) == 0 &&
                process_ref[0] != '\0') {
                process_id_ptr = process_ref;
            }
            (void)cubicle_rpc_get_uint64(params, "after_sequence",
                                         &after_sequence);
            (void)cubicle_rpc_get_uint64(params, "limit", &limit);
        }

        FILE *file = open_state_file_for_read(state, "workspace-events.log");
        char result[8192];
        size_t used = 0;
        int written = snprintf(result, sizeof(result), "{\"events\":[");
        if (written < 0 || (size_t)written >= sizeof(result)) {
            return -1;
        }
        used = (size_t)written;

        size_t count = 0;
        uint64_t global_sequence = 0;
        if (file != NULL) {
            char line[PATH_MAX + 1400];
            while (fgets(line, sizeof(line), file) != NULL) {
                ++global_sequence;
                if (global_sequence <= after_sequence ||
                    (limit > 0 && count >= limit)) {
                    continue;
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

                char item[2048];
                if (event_info_json(global_sequence, event_workspace_id,
                                    event_process_id, payload, item,
                                    sizeof(item)) < 0) {
                    continue;
                }
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s%s", count == 0 ? "" : ",", item);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    fclose(file);
                    return manager_api_error(client_fd, request_id,
                                             CUBICLE_ERR_RESOURCE_LIMIT,
                                             "events response too large",
                                             false, 0);
                }
                used += (size_t)written;
                ++count;
            }
            fclose(file);
        }

        written = snprintf(result + used, sizeof(result) - used,
                           "],\"count\":%zu,\"has_more\":false}", count);
        if (written < 0 || (size_t)written >= sizeof(result) - used) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_RESOURCE_LIMIT,
                                     "events response too large", false, 0);
        }
        return manager_api_success(client_fd, request_id, result);
    }

    if (strcmp(method, "process.read_output") == 0) {
        char params[1024];
        char process_id[128];
        char stream[32];
        uint64_t offset = 0;
        uint64_t maximum_length = 0;
        if (cubicle_rpc_get_object(request, "params", params,
                                   sizeof(params)) < 0 ||
            cubicle_rpc_get_string(params, "process_id", process_id,
                                   sizeof(process_id)) < 0 ||
            cubicle_rpc_get_string(params, "stream", stream,
                                   sizeof(stream)) < 0 ||
            cubicle_rpc_get_uint64(params, "offset", &offset) < 0 ||
            cubicle_rpc_get_uint64(params, "maximum_length",
                                   &maximum_length) < 0 ||
            maximum_length > 8192) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INVALID_ARGUMENT,
                                     "invalid read_output request", false, 0);
        }

        const char *file_name = api_stream_file_name(stream);
        if (file_name == NULL) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INVALID_ARGUMENT,
                                     "unknown output stream", false, 0);
        }

        cubicle_process_record_t process;
        int ambiguous = 0;
        if (find_process_record(state, process_id, NULL, &process,
                                &ambiguous) < 0) {
            (void)ambiguous;
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_NOT_FOUND,
                                     "process not found", false, 0);
        }

        char path[PATH_MAX];
        int path_length = snprintf(path, sizeof(path),
                                   "%s/controllers/%s/%s", state->dir,
                                   process.process_id, file_name);
        if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_RESOURCE_LIMIT,
                                     "output path too long", false, 0);
        }

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_IO,
                                     "failed to open output", true, errno);
        }

        struct stat status;
        if (fstat(fd, &status) < 0) {
            int saved_errno = errno;
            close(fd);
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_IO,
                                     "failed to stat output", true,
                                     saved_errno);
        }
        if (offset > (uint64_t)status.st_size) {
            close(fd);
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_INVALID_ARGUMENT,
                                     "offset is past end of output", false,
                                     0);
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
                return manager_api_error(client_fd, request_id,
                                         CUBICLE_ERR_IO,
                                         "failed to read output", true,
                                         saved_errno);
            }
            if (nread == 0) {
                break;
            }
            total += (size_t)nread;
        }
        close(fd);
        data[total] = '\0';

        char escaped_data[65536];
        if (cubicle_json_escape(escaped_data, sizeof(escaped_data),
                                data) < 0) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_RESOURCE_LIMIT,
                                     "output chunk is too large", false, 0);
        }

        char result[131072];
        int result_length = snprintf(result, sizeof(result),
                                     "{\"start_offset\":%llu,\"next_offset\":%llu,\"end_of_stream\":%s,\"data\":\"%s\",\"length\":%zu}",
                                     (unsigned long long)offset,
                                     (unsigned long long)(offset + total),
                                     offset + total >=
                                             (uint64_t)status.st_size
                                         ? "true"
                                         : "false",
                                     escaped_data, total);
        if (result_length < 0 || (size_t)result_length >= sizeof(result)) {
            return manager_api_error(client_fd, request_id,
                                     CUBICLE_ERR_RESOURCE_LIMIT,
                                     "read_output response too large",
                                     false, 0);
        }
        return manager_api_success(client_fd, request_id, result);
    }

    return manager_api_error(client_fd, request_id, CUBICLE_ERR_UNSUPPORTED,
                             "method is not implemented", false, 0);
}

static int handle_manager_connection(const manager_state_t *state,
                                     int client_fd,
                                     int *shutdown_requested,
                                     uint64_t started_at_ms)
{
    while (!*shutdown_requested) {
        if (handle_manager_client(state, client_fd, shutdown_requested,
                                  started_at_ms) == 0) {
            continue;
        }
        return errno == ECONNRESET ? 0 : -1;
    }
    return 0;
}

static int command_daemon(const manager_state_t *state, int argc, char **argv)
{
    const char *requested_socket = NULL;
    int poll_interval_ms = 250;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--control-socket") == 0 && i + 1 < argc) {
            requested_socket = argv[++i];
        } else if (strcmp(argv[i], "--event-interval-ms") == 0 && i + 1 < argc) {
            poll_interval_ms = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown daemon option: %s\n", argv[i]);
            return 2;
        }
    }

    if (poll_interval_ms < 0) {
        fprintf(stderr, "daemon requires nonnegative --event-interval-ms\n");
        return 2;
    }

    int daemon_lock_fd = lock_daemon(state);
    if (daemon_lock_fd < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    char socket_path[PATH_MAX];
    if (manager_socket_path(socket_path, state, requested_socket) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(daemon_lock_fd);
        return 1;
    }

    int listen_fd = open_manager_socket(socket_path);
    if (listen_fd < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        unlock_state(daemon_lock_fd);
        return 1;
    }

    if (set_fd_nonblocking(listen_fd) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        close(listen_fd);
        unlink(socket_path);
        unlock_state(daemon_lock_fd);
        return 1;
    }

    int result = 0;
    int shutdown_requested = 0;
    uint64_t started_at_ms = manager_time_ms();
    while (!shutdown_requested) {
        if (poll_workspace_events(state, NULL, NULL) != 0) {
            cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
            result = 1;
            break;
        }

        struct pollfd daemon_fd = {.fd = listen_fd, .events = POLLIN};
        int ready = poll(&daemon_fd, 1, poll_interval_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
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
                cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
                result = 1;
                break;
            }

            if (handle_manager_connection(state, client_fd,
                                          &shutdown_requested,
                                          started_at_ms) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
                result = 1;
            }
            close(client_fd);
        }
    }

    close(listen_fd);
    unlink(socket_path);
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
        strcmp(argv[1], "create") == 0 && argc == 3) {
        return command_workspace_create(state, argv[2]);
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
    manager_state_t state;
    snprintf(state.dir, sizeof(state.dir), ".cubicle/manager");
    snprintf(state.controller_bin, sizeof(state.controller_bin), "cubicle-controller");

    int command_index = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            int result = snprintf(state.dir, sizeof(state.dir), "%s", argv[++i]);
            if (result < 0 || (size_t)result >= sizeof(state.dir)) {
                fprintf(stderr, "State directory path is too long\n");
                return 2;
            }
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

    if (cubicle_mkdir_p(state.dir) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    int result = dispatch_command(&state, argc - command_index, &argv[command_index]);
    if (result == 2) {
        print_usage(argv[0]);
    }

    return result;
}
