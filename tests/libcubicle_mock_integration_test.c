#define _POSIX_C_SOURCE 200809L

#include "cubicle/cubicle.h"
#include "cubicle/transport_unix.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void make_endpoint(cubicle_endpoint_t *endpoint, const char *socket_path)
{
    memset(endpoint, 0, sizeof(*endpoint));
    int length = snprintf(endpoint->uri, sizeof(endpoint->uri), "unix://%s",
                          socket_path);
    assert(length > 0 && (size_t)length < sizeof(endpoint->uri));
}

static void wait_for_socket(const char *path)
{
    for (int i = 0; i < 100; ++i) {
        struct stat socket_stat;
        if (stat(path, &socket_stat) == 0 && S_ISSOCK(socket_stat.st_mode)) {
            return;
        }
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
        nanosleep(&delay, NULL);
    }
    assert(!"mock socket was not created");
}

static pid_t start_server(const char *socket_path, const char *log_path,
                          const char *mode, const char *scenario,
                          const char *controller_uri, int max_requests)
{
    const char *server = getenv("CUBICLE_MOCK_API_SERVER");
    assert(server != NULL && server[0] != '\0');

    char max_requests_text[32];
    snprintf(max_requests_text, sizeof(max_requests_text), "%d", max_requests);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(server, server,
              "--socket", socket_path,
              "--log", log_path,
              "--mode", mode,
              "--scenario", scenario,
              "--controller-uri", controller_uri,
              "--max-requests", max_requests_text,
              (char *)NULL);
        _exit(127);
    }

    wait_for_socket(socket_path);
    return pid;
}

static void expect_server_exit(pid_t pid)
{
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static cubicle_client_t *connect_client(const char *socket_path)
{
    cubicle_transport_t *transport = NULL;
    assert(cubicle_transport_unix_create(&transport) == CUBICLE_OK);

    cubicle_client_options_t options;
    memset(&options, 0, sizeof(options));
    make_endpoint(&options.endpoint, socket_path);
    options.transport = transport;

    cubicle_client_t *client = NULL;
    assert(cubicle_client_connect(&options, &client) == CUBICLE_OK);
    return client;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "r");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    char *data = calloc((size_t)size + 1, 1);
    assert(data != NULL);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    return data;
}

static void assert_log_contains(const char *log_path, const char *needle)
{
    char *log = read_file(log_path);
    if (strstr(log, needle) == NULL) {
        fprintf(stderr, "log %s missing %s\n%s\n", log_path, needle, log);
        assert(!"expected request log entry not found");
    }
    free(log);
}

