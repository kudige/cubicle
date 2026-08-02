#ifndef CUBICLE_CONTROLLER_INTERNAL_H
#define CUBICLE_CONTROLLER_INTERNAL_H

#include "cubicle/process.h"
#include "cubicle/util.h"

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX CUBICLE_PATH_MAX
#endif

#define CUBICLE_MAX_SIGNAL_NUMBER 128
#define CUBICLE_MAX_CONTROL_CLIENTS 32
#define CUBICLE_LINE_REQUEST_MAX 256
#define CUBICLE_REQUEST_MAX 8192
#define CUBICLE_RESPONSE_MAX 65792

typedef enum control_client_kind {
    CONTROL_CLIENT_EMPTY = 0,
    CONTROL_CLIENT_READING = 1,
    CONTROL_CLIENT_RESPONDING = 2,
    CONTROL_CLIENT_ATTACHED_STDOUT = 3,
    CONTROL_CLIENT_ATTACHED_STDERR = 4,
    CONTROL_CLIENT_ATTACHING_STDIN = 5,
    CONTROL_CLIENT_ATTACHED_STDIN = 6
} control_client_kind_t;

typedef enum stdin_policy {
    STDIN_POLICY_OPEN = 0,
    STDIN_POLICY_EOF = 1
} stdin_policy_t;

typedef struct stream_pipe {
    int fd;
    int output_fd;
    int log_fd;
    const char *name;
    long long *offset;
    int open;
} stream_pipe_t;

typedef struct controller_state {
    char dir[PATH_MAX];
    char log_dir[PATH_MAX];
    char controller_id[33];
    int events_fd;
    int stdout_fd;
    int stderr_fd;
    long long next_sequence;
    long long stdout_offset;
    long long stderr_offset;
} controller_state_t;

typedef struct terminal_size_state {
    unsigned short rows;
    unsigned short columns;
    int known;
} terminal_size_state_t;

typedef struct control_client {
    int fd;
    control_client_kind_t kind;
    char request[CUBICLE_REQUEST_MAX];
    size_t request_length;
    int framed_request;
    size_t framed_length;
    char response[CUBICLE_RESPONSE_MAX];
    size_t response_length;
    size_t response_offset;
} control_client_t;

void close_if_open(int *fd);
int set_nonblocking(int fd);
int set_cloexec(int fd);
int write_best_effort(int fd, const char *buffer, size_t length);

int make_state_file_path(char path[PATH_MAX], const char *dir,
                         const char *name);
int make_log_file_path(char path[PATH_MAX], const controller_state_t *state,
                       const char *name);
void initialize_empty_controller_state(controller_state_t *state);
void close_controller_state(controller_state_t *state);
int append_event(controller_state_t *state, const char *event);
int initialize_controller_state(controller_state_t *state,
                                const char *requested_dir,
                                const char *requested_log_dir,
                                pid_t child_pid,
                                char **command,
                                cubicle_process_mode_t mode,
                                stdin_policy_t stdin_policy);

int make_control_socket_path(char path[PATH_MAX], const char *requested_socket,
                             const controller_state_t *state);
int open_control_socket(const char *path);
void initialize_control_clients(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS]);
void close_control_client(control_client_t *client, controller_state_t *state);
void close_all_control_clients(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS],
                               controller_state_t *state);
int accept_control_clients(int listen_fd,
                           control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS],
                           controller_state_t *state);
int read_control_client_request(control_client_t *client,
                                controller_state_t *state,
                                pid_t child_pid,
                                int child_stdin_fd,
                                int resize_fd,
                                terminal_size_state_t *terminal_size,
                                int process_completed,
                                int child_result);
int flush_control_client_response(control_client_t *client,
                                  controller_state_t *state);
void broadcast_attached_output(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS],
                               controller_state_t *state,
                               const char *stream,
                               const char *buffer,
                               size_t length);
int forward_attached_stdin(control_client_t *client,
                           controller_state_t *state,
                           int child_stdin_fd);

int run_stream(char **command, const char *state_dir,
               const char *log_dir,
               const char *control_socket,
               stdin_policy_t stdin_policy,
               int completed_retention_ms);
int run_tty(char **command, const char *state_dir,
            const char *log_dir,
            const char *control_socket,
            stdin_policy_t stdin_policy,
            int completed_retention_ms);
int run_term(char **command, const char *state_dir,
             const char *log_dir,
             const char *control_socket,
             stdin_policy_t stdin_policy,
             int completed_retention_ms);

#endif
