#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int library_debug_enabled(void)
{
    const char *debug = getenv("CUBICLE_LIBRARY_DEBUG");
    return debug != NULL && strcmp(debug, "library") == 0;
}

static void library_debug_timestamp(char *buffer, size_t buffer_size)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) < 0) {
        snprintf(buffer, buffer_size, "unknown-time");
        return;
    }
    struct tm *tm_now = localtime(&now.tv_sec);
    if (tm_now == NULL) {
        snprintf(buffer, buffer_size, "unknown-time");
        return;
    }
    size_t used = strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%S",
                           tm_now);
    if (used == 0 || used >= buffer_size) {
        snprintf(buffer, buffer_size, "unknown-time");
        return;
    }
    snprintf(buffer + used, buffer_size - used, ".%03ld",
             (long)(now.tv_usec / 1000));
}

void cubicle_library_debug_log(const char *event,
                               const char *method,
                               cubicle_error_code_t code,
                               size_t request_bytes,
                               size_t response_bytes,
                               const cubicle_error_t *error)
{
    const char *safe_event = event != NULL ? event : "event";
    const char *safe_method = method != NULL ? method : "-";
    if (!library_debug_enabled()) {
        return;
    }
    FILE *stream = NULL;
    const char *path = getenv("CUBICLE_LIBRARY_DEBUG_LOG");
    if (path != NULL && path[0] != '\0') {
        stream = fopen(path, "a");
    }
    if (stream == NULL) {
        return;
    }

    char timestamp[64];
    library_debug_timestamp(timestamp, sizeof(timestamp));
    const char *program = getenv("CUBICLE_LIBRARY_DEBUG_PROGRAM");
    if (program == NULL || program[0] == '\0') {
        program = "unknown";
    }
    fprintf(stream,
            "%s program=%s pid=%ld event=%s method=%s code=%s request_bytes=%zu response_bytes=%zu",
            timestamp, program, (long)getpid(), safe_event, safe_method,
            cubicle_error_code_name(code), request_bytes, response_bytes);
    if (error != NULL) {
        if (error->system_errno != 0) {
            fprintf(stream, " errno=%d", error->system_errno);
        }
        if (error->message[0] != '\0') {
            fprintf(stream, " message=\"");
            for (const char *cursor = error->message; *cursor != '\0';
                 ++cursor) {
                if (*cursor == '"' || *cursor == '\\') {
                    fputc('\\', stream);
                }
                if ((unsigned char)*cursor >= 0x20) {
                    fputc(*cursor, stream);
                }
            }
            fputc('"', stream);
        }
    }
    fputc('\n', stream);
    fclose(stream);
}

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

    const char *request_params = params == NULL ? "{}" : params;
    cubicle_json_doc_t parsed_params;
    if (cubicle_json_parse(&parsed_params, request_params) < 0) {
        return set_client_error(client, CUBICLE_ERR_INTERNAL, 0,
                                "invalid request params JSON");
    }
    cubicle_json_cleanup(&parsed_params);

    size_t request_size = strlen(request_params) + strlen(method) +
                          strlen(request_id) + 256;
    char *request = malloc(request_size);
    if (request == NULL ||
        cubicle_rpc_request(request, request_size, request_id,
                            client->session.session_id, method,
                            request_params) < 0) {
        free(request);
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to build request");
    }

    void *response_data = NULL;
    size_t response_length = 0;
    size_t request_length = strlen(request);
    cubicle_library_debug_log("rpc.request", method, CUBICLE_OK,
                              request_length, 0, NULL);
    cubicle_error_code_t result = client->transport->vtable->request(
        client->transport, request, request_length, &response_data,
        &response_length, &client->last_error);
    free(request);
    if (result != CUBICLE_OK) {
        cubicle_library_debug_log("rpc.response", method, result,
                                  request_length, response_length,
                                  &client->last_error);
        return result;
    }

    char *response = calloc(response_length + 1, 1);
    if (response == NULL) {
        if (client->transport->vtable->response_free != NULL) {
            client->transport->vtable->response_free(client->transport,
                                                     response_data);
        }
        cubicle_library_debug_log("rpc.response", method, CUBICLE_ERR_INTERNAL,
                                  request_length, response_length, NULL);
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

    cubicle_rpc_response_envelope_t envelope;
    if (cubicle_rpc_decode_response(&envelope, response, request_id) < 0) {
        free(response);
        cubicle_library_debug_log("rpc.response", method, CUBICLE_ERR_PROTOCOL,
                                  request_length, response_length, NULL);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "malformed response envelope");
    }

    if (!envelope.success) {
        cubicle_error_t decoded_error;
        if (cubicle_rpc_decode_error_value(envelope.error,
                                           &decoded_error) < 0) {
            cubicle_rpc_response_envelope_cleanup(&envelope);
            free(response);
            cubicle_library_debug_log("rpc.response", method,
                                      CUBICLE_ERR_PROTOCOL, request_length,
                                      response_length, NULL);
            return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                    "malformed response error");
        }
        client->last_error = decoded_error;
        cubicle_rpc_response_envelope_cleanup(&envelope);
        free(response);
        cubicle_library_debug_log("rpc.response", method, decoded_error.code,
                                  request_length, response_length,
                                  &decoded_error);
        return decoded_error.code;
    }

    cubicle_rpc_response_envelope_cleanup(&envelope);
    *response_out = response;
    memset(&client->last_error, 0, sizeof(client->last_error));
    cubicle_library_debug_log("rpc.response", method, CUBICLE_OK,
                              request_length, response_length, NULL);
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
