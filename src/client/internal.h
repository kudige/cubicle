#ifndef CUBICLE_CLIENT_INTERNAL_H
#define CUBICLE_CLIENT_INTERNAL_H

#include "cubicle/attachment.h"
#include "cubicle/auth.h"
#include "cubicle/client.h"
#include "cubicle/events.h"
#include "cubicle/manager.h"
#include "cubicle/process.h"
#include "cubicle/rpc.h"
#include "cubicle/workspace.h"

#include "../common/json.h"
#include "../common/rpc_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct cubicle_client {
    cubicle_endpoint_t endpoint;
    int connect_timeout_ms;
    int request_timeout_ms;
    uint64_t next_request_id;
    cubicle_transport_t *transport;
    cubicle_error_t last_error;
    cubicle_session_info_t session;
};

struct cubicle_signer {
    cubicle_signer_callbacks_t callbacks;
    void *context;
};

struct cubicle_attachment {
    cubicle_attachment_grant_t grant;
    cubicle_error_t last_error;
    cubicle_channel_mask_t channels;
    cubicle_attachment_mode_t mode;
    uint64_t stdout_offset;
    uint64_t stderr_offset;
    uint64_t tty_offset;
};

struct cubicle_event_subscription {
    cubicle_client_t *client;
    char workspace_id[CUBICLE_ID_STRING_LENGTH];
    char process_id[CUBICLE_ID_STRING_LENGTH];
    uint64_t after_sequence;
    size_t limit;
    cubicle_event_t *pending;
    size_t pending_count;
    size_t pending_index;
    cubicle_error_t last_error;
};

cubicle_error_code_t set_error(cubicle_error_t *error,
                               cubicle_error_code_t code,
                               int system_errno,
                               bool retryable,
                               const char *message);
cubicle_error_code_t set_client_error(cubicle_client_t *client,
                                      cubicle_error_code_t code,
                                      int system_errno,
                                      const char *message);
cubicle_error_code_t unsupported_client(cubicle_client_t *client,
                                        const char *message);

int json_bool_field(const char *json, const char *key, bool *value_out);
int json_u64_field(const char *json, const char *key, uint64_t *value_out);
int json_i64_field(const char *json, const char *key, int64_t *value_out);
int json_string_field(const char *json, const char *key,
                      char *buffer, size_t buffer_size);
const char *json_object_field(const char *json, const char *key);
const char *json_array_field(const char *json, const char *key);
const char *mode_name(cubicle_process_mode_t mode);
const char *state_name(cubicle_process_state_t state);
const char *stream_name(cubicle_stream_kind_t stream);
const char *attachment_mode_name(cubicle_attachment_mode_t mode);
int process_state_from_string(const char *text, cubicle_process_state_t *out);
cubicle_error_code_t error_code_from_name(const char *name);
int channel_mask_from_string(const char *text, cubicle_channel_mask_t *out);
int parse_endpoint(const char *json, cubicle_endpoint_t *endpoint);
int parse_page(const char *json, cubicle_page_info_t *page);
int parse_workspace_info(const char *json, cubicle_workspace_info_t *out);
int parse_process_info(const char *json, cubicle_process_info_t *out);
int parse_key_info(const char *json, cubicle_workspace_key_info_t *out);
int parse_event(const char *json, cubicle_event_t *out);
size_t json_array_field_count(const char *json, const char *key);
char *json_array_field_object_copy(const char *json, const char *key,
                                   size_t index);
char *json_object_field_copy(const char *json, const char *key);

int append_common_options(cubicle_json_builder_t *params,
                          const cubicle_request_options_t *options);
cubicle_error_code_t rpc_call(cubicle_client_t *client,
                              const char *method,
                              const char *params,
                              char **response_out);
const char *result_object(cubicle_client_t *client, const char *response);
cubicle_error_code_t rpc_object(cubicle_client_t *client,
                                const char *method,
                                const char *params,
                                char **object_out);
cubicle_error_code_t simple_string_rpc(cubicle_client_t *client,
                                       const char *method,
                                       const char *key,
                                       const char *value,
                                       const cubicle_request_options_t *request_options);

#endif