static void run_manager_integration(const char *directory)
{
    char manager_socket[256];
    char controller_socket[256];
    char manager_log[256];
    char controller_uri[320];
    snprintf(manager_socket, sizeof(manager_socket), "%s/manager.sock", directory);
    snprintf(controller_socket, sizeof(controller_socket), "%s/controller.sock", directory);
    snprintf(manager_log, sizeof(manager_log), "%s/manager.log", directory);
    snprintf(controller_uri, sizeof(controller_uri), "unix://%s", controller_socket);

    pid_t manager_pid = start_server(manager_socket, manager_log, "manager",
                                     "normal", controller_uri, 9);
    cubicle_client_t *client = connect_client(manager_socket);

    cubicle_manager_ping_result_t ping;
    // Endpoint test for manager.ping
    assert(cubicle_manager_ping(client, &ping) == CUBICLE_OK);
    assert(strcmp(ping.manager_id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);

    cubicle_manager_status_t status;
    // Endpoint test for manager.status
    assert(cubicle_manager_status(client, &status) == CUBICLE_OK);
    assert(status.workspace_count == 2);

    cubicle_workspace_create_options_t create = {
        .name = "default",
        .request = { .idempotency_key = "idem-1", .timeout_ms = 42 },
    };
    cubicle_workspace_info_t workspace;
    // Endpoint test for workspace.create
    assert(cubicle_workspace_create(client, &create, &workspace) == CUBICLE_OK);
    assert(strcmp(workspace.id, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0);

    cubicle_workspace_info_t *workspaces = NULL;
    size_t workspace_count = 0;
    cubicle_page_info_t page;
    // Endpoint test for workspace.list
    assert(cubicle_workspace_list(client, NULL, &workspaces, &workspace_count,
                                  &page) == CUBICLE_OK);
    assert(workspace_count == 1 && page.has_more);
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
    // Endpoint test for process.start
    assert(cubicle_process_start(client, &start, &process) == CUBICLE_OK);
    assert(process.state == CUBICLE_PROCESS_RUNNING);

    cubicle_output_chunk_t chunk;
    // Endpoint test for process.read_output
    assert(cubicle_process_read_output(client, process.id, CUBICLE_STREAM_STDOUT,
                                       5, 16, &chunk) == CUBICLE_OK);
    assert(chunk.length == 5 && memcmp(chunk.data, "hello", 5) == 0);
    cubicle_output_chunk_free(&chunk);

    cubicle_attachment_request_t attachment_request = {
        .process_id = process.id,
        .channels = CUBICLE_CHANNEL_STDOUT | CUBICLE_CHANNEL_STDIN,
        .mode = CUBICLE_ATTACHMENT_INTERACTIVE,
    };
    cubicle_attachment_grant_t grant;
    // Endpoint test for attachment.request
    assert(cubicle_attachment_request(client, &attachment_request, &grant) ==
           CUBICLE_OK);
    assert(strcmp(grant.endpoint.uri, controller_uri) == 0);

    cubicle_event_t *events = NULL;
    size_t event_count = 0;
    // Endpoint test for events.list
    assert(cubicle_events_list(client, NULL, &events, &event_count) ==
           CUBICLE_OK);
    assert(event_count == 1);
    cubicle_events_free(events);

    // Endpoint test for process.signal
    assert(cubicle_process_signal(client, process.id, 15) == CUBICLE_OK);

    cubicle_client_disconnect(client);
    expect_server_exit(manager_pid);

    assert_log_contains(manager_log, "\"method\":\"manager.ping\"");
    assert_log_contains(manager_log, "\"method\":\"workspace.create\"");
    assert_log_contains(manager_log, "\"idempotency_key\":\"idem-1\"");
    assert_log_contains(manager_log, "\"method\":\"process.start\"");
    assert_log_contains(manager_log, "\"argv\":[\"echo\",\"ok\"]");
    assert_log_contains(manager_log, "\"method\":\"attachment.request\"");
    assert_log_contains(manager_log, "\"method\":\"events.list\"");
    assert_log_contains(manager_log, "\"signal_number\":15");
}

static void run_controller_integration(const char *directory)
{
    char controller_socket[256];
    char controller_log[256];
    snprintf(controller_socket, sizeof(controller_socket), "%s/controller.sock", directory);
    snprintf(controller_log, sizeof(controller_log), "%s/controller.log", directory);

    pid_t controller_pid = start_server(controller_socket, controller_log,
                                        "controller", "normal",
                                        "unix:///unused.sock", 1);

    cubicle_transport_t *transport = NULL;
    assert(cubicle_transport_unix_create(&transport) == CUBICLE_OK);
    cubicle_endpoint_t endpoint;
    make_endpoint(&endpoint, controller_socket);
    cubicle_error_t error;
    memset(&error, 0, sizeof(error));
    assert(transport->vtable->connect(transport, &endpoint, &error) == CUBICLE_OK);

    const char request[] =
        "{\"request_id\":\"raw-1\",\"method\":\"controller.status\",\"params\":{}}";
    void *response = NULL;
    size_t response_length = 0;
    // Endpoint test for controller.status
    assert(transport->vtable->request(transport, request, sizeof(request) - 1,
                                      &response, &response_length,
                                      &error) == CUBICLE_OK);
    assert(response_length > 0);
    char *response_text = calloc(response_length + 1, 1);
    assert(response_text != NULL);
    memcpy(response_text, response, response_length);
    assert(strstr(response_text, "\"success\":true") != NULL);
    assert(strstr(response_text, "controller_id") != NULL);
    free(response_text);
    transport->vtable->response_free(transport, response);
    transport->vtable->destroy(transport);
    expect_server_exit(controller_pid);

    assert_log_contains(controller_log, "\"method\":\"controller.status\"");
}

static void run_error_scenario(const char *directory, const char *scenario,
                               cubicle_error_code_t expected)
{
    char socket_path[256];
    char log_path[256];
    snprintf(socket_path, sizeof(socket_path), "%s/%s.sock", directory, scenario);
    snprintf(log_path, sizeof(log_path), "%s/%s.log", directory, scenario);

    pid_t server_pid = start_server(socket_path, log_path, "manager",
                                    scenario, "unix:///unused.sock", 1);
    cubicle_client_t *client = connect_client(socket_path);
    cubicle_manager_ping_result_t ping;
    // Endpoint test for manager.ping error handling
    assert(cubicle_manager_ping(client, &ping) == expected);
    const cubicle_error_t *error = cubicle_client_last_error(client);
    assert(error != NULL && error->code == expected);
    cubicle_client_disconnect(client);
    expect_server_exit(server_pid);
    assert_log_contains(log_path, "\"method\":\"manager.ping\"");
}

int main(void)
{
    char directory_template[] = "/tmp/libcubicle-mock-test-XXXXXX";
    char *directory = mkdtemp(directory_template);
    assert(directory != NULL);

    run_manager_integration(directory);
    run_controller_integration(directory);
    run_error_scenario(directory, "error", CUBICLE_ERR_UNSUPPORTED);
    run_error_scenario(directory, "malformed", CUBICLE_ERR_PROTOCOL);
    run_error_scenario(directory, "mismatch", CUBICLE_ERR_PROTOCOL);

    rmdir(directory);
    return 0;
}
