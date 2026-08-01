#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cubicle_error_code_t set_error(cubicle_error_t *error,
                                      cubicle_error_code_t code,
                                      int system_errno,
                                      bool retryable,
                                      const char *message)
{
    if (error != NULL) {
        error->code = code;
        error->system_errno = system_errno;
        error->retryable = retryable;
        snprintf(error->message, sizeof(error->message), "%s",
                 message == NULL ? "" : message);
    }
    return code;
}

cubicle_error_code_t set_client_error(cubicle_client_t *client,
                                             cubicle_error_code_t code,
                                             int system_errno,
                                             const char *message)
{
    return set_error(client == NULL ? NULL : &client->last_error, code,
                     system_errno, false, message);
}

cubicle_error_code_t unsupported_client(cubicle_client_t *client,
                                               const char *message)
{
    return set_client_error(client, CUBICLE_ERR_UNSUPPORTED, 0, message);
}

int append_common_options(cubicle_json_builder_t *params,
                                 const cubicle_request_options_t *options)
{
    if (options == NULL) {
        return 0;
    }
    if (options->idempotency_key != NULL) {
        if (cubicle_json_builder_append(params, ",\"idempotency_key\":") < 0 ||
            cubicle_json_builder_append_string(params, options->idempotency_key) < 0) {
            return -1;
        }
    }
    if (options->timeout_ms > 0 &&
        cubicle_json_builder_appendf(params, ",\"timeout_ms\":%d", options->timeout_ms) < 0) {
        return -1;
    }
    if (options->deadline_ms > 0 &&
        cubicle_json_builder_appendf(params, ",\"deadline_ms\":%llu",
                       (unsigned long long)options->deadline_ms) < 0) {
        return -1;
    }
    return 0;
}

cubicle_error_code_t rpc_call(cubicle_client_t *client,
                                     const char *method,
                                     const char *params,
                                     char **response_out)
{
    if (client == NULL || method == NULL || response_out == NULL ||
        client->transport == NULL || client->transport->vtable == NULL ||
        client->transport->vtable->request == NULL) {
        return set_client_error(client, CUBICLE_ERR_INVALID_ARGUMENT, 0,
                                "client transport does not support requests");
    }

    char request_id[32];
    snprintf(request_id, sizeof(request_id), "req-%llu",
             (unsigned long long)++client->next_request_id);

    cubicle_json_builder_t request = {0};
    if (cubicle_json_builder_append(&request, "{\"request_id\":") < 0 ||
        cubicle_json_builder_append_string(&request, request_id) < 0 ||
        cubicle_json_builder_append(&request, ",\"method\":") < 0 ||
        cubicle_json_builder_append_string(&request, method) < 0 ||
        cubicle_json_builder_append(&request, ",\"params\":") < 0 ||
        cubicle_json_builder_append(&request, params == NULL ? "{}" : params) < 0 ||
        cubicle_json_builder_append(&request, "}") < 0) {
        cubicle_json_builder_cleanup(&request);
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to build request");
    }

    void *response_data = NULL;
    size_t response_length = 0;
    cubicle_error_code_t result = client->transport->vtable->request(
        client->transport, request.data, request.length, &response_data,
        &response_length, &client->last_error);
    cubicle_json_builder_cleanup(&request);
    if (result != CUBICLE_OK) {
        return result;
    }

    char *response = calloc(response_length + 1, 1);
    if (response == NULL) {
        if (client->transport->vtable->response_free != NULL) {
            client->transport->vtable->response_free(client->transport,
                                                     response_data);
        }
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to allocate response");
    }
    if (response_length > 0) {
        memcpy(response, response_data, response_length);
    }
    if (client->transport->vtable->response_free != NULL) {
        client->transport->vtable->response_free(client->transport,
                                                 response_data);
    }

    char response_request_id[32];
    bool success = false;
    if (json_string_field(response, "request_id", response_request_id,
                          sizeof(response_request_id)) < 0 ||
        strcmp(response_request_id, request_id) != 0 ||
        json_bool_field(response, "success", &success) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "malformed response envelope");
    }

    if (!success) {
        const char *error_object = json_object_field(response, "error");
        char code_name[64] = "protocol";
        char message[CUBICLE_ERROR_MESSAGE_MAX] = "";
        bool retryable = false;
        int64_t system_errno = 0;
        if (error_object != NULL) {
            json_string_field(error_object, "code", code_name,
                              sizeof(code_name));
            json_string_field(error_object, "message", message,
                              sizeof(message));
            json_bool_field(error_object, "retryable", &retryable);
            json_i64_field(error_object, "system_errno", &system_errno);
        }
        cubicle_error_code_t code = error_code_from_name(code_name);
        set_error(&client->last_error, code, (int)system_errno, retryable,
                  message);
        free(response);
        return code;
    }

    *response_out = response;
    memset(&client->last_error, 0, sizeof(client->last_error));
    return CUBICLE_OK;
}

const char *result_object(cubicle_client_t *client, const char *response)
{
    const char *result = json_object_field(response, "result");
    if (result == NULL) {
        set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                         "response missing result object");
    }
    return result;
}

cubicle_error_code_t rpc_object(cubicle_client_t *client,
                                       const char *method,
                                       const char *params,
                                       char **object_out)
{
    char *response = NULL;
    cubicle_error_code_t code = rpc_call(client, method, params, &response);
    if (code != CUBICLE_OK) {
        return code;
    }
    const char *object = result_object(client, response);
    if (object == NULL) {
        free(response);
        return CUBICLE_ERR_PROTOCOL;
    }
    *object_out = response;
    return CUBICLE_OK;
}

cubicle_error_code_t simple_string_rpc(cubicle_client_t *client,
    const char *method, const char *key, const char *value,
    const cubicle_request_options_t *request_options)
{
    if (client == NULL || value == NULL || value[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{");
    cubicle_json_builder_append_string(&params, key);
    cubicle_json_builder_append(&params, ":");
    cubicle_json_builder_append_string(&params, value);
    append_common_options(&params, request_options);
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, method, params.data, &response);
    cubicle_json_builder_cleanup(&params);
    free(response);
    return code;
}

