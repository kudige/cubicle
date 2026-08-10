#ifndef CUBICLE_PROCESS_H
#define CUBICLE_PROCESS_H

#include "cubicle/client_error.h"
#include "cubicle/types.h"
#include "cubicle/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_client cubicle_client_t;

typedef enum cubicle_process_mode {
    CUBICLE_PROCESS_STREAM = 1,
    CUBICLE_PROCESS_TTY = 2,
    CUBICLE_PROCESS_TTY_CAPTURED_STDERR = 3
} cubicle_process_mode_t;

typedef enum cubicle_process_state {
    CUBICLE_PROCESS_ALLOCATED = 0,
    CUBICLE_PROCESS_STARTING,
    CUBICLE_PROCESS_RUNNING,
    CUBICLE_PROCESS_STOPPING,
    CUBICLE_PROCESS_DRAINING,
    CUBICLE_PROCESS_COMPLETED,
    CUBICLE_PROCESS_FAILED,
    CUBICLE_PROCESS_LOST,
    CUBICLE_PROCESS_REMOVED
} cubicle_process_state_t;

typedef enum cubicle_stdin_policy {
    CUBICLE_STDIN_OPEN = 0,
    CUBICLE_STDIN_EOF = 1
} cubicle_stdin_policy_t;

typedef enum cubicle_stream_kind {
    CUBICLE_STREAM_STDOUT = 0,
    CUBICLE_STREAM_STDERR,
    CUBICLE_STREAM_TTY
} cubicle_stream_kind_t;

typedef struct cubicle_process_info {
    cubicle_manager_id_t manager_id;
    cubicle_workspace_id_t workspace_id;
    cubicle_process_id_t id;
    char friendly_name[CUBICLE_NAME_MAX];
    cubicle_process_mode_t mode;
    cubicle_process_state_t state;
    bool saved;
    bool restart;
    int exit_code;
    int termination_signal;
    bool has_exit_status;
    uint64_t stdout_start_offset;
    uint64_t stdout_offset;
    uint64_t stderr_start_offset;
    uint64_t stderr_offset;
    uint64_t tty_offset;
    uint64_t created_at_ms;
    uint64_t started_at_ms;
    uint64_t exited_at_ms;
    int64_t local_pid;
    int64_t local_pgid;
    char cwd[CUBICLE_PATH_MAX];
} cubicle_process_info_t;

typedef struct cubicle_process_start_options {
    const char *workspace_id;
    const char *friendly_name;
    cubicle_process_mode_t mode;
    cubicle_stdin_policy_t stdin_policy;
    const char *cwd;
    bool restart;
    const char *const *argv;
    size_t argc;
    const char *const *env;
    size_t env_count;
    unsigned int tty_rows;
    unsigned int tty_cols;
    cubicle_request_options_t request;
} cubicle_process_start_options_t;

typedef struct cubicle_process_filter {
    const char *workspace_id;
    const char *name_prefix;
    const cubicle_process_state_t *states;
    size_t state_count;
    bool include_completed;
    cubicle_page_options_t page;
} cubicle_process_filter_t;

typedef struct cubicle_process_terminate_options {
    int grace_period_ms;
    bool force_after_grace;
} cubicle_process_terminate_options_t;

typedef struct cubicle_process_update_options {
    const char *workspace_id;
    const char *friendly_name;
    bool has_restart;
    bool restart;
    cubicle_request_options_t request;
} cubicle_process_update_options_t;

typedef struct cubicle_output_chunk {
    uint64_t start_offset;
    uint64_t next_offset;
    bool end_of_stream;
    void *data;
    size_t length;
} cubicle_output_chunk_t;

cubicle_error_code_t cubicle_process_start(cubicle_client_t *client, const cubicle_process_start_options_t *options, cubicle_process_info_t *process_out);
cubicle_error_code_t cubicle_process_get(cubicle_client_t *client, const char *process_id_or_name, const char *workspace_id, cubicle_process_info_t *process_out);
cubicle_error_code_t cubicle_process_list(cubicle_client_t *client, const cubicle_process_filter_t *filter, cubicle_process_info_t **processes_out, size_t *count_out, cubicle_page_info_t *page_out);
cubicle_error_code_t cubicle_process_signal(cubicle_client_t *client, const char *process_id, int signal_number);
cubicle_error_code_t cubicle_process_terminate(cubicle_client_t *client, const char *process_id, const cubicle_process_terminate_options_t *options);
cubicle_error_code_t cubicle_process_kill(cubicle_client_t *client, const char *process_id);
cubicle_error_code_t cubicle_process_update(cubicle_client_t *client, const char *process_id_or_name, const cubicle_process_update_options_t *options, cubicle_process_info_t *process_out);
cubicle_error_code_t cubicle_process_save(cubicle_client_t *client, const char *process_id, cubicle_process_info_t *process_out);
cubicle_error_code_t cubicle_process_unsave(cubicle_client_t *client, const char *process_id, cubicle_process_info_t *process_out);
cubicle_error_code_t cubicle_process_wait(cubicle_client_t *client, const char *process_id, int timeout_ms, cubicle_process_info_t *process_out);
cubicle_error_code_t cubicle_process_remove(cubicle_client_t *client, const char *process_id);
cubicle_error_code_t cubicle_process_read_output(cubicle_client_t *client, const char *process_id, cubicle_stream_kind_t stream, uint64_t offset, size_t maximum_length, cubicle_output_chunk_t *chunk_out);

void cubicle_process_list_free(cubicle_process_info_t *processes);
void cubicle_output_chunk_free(cubicle_output_chunk_t *chunk);
const char *cubicle_process_mode_name(cubicle_process_mode_t mode);
const char *cubicle_process_state_name(cubicle_process_state_t state);

#ifdef __cplusplus
}
#endif

#endif
