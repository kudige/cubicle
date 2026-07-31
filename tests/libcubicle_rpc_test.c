#include "cubicle/cubicle.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mock_transport {
    cubicle_transport_t transport;
    char last_request[4096];
    int mismatch_next_response;
} mock_transport_t;

static const char *json_field(const char *json, const char *key,
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
    return buffer;
}

static int has_method(const char *request, const char *method)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"method\":\"%s\"", method);
    return strstr(request, pattern) != NULL;
}

static char *format_response(const char *request_id, const char *result)
{
    size_t length = strlen(request_id) + strlen(result) + 64;
    char *response = malloc(length);
    assert(response != NULL);
    snprintf(response, length,
             "{\"request_id\":\"%s\",\"success\":true,\"result\":%s}",
             request_id, result);
    return response;
}

static cubicle_error_code_t mock_connect(cubicle_transport_t *transport,
                                         const cubicle_endpoint_t *endpoint,
                                         cubicle_error_t *error)
{
    (void)transport;
    (void)error;
    return strncmp(endpoint->uri, "unix://", 7) == 0
               ? CUBICLE_OK
               : CUBICLE_ERR_INVALID_ARGUMENT;
}

static cubicle_error_code_t mock_request(cubicle_transport_t *transport,
                                         const void *request,
                                         size_t request_length,
                                         void **response_out,
                                         size_t *response_length_out,
                                         cubicle_error_t *error)
{
    (void)error;
    mock_transport_t *mock = (mock_transport_t *)transport;
    assert(request_length < sizeof(mock->last_request));
    memcpy(mock->last_request, request, request_length);
    mock->last_request[request_length] = '\0';

    char request_id[64];
    json_field(mock->last_request, "request_id", request_id,
               sizeof(request_id));
    const char *response_id = mock->mismatch_next_response ? "wrong" : request_id;
    mock->mismatch_next_response = 0;

    char *response = NULL;
    if (has_method(mock->last_request, "manager.ping")) {
        response = format_response(response_id,
            "{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"protocol_major\":0,\"protocol_minor\":1,\"server_time_ms\":100,\"uptime_ms\":7}");
    } else if (has_method(mock->last_request, "manager.status")) {
        response = format_response(response_id,
            "{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"protocol_major\":0,\"protocol_minor\":1,\"capabilities\":258,\"started_at_ms\":10,\"server_time_ms\":20,\"workspace_count\":2,\"process_count\":3,\"controller_count\":4,\"active_client_sessions\":5}");
    } else if (has_method(mock->last_request, "workspace.create") ||
               has_method(mock->last_request, "workspace.get")) {
        response = format_response(response_id,
            "{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"name\":\"default\",\"created_at_ms\":1,\"updated_at_ms\":2,\"process_count\":3,\"running_process_count\":1}");
    } else if (has_method(mock->last_request, "workspace.list")) {
        response = format_response(response_id,
            "{\"workspaces\":[{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"name\":\"default\",\"created_at_ms\":1,\"updated_at_ms\":2}],\"continuation_token\":\"next\",\"has_more\":true}");
    } else if (has_method(mock->last_request, "process.start") ||
               has_method(mock->last_request, "process.get") ||
               has_method(mock->last_request, "process.wait")) {
        response = format_response(response_id,
            "{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"workspace_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"id\":\"cccccccccccccccccccccccccccccccc\",\"friendly_name\":\"build\",\"mode\":\"stream\",\"state\":\"running\",\"stdout_offset\":11,\"stderr_offset\":12,\"local_pid\":123,\"local_pgid\":123}");
    } else if (has_method(mock->last_request, "process.list")) {
        response = format_response(response_id,
            "{\"processes\":[{\"id\":\"cccccccccccccccccccccccccccccccc\",\"friendly_name\":\"build\",\"mode\":\"stream\",\"state\":\"completed\",\"has_exit_status\":true,\"exit_code\":0}],\"has_more\":false}");
    } else if (has_method(mock->last_request, "process.read_output")) {
        response = format_response(response_id,
            "{\"start_offset\":5,\"next_offset\":10,\"end_of_stream\":false,\"data\":\"hello\"}");
    } else if (has_method(mock->last_request, "workspace.key.add")) {
        response = format_response(response_id,
            "{\"key_id\":\"dddddddddddddddddddddddddddddddd\",\"fingerprint\":\"fp\",\"label\":\"owner\",\"capabilities\":257,\"created_at_ms\":1}");
    } else if (has_method(mock->last_request, "workspace.key.list")) {
        response = format_response(response_id,
            "{\"keys\":[{\"key_id\":\"dddddddddddddddddddddddddddddddd\",\"fingerprint\":\"fp\",\"label\":\"owner\",\"capabilities\":257}]}");
    } else if (has_method(mock->last_request, "attachment.request")) {
        response = format_response(response_id,
            "{\"grant_id\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\",\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"workspace_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"process_id\":\"cccccccccccccccccccccccccccccccc\",\"client_key_id\":\"dddddddddddddddddddddddddddddddd\",\"endpoint\":{\"uri\":\"unix:///tmp/controller.sock\",\"server_identity\":\"controller\"},\"token\":\"tok\",\"issued_at_ms\":1,\"expires_at_ms\":2,\"connection_limit\":1,\"granted_channels\":\"stdout,stdin\",\"mode\":\"interactive\"}");
    } else if (has_method(mock->last_request, "events.list")) {
        response = format_response(response_id,
            "{\"events\":[{\"global_sequence\":1,\"workspace_sequence\":2,\"timestamp_ms\":3,\"type\":\"process_started\",\"workspace_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"process_id\":\"cccccccccccccccccccccccccccccccc\",\"payload\":\"ok\"}]}");
    } else if (has_method(mock->last_request, "process.signal") ||
               has_method(mock->last_request, "process.terminate") ||
               has_method(mock->last_request, "process.kill") ||
               has_method(mock->last_request, "process.remove") ||
               has_method(mock->last_request, "workspace.rename") ||
               has_method(mock->last_request, "workspace.stop") ||
               has_method(mock->last_request, "workspace.delete") ||
               has_method(mock->last_request, "workspace.key.update") ||
               has_method(mock->last_request, "workspace.key.revoke") ||
               has_method(mock->last_request, "manager.reconcile") ||
               has_method(mock->last_request, "manager.shutdown")) {
        response = format_response(response_id, "{}");
    } else {
        size_t length = strlen(response_id) + 160;
        response = malloc(length);
        assert(response != NULL);
        snprintf(response, length,
            "{\"request_id\":\"%s\",\"success\":false,\"error\":{\"code\":\"unsupported\",\"message\":\"unsupported method\",\"system_errno\":0,\"retryable\":false}}",
            response_id);
    }

    *response_out = response;
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
};

