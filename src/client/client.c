#include "cubicle/client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cubicle_client {
    cubicle_endpoint_t endpoint;
    int connect_timeout_ms;
    int request_timeout_ms;
    cubicle_transport_t *transport;
    cubicle_error_t last_error;
};

static cubicle_error_code_t set_error(cubicle_client_t *client,
                                      cubicle_error_code_t code,
                                      int system_errno,
                                      const char *message)
{
    if (client != NULL) {
        client->last_error.code = code;
        client->last_error.system_errno = system_errno;
        if (message == NULL) {
            client->last_error.message[0] = '\0';
        } else {
            snprintf(client->last_error.message,
                     sizeof(client->last_error.message), "%s", message);
        }
    }

    return code;
}

cubicle_error_code_t cubicle_client_connect(
    const cubicle_client_options_t *options,
    cubicle_client_t **client_out)
{
    if (options == NULL || client_out == NULL || options->transport == NULL ||
        options->transport->vtable == NULL ||
        options->transport->vtable->connect == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    cubicle_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return CUBICLE_ERR_INTERNAL;
    }

    client->endpoint = options->endpoint;
    client->connect_timeout_ms = options->connect_timeout_ms;
    client->request_timeout_ms = options->request_timeout_ms;
    client->transport = options->transport;

    cubicle_error_code_t result = client->transport->vtable->connect(
        client->transport, &client->endpoint, &client->last_error);
    if (result != CUBICLE_OK) {
        free(client);
        return result;
    }

    *client_out = client;
    return CUBICLE_OK;
}

void cubicle_client_disconnect(cubicle_client_t *client)
{
    if (client == NULL) {
        return;
    }

    if (client->transport != NULL && client->transport->vtable != NULL) {
        if (client->transport->vtable->close != NULL) {
            client->transport->vtable->close(client->transport);
        }
        if (client->transport->vtable->destroy != NULL) {
            client->transport->vtable->destroy(client->transport);
        }
    }

    free(client);
}

const cubicle_error_t *cubicle_client_last_error(const cubicle_client_t *client)
{
    return client == NULL ? NULL : &client->last_error;
}

static cubicle_error_code_t rpc_not_implemented(cubicle_client_t *client,
                                                const char *method)
{
    char message[CUBICLE_ERROR_MESSAGE_MAX];
    snprintf(message, sizeof(message), "%s encoding is not implemented", method);
    return set_error(client, CUBICLE_ERR_UNSUPPORTED, 0, message);
}

cubicle_error_code_t cubicle_manager_ping(cubicle_client_t *client)
{
    if (client == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "manager.ping");
}

cubicle_error_code_t cubicle_workspace_create(
    cubicle_client_t *client, const char *name,
    cubicle_workspace_info_t *workspace_out)
{
    if (client == NULL || name == NULL || name[0] == '\0' ||
        workspace_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.create");
}

cubicle_error_code_t cubicle_workspace_list(
    cubicle_client_t *client, cubicle_workspace_info_t **workspaces_out,
    size_t *count_out)
{
    if (client == NULL || workspaces_out == NULL || count_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.list");
}

cubicle_error_code_t cubicle_process_start(
    cubicle_client_t *client,
    const cubicle_process_start_options_t *options,
    cubicle_process_info_t *process_out)
{
    if (client == NULL || options == NULL || process_out == NULL ||
        options->workspace_id == NULL || options->argv == NULL ||
        options->argc == 0) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.start");
}

cubicle_error_code_t cubicle_process_get(
    cubicle_client_t *client, const char *process_id_or_name,
    const char *workspace_id, cubicle_process_info_t *process_out)
{
    (void)workspace_id;
    if (client == NULL || process_id_or_name == NULL ||
        process_id_or_name[0] == '\0' || process_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.get");
}

void cubicle_workspace_list_free(cubicle_workspace_info_t *workspaces)
{
    free(workspaces);
}
