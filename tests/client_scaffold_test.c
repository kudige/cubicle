#include "cubicle/client.h"
#include "cubicle/manager.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_field(const char *json, const char *key,
                       char *buffer, size_t buffer_size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    assert(start != NULL);
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    assert(end != NULL);
    size_t length = (size_t)(end - start);
    assert(length < buffer_size);
    memcpy(buffer, start, length);
    buffer[length] = '\0';
}

static cubicle_error_code_t mock_connect(cubicle_transport_t *transport, const cubicle_endpoint_t *endpoint, cubicle_error_t *error)
{
    (void)transport;
    (void)error;
    return strncmp(endpoint->uri, "unix://", 7) == 0 ? CUBICLE_OK : CUBICLE_ERR_INVALID_ARGUMENT;
}

static cubicle_error_code_t mock_request(cubicle_transport_t *transport,
                                         const void *request,
                                         size_t request_length,
                                         void **response_out,
                                         size_t *response_length_out,
                                         cubicle_error_t *error)
{
    (void)transport;
    (void)error;
    char request_json[1024];
    assert(request_length < sizeof(request_json));
    memcpy(request_json, request, request_length);
    request_json[request_length] = '\0';

    char request_id[64];
    json_field(request_json, "request_id", request_id, sizeof(request_id));

    char response[1024];
    if (strstr(request_json, "\"method\":\"session.local_bootstrap\"") != NULL) {
        snprintf(response, sizeof(response),
                 "{\"request_id\":\"%s\",\"success\":true,\"result\":{\"session_id\":\"session-1\",\"manager_id\":\"manager-1\",\"client_key_id\":\"local-key\",\"protocol_major\":0,\"protocol_minor\":1,\"negotiated_capabilities\":258,\"authenticated_at_ms\":10,\"expires_at_ms\":0}}",
                 request_id);
    } else {
        snprintf(response, sizeof(response),
                 "{\"request_id\":\"%s\",\"success\":false,\"error\":{\"code\":\"unsupported\",\"message\":\"manager.ping unsupported\",\"system_errno\":0,\"retryable\":false}}",
                 request_id);
    }
    char *copy = malloc(strlen(response));
    assert(copy != NULL);
    memcpy(copy, response, strlen(response));
    *response_out = copy;
    *response_length_out = strlen(response);
    return CUBICLE_OK;
}

static void mock_response_free(cubicle_transport_t *transport, void *response)
{
    (void)transport;
    free(response);
}

static void mock_close(cubicle_transport_t *transport) { (void)transport; }

static const cubicle_transport_vtable_t mock_vtable = {
    .connect = mock_connect,
    .request = mock_request,
    .response_free = mock_response_free,
    .close = mock_close,
    .destroy = NULL,
};

int main(void)
{
    assert(strcmp(cubicle_error_code_name(CUBICLE_ERR_TIMEOUT), "timeout") == 0);
    cubicle_transport_t transport = { .vtable = &mock_vtable, .context = NULL };
    cubicle_client_options_t options = {
        .endpoint = {
            .uri = "unix:///tmp/cubicle-manager.sock",
            .server_identity = "local",
        },
        .connect_timeout_ms = 1000,
        .request_timeout_ms = 5000,
        .transport = &transport,
    };

    cubicle_client_t *client = NULL;
    assert(cubicle_client_connect(&options, &client) == CUBICLE_OK);
    assert(client != NULL);
    cubicle_manager_ping_result_t ping_result;
    memset(&ping_result, 0, sizeof(ping_result));
    assert(cubicle_manager_ping(client, &ping_result) == CUBICLE_ERR_UNSUPPORTED);
    const cubicle_error_t *error = cubicle_client_last_error(client);
    assert(error != NULL && error->code == CUBICLE_ERR_UNSUPPORTED);
    assert(strstr(error->message, "manager.ping") != NULL);
    cubicle_client_disconnect(client);
    return 0;
}