static cubicle_client_t *connect_client(mock_transport_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    mock->transport.vtable = &mock_vtable;
    cubicle_client_options_t options = {
        .endpoint = { .uri = "unix:///tmp/manager.sock" },
        .transport = &mock->transport,
    };
    cubicle_client_t *client = NULL;
    assert(cubicle_client_connect(&options, &client) == CUBICLE_OK);
    return client;
}

int main(void)
{
    mock_transport_t mock;
    cubicle_client_t *client = connect_client(&mock);

    cubicle_manager_ping_result_t ping;
    assert(cubicle_manager_ping(client, &ping) == CUBICLE_OK);
    assert(strcmp(ping.manager_id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);
    assert(ping.protocol_minor == 1);
    assert(has_method(mock.last_request, "manager.ping"));

    cubicle_manager_status_t status;
    assert(cubicle_manager_status(client, &status) == CUBICLE_OK);
    assert(status.workspace_count == 2);

    cubicle_workspace_create_options_t create = {
        .name = "default",
        .request = { .idempotency_key = "idem-1", .timeout_ms = 42 },
    };
    cubicle_workspace_info_t workspace;
    assert(cubicle_workspace_create(client, &create, &workspace) == CUBICLE_OK);
    assert(strcmp(workspace.name, "default") == 0);
    assert(strstr(mock.last_request, "\"idempotency_key\":\"idem-1\"") != NULL);

    cubicle_workspace_info_t *workspaces = NULL;
    size_t workspace_count = 0;
    cubicle_page_info_t page;
    assert(cubicle_workspace_list(client, NULL, &workspaces, &workspace_count,
                                  &page) == CUBICLE_OK);
    assert(workspace_count == 1);
    assert(page.has_more);
    cubicle_workspace_list_free(workspaces);

    const char *argv[] = { "echo", "ok" };
    cubicle_process_start_options_t start = {
        .workspace_id = workspace.id,
        .friendly_name = "build",
        .mode = CUBICLE_PROCESS_STREAM,
        .argv = argv,
        .argc = 2,
    };
    cubicle_process_info_t process;
    assert(cubicle_process_start(client, &start, &process) == CUBICLE_OK);
    assert(process.state == CUBICLE_PROCESS_RUNNING);
    assert(strstr(mock.last_request, "\"argv\":[\"echo\",\"ok\"]") != NULL);

    cubicle_process_info_t *processes = NULL;
    size_t process_count = 0;
    assert(cubicle_process_list(client, NULL, &processes, &process_count,
                                NULL) == CUBICLE_OK);
    assert(process_count == 1);
    assert(processes[0].state == CUBICLE_PROCESS_COMPLETED);
    cubicle_process_list_free(processes);

    cubicle_output_chunk_t chunk;
    assert(cubicle_process_read_output(client, process.id, CUBICLE_STREAM_STDOUT,
                                       5, 16, &chunk) == CUBICLE_OK);
    assert(chunk.length == 5 && memcmp(chunk.data, "hello", 5) == 0);
    cubicle_output_chunk_free(&chunk);

    unsigned char key[] = { 0xab, 0xcd };
    cubicle_workspace_key_info_t key_info;
    assert(cubicle_workspace_key_add(client, workspace.id, key, sizeof(key),
                                     "owner", CUBICLE_CAP_PROCESS_START,
                                     &key_info) == CUBICLE_OK);
    assert(strcmp(key_info.fingerprint, "fp") == 0);

    cubicle_workspace_key_info_t *keys = NULL;
    size_t key_count = 0;
    assert(cubicle_workspace_key_list(client, workspace.id, &keys,
                                      &key_count) == CUBICLE_OK);
    assert(key_count == 1);
    cubicle_workspace_key_list_free(keys);

    cubicle_attachment_request_t attachment_request = {
        .process_id = process.id,
        .channels = CUBICLE_CHANNEL_STDOUT | CUBICLE_CHANNEL_STDIN,
        .mode = CUBICLE_ATTACHMENT_INTERACTIVE,
    };
    cubicle_attachment_grant_t grant;
    assert(cubicle_attachment_request(client, &attachment_request, &grant) ==
           CUBICLE_OK);
    assert(strcmp(grant.endpoint.uri, "unix:///tmp/controller.sock") == 0);
    assert((grant.granted_channels & CUBICLE_CHANNEL_STDOUT) != 0);

    cubicle_event_t *events = NULL;
    size_t event_count = 0;
    assert(cubicle_events_list(client, NULL, &events, &event_count) ==
           CUBICLE_OK);
    assert(event_count == 1);
    assert(events[0].type == CUBICLE_EVENT_PROCESS_STARTED);
    cubicle_events_free(events);

    assert(cubicle_process_signal(client, process.id, 15) == CUBICLE_OK);
    assert(cubicle_process_terminate(client, process.id, NULL) == CUBICLE_OK);
    assert(cubicle_process_kill(client, process.id) == CUBICLE_OK);
    assert(cubicle_process_remove(client, process.id) == CUBICLE_OK);
    assert(cubicle_workspace_rename(client, workspace.id, "renamed", NULL) == CUBICLE_OK);
    assert(cubicle_workspace_stop(client, workspace.id, NULL) == CUBICLE_OK);
    assert(cubicle_workspace_delete(client, workspace.id, NULL) == CUBICLE_OK);
    assert(cubicle_workspace_key_set_capabilities(client, workspace.id,
                                                  key_info.key_id, 1) == CUBICLE_OK);
    assert(cubicle_workspace_key_revoke(client, workspace.id,
                                        key_info.key_id) == CUBICLE_OK);
    assert(cubicle_manager_reconcile(client) == CUBICLE_OK);
    assert(cubicle_manager_shutdown(client, false) == CUBICLE_OK);

    mock.mismatch_next_response = 1;
    assert(cubicle_manager_ping(client, &ping) == CUBICLE_ERR_PROTOCOL);

    cubicle_client_disconnect(client);
    return 0;
}
