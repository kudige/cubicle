#define _POSIX_C_SOURCE 200809L

#include "cubicle/log.h"
#include "cubicle/process.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MANAGER_ID_LENGTH 32

typedef struct manager_state {
    char dir[PATH_MAX];
} manager_state_t;

typedef struct workspace_record {
    char id[MANAGER_ID_LENGTH + 1];
    char name[128];
} workspace_record_t;

typedef struct process_record {
    char process_id[MANAGER_ID_LENGTH + 1];
    char workspace_id[MANAGER_ID_LENGTH + 1];
    char friendly_name[128];
    char mode[32];
    char state[32];
    char controller_id[MANAGER_ID_LENGTH + 1];
    char control_socket[PATH_MAX];
} process_record_t;

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--state-dir dir] workspace create NAME\n"
            "       %s [--state-dir dir] workspace list\n"
            "       %s [--state-dir dir] process register --workspace NAME_OR_ID --friendly-name NAME --mode MODE --controller-id ID --control-socket PATH [--process-id ID]\n"
            "       %s [--state-dir dir] process list [--workspace NAME_OR_ID]\n",
            program, program, program, program);
}

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(fd, buffer + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        written += (size_t)result;
    }

    return 0;
}

static int mkdir_if_needed(const char *path)
{
    if (mkdir(path, 0700) == 0) {
        return 0;
    }

    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
    }

    return -1;
}

static int mkdir_p(const char *path)
{
    char current[PATH_MAX];
    size_t length = strlen(path);

    if (length == 0 || length >= sizeof(current)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(current, path, length + 1);

    for (char *p = current + 1; *p != '\0'; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        if (mkdir_if_needed(current) < 0) {
            return -1;
        }
        *p = '/';
    }

    return mkdir_if_needed(current);
}

static int generate_id(char id[MANAGER_ID_LENGTH + 1])
{
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        ssize_t result = read(fd, bytes + offset, sizeof(bytes) - offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(fd);
            return -1;
        }

        if (result == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }

        offset += (size_t)result;
    }

    close(fd);

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        id[i * 2] = hex[bytes[i] >> 4];
        id[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    id[MANAGER_ID_LENGTH] = '\0';
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
    int result = write_all(fd, line, length);
    close(fd);
    return result;
}

static int parse_workspace_line(char *line, workspace_record_t *record)
{
    char *id = strtok(line, "\t\n");
    char *name = strtok(NULL, "\t\n");
    if (id == NULL || name == NULL) {
        return -1;
    }

    snprintf(record->id, sizeof(record->id), "%s", id);
    snprintf(record->name, sizeof(record->name), "%s", name);
    return 0;
}

static int parse_process_line(char *line, process_record_t *record)
{
    char *process_id = strtok(line, "\t\n");
    char *workspace_id = strtok(NULL, "\t\n");
    char *friendly_name = strtok(NULL, "\t\n");
    char *mode = strtok(NULL, "\t\n");
    char *state = strtok(NULL, "\t\n");
    char *controller_id = strtok(NULL, "\t\n");
    char *control_socket = strtok(NULL, "\t\n");
    if (process_id == NULL || workspace_id == NULL || friendly_name == NULL ||
        mode == NULL || state == NULL || controller_id == NULL ||
        control_socket == NULL) {
        return -1;
    }

    snprintf(record->process_id, sizeof(record->process_id), "%s", process_id);
    snprintf(record->workspace_id, sizeof(record->workspace_id), "%s", workspace_id);
    snprintf(record->friendly_name, sizeof(record->friendly_name), "%s", friendly_name);
    snprintf(record->mode, sizeof(record->mode), "%s", mode);
    snprintf(record->state, sizeof(record->state), "%s", state);
    snprintf(record->controller_id, sizeof(record->controller_id), "%s", controller_id);
    snprintf(record->control_socket, sizeof(record->control_socket), "%s", control_socket);
    return 0;
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

static int find_workspace(const manager_state_t *state, const char *name_or_id,
                          workspace_record_t *record)
{
    FILE *file = open_state_file_for_read(state, "workspaces.tsv");
    if (file == NULL) {
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        workspace_record_t candidate;
        if (parse_workspace_line(line, &candidate) == 0 &&
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
    workspace_record_t record;
    return find_workspace(state, name, &record) == 0;
}

static int command_workspace_create(const manager_state_t *state, const char *name)
{
    if (validate_field(name, "workspace name") < 0) {
        return 2;
    }

    if (workspace_name_exists(state, name)) {
        fprintf(stderr, "Workspace already exists: %s\n", name);
        return 1;
    }

    char id[MANAGER_ID_LENGTH + 1];
    if (generate_id(id) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    char line[256];
    int length = snprintf(line, sizeof(line), "%s\t%s\n", id, name);
    if (length < 0 || (size_t)length >= sizeof(line) ||
        append_line(state, "workspaces.tsv", line) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

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
        workspace_record_t record;
        if (parse_workspace_line(line, &record) == 0) {
            printf("%s\t%s\n", record.id, record.name);
        }
    }

    fclose(file);
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

    if (strcmp(mode, cubicle_process_mode_name(CUBICLE_PROCESS_STREAM)) != 0) {
        fprintf(stderr, "Only stream mode can be registered currently\n");
        return 2;
    }

    workspace_record_t workspace_record;
    if (find_workspace(state, workspace, &workspace_record) < 0) {
        fprintf(stderr, "Unknown workspace: %s\n", workspace);
        return 1;
    }

    char process_id[MANAGER_ID_LENGTH + 1];
    if (requested_process_id != NULL) {
        if (validate_field(requested_process_id, "process id") < 0 ||
            strlen(requested_process_id) > MANAGER_ID_LENGTH) {
            return 2;
        }
        snprintf(process_id, sizeof(process_id), "%s", requested_process_id);
    } else if (generate_id(process_id) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    char line[PATH_MAX + 512];
    int length = snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\trunning\t%s\t%s\n",
                          process_id, workspace_record.id, friendly_name, mode,
                          controller_id, control_socket);
    if (length < 0 || (size_t)length >= sizeof(line) ||
        append_line(state, "processes.tsv", line) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

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

    workspace_record_t workspace_record;
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
        process_record_t record;
        if (parse_process_line(line, &record) != 0) {
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

static int dispatch_command(const manager_state_t *state, int argc, char **argv)
{
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
        strcmp(argv[1], "list") == 0) {
        return command_process_list(state, argc - 2, &argv[2]);
    }

    return 2;
}

int main(int argc, char **argv)
{
    manager_state_t state;
    snprintf(state.dir, sizeof(state.dir), ".cubicle/manager");

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

        command_index = i;
        break;
    }

    if (command_index < 0) {
        print_usage(argv[0]);
        return 2;
    }

    if (mkdir_p(state.dir) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "manager", strerror(errno));
        return 1;
    }

    int result = dispatch_command(&state, argc - command_index, &argv[command_index]);
    if (result == 2) {
        print_usage(argv[0]);
    }

    return result;
}
