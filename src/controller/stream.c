#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include "cubicle/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t terminal_resize_pending = 0;

typedef struct terminal_mode {
    int enabled;
    struct termios original;
} terminal_mode_t;

static void handle_terminal_resize_signal(int signal_number)
{
    (void)signal_number;
    terminal_resize_pending = 1;
}

static int enable_terminal_raw_mode(terminal_mode_t *mode)
{
    mode->enabled = 0;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return 0;
    }

    if (tcgetattr(STDIN_FILENO, &mode->original) < 0) {
        return -1;
    }

    struct termios raw = mode->original;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        return -1;
    }

    mode->enabled = 1;
    return 0;
}

static void restore_terminal_mode(terminal_mode_t *mode)
{
    if (mode->enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &mode->original);
        mode->enabled = 0;
    }
}

static int get_terminal_window_size(struct winsize *size)
{
    if (!isatty(STDOUT_FILENO)) {
        return 0;
    }

    memset(size, 0, sizeof(*size));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, size) < 0) {
        return -1;
    }

    if (size->ws_row == 0 || size->ws_col == 0) {
        return 0;
    }

    return 1;
}

static int apply_window_size_to_fd(int fd, const struct winsize *size)
{
    if (fd < 0) {
        return 0;
    }
    return ioctl(fd, TIOCSWINSZ, size);
}

static int apply_terminal_window_size(int pty_fd,
                                      int stderr_pty_fd,
                                      terminal_size_state_t *terminal_size)
{
    struct winsize size;
    int result = get_terminal_window_size(&size);
    if (result <= 0) {
        return result;
    }

    if (terminal_size != NULL && terminal_size->known &&
        terminal_size->rows == size.ws_row &&
        terminal_size->columns == size.ws_col) {
        return 0;
    }

    if (apply_window_size_to_fd(pty_fd, &size) < 0 ||
        apply_window_size_to_fd(stderr_pty_fd, &size) < 0) {
        return -1;
    }

    if (terminal_size != NULL) {
        terminal_size->rows = size.ws_row;
        terminal_size->columns = size.ws_col;
        terminal_size->known = 1;
    }

    return 1;
}

static int sync_local_terminal_window_size(int resize_fd,
                                           int stderr_resize_fd,
                                           terminal_size_state_t *terminal_size,
                                           cubicle_terminal_model_t *terminal_model,
                                           pid_t child_pid)
{
    (void)child_pid;
    if (resize_fd < 0) {
        return 0;
    }

    int result = apply_terminal_window_size(resize_fd, stderr_resize_fd,
                                            terminal_size);
    if (result < 0) {
        return -1;
    }

    if (result > 0) {
        if (terminal_model != NULL && terminal_size != NULL &&
            cubicle_terminal_model_resize(terminal_model, terminal_size->rows,
                                          terminal_size->columns) < 0) {
            return -1;
        }
    }
    return 0;
}

