#include "cubicle/client.h"
#include "cubicle/attachment.h"
#include "cubicle/events.h"
#include "cubicle/manager.h"
#include "cubicle/process.h"
#include "cubicle/rpc.h"
#include "cubicle/workspace.h"

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
    cubicle_session_info_t session;
    unsigned long long next_request_id;
};

static cubicle_error_code_t set_error(cubicle_client_t *client,
                                      cubicle_error_code_t code,
                                      int system_errno,
                                      const char *message)
{
    if (client != NULL) {
        client->last_error.code = code;
        client->last_error.system_errno = system_errno;
        client->last_error.retryable = false;
        snprintf(client->last_error.message,
                 sizeof(client->last_error.message), "%s",
                 message == NULL ? "" : message);
    }
    return code;
}

static cubicle_error_code_t rpc_not_implemented(cubicle_client_t *client,
                                                const char *method)
{
    char message[CUBICLE_ERROR_MESSAGE_MAX];
    snprintf(message, sizeof(message), "%s is not implemented", method);
    return set_error(client, CUBICLE_ERR_UNSUPPORTED, 0, message);
}

static cubicle_error_code_t client_request(cubicle_client_t *client,
                                           const char *method,
                                           const char *params_json,
                                           char *result,
                                           size_t result_size)
{
    if (client == NULL || method == NULL || params_json == NULL ||
        result == NULL || result_size == 0) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    if (client->transport == NULL || client->transport->vtable == NULL ||
        client->transport->vtable->request == NULL) {
        return set_error(client, CUBICLE_ERR_INVALID_STATE, 0,
                         "transport does not support requests");
    }

    char request_id[32];
    snprintf(request_id, sizeof(request_id), "c%llu",
             ++client->next_request_id);

    char request[4096];
    if (cubicle_rpc_request(request, sizeof(request), request_id,
                            client->session.session_id, method,
                            params_json) < 0) {
        return set_error(client, CUBICLE_ERR_INTERNAL, errno,
                         "failed to encode request");
    }

    void *response = NULL;
    size_t response_length = 0;
    cubicle_error_code_t code = client->transport->vtable->request(
        client->transport, request, strlen(request), &response,
        &response_length, &client->last_error);
    if (code != CUBICLE_OK) {
        return code;
    }

    char response_json[8192];
    if (response_length >= sizeof(response_json)) {
        if (client->transport->vtable->response_free != NULL) {
            client->transport->vtable->response_free(client->transport,
                                                     response);
        }
        return set_error(client, CUBICLE_ERR_PROTOCOL, EMSGSIZE,
                         "response is too large");
    }
    memcpy(response_json, response, response_length);
    response_json[response_length] = '\0';
    if (client->transport->vtable->response_free != NULL) {
        client->transport->vtable->response_free(client->transport, response);
    }

    int ok = 0;
    if (cubicle_rpc_response_ok(response_json, &ok) < 0) {
        return set_error(client, CUBICLE_ERR_PROTOCOL, errno,
                         "response missing ok flag");
    }
    if (!ok) {
        if (cubicle_rpc_response_error(response_json, &client->last_error) <
            0) {
            return set_error(client, CUBICLE_ERR_PROTOCOL, errno,
                             "invalid error response");
        }
        return client->last_error.code;
    }
    if (cubicle_rpc_get_object(response_json, "result", result,
                               result_size) < 0) {
        return set_error(client, CUBICLE_ERR_PROTOCOL, errno,
                         "response missing result object");
    }

    memset(&client->last_error, 0, sizeof(client->last_error));
    return CUBICLE_OK;
}

