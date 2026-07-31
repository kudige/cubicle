#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include "cubicle/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int open_state_file(const char *dir, const char *name, int flags)
{
    char path[PATH_MAX];
    int result = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (result < 0 || (size_t)result >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return open(path, flags, 0600);
}

static int create_state_directory(const char *path)
{
    char parent[PATH_MAX];
    int result = snprintf(parent, sizeof(parent), "%s", path);
    if (result < 0 || (size_t)result >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char *slash = strrchr(parent, '/');
    if (slash != NULL) {
        if (slash == parent) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }

        if (cubicle_mkdir_p(parent) < 0) {
            return -1;
        }
    }

    return mkdir(path, 0700);
}

int make_state_file_path(char path[PATH_MAX], const char *dir,
                                const char *name)
{
    int result = snprintf(path, PATH_MAX, "%s/%s", dir, name);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

void initialize_empty_controller_state(controller_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->events_fd = -1;
    state->stdout_fd = -1;
    state->stderr_fd = -1;
}

void close_controller_state(controller_state_t *state)
{
    close_if_open(&state->events_fd);
    close_if_open(&state->stdout_fd);
    close_if_open(&state->stderr_fd);
}

int append_event(controller_state_t *state, const char *event)
{
    if (state->events_fd < 0) {
        errno = EBADF;
        return -1;
    }

    char line[1024];
    int length = snprintf(line, sizeof(line), "seq=%lld %s\n",
                          state->next_sequence++, event);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    for (;;) {
        ssize_t result = write(state->events_fd, line, (size_t)length);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (result != length) {
            errno = EIO;
            return -1;
        }

        return 0;
    }
}

int initialize_controller_state(controller_state_t *state,
                                       const char *requested_dir,
                                       pid_t child_pid,
                                       char **command,
                                       stdin_policy_t stdin_policy)
{
    initialize_empty_controller_state(state);
    state->next_sequence = 1;

    if (cubicle_generate_hex_id(state->controller_id, sizeof(state->controller_id)) < 0) {
        return -1;
    }

    if (requested_dir != NULL) {
        int result = snprintf(state->dir, sizeof(state->dir), "%s", requested_dir);
        if (result < 0 || (size_t)result >= sizeof(state->dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        int result = snprintf(state->dir, sizeof(state->dir),
                              ".cubicle/controllers/%s", state->controller_id);
        if (result < 0 || (size_t)result >= sizeof(state->dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    if (create_state_directory(state->dir) < 0) {
        return -1;
    }

    int metadata_fd = open_state_file(state->dir, "metadata",
                                      O_WRONLY | O_CREAT | O_TRUNC);
    if (metadata_fd < 0) {
        return -1;
    }

    char command_line[512] = "";
    size_t used = 0;
    for (int i = 0; command[i] != NULL; ++i) {
        int result = snprintf(command_line + used, sizeof(command_line) - used,
                              "%s%s", i == 0 ? "" : " ", command[i]);
        if (result < 0 || (size_t)result >= sizeof(command_line) - used) {
            break;
        }
        used += (size_t)result;
    }

    char metadata[1024];
    int metadata_length = snprintf(metadata, sizeof(metadata),
                                   "controller_id=%s\nmode=stream\npid=%ld\npgid=%ld\nstdin_policy=%s\ncommand=%s\n",
                                   state->controller_id,
                                   (long)child_pid, (long)child_pid,
                                   stdin_policy == STDIN_POLICY_EOF ? "eof" : "open",
                                   command_line);
    if (metadata_length < 0 || (size_t)metadata_length >= sizeof(metadata) ||
        cubicle_write_all(metadata_fd, metadata, (size_t)metadata_length) < 0) {
        close(metadata_fd);
        return -1;
    }
    close(metadata_fd);

    state->events_fd = open_state_file(state->dir, "events.log",
                                       O_WRONLY | O_CREAT | O_TRUNC | O_APPEND);
    state->stdout_fd = open_state_file(state->dir, "stdout.log",
                                       O_WRONLY | O_CREAT | O_TRUNC);
    state->stderr_fd = open_state_file(state->dir, "stderr.log",
                                       O_WRONLY | O_CREAT | O_TRUNC);

    if (state->events_fd < 0 || state->stdout_fd < 0 || state->stderr_fd < 0) {
        return -1;
    }

    char event[256];
    int event_length = snprintf(event, sizeof(event),
                                "type=process_started controller_id=%s pid=%ld pgid=%ld mode=stream",
                                state->controller_id, (long)child_pid,
                                (long)child_pid);
    if (event_length < 0 || (size_t)event_length >= sizeof(event)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return append_event(state, event);
}
