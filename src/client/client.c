#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cubicle_error_code_t cubicle_client_connect(const cubicle_client_options_t *options,
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
    client->next_request_id = 0;
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
    if (client == NULL) return;
    if (client->transport != NULL && client->transport->vtable != NULL) {
        if (client->transport->vtable->close != NULL) client->transport->vtable->close(client->transport);
        if (client->transport->vtable->destroy != NULL) client->transport->vtable->destroy(client->transport);
    }
    free(client);
}

const cubicle_error_t *cubicle_client_last_error(const cubicle_client_t *client)
{
    return client == NULL ? NULL : &client->last_error;
}

cubicle_error_code_t cubicle_client_session_info(const cubicle_client_t *client,
                                                 cubicle_session_info_t *session_out)
{
    if (client == NULL || session_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    *session_out = client->session;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_ping(cubicle_client_t *client,
                                          cubicle_manager_ping_result_t *result_out)
{
    if (client == NULL || result_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.ping", "{}", &response);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(result_out, 0, sizeof(*result_out));
    if (json_string_field(result, "manager_id", result_out->manager_id,
                          sizeof(result_out->manager_id)) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "manager.ping result missing manager_id");
    }
    uint64_t value = 0;
    if (json_u64_field(result, "protocol_major", &value) == 0) result_out->protocol_major = (uint32_t)value;
    if (json_u64_field(result, "protocol_minor", &value) == 0) result_out->protocol_minor = (uint32_t)value;
    json_u64_field(result, "server_time_ms", &result_out->server_time_ms);
    json_u64_field(result, "uptime_ms", &result_out->uptime_ms);
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_status(cubicle_client_t *client,
                                            cubicle_manager_status_t *status_out)
{
    if (client == NULL || status_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.status", "{}", &response);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(status_out, 0, sizeof(*status_out));
    if (json_string_field(result, "manager_id", status_out->manager_id,
                          sizeof(status_out->manager_id)) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "manager.status result missing manager_id");
    }
    uint64_t value = 0;
    if (json_u64_field(result, "protocol_major", &value) == 0) status_out->protocol_major = (uint32_t)value;
    if (json_u64_field(result, "protocol_minor", &value) == 0) status_out->protocol_minor = (uint32_t)value;
    json_u64_field(result, "capabilities", &status_out->capabilities);
    json_u64_field(result, "started_at_ms", &status_out->started_at_ms);
    json_u64_field(result, "server_time_ms", &status_out->server_time_ms);
    json_u64_field(result, "workspace_count", &status_out->workspace_count);
    json_u64_field(result, "process_count", &status_out->process_count);
    json_u64_field(result, "controller_count", &status_out->controller_count);
    json_u64_field(result, "active_client_sessions", &status_out->active_client_sessions);
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_cleanup(
    cubicle_client_t *client,
    const char *workspace_id,
    cubicle_manager_cleanup_result_t *result_out)
{
    if (client == NULL || result_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{");
    if (workspace_id != NULL && workspace_id[0] != '\0') {
        cubicle_json_builder_append(&params, "\"workspace_id\":");
        cubicle_json_builder_append_string(&params, workspace_id);
    }
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.cleanup",
                                           params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(result_out, 0, sizeof(*result_out));
    if (json_u64_field(result, "removed_count",
                       &result_out->removed_count) < 0 ||
        json_u64_field(result, "skipped_live_count",
                       &result_out->skipped_live_count) < 0 ||
        json_u64_field(result, "failed_count",
                       &result_out->failed_count) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "invalid manager.cleanup result");
    }
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_reconcile(cubicle_client_t *client)
{
    if (client == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.reconcile", "{}", &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_manager_shutdown(cubicle_client_t *client,
                                              bool stop_managed_processes)
{
    if (client == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[64];
    snprintf(params, sizeof(params), "{\"stop_managed_processes\":%s}",
             stop_managed_processes ? "true" : "false");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.shutdown", params, &response);
    free(response);
    return code;
}