static cubicle_error_code_t parse_session(cubicle_client_t *client,
                                          const char *result)
{
    uint64_t protocol_major = 0;
    uint64_t protocol_minor = 0;
    uint64_t capabilities = 0;
    if (cubicle_rpc_get_string(result, "session_id",
                               client->session.session_id,
                               sizeof(client->session.session_id)) < 0 ||
        cubicle_rpc_get_string(result, "manager_id",
                               client->session.manager_id,
                               sizeof(client->session.manager_id)) < 0 ||
        cubicle_rpc_get_string(result, "client_key_id",
                               client->session.client_key_id,
                               sizeof(client->session.client_key_id)) < 0 ||
        cubicle_rpc_get_uint64(result, "protocol_major",
                               &protocol_major) < 0 ||
        cubicle_rpc_get_uint64(result, "protocol_minor",
                               &protocol_minor) < 0 ||
        cubicle_rpc_get_uint64(result, "negotiated_capabilities",
                               &capabilities) < 0 ||
        cubicle_rpc_get_uint64(result, "authenticated_at_ms",
                               &client->session.authenticated_at_ms) < 0 ||
        cubicle_rpc_get_uint64(result, "expires_at_ms",
                               &client->session.expires_at_ms) < 0) {
        return set_error(client, CUBICLE_ERR_PROTOCOL, errno,
                         "invalid session response");
    }

    client->session.protocol_major = (uint32_t)protocol_major;
    client->session.protocol_minor = (uint32_t)protocol_minor;
    client->session.negotiated_capabilities =
        (cubicle_protocol_capability_mask_t)capabilities;
    return CUBICLE_OK;
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

    if (client->transport->vtable->request != NULL) {
        char session_result[2048];
        result = client_request(client, "session.local_bootstrap", "{}",
                                session_result, sizeof(session_result));
        if (result == CUBICLE_OK) {
            result = parse_session(client, session_result);
        }
        if (result != CUBICLE_OK) {
            if (client->transport->vtable->close != NULL) {
                client->transport->vtable->close(client->transport);
            }
            if (client->transport->vtable->destroy != NULL) {
                client->transport->vtable->destroy(client->transport);
            }
            free(client);
            return result;
        }
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

cubicle_error_code_t cubicle_client_session_info(
    const cubicle_client_t *client,
    cubicle_session_info_t *session_out)
{
    if (client == NULL || session_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    *session_out = client->session;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_ping(
    cubicle_client_t *client,
    cubicle_manager_ping_result_t *result_out)
{
    if (client == NULL || result_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    char result[1024];
    cubicle_error_code_t code = client_request(client, "manager.ping", "{}",
                                               result, sizeof(result));
    if (code != CUBICLE_OK) {
        return code;
    }

    uint64_t protocol_major = 0;
    uint64_t protocol_minor = 0;
    if (cubicle_rpc_get_string(result, "manager_id", result_out->manager_id,
                               sizeof(result_out->manager_id)) < 0 ||
        cubicle_rpc_get_uint64(result, "protocol_major",
                               &protocol_major) < 0 ||
        cubicle_rpc_get_uint64(result, "protocol_minor",
                               &protocol_minor) < 0 ||
        cubicle_rpc_get_uint64(result, "server_time_ms",
                               &result_out->server_time_ms) < 0 ||
        cubicle_rpc_get_uint64(result, "uptime_ms",
                               &result_out->uptime_ms) < 0) {
        return set_error(client, CUBICLE_ERR_PROTOCOL, errno,
                         "invalid manager.ping response");
    }

    result_out->protocol_major = (uint32_t)protocol_major;
    result_out->protocol_minor = (uint32_t)protocol_minor;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_status(
    cubicle_client_t *client,
    cubicle_manager_status_t *status_out)
{
    if (client == NULL || status_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    char result[2048];
    cubicle_error_code_t code = client_request(client, "manager.status", "{}",
                                               result, sizeof(result));
    if (code != CUBICLE_OK) {
        return code;
    }

    uint64_t protocol_major = 0;
    uint64_t protocol_minor = 0;
    uint64_t capabilities = 0;
    if (cubicle_rpc_get_string(result, "manager_id", status_out->manager_id,
                               sizeof(status_out->manager_id)) < 0 ||
        cubicle_rpc_get_uint64(result, "protocol_major",
                               &protocol_major) < 0 ||
        cubicle_rpc_get_uint64(result, "protocol_minor",
                               &protocol_minor) < 0 ||
        cubicle_rpc_get_uint64(result, "capabilities", &capabilities) < 0 ||
        cubicle_rpc_get_uint64(result, "started_at_ms",
                               &status_out->started_at_ms) < 0 ||
        cubicle_rpc_get_uint64(result, "server_time_ms",
                               &status_out->server_time_ms) < 0 ||
        cubicle_rpc_get_uint64(result, "workspace_count",
                               &status_out->workspace_count) < 0 ||
        cubicle_rpc_get_uint64(result, "process_count",
                               &status_out->process_count) < 0 ||
        cubicle_rpc_get_uint64(result, "controller_count",
                               &status_out->controller_count) < 0 ||
        cubicle_rpc_get_uint64(result, "active_client_sessions",
                               &status_out->active_client_sessions) < 0) {
        return set_error(client, CUBICLE_ERR_PROTOCOL, errno,
                         "invalid manager.status response");
    }

    status_out->protocol_major = (uint32_t)protocol_major;
    status_out->protocol_minor = (uint32_t)protocol_minor;
    status_out->capabilities =
        (cubicle_protocol_capability_mask_t)capabilities;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_reconcile(cubicle_client_t *client)
{
    if (client == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "manager.reconcile");
}

cubicle_error_code_t cubicle_manager_shutdown(cubicle_client_t *client,
                                              bool stop_managed_processes)
{
    if (client == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    char params[64];
    snprintf(params, sizeof(params), "{\"stop_managed_processes\":%s}",
             stop_managed_processes ? "true" : "false");
    char result[128];
    return client_request(client, "manager.shutdown", params, result,
                          sizeof(result));
}

cubicle_error_code_t cubicle_workspace_create(
    cubicle_client_t *client,
    const cubicle_workspace_create_options_t *options,
    cubicle_workspace_info_t *workspace_out)
{
    if (client == NULL || options == NULL || options->name == NULL ||
        options->name[0] == '\0' || workspace_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.create");
}

cubicle_error_code_t cubicle_workspace_get(
    cubicle_client_t *client,
    const char *workspace_id_or_name,
    cubicle_workspace_info_t *workspace_out)
{
    if (client == NULL || workspace_id_or_name == NULL ||
        workspace_id_or_name[0] == '\0' || workspace_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.get");
}

cubicle_error_code_t cubicle_workspace_list(
    cubicle_client_t *client,
    const cubicle_workspace_list_options_t *options,
    cubicle_workspace_info_t **workspaces_out,
    size_t *count_out,
    cubicle_page_info_t *page_out)
{
    (void)options;
    (void)page_out;
    if (client == NULL || workspaces_out == NULL || count_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.list");
}

cubicle_error_code_t cubicle_workspace_rename(
    cubicle_client_t *client,
    const char *workspace_id,
    const char *new_name,
    const cubicle_request_options_t *request_options)
{
    (void)request_options;
    if (client == NULL || workspace_id == NULL || new_name == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.rename");
}

cubicle_error_code_t cubicle_workspace_stop(
    cubicle_client_t *client,
    const char *workspace_id,
    const cubicle_workspace_stop_options_t *options)
{
    (void)options;
    if (client == NULL || workspace_id == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.stop");
}

cubicle_error_code_t cubicle_workspace_delete(
    cubicle_client_t *client,
    const char *workspace_id,
    const cubicle_workspace_delete_options_t *options)
{
    (void)options;
    if (client == NULL || workspace_id == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.delete");
}

cubicle_error_code_t cubicle_workspace_key_add(
    cubicle_client_t *client,
    const char *workspace_id,
    const unsigned char *public_key,
    size_t public_key_length,
    const char *label,
    cubicle_capability_mask_t capabilities,
    cubicle_workspace_key_info_t *key_out)
{
    (void)public_key_length;
    (void)label;
    (void)capabilities;
    if (client == NULL || workspace_id == NULL || public_key == NULL ||
        key_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.key.add");
}

cubicle_error_code_t cubicle_workspace_key_list(
    cubicle_client_t *client,
    const char *workspace_id,
    cubicle_workspace_key_info_t **keys_out,
    size_t *count_out)
{
    if (client == NULL || workspace_id == NULL || keys_out == NULL ||
        count_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.key.list");
}

cubicle_error_code_t cubicle_workspace_key_set_capabilities(
    cubicle_client_t *client,
    const char *workspace_id,
    const char *key_id,
    cubicle_capability_mask_t capabilities)
{
    (void)capabilities;
    if (client == NULL || workspace_id == NULL || key_id == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.key.update");
}

cubicle_error_code_t cubicle_workspace_key_revoke(
    cubicle_client_t *client,
    const char *workspace_id,
    const char *key_id)
{
    if (client == NULL || workspace_id == NULL || key_id == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "workspace.key.revoke");
}

void cubicle_workspace_list_free(cubicle_workspace_info_t *workspaces)
{
    free(workspaces);
}

void cubicle_workspace_key_list_free(cubicle_workspace_key_info_t *keys)
{
    free(keys);
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
    cubicle_client_t *client,
    const char *process_id_or_name,
    const char *workspace_id,
    cubicle_process_info_t *process_out)
{
    (void)workspace_id;
    if (client == NULL || process_id_or_name == NULL ||
        process_id_or_name[0] == '\0' || process_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.get");
}

cubicle_error_code_t cubicle_process_list(
    cubicle_client_t *client,
    const cubicle_process_filter_t *filter,
    cubicle_process_info_t **processes_out,
    size_t *count_out,
    cubicle_page_info_t *page_out)
{
    (void)filter;
    (void)page_out;
    if (client == NULL || processes_out == NULL || count_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.list");
}

cubicle_error_code_t cubicle_process_signal(cubicle_client_t *client,
                                            const char *process_id,
                                            int signal_number)
{
    if (client == NULL || process_id == NULL || signal_number <= 0) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.signal");
}

cubicle_error_code_t cubicle_process_terminate(
    cubicle_client_t *client,
    const char *process_id,
    const cubicle_process_terminate_options_t *options)
{
    (void)options;
    if (client == NULL || process_id == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.terminate");
}

cubicle_error_code_t cubicle_process_kill(cubicle_client_t *client,
                                          const char *process_id)
{
    if (client == NULL || process_id == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.kill");
}

cubicle_error_code_t cubicle_process_wait(cubicle_client_t *client,
                                          const char *process_id,
                                          int timeout_ms,
                                          cubicle_process_info_t *process_out)
{
    (void)timeout_ms;
    if (client == NULL || process_id == NULL || process_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.wait");
}

cubicle_error_code_t cubicle_process_remove(cubicle_client_t *client,
                                            const char *process_id)
{
    if (client == NULL || process_id == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.remove");
}

cubicle_error_code_t cubicle_process_read_output(
    cubicle_client_t *client,
    const char *process_id,
    cubicle_stream_kind_t stream,
    uint64_t offset,
    size_t maximum_length,
    cubicle_output_chunk_t *chunk_out)
{
    (void)stream;
    (void)offset;
    (void)maximum_length;
    if (client == NULL || process_id == NULL || chunk_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "process.read_output");
}

void cubicle_process_list_free(cubicle_process_info_t *processes)
{
    free(processes);
}

void cubicle_output_chunk_free(cubicle_output_chunk_t *chunk)
{
    if (chunk != NULL) {
        free(chunk->data);
        memset(chunk, 0, sizeof(*chunk));
    }
}

cubicle_error_code_t cubicle_events_list(
    cubicle_client_t *client,
    const cubicle_event_query_t *query,
    cubicle_event_t **events_out,
    size_t *count_out)
{
    (void)query;
    if (client == NULL || events_out == NULL || count_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "events.list");
}

cubicle_error_code_t cubicle_events_subscribe(
    cubicle_client_t *client,
    const cubicle_event_query_t *query,
    cubicle_event_subscription_t **subscription_out)
{
    (void)query;
    if (client == NULL || subscription_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "events.subscribe");
}

cubicle_error_code_t cubicle_events_next(
    cubicle_event_subscription_t *subscription,
    int timeout_ms,
    cubicle_event_t *event_out)
{
    (void)subscription;
    (void)timeout_ms;
    (void)event_out;
    return CUBICLE_ERR_UNSUPPORTED;
}

const cubicle_error_t *cubicle_events_subscription_last_error(
    const cubicle_event_subscription_t *subscription)
{
    (void)subscription;
    return NULL;
}

void cubicle_events_unsubscribe(cubicle_event_subscription_t *subscription)
{
    (void)subscription;
}

void cubicle_events_free(cubicle_event_t *events)
{
    free(events);
}

cubicle_error_code_t cubicle_attachment_request(
    cubicle_client_t *client,
    const cubicle_attachment_request_t *request,
    cubicle_attachment_grant_t *grant_out)
{
    if (client == NULL || request == NULL || grant_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return rpc_not_implemented(client, "attachment.request");
}