static int write_terminal_response(controller_state_t *state,
                                   int child_stdin_fd,
                                   const char *name,
                                   const char *response)
{
    size_t length = strlen(response);
    if (write_best_effort(child_stdin_fd, response, length) < 0 &&
        errno != EAGAIN) {
        return -1;
    }

    char event[128];
    int event_length = snprintf(event, sizeof(event),
                                "type=terminal_response query=%s length=%zu",
                                name, length);
    if (event_length < 0 || (size_t)event_length >= sizeof(event)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return append_event(state, event);
}

static int answer_unattached_terminal_queries(controller_state_t *state,
                                              int child_stdin_fd,
                                              const char *buffer,
                                              size_t length)
{
    if (child_stdin_fd < 0 || buffer == NULL) {
        return 0;
    }

    for (size_t i = 0; i < length; ++i) {
        const unsigned char *input = (const unsigned char *)buffer;
        if (input[i] != '\033' || i + 1 >= length) {
            continue;
        }

        if (input[i + 1] == '[') {
            if (i + 4 <= length && memcmp(&buffer[i], "\033[6n", 4) == 0) {
                char response[32];
                snprintf(response, sizeof(response), "\033[1;1R");
                if (write_terminal_response(state, child_stdin_fd, "dsr",
                                            response) < 0) {
                    return -1;
                }
                i += 3;
                continue;
            }
            if (i + 3 <= length && memcmp(&buffer[i], "\033[c", 3) == 0) {
                if (write_terminal_response(state, child_stdin_fd,
                                            "primary-da",
                                            "\033[?1;2c") < 0) {
                    return -1;
                }
                i += 2;
                continue;
            }
            if (i + 4 <= length && memcmp(&buffer[i], "\033[?u", 4) == 0) {
                if (write_terminal_response(state, child_stdin_fd,
                                            "keyboard-protocol",
                                            "\033[?0u") < 0) {
                    return -1;
                }
                i += 3;
                continue;
            }
        }

        if (input[i + 1] == ']' && i + 5 < length &&
            input[i + 2] == '1' &&
            (input[i + 3] == '0' || input[i + 3] == '1') &&
            input[i + 4] == ';' && input[i + 5] == '?') {
            size_t cursor = i + 6;
            if (cursor + 1 < length && input[cursor] == '\033' &&
                input[cursor + 1] == '\\') {
                const char *query = input[i + 3] == '0' ? "foreground-color"
                                                        : "background-color";
                const char *response = input[i + 3] == '0'
                                           ? "\033]10;rgb:e3e3/e3e3/eaea\033\\"
                                           : "\033]11;rgb:0808/0505/2b2b\033\\";
                if (write_terminal_response(state, child_stdin_fd, query,
                                            response) < 0) {
                    return -1;
                }
                i = cursor + 1;
                continue;
            }
        }
    }

    return 0;
}

static int create_pipe(int pipe_fds[2], const char *name)
{
    if (pipe(pipe_fds) == 0) {
        return 0;
    }

    char message[256];
    snprintf(message, sizeof(message), "failed to create %s pipe: %s", name,
             strerror(errno));
    cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
    return -1;
}

static int child_result_from_status(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}

static void record_child_exit(controller_state_t *state, int status)
{
    if (state->events_fd < 0) {
        return;
    }

    char event[256];
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        int event_length = snprintf(event, sizeof(event),
                                    "type=process_exited status=exited exit_code=%d",
                                    exit_code);
        if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
            append_event(state, event);
        }
        return;
    }

    if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        int event_length = snprintf(event, sizeof(event),
                                    "type=process_exited status=signaled signal=%d",
                                    signal_number);
        if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
            append_event(state, event);
        }
    }
}

static int reap_child_nonblocking(pid_t child_pid, controller_state_t *state,
                                  int *child_reaped, int *child_result)
{
    if (*child_reaped) {
        return 0;
    }

    int status = 0;
    for (;;) {
        pid_t result = waitpid(child_pid, &status, WNOHANG);
        if (result == 0) {
            return 0;
        }

        if (result == child_pid) {
            *child_reaped = 1;
            *child_result = child_result_from_status(status);
            record_child_exit(state, status);
            return 0;
        }

        if (errno == EINTR) {
            continue;
        }

        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return -1;
    }
}

