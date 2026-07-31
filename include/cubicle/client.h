#ifndef CUBICLE_CLIENT_H
#define CUBICLE_CLIENT_H

#include "cubicle/client_error.h"
#include "cubicle/client_types.h"
#include "cubicle/transport.h"

#include <stddef.h>

typedef struct cubicle_client cubicle_client_t;

typedef struct cubicle_client_options {
    cubicle_endpoint_t endpoint;
    int connect_timeout_ms;
    int request_timeout_ms;
    cubicle_transport_t *transport;
} cubicle_client_options_t;

cubicle_error_code_t cubicle_client_connect(
    const cubicle_client_options_t *options,
    cubicle_client_t **client_out);

void cubicle_client_disconnect(cubicle_client_t *client);

const cubicle_error_t *cubicle_client_last_error(
    const cubicle_client_t *client);

cubicle_error_code_t cubicle_manager_ping(cubicle_client_t *client);

cubicle_error_code_t cubicle_workspace_create(
    cubicle_client_t *client,
    const char *name,
    cubicle_workspace_info_t *workspace_out);

cubicle_error_code_t cubicle_workspace_list(
    cubicle_client_t *client,
    cubicle_workspace_info_t **workspaces_out,
    size_t *count_out);

cubicle_error_code_t cubicle_process_start(
    cubicle_client_t *client,
    const cubicle_process_start_options_t *options,
    cubicle_process_info_t *process_out);

cubicle_error_code_t cubicle_process_get(
    cubicle_client_t *client,
    const char *process_id_or_name,
    const char *workspace_id,
    cubicle_process_info_t *process_out);

void cubicle_workspace_list_free(cubicle_workspace_info_t *workspaces);

#endif
