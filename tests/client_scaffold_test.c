#include "cubicle/client.h"
#include "cubicle/manager.h"
#include "cubicle/rpc.h"
#include "cubicle/workspace.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static cubicle_error_code_t mock_connect(cubicle_transport_t *transport, const cubicle_endpoint_t *endpoint, cubicle_error_t *error)
{
    (void)transport;
    (void)error;
    return strncmp(endpoint->uri, "unix://", 7) == 0 ? CUBICLE_OK : CUBICLE_ERR_INVALID_ARGUMENT;
}

static void mock_close(cubicle_transport_t *transport) { (void)transport; }

static cubicle_error_code_t mock_request(cubicle_transport_t *transport,
                                         const void *request,
                                         size_t request_length,
                                         void **response_out,
                                         size_t *response_length_out,
                                         cubicle_error_t *error)
{
    (void)transport;
    (void)error;
    assert(request != NULL && request_length > 0);
    char request_json[1024];
    assert(request_length < sizeof(request_json));
    memcpy(request_json, request, request_length);
    request_json[request_length] = '\0';

    char response[1024];
    if (strstr(request_json, "\"session.local_bootstrap\"") != NULL) {
        assert(cubicle_rpc_success(response, sizeof(response), "c1",
                                   "{\"session_id\":\"session-1\",\"manager_id\":\"manager-1\",\"client_key_id\":\"local-key\",\"protocol_major\":0,\"protocol_minor\":1,\"negotiated_capabilities\":258,\"authenticated_at_ms\":10,\"expires_at_ms\":0}") == 0);
    } else if (strstr(request_json, "\"manager.ping\"") != NULL) {
        assert(cubicle_rpc_success(response, sizeof(response), "c2",
                                   "{\"manager_id\":\"manager-1\",\"protocol_major\":0,\"protocol_minor\":1,\"server_time_ms\":20,\"uptime_ms\":10}") == 0);
    } else if (strstr(request_json, "\"workspace.create\"") != NULL) {
        assert(cubicle_rpc_success(response, sizeof(response), "c3",
                                   "{\"manager_id\":\"manager-1\",\"id\":\"workspace-1\",\"name\":\"Project A\",\"created_at_ms\":0,\"updated_at_ms\":0,\"process_count\":0,\"running_process_count\":0}") == 0);
    } else if (strstr(request_json, "\"workspace.get\"") != NULL) {
        assert(cubicle_rpc_success(response, sizeof(response), "c4",
                                   "{\"manager_id\":\"manager-1\",\"id\":\"workspace-1\",\"name\":\"Project A\",\"created_at_ms\":0,\"updated_at_ms\":0,\"process_count\":2,\"running_process_count\":1}") == 0);
    } else if (strstr(request_json, "\"workspace.list\"") != NULL) {
        assert(cubicle_rpc_success(response, sizeof(response), "c5",
                                   "{\"workspaces\":[{\"manager_id\":\"manager-1\",\"id\":\"workspace-1\",\"name\":\"Project A\",\"created_at_ms\":0,\"updated_at_ms\":0,\"process_count\":2,\"running_process_count\":1},{\"manager_id\":\"manager-1\",\"id\":\"workspace-2\",\"name\":\"Project B\",\"created_at_ms\":0,\"updated_at_ms\":0,\"process_count\":0,\"running_process_count\":0}],\"count\":2,\"has_more\":false}") == 0);
    } else {
        assert(cubicle_rpc_error(response, sizeof(response), "cX",
                                 CUBICLE_ERR_UNSUPPORTED,
                                 "unsupported", 0, 0) == 0);
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
    cubicle_session_info_t session;
    memset(&session, 0, sizeof(session));
    assert(cubicle_client_session_info(client, &session) == CUBICLE_OK);
    assert(strcmp(session.session_id, "session-1") == 0);
    assert(session.negotiated_capabilities == 258);

    cubicle_manager_ping_result_t ping_result;
    memset(&ping_result, 0, sizeof(ping_result));
    assert(cubicle_manager_ping(client, &ping_result) == CUBICLE_OK);
    assert(strcmp(ping_result.manager_id, "manager-1") == 0);
    assert(ping_result.protocol_major == 0);
    assert(ping_result.protocol_minor == 1);
    assert(ping_result.server_time_ms == 20);
    assert(ping_result.uptime_ms == 10);

    cubicle_workspace_create_options_t create_options = {
        .name = "Project A",
    };
    cubicle_workspace_info_t workspace;
    memset(&workspace, 0, sizeof(workspace));
    assert(cubicle_workspace_create(client, &create_options, &workspace) ==
           CUBICLE_OK);
    assert(strcmp(workspace.id, "workspace-1") == 0);
    assert(strcmp(workspace.name, "Project A") == 0);

    memset(&workspace, 0, sizeof(workspace));
    assert(cubicle_workspace_get(client, "Project A", &workspace) ==
           CUBICLE_OK);
    assert(workspace.process_count == 2);
    assert(workspace.running_process_count == 1);

    cubicle_workspace_info_t *workspaces = NULL;
    size_t workspace_count = 0;
    cubicle_page_info_t page;
    memset(&page, 0, sizeof(page));
    assert(cubicle_workspace_list(client, NULL, &workspaces,
                                  &workspace_count, &page) == CUBICLE_OK);
    assert(workspace_count == 2);
    assert(page.has_more == false);
    assert(strcmp(workspaces[0].name, "Project A") == 0);
    assert(strcmp(workspaces[1].name, "Project B") == 0);
    cubicle_workspace_list_free(workspaces);

    cubicle_client_disconnect(client);
    return 0;
}