static int stream_event_loop(stream_pipe_t pipes[2],
                             controller_state_t *state,
                             int control_fd,
                             pid_t child_pid,
                             int child_stdin_fd,
                             int local_input_fd,
                             int resize_fd,
                             int stderr_resize_fd,
                             terminal_size_state_t *terminal_size,
                             cubicle_terminal_model_t *terminal_model,
                             int *child_reaped,
                             int *child_result)
{
    control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS];
    initialize_control_clients(clients);

    int open_count = 0;

    for (size_t i = 0; i < 2; ++i) {
        if (pipes[i].open) {
            ++open_count;
        }
    }

    while (open_count > 0 || !*child_reaped) {
        if (reap_child_nonblocking(child_pid, state, child_reaped,
                                   child_result) < 0) {
            return -1;
        }

        if (open_count == 0 && *child_reaped) {
            break;
        }

        if (resize_fd >= 0 && terminal_resize_pending) {
            terminal_resize_pending = 0;
            if (sync_local_terminal_window_size(resize_fd, stderr_resize_fd,
                                                terminal_size, terminal_model,
                                                child_pid) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
                return -1;
            }
        }

        struct pollfd poll_fds[4 + CUBICLE_MAX_CONTROL_CLIENTS];
        nfds_t poll_count = 0;
        nfds_t control_index = (nfds_t)-1;
        nfds_t local_input_index = (nfds_t)-1;
        nfds_t client_indexes[CUBICLE_MAX_CONTROL_CLIENTS];

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            client_indexes[i] = (nfds_t)-1;
        }

        if (control_fd >= 0) {
            control_index = poll_count;
            poll_fds[poll_count].fd = control_fd;
            poll_fds[poll_count].events = POLLIN;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        if (local_input_fd >= 0 && child_stdin_fd >= 0) {
            local_input_index = poll_count;
            poll_fds[poll_count].fd = local_input_fd;
            poll_fds[poll_count].events = POLLIN | POLLHUP | POLLERR;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        nfds_t pipe_indexes[2] = {(nfds_t)-1, (nfds_t)-1};
        for (size_t i = 0; i < 2; ++i) {
            if (!pipes[i].open) {
                continue;
            }

            pipe_indexes[i] = poll_count;
            poll_fds[poll_count].fd = pipes[i].fd;
            poll_fds[poll_count].events = POLLIN | POLLHUP | POLLERR;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            if (clients[i].kind == CONTROL_CLIENT_EMPTY) {
                continue;
            }

            client_indexes[i] = poll_count;
            poll_fds[poll_count].fd = clients[i].fd;
            poll_fds[poll_count].events = POLLHUP | POLLERR;
            if (clients[i].response_offset < clients[i].response_length) {
                poll_fds[poll_count].events |= POLLOUT;
            }
            if ((clients[i].kind == CONTROL_CLIENT_READING ||
                 clients[i].kind == CONTROL_CLIENT_ATTACHED_STDIN) &&
                clients[i].response_offset == clients[i].response_length) {
                poll_fds[poll_count].events |= POLLIN;
            }
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        int poll_result = poll(poll_fds, poll_count, 100);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
            return -1;
        }

        if (poll_result == 0) {
            continue;
        }

        if (control_index != (nfds_t)-1 &&
            (poll_fds[control_index].revents & POLLIN) != 0) {
            if (accept_control_clients(control_fd, clients, state) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed accepting control client");
                return -1;
            }
        }

        if (local_input_index != (nfds_t)-1) {
            short revents = poll_fds[local_input_index].revents;
            if ((revents & POLLIN) != 0) {
                char buffer[1024];
                ssize_t read_result = read(local_input_fd, buffer,
                                           sizeof(buffer));
                if (read_result < 0) {
                    if (errno != EINTR) {
                        cubicle_log(CUBICLE_LOG_ERROR, "controller",
                                    strerror(errno));
                        return -1;
                    }
                } else if (read_result == 0) {
                    local_input_fd = -1;
                } else if (write_best_effort(child_stdin_fd, buffer,
                                             (size_t)read_result) < 0 &&
                           errno != EAGAIN) {
                    cubicle_log(CUBICLE_LOG_ERROR, "controller",
                                strerror(errno));
                    return -1;
                } else if (sync_local_terminal_window_size(resize_fd,
                                                           stderr_resize_fd,
                                                           terminal_size,
                                                           terminal_model,
                                                           child_pid) < 0) {
                    cubicle_log(CUBICLE_LOG_ERROR, "controller",
                                strerror(errno));
                    return -1;
                } else {
                    append_input_event(state, "local", buffer,
                                       (size_t)read_result);
                }
            }

            if ((revents & (POLLHUP | POLLERR)) != 0) {
                local_input_fd = -1;
            }
        }

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            if (clients[i].kind == CONTROL_CLIENT_EMPTY ||
                client_indexes[i] == (nfds_t)-1) {
                continue;
            }

            short revents = poll_fds[client_indexes[i]].revents;
            if ((revents & POLLOUT) != 0 &&
                flush_control_client_response(&clients[i], state) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed flushing control response");
                return -1;
            }

            if (clients[i].kind == CONTROL_CLIENT_EMPTY) {
                continue;
            }

            if (clients[i].kind == CONTROL_CLIENT_READING &&
                (revents & POLLIN) != 0 &&
                read_control_client_request(&clients[i], state, child_pid,
                                            child_stdin_fd, resize_fd,
                                            stderr_resize_fd,
                                            terminal_size, terminal_model, 0,
                                            *child_result) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed reading control request");
                return -1;
            }

            if (clients[i].kind == CONTROL_CLIENT_ATTACHED_STDIN &&
                (revents & POLLIN) != 0 &&
                forward_attached_stdin(&clients[i], state, child_stdin_fd) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed forwarding stdin attachment");
                return -1;
            }

            if (clients[i].kind != CONTROL_CLIENT_EMPTY &&
                (revents & (POLLHUP | POLLERR)) != 0 &&
                clients[i].response_offset == clients[i].response_length) {
                close_control_client(&clients[i], state);
                continue;
            }
        }

        for (size_t i = 0; i < 2; ++i) {
            if (!pipes[i].open) {
                continue;
            }

            short revents = poll_fds[pipe_indexes[i]].revents;
            if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }

            char buffer[4096];
            ssize_t read_result = read(pipes[i].fd, buffer, sizeof(buffer));
            if (read_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EIO) {
                    close_if_open(&pipes[i].fd);
                    pipes[i].open = 0;
                    --open_count;
                    continue;
                }

                char message[256];
                snprintf(message, sizeof(message), "failed reading %s: %s",
                         pipes[i].name, strerror(errno));
                cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
                return -1;
            }

            if (read_result == 0) {
                close_if_open(&pipes[i].fd);
                pipes[i].open = 0;
                --open_count;
                continue;
            }

            if (cubicle_write_all(pipes[i].output_fd, buffer, (size_t)read_result) < 0) {
                char message[256];
                snprintf(message, sizeof(message), "failed writing %s: %s",
                         pipes[i].name, strerror(errno));
                cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
                return -1;
            }

            long long start = *pipes[i].offset;
            if (cubicle_write_all(pipes[i].log_fd, buffer, (size_t)read_result) < 0) {
                char message[256];
                snprintf(message, sizeof(message), "failed persisting %s: %s",
                         pipes[i].name, strerror(errno));
                cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
                return -1;
            }
            *pipes[i].offset += read_result;

            if (terminal_model != NULL &&
                pipes[i].offset == &state->stdout_offset &&
                cubicle_terminal_model_feed(terminal_model, buffer,
                                            (size_t)read_result) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed updating terminal snapshot");
                return -1;
            }

            if (terminal_model != NULL && local_input_fd < 0 &&
                !state->terminal_attachment_active &&
                pipes[i].offset == &state->stdout_offset &&
                answer_unattached_terminal_queries(state, child_stdin_fd,
                                                   buffer,
                                                   (size_t)read_result) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed answering terminal query");
                return -1;
            }

            char event[256];
            int event_length = snprintf(event, sizeof(event),
                                        "type=output stream=%s start=%lld length=%zd",
                                        pipes[i].name, start, read_result);
            if (event_length < 0 || (size_t)event_length >= sizeof(event) ||
                append_event(state, event) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed appending output event");
                return -1;
            }

            broadcast_attached_output(clients, state, pipes[i].name, buffer,
                                      (size_t)read_result);
        }
    }

    close_all_control_clients(clients, state);
    return 0;
}

