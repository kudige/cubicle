#ifndef CUBICLE_CLIENT_TYPES_H
#define CUBICLE_CLIENT_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CUBICLE_ID_STRING_LENGTH 33
#define CUBICLE_NAME_MAX 256
#define CUBICLE_ENDPOINT_MAX 512
#define CUBICLE_ERROR_MESSAGE_MAX 256

typedef char cubicle_manager_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_workspace_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_process_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_key_id_t[CUBICLE_ID_STRING_LENGTH];

typedef enum cubicle_process_mode {
    CUBICLE_MODE_STREAM = 0,
    CUBICLE_MODE_TTY = 1,
    CUBICLE_MODE_TTY_CAPTURED_STDERR = 2
} cubicle_process_mode_t;

typedef enum cubicle_process_state {
    CUBICLE_PROCESS_STARTING = 0,
    CUBICLE_PROCESS_RUNNING,
    CUBICLE_PROCESS_STOPPING,
    CUBICLE_PROCESS_EXITED,
    CUBICLE_PROCESS_FAILED,
    CUBICLE_PROCESS_LOST
} cubicle_process_state_t;

typedef enum cubicle_stdin_policy {
    CUBICLE_STDIN_OPEN = 0,
    CUBICLE_STDIN_EOF = 1
} cubicle_stdin_policy_t;

typedef enum cubicle_transport_kind {
    CUBICLE_TRANSPORT_UNSPECIFIED = 0,
    CUBICLE_TRANSPORT_UNIX,
    CUBICLE_TRANSPORT_TLS,
    CUBICLE_TRANSPORT_RELAY
} cubicle_transport_kind_t;

typedef struct cubicle_endpoint {
    cubicle_transport_kind_t transport;
    char address[CUBICLE_ENDPOINT_MAX];
    char server_name[CUBICLE_NAME_MAX];
} cubicle_endpoint_t;

typedef struct cubicle_workspace_info {
    cubicle_workspace_id_t id;
    char name[CUBICLE_NAME_MAX];
    uint64_t created_at_ms;
} cubicle_workspace_info_t;

typedef struct cubicle_process_info {
    cubicle_process_id_t id;
    cubicle_workspace_id_t workspace_id;
    char friendly_name[CUBICLE_NAME_MAX];
    cubicle_process_mode_t mode;
    cubicle_process_state_t state;
    int64_t pid;
    int64_t pgid;
    int exit_code;
    int term_signal;
    uint64_t created_at_ms;
    uint64_t started_at_ms;
    uint64_t exited_at_ms;
} cubicle_process_info_t;

typedef struct cubicle_process_start_options {
    const char *workspace_id;
    const char *friendly_name;
    cubicle_process_mode_t mode;
    cubicle_stdin_policy_t stdin_policy;
    const char *cwd;
    const char *const *argv;
    size_t argc;
    const char *const *env;
    size_t env_count;
    unsigned int tty_rows;
    unsigned int tty_cols;
} cubicle_process_start_options_t;

#endif
