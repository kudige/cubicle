#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include "cubicle/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

static int child_result_from_status(int status, controller_state_t *state)
{
    char event[256];
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        int event_length = snprintf(event, sizeof(event),
                                    "type=process_exited status=exited exit_code=%d",
                                    exit_code);
        if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
            append_event(state, event);
        }
        return exit_code;
    }

    if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        int event_length = snprintf(event, sizeof(event),
                                    "type=process_exited status=signaled signal=%d",
                                    signal_number);
        if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
            append_event(state, event);
        }
        return 128 + signal_number;
    }

    return 1;
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
            *child_result = child_result_from_status(status, state);
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

        struct pollfd poll_fds[3 + CUBICLE_MAX_CONTROL_CLIENTS];
        nfds_t poll_count = 0;
        nfds_t control_index = (nfds_t)-1;
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
            if (clients[i].kind == CONTROL_CLIENT_READING ||
                clients[i].kind == CONTROL_CLIENT_ATTACHED_STDIN) {
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

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            if (clients[i].kind == CONTROL_CLIENT_EMPTY ||
                client_indexes[i] == (nfds_t)-1) {
                continue;
            }

            short revents = poll_fds[client_indexes[i]].revents;
            if (clients[i].kind == CONTROL_CLIENT_READING &&
                (revents & POLLIN) != 0 &&
                read_control_client_request(&clients[i], state, child_pid,
                                            child_stdin_fd) < 0) {
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
                (revents & (POLLHUP | POLLERR)) != 0) {
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
    *child_result = child_result_from_status(status, state);
    return *child_result;
}

int run_stream(char **command, const char *state_dir,
                      const char *control_socket,
                      stdin_policy_t stdin_policy)
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
    int child_reaped = 0;
    int child_result = 1;
    if (initialize_controller_state(&state, state_dir, child_pid, command,
                                    stdin_policy) < 0) {
        snprintf(message, sizeof(message), "failed to initialize state: %s",
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
                                          stdin_pipe[1], &child_reaped,
                                          &child_result);
    close_if_open(&pipes[0].fd);
    close_if_open(&pipes[1].fd);
    close_if_open(&stdin_pipe[1]);
    close_if_open(&control_fd);
    unlink(control_socket_path);

    child_result = wait_for_child(child_pid, &state, &child_reaped,
                                  &child_result);
    close_controller_state(&state);
    if (output_result < 0 && child_result == 0) {
        return 1;
    }

    return child_result;
}