static long long monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }

    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int completed_retention_loop(controller_state_t *state,
                                    int control_fd,
                                    pid_t child_pid,
                                    int child_result,
                                    cubicle_terminal_model_t *terminal_model,
                                    int retention_ms)
{
    if (retention_ms <= 0) {
        return 0;
    }

    long long start_ms = monotonic_milliseconds();
    if (start_ms < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return -1;
    }

    control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS];
    initialize_control_clients(clients);

    for (;;) {
        long long now_ms = monotonic_milliseconds();
        if (now_ms < 0) {
            cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
            close_all_control_clients(clients, state);
            return -1;
        }

        long long remaining_ms = retention_ms - (now_ms - start_ms);
        if (remaining_ms <= 0) {
            break;
        }

        struct pollfd poll_fds[1 + CUBICLE_MAX_CONTROL_CLIENTS];
        nfds_t poll_count = 0;
        nfds_t control_index = (nfds_t)-1;
        nfds_t client_indexes[CUBICLE_MAX_CONTROL_CLIENTS];

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            client_indexes[i] = (nfds_t)-1;
        }

        control_index = poll_count;
        poll_fds[poll_count].fd = control_fd;
        poll_fds[poll_count].events = POLLIN;
        poll_fds[poll_count].revents = 0;
        ++poll_count;

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            if (clients[i].kind == CONTROL_CLIENT_EMPTY) {
                continue;
            }

            client_indexes[i] = poll_count;
            poll_fds[poll_count].fd = clients[i].fd;
            poll_fds[poll_count].events = POLLHUP | POLLERR;
            if (clients[i].response_offset < clients[i].response_length) {
                poll_fds[poll_count].events |= POLLOUT;
            }
            if (clients[i].kind == CONTROL_CLIENT_READING &&
                clients[i].response_offset == clients[i].response_length) {
                poll_fds[poll_count].events |= POLLIN;
            }
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        int timeout = remaining_ms > 100 ? 100 : (int)remaining_ms;
        int poll_result = poll(poll_fds, poll_count, timeout);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
            close_all_control_clients(clients, state);
            return -1;
        }

        if (poll_result == 0) {
            continue;
        }

        if ((poll_fds[control_index].revents & POLLIN) != 0 &&
            accept_control_clients(control_fd, clients, state) < 0) {
            cubicle_log(CUBICLE_LOG_ERROR, "controller",
                        "failed accepting completed control client");
            close_all_control_clients(clients, state);
            return -1;
        }

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            if (clients[i].kind == CONTROL_CLIENT_EMPTY ||
                client_indexes[i] == (nfds_t)-1) {
                continue;
            }

            short revents = poll_fds[client_indexes[i]].revents;
            if ((revents & POLLOUT) != 0 &&
                flush_control_client_response(&clients[i], state) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed flushing completed control response");
                close_all_control_clients(clients, state);
                return -1;
            }

            if (clients[i].kind == CONTROL_CLIENT_EMPTY) {
                continue;
            }

            if (clients[i].kind == CONTROL_CLIENT_READING &&
                (revents & POLLIN) != 0 &&
                read_control_client_request(&clients[i], state, child_pid, -1,
                                            -1, -1, NULL, terminal_model, 1,
                                            child_result) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed reading completed control request");
                close_all_control_clients(clients, state);
                return -1;
            }

            if (clients[i].kind != CONTROL_CLIENT_EMPTY &&
                (revents & (POLLHUP | POLLERR)) != 0 &&
                clients[i].response_offset == clients[i].response_length) {
                close_control_client(&clients[i], state);
            }
        }
    }

    close_all_control_clients(clients, state);
    return 0;
}

