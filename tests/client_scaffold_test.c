#include "cubicle/client.h"
#include "cubicle/manager.h"

#include <assert.h>
#include <string.h>

static cubicle_error_code_t mock_connect(cubicle_transport_t *transport, const cubicle_endpoint_t *endpoint, cubicle_error_t *error)
{
    (void)transport;
    (void)error;
    return strncmp(endpoint->uri, "unix://", 7) == 0 ? CUBICLE_OK : CUBICLE_ERR_INVALID_ARGUMENT;
}

static void mock_close(cubicle_transport_t *transport) { (void)transport; }

static const cubicle_transport_vtable_t mock_vtable = {
    .connect = mock_connect,
    .request = NULL,
    .response_free = NULL,
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
