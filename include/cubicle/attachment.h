#ifndef CUBICLE_ATTACHMENT_H
#define CUBICLE_ATTACHMENT_H

#include "cubicle/client_error.h"
#include "cubicle/process.h"
#include "cubicle/terminal_snapshot.h"
#include "cubicle/types.h"

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_client cubicle_client_t;
typedef struct cubicle_attachment cubicle_attachment_t;

typedef enum cubicle_channel_mask {
    CUBICLE_CHANNEL_NONE = 0,
    CUBICLE_CHANNEL_STDIN = 1U << 0,
    CUBICLE_CHANNEL_STDOUT = 1U << 1,
    CUBICLE_CHANNEL_STDERR = 1U << 2,
    CUBICLE_CHANNEL_TTY = 1U << 3
} cubicle_channel_mask_t;

typedef enum cubicle_attachment_mode {
    CUBICLE_ATTACHMENT_OBSERVER = 0,
    CUBICLE_ATTACHMENT_INTERACTIVE = 1
} cubicle_attachment_mode_t;

typedef struct cubicle_attachment_request {
    const char *process_id;
    cubicle_channel_mask_t channels;
    cubicle_attachment_mode_t mode;
    uint64_t stdout_offset;
    uint64_t stderr_offset;
    uint64_t tty_offset;
    unsigned int rows;
    unsigned int cols;
    cubicle_request_options_t request;
} cubicle_attachment_request_t;

typedef struct cubicle_attachment_grant {
    cubicle_grant_id_t grant_id;
    cubicle_manager_id_t manager_id;
    cubicle_workspace_id_t workspace_id;
    cubicle_process_id_t process_id;
    cubicle_key_id_t client_key_id;
    cubicle_endpoint_t endpoint;
    char token[CUBICLE_TOKEN_MAX];
    uint64_t issued_at_ms;
    uint64_t expires_at_ms;
    uint32_t connection_limit;
    cubicle_channel_mask_t granted_channels;
    cubicle_attachment_mode_t mode;
} cubicle_attachment_grant_t;

typedef struct cubicle_attachment_options {
    int connect_timeout_ms;
    int io_timeout_ms;
} cubicle_attachment_options_t;

typedef struct cubicle_resize_tracker {
    unsigned int rows;
    unsigned int cols;
    bool has_size;
} cubicle_resize_tracker_t;

typedef struct cubicle_attachment_status {
    cubicle_process_state_t state;
    int64_t local_pid;
    int64_t local_pgid;
    int exit_code;
    bool has_exit_status;
    uint64_t stdout_offset;
    uint64_t stderr_offset;
    uint64_t tty_offset;
} cubicle_attachment_status_t;

cubicle_error_code_t cubicle_attachment_request(cubicle_client_t *client, const cubicle_attachment_request_t *request, cubicle_attachment_grant_t *grant_out);
cubicle_error_code_t cubicle_attachment_connect(const cubicle_attachment_grant_t *grant, const cubicle_attachment_options_t *options, cubicle_attachment_t **attachment_out);
cubicle_channel_mask_t cubicle_attachment_channels(const cubicle_attachment_t *attachment);
void cubicle_attachment_replay(cubicle_attachment_t *attachment, uint64_t replay_bytes);
ssize_t cubicle_attachment_read(cubicle_attachment_t *attachment, void *buffer, size_t length);
ssize_t cubicle_attachment_read_stream(cubicle_attachment_t *attachment, cubicle_stream_kind_t stream, void *buffer, size_t length, bool *end_of_stream_out);
ssize_t cubicle_attachment_write(cubicle_attachment_t *attachment, const void *buffer, size_t length);
cubicle_error_code_t cubicle_attachment_resize(cubicle_attachment_t *attachment, unsigned int rows, unsigned int cols);
cubicle_error_code_t cubicle_attachment_resize_tracked(cubicle_attachment_t *attachment, cubicle_resize_tracker_t *tracker, unsigned int rows, unsigned int cols, bool force, bool *sent_out);
cubicle_error_code_t cubicle_attachment_close_input(cubicle_attachment_t *attachment);
cubicle_error_code_t cubicle_attachment_status(cubicle_attachment_t *attachment, cubicle_attachment_status_t *status_out);
cubicle_error_code_t cubicle_attachment_snapshot(cubicle_attachment_t *attachment, cubicle_terminal_snapshot_t *snapshot_out);
cubicle_error_code_t cubicle_attachment_detach(cubicle_attachment_t *attachment);
const cubicle_error_t *cubicle_attachment_last_error(const cubicle_attachment_t *attachment);
void cubicle_attachment_disconnect(cubicle_attachment_t *attachment);

#ifdef __cplusplus
}
#endif

#endif