static int wait_for_child(pid_t child_pid, controller_state_t *state,
                          int *child_reaped, int *child_result)
{
    if (*child_reaped) {
        return *child_result;
    }

    int status = 0;
    for (;;) {
        if (waitpid(child_pid, &status, 0) >= 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return 1;
    }

    *child_reaped = 1;
    *child_result = child_result_from_status(status);
    record_child_exit(state, status);
    return *child_result;
}

static void child_chdir_or_exit(const char *cwd)
{
    if (cwd != NULL && cwd[0] != '\0' && chdir(cwd) < 0) {
        _exit(127);
    }
}

int run_stream(char **command, const char *state_dir,
               const char *log_dir,
               const char *control_socket,
               const char *cwd,
               stdin_policy_t stdin_policy,
               int completed_retention_ms,
               int debug_input)
{
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (create_pipe(stdin_pipe, "stdin") < 0 ||
        create_pipe(stdout_pipe, "stdout") < 0 ||
        create_pipe(stderr_pipe, "stderr") < 0) {
        close_if_open(&stdin_pipe[0]);
        close_if_open(&stdin_pipe[1]);
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stdout_pipe[1]);
        close_if_open(&stderr_pipe[0]);
        close_if_open(&stderr_pipe[1]);
        return 1;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return 1;
    }

    if (child_pid == 0) {
        setpgid(0, 0);

        close_if_open(&stdin_pipe[1]);
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stderr_pipe[0]);

        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }

        close_if_open(&stdin_pipe[0]);
        close_if_open(&stdout_pipe[1]);
        close_if_open(&stderr_pipe[1]);

        child_chdir_or_exit(cwd);
        execvp(command[0], command);
        _exit(errno == ENOENT ? 127 : 126);
    }

    setpgid(child_pid, child_pid);

    close_if_open(&stdin_pipe[0]);
    close_if_open(&stdout_pipe[1]);
    close_if_open(&stderr_pipe[1]);

    if (stdin_policy == STDIN_POLICY_EOF) {
        close_if_open(&stdin_pipe[1]);
    } else if (set_nonblocking(stdin_pipe[1]) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        kill(-child_pid, SIGTERM);
        close_if_open(&stdin_pipe[1]);
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stderr_pipe[0]);
        return 1;
    }

    char message[256];
    snprintf(message, sizeof(message), "started pid %ld in stream mode",
             (long)child_pid);
    cubicle_log(CUBICLE_LOG_INFO, "controller", message);

    controller_state_t state;
    initialize_empty_controller_state(&state);
    int child_reaped = 0;
    int child_result = 1;
    if (initialize_controller_state(&state, state_dir, log_dir, child_pid, command,
                                    CUBICLE_PROCESS_STREAM, stdin_policy) < 0) {
        snprintf(message, sizeof(message), "failed to initialize state %s: %s",
                 state_dir == NULL ? "(default)" : state_dir,
                 strerror(errno));
        cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
        kill(-child_pid, SIGTERM);
        wait_for_child(child_pid, &state, &child_reaped, &child_result);
        close_if_open(&stdin_pipe[1]);
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stderr_pipe[0]);
        close_controller_state(&state);
        return 1;
    }
    state.debug_input = debug_input;

    cubicle_log(CUBICLE_LOG_INFO, "controller", "state directory initialized");

    char control_socket_path[PATH_MAX];
    int control_fd = -1;
    if (make_control_socket_path(control_socket_path, control_socket, &state) < 0 ||
        (control_fd = open_control_socket(control_socket_path)) < 0) {
        snprintf(message, sizeof(message), "failed to initialize control socket: %s",
                 strerror(errno));
        cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
        kill(-child_pid, SIGTERM);
        wait_for_child(child_pid, &state, &child_reaped, &child_result);
        close_if_open(&stdin_pipe[1]);
        close_controller_state(&state);
        return 1;
    }
    cubicle_log(CUBICLE_LOG_INFO, "controller", "control socket initialized");

    stream_pipe_t pipes[2] = {
        {.fd = stdout_pipe[0],
         .output_fd = STDOUT_FILENO,
         .log_fd = state.stdout_fd,
         .name = "stdout",
         .offset = &state.stdout_offset,
         .open = 1},
        {.fd = stderr_pipe[0],
         .output_fd = STDERR_FILENO,
         .log_fd = state.stderr_fd,
         .name = "stderr",
         .offset = &state.stderr_offset,
         .open = 1},
    };

    int output_result = stream_event_loop(pipes, &state, control_fd, child_pid,
                                          stdin_pipe[1], -1, -1, -1,
                                          NULL, NULL,
                                          &child_reaped, &child_result);
    close_if_open(&pipes[0].fd);
    close_if_open(&pipes[1].fd);
    close_if_open(&stdin_pipe[1]);

    child_result = wait_for_child(child_pid, &state, &child_reaped,
                                  &child_result);
    int retention_result = 0;
    if (output_result == 0) {
        retention_result = completed_retention_loop(&state, control_fd,
                                                    child_pid, child_result,
                                                    NULL,
                                                    completed_retention_ms);
    }
    close_if_open(&control_fd);
    unlink(control_socket_path);
    close_controller_state(&state);
    if ((output_result < 0 || retention_result < 0) && child_result == 0) {
        return 1;
    }

    return child_result;
}

static int open_pty_pair(int *master_fd, int *slave_fd)
{
    *master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (*master_fd < 0) {
        return -1;
    }

    if (grantpt(*master_fd) < 0 || unlockpt(*master_fd) < 0) {
        close_if_open(master_fd);
        return -1;
    }

    char *slave_name = ptsname(*master_fd);
    if (slave_name == NULL) {
        close_if_open(master_fd);
        return -1;
    }

    *slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (*slave_fd < 0) {
        close_if_open(master_fd);
        return -1;
    }

    return 0;
}

static int run_pty_mode(char **command, const char *state_dir,
                        const char *log_dir,
                        const char *control_socket,
                        const char *cwd,
                        stdin_policy_t stdin_policy,
                        int completed_retention_ms,
                        cubicle_process_mode_t process_mode,
                        int debug_input)
{
    int master_fd = -1;
    int slave_fd = -1;
    int stdout_master_fd = -1;
    int stdout_slave_fd = -1;
    int capture_stderr =
        process_mode == CUBICLE_PROCESS_TTY_CAPTURED_STDERR;

    if (open_pty_pair(&master_fd, &slave_fd) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return 1;
    }
    if (capture_stderr &&
        open_pty_pair(&stdout_master_fd, &stdout_slave_fd) < 0) {
        close_if_open(&master_fd);
        close_if_open(&slave_fd);
        return 1;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        close_if_open(&master_fd);
        close_if_open(&slave_fd);
        close_if_open(&stdout_master_fd);
        close_if_open(&stdout_slave_fd);
        return 1;
    }

    if (child_pid == 0) {
        close_if_open(&master_fd);
        close_if_open(&stdout_master_fd);
        if (setsid() < 0) {
            _exit(127);
        }
        ioctl(slave_fd, TIOCSCTTY, 0);

        if (dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(capture_stderr ? stdout_slave_fd : slave_fd,
                 STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(127);
        }

        close_if_open(&slave_fd);
        close_if_open(&stdout_slave_fd);
        child_chdir_or_exit(cwd);
        execvp(command[0], command);
        _exit(errno == ENOENT ? 127 : 126);
    }

    close_if_open(&slave_fd);
    close_if_open(&stdout_slave_fd);

    if (set_nonblocking(master_fd) < 0 ||
        (capture_stderr && set_nonblocking(stdout_master_fd) < 0)) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        kill(-child_pid, SIGTERM);
        close_if_open(&master_fd);
        close_if_open(&stdout_master_fd);
        return 1;
    }

    terminal_size_state_t terminal_size = {.rows = 0, .columns = 0, .known = 0};
    if (apply_terminal_window_size(master_fd, stdout_master_fd,
                                   &terminal_size) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        kill(-child_pid, SIGTERM);
        close_if_open(&master_fd);
        close_if_open(&stdout_master_fd);
        return 1;
    }

    char message[256];
    snprintf(message, sizeof(message), "started pid %ld in %s mode",
             (long)child_pid, cubicle_process_mode_name(process_mode));
    cubicle_log(CUBICLE_LOG_INFO, "controller", message);

    controller_state_t state;
    initialize_empty_controller_state(&state);
    int child_reaped = 0;
    int child_result = 1;
    if (initialize_controller_state(&state, state_dir, log_dir, child_pid, command,
                                    process_mode, stdin_policy) < 0) {
        snprintf(message, sizeof(message), "failed to initialize state %s: %s",
                 state_dir == NULL ? "(default)" : state_dir,
                 strerror(errno));
        cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
        kill(-child_pid, SIGTERM);
        wait_for_child(child_pid, &state, &child_reaped, &child_result);
        close_if_open(&master_fd);
        close_if_open(&stdout_master_fd);
        close_controller_state(&state);
        return 1;
    }
    state.debug_input = debug_input;

    cubicle_log(CUBICLE_LOG_INFO, "controller", "state directory initialized");

    char control_socket_path[PATH_MAX];
    int control_fd = -1;
    if (make_control_socket_path(control_socket_path, control_socket, &state) < 0 ||
        (control_fd = open_control_socket(control_socket_path)) < 0) {
        snprintf(message, sizeof(message), "failed to initialize control socket: %s",
                 strerror(errno));
        cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
        kill(-child_pid, SIGTERM);
        wait_for_child(child_pid, &state, &child_reaped, &child_result);
        close_if_open(&master_fd);
        close_if_open(&stdout_master_fd);
        close_controller_state(&state);
        return 1;
    }
    cubicle_log(CUBICLE_LOG_INFO, "controller", "control socket initialized");

    stream_pipe_t pipes[2] = {
        {.fd = capture_stderr ? stdout_master_fd : master_fd,
         .output_fd = STDOUT_FILENO,
         .log_fd = state.stdout_fd,
         .name = "stdout",
         .offset = &state.stdout_offset,
         .open = 1},
        {.fd = capture_stderr ? master_fd : -1,
         .output_fd = STDERR_FILENO,
         .log_fd = state.stderr_fd,
         .name = "stderr",
         .offset = &state.stderr_offset,
         .open = capture_stderr ? 1 : 0},
    };

    unsigned int model_rows = terminal_size.known ? terminal_size.rows : 24;
    unsigned int model_cols = terminal_size.known ? terminal_size.columns : 80;
    cubicle_terminal_model_t *terminal_model = NULL;
    if (cubicle_terminal_model_create(model_rows, model_cols,
                                      &terminal_model) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        kill(-child_pid, SIGTERM);
        close_if_open(&control_fd);
        close_if_open(&master_fd);
        close_if_open(&stdout_master_fd);
        close_controller_state(&state);
        return 1;
    }

    terminal_mode_t terminal_mode = {.enabled = 0};
    int local_input_fd = -1;
    struct sigaction previous_winch;
    int restore_winch = 0;
    if (stdin_policy == STDIN_POLICY_OPEN &&
        enable_terminal_raw_mode(&terminal_mode) < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        kill(-child_pid, SIGTERM);
        close_if_open(&control_fd);
        close_if_open(&master_fd);
        close_if_open(&stdout_master_fd);
        cubicle_terminal_model_destroy(terminal_model);
        close_controller_state(&state);
        return 1;
    }

    if (terminal_mode.enabled) {
        local_input_fd = STDIN_FILENO;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = handle_terminal_resize_signal;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGWINCH, &action, &previous_winch) == 0) {
            restore_winch = 1;
        }
    }

    int input_fd = stdin_policy == STDIN_POLICY_OPEN ? master_fd : -1;
    int output_result = stream_event_loop(pipes, &state, control_fd, child_pid,
                                          input_fd, local_input_fd, master_fd,
                                          stdout_master_fd,
                                          &terminal_size, terminal_model,
                                          &child_reaped, &child_result);

    restore_terminal_mode(&terminal_mode);
    if (restore_winch) {
        sigaction(SIGWINCH, &previous_winch, NULL);
    }

    close_if_open(&pipes[0].fd);
    close_if_open(&pipes[1].fd);

    child_result = wait_for_child(child_pid, &state, &child_reaped,
                                  &child_result);
    int retention_result = 0;
    if (output_result == 0) {
        retention_result = completed_retention_loop(&state, control_fd,
                                                    child_pid, child_result,
                                                    terminal_model,
                                                    completed_retention_ms);
    }
    close_if_open(&control_fd);
    unlink(control_socket_path);
    cubicle_terminal_model_destroy(terminal_model);
    close_controller_state(&state);
    if ((output_result < 0 || retention_result < 0) && child_result == 0) {
        return 1;
    }

    return child_result;
}

int run_tty(char **command, const char *state_dir,
            const char *log_dir,
            const char *control_socket,
            const char *cwd,
            stdin_policy_t stdin_policy,
            int completed_retention_ms,
            int debug_input)
{
    return run_pty_mode(command, state_dir, log_dir, control_socket, cwd,
                        stdin_policy, completed_retention_ms,
                        CUBICLE_PROCESS_TTY, debug_input);
}

int run_term(char **command, const char *state_dir,
             const char *log_dir,
             const char *control_socket,
             const char *cwd,
             stdin_policy_t stdin_policy,
             int completed_retention_ms,
             int debug_input)
{
    return run_pty_mode(command, state_dir, log_dir, control_socket, cwd,
                        stdin_policy, completed_retention_ms,
                        CUBICLE_PROCESS_TTY_CAPTURED_STDERR, debug_input);
}
