#define _POSIX_C_SOURCE 200809L

#include "cubicle/cubicle.h"
#include "cubicle/transport_tcp.h"
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

typedef enum test_transport_kind {
    TEST_TRANSPORT_UNIX,
    TEST_TRANSPORT_TCP,
} test_transport_kind_t;

static const char *transport_name(test_transport_kind_t kind)
{
    return kind == TEST_TRANSPORT_TCP ? "tcp" : "unix";
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [all|unix|tcp]\n", program);
}

static void make_endpoint_uri(cubicle_endpoint_t *endpoint, const char *uri)
{
    memset(endpoint, 0, sizeof(*endpoint));
    int length = snprintf(endpoint->uri, sizeof(endpoint->uri), "%s", uri);
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

static void wait_for_file(const char *path)
{
    for (int i = 0; i < 100; ++i) {
        struct stat file_stat;
        if (stat(path, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
            return;
        }
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
        nanosleep(&delay, NULL);
    }
    assert(!"mock port file was not created");
}

static uint16_t read_port_file(const char *path)
{
    FILE *file = fopen(path, "r");
    assert(file != NULL);
    unsigned int port = 0;
    assert(fscanf(file, "%u", &port) == 1);
    fclose(file);
    assert(port > 0 && port <= 65535);
    return (uint16_t)port;
}

static cubicle_error_code_t create_transport(test_transport_kind_t kind,
                                             cubicle_transport_t **transport)
{
    if (kind == TEST_TRANSPORT_TCP) {
        return cubicle_transport_tcp_create(transport);
    }
    return cubicle_transport_unix_create(transport);
}

static pid_t start_unix_server(const char *socket_path, const char *log_path,
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

static pid_t start_tcp_server(const char *port_file, const char *log_path,
                              const char *mode, const char *scenario,
                              const char *controller_uri, int max_requests,
                              char *endpoint_uri, size_t endpoint_uri_size)
{
    const char *server = getenv("CUBICLE_MOCK_API_SERVER");
    assert(server != NULL && server[0] != '\0');

    char max_requests_text[32];
    snprintf(max_requests_text, sizeof(max_requests_text), "%d", max_requests);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(server, server,
              "--tcp-host", "127.0.0.1",
              "--tcp-port", "0",
              "--port-file", port_file,
              "--log", log_path,
              "--mode", mode,
              "--scenario", scenario,
              "--controller-uri", controller_uri,
              "--max-requests", max_requests_text,
              (char *)NULL);
        _exit(127);
    }

    wait_for_file(port_file);
    uint16_t port = read_port_file(port_file);
    int length = snprintf(endpoint_uri, endpoint_uri_size,
                          "tcp://127.0.0.1:%u", (unsigned int)port);
    assert(length > 0 && (size_t)length < endpoint_uri_size);
    return pid;
}

static pid_t start_server(test_transport_kind_t kind, const char *directory,
                          const char *label, const char *log_path,
                          const char *mode, const char *scenario,
                          const char *controller_uri, int max_requests,
                          char *endpoint_uri, size_t endpoint_uri_size)
{
    if (kind == TEST_TRANSPORT_TCP) {
        char port_file[256];
        snprintf(port_file, sizeof(port_file), "%s/%s-%s.port", directory,
                 label, transport_name(kind));
        return start_tcp_server(port_file, log_path, mode, scenario,
                                controller_uri, max_requests, endpoint_uri,
                                endpoint_uri_size);
    }

    char socket_path[256];
    snprintf(socket_path, sizeof(socket_path), "%s/%s-%s.sock", directory,
             label, transport_name(kind));
    int length = snprintf(endpoint_uri, endpoint_uri_size, "unix://%s",
                          socket_path);
    assert(length > 0 && (size_t)length < endpoint_uri_size);
    return start_unix_server(socket_path, log_path, mode, scenario,
                             controller_uri, max_requests);
}

static void expect_server_exit(pid_t pid)
{
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static cubicle_client_t *connect_client(test_transport_kind_t kind,
                                        const char *endpoint_uri)
{
    cubicle_transport_t *transport = NULL;
    assert(create_transport(kind, &transport) == CUBICLE_OK);

    cubicle_client_options_t options;
    memset(&options, 0, sizeof(options));
    make_endpoint_uri(&options.endpoint, endpoint_uri);
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

static void assert_log_contains_all(const char *log_path,
                                    const char *const *needles,
                                    size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        assert_log_contains(log_path, needles[i]);
    }
}

static void run_manager_integration(const char *directory,
                                    test_transport_kind_t kind)
{
    char manager_log[256];
    char manager_uri[320];
    char controller_uri[320];
    snprintf(manager_log, sizeof(manager_log), "%s/manager-%s.log", directory,
             transport_name(kind));
    if (kind == TEST_TRANSPORT_TCP) {
        snprintf(controller_uri, sizeof(controller_uri), "tcp://127.0.0.1:1");
    } else {
        snprintf(controller_uri, sizeof(controller_uri),
                 "unix://%s/controller-%s.sock", directory,
                 transport_name(kind));
    }

    pid_t manager_pid = start_server(kind, directory, "manager", manager_log,
                                     "manager", "normal", controller_uri, 25,
                                     manager_uri, sizeof(manager_uri));
    cubicle_client_t *client = connect_client(kind, manager_uri);

    cubicle_manager_ping_result_t ping;
    // Endpoint test for manager.ping
    assert(cubicle_manager_ping(client, &ping) == CUBICLE_OK);
    assert(strcmp(ping.manager_id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);

    cubicle_manager_status_t status;
    // Endpoint test for manager.status
    assert(cubicle_manager_status(client, &status) == CUBICLE_OK);
    assert(status.workspace_count == 2);

    // Endpoint test for manager.reconcile
    assert(cubicle_manager_reconcile(client) == CUBICLE_OK);

    cubicle_workspace_create_options_t create = {
        .name = "default",
        .request = { .idempotency_key = "idem-1", .timeout_ms = 42 },
    };
    cubicle_workspace_info_t workspace;
    // Endpoint test for workspace.create
    assert(cubicle_workspace_create(client, &create, &workspace) == CUBICLE_OK);
    assert(strcmp(workspace.id, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0);

    cubicle_workspace_info_t fetched_workspace;
    // Endpoint test for workspace.get
    assert(cubicle_workspace_get(client, workspace.id, &fetched_workspace) ==
           CUBICLE_OK);
    assert(strcmp(fetched_workspace.name, "default") == 0);

    cubicle_workspace_info_t *workspaces = NULL;
    size_t workspace_count = 0;
    cubicle_page_info_t page;
    // Endpoint test for workspace.list
    assert(cubicle_workspace_list(client, NULL, &workspaces, &workspace_count,
                                  &page) == CUBICLE_OK);
    assert(workspace_count == 1 && page.has_more);
    cubicle_workspace_list_free(workspaces);

    // Endpoint test for workspace.rename
    assert(cubicle_workspace_rename(client, workspace.id, "renamed", NULL) ==
           CUBICLE_OK);

    cubicle_workspace_stop_options_t stop = {
        .grace_period_ms = 250,
        .force_after_grace = true,
    };
    // Endpoint test for workspace.stop
    assert(cubicle_workspace_stop(client, workspace.id, &stop) == CUBICLE_OK);

    cubicle_workspace_delete_options_t delete_options = {
        .stop_running_processes = true,
        .remove_retained_processes = true,
    };
    // Endpoint test for workspace.delete
    assert(cubicle_workspace_delete(client, workspace.id, &delete_options) ==
           CUBICLE_OK);

    unsigned char key[] = { 0xab, 0xcd };
    cubicle_workspace_key_info_t key_info;
    // Endpoint test for workspace.key.add
    assert(cubicle_workspace_key_add(client, workspace.id, key, sizeof(key),
                                     "owner", CUBICLE_CAP_PROCESS_START,
                                     &key_info) == CUBICLE_OK);
    assert(strcmp(key_info.fingerprint, "fp") == 0);

    cubicle_workspace_key_info_t *keys = NULL;
    size_t key_count = 0;
    // Endpoint test for workspace.key.list
    assert(cubicle_workspace_key_list(client, workspace.id, &keys,
                                      &key_count) == CUBICLE_OK);
    assert(key_count == 1);
    cubicle_workspace_key_list_free(keys);

    // Endpoint test for workspace.key.update
    assert(cubicle_workspace_key_set_capabilities(client, workspace.id,
                                                  key_info.key_id,
                                                  CUBICLE_CAP_PROCESS_READ) ==
           CUBICLE_OK);

    // Endpoint test for workspace.key.revoke
    assert(cubicle_workspace_key_revoke(client, workspace.id,
                                        key_info.key_id) == CUBICLE_OK);

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

    cubicle_process_info_t fetched_process;
    // Endpoint test for process.get
    assert(cubicle_process_get(client, process.id, workspace.id,
                               &fetched_process) == CUBICLE_OK);
    assert(strcmp(fetched_process.id, process.id) == 0);

    cubicle_process_info_t *processes = NULL;
    size_t process_count = 0;
    // Endpoint test for process.list
    assert(cubicle_process_list(client, NULL, &processes, &process_count,
                                NULL) == CUBICLE_OK);
    assert(process_count == 1);
    cubicle_process_list_free(processes);

    cubicle_output_chunk_t chunk;
    // Endpoint test for process.read_output
    assert(cubicle_process_read_output(client, process.id, CUBICLE_STREAM_STDOUT,
                                       5, 16, &chunk) == CUBICLE_OK);
    assert(chunk.length == 5 && memcmp(chunk.data, "hello", 5) == 0);
    cubicle_output_chunk_free(&chunk);

    cubicle_process_terminate_options_t terminate = {
        .grace_period_ms = 100,
        .force_after_grace = true,
    };
    // Endpoint test for process.terminate
    assert(cubicle_process_terminate(client, process.id, &terminate) ==
           CUBICLE_OK);

    // Endpoint test for process.kill
    assert(cubicle_process_kill(client, process.id) == CUBICLE_OK);

    cubicle_process_info_t waited_process;
    // Endpoint test for process.wait
    assert(cubicle_process_wait(client, process.id, 1000, &waited_process) ==
           CUBICLE_OK);
    assert(strcmp(waited_process.id, process.id) == 0);

    // Endpoint test for process.remove
    assert(cubicle_process_remove(client, process.id) == CUBICLE_OK);

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

    // Endpoint test for manager.shutdown
    assert(cubicle_manager_shutdown(client, false) == CUBICLE_OK);

    cubicle_client_disconnect(client);
    expect_server_exit(manager_pid);

    const char *const expected_methods[] = {
        "\"method\":\"manager.ping\"",
        "\"method\":\"manager.status\"",
        "\"method\":\"manager.reconcile\"",
        "\"method\":\"manager.shutdown\"",
        "\"method\":\"workspace.create\"",
        "\"method\":\"workspace.get\"",
        "\"method\":\"workspace.list\"",
        "\"method\":\"workspace.rename\"",
        "\"method\":\"workspace.stop\"",
        "\"method\":\"workspace.delete\"",
        "\"method\":\"workspace.key.add\"",
        "\"method\":\"workspace.key.list\"",
        "\"method\":\"workspace.key.update\"",
        "\"method\":\"workspace.key.revoke\"",
        "\"method\":\"process.start\"",
        "\"method\":\"process.get\"",
        "\"method\":\"process.list\"",
        "\"method\":\"process.signal\"",
        "\"method\":\"process.terminate\"",
        "\"method\":\"process.kill\"",
        "\"method\":\"process.wait\"",
        "\"method\":\"process.remove\"",
        "\"method\":\"process.read_output\"",
        "\"method\":\"attachment.request\"",
        "\"method\":\"events.list\"",
    };
    assert_log_contains_all(manager_log, expected_methods,
                            sizeof(expected_methods) /
                                sizeof(expected_methods[0]));
    assert_log_contains(manager_log, "\"idempotency_key\":\"idem-1\"");
    assert_log_contains(manager_log, "\"argv\":[\"echo\",\"ok\"]");
    assert_log_contains(manager_log, "\"signal_number\":15");
    assert_log_contains(manager_log, "\"force_after_grace\":true");
}

static void send_controller_request(cubicle_transport_t *transport,
                                    const char *method,
                                    const char *params)
{
    char request[512];
    int length = snprintf(request, sizeof(request),
                          "{\"request_id\":\"raw-%s\",\"method\":\"%s\",\"params\":%s}",
                          method, method, params);
    assert(length > 0 && (size_t)length < sizeof(request));

    cubicle_error_t error;
    memset(&error, 0, sizeof(error));
    void *response = NULL;
    size_t response_length = 0;
    assert(transport->vtable->request(transport, request, (size_t)length,
                                      &response, &response_length,
                                      &error) == CUBICLE_OK);
    assert(response_length > 0);
    char *response_text = calloc(response_length + 1, 1);
    assert(response_text != NULL);
    memcpy(response_text, response, response_length);
    assert(strstr(response_text, "\"success\":true") != NULL);
    free(response_text);
    transport->vtable->response_free(transport, response);
}

static void run_controller_integration(const char *directory,
                                       test_transport_kind_t kind)
{
    char controller_log[256];
    char controller_uri[320];
    snprintf(controller_log, sizeof(controller_log), "%s/controller-%s.log",
             directory, transport_name(kind));

    pid_t controller_pid = start_server(kind, directory, "controller",
                                        controller_log, "controller", "normal",
                                        "unix:///unused.sock", 5,
                                        controller_uri, sizeof(controller_uri));

    cubicle_transport_t *transport = NULL;
    assert(create_transport(kind, &transport) == CUBICLE_OK);
    cubicle_endpoint_t endpoint;
    make_endpoint_uri(&endpoint, controller_uri);
    cubicle_error_t error;
    memset(&error, 0, sizeof(error));
    assert(transport->vtable->connect(transport, &endpoint, &error) == CUBICLE_OK);

    // Endpoint test for controller.status
    send_controller_request(transport, "controller.status", "{}");
    // Endpoint test for controller.read
    send_controller_request(transport, "controller.read",
                            "{\"stream\":\"stdout\",\"offset\":0,\"maximum_length\":5}");
    // Endpoint test for controller.write
    send_controller_request(transport, "controller.write",
                            "{\"stream\":\"stdin\",\"data\":\"hello\"}");
    // Endpoint test for controller.resize
    send_controller_request(transport, "controller.resize",
                            "{\"rows\":40,\"cols\":120}");
    // Endpoint test for controller.detach
    send_controller_request(transport, "controller.detach", "{}");
    transport->vtable->destroy(transport);
    expect_server_exit(controller_pid);

    const char *const expected_controller_methods[] = {
        "\"method\":\"controller.status\"",
        "\"method\":\"controller.read\"",
        "\"method\":\"controller.write\"",
        "\"method\":\"controller.resize\"",
        "\"method\":\"controller.detach\"",
    };
    assert_log_contains_all(controller_log, expected_controller_methods,
                            sizeof(expected_controller_methods) /
                                sizeof(expected_controller_methods[0]));
}

static void run_error_scenario(const char *directory, test_transport_kind_t kind,
                               const char *scenario,
                               cubicle_error_code_t expected)
{
    char log_path[256];
    char manager_uri[320];
    char label[128];
    snprintf(label, sizeof(label), "%s-manager", scenario);
    snprintf(log_path, sizeof(log_path), "%s/%s-%s.log", directory, scenario,
             transport_name(kind));

    pid_t server_pid = start_server(kind, directory, label, log_path,
                                    "manager", scenario,
                                    "unix:///unused.sock", 1, manager_uri,
                                    sizeof(manager_uri));
    cubicle_client_t *client = connect_client(kind, manager_uri);
    cubicle_manager_ping_result_t ping;
    // Endpoint test for manager.ping error handling
    assert(cubicle_manager_ping(client, &ping) == expected);
    const cubicle_error_t *error = cubicle_client_last_error(client);
    assert(error != NULL && error->code == expected);
    cubicle_client_disconnect(client);
    expect_server_exit(server_pid);
    assert_log_contains(log_path, "\"method\":\"manager.ping\"");
}

static void run_transport_suite(const char *directory,
                                test_transport_kind_t kind)
{
    run_manager_integration(directory, kind);
    run_controller_integration(directory, kind);
    run_error_scenario(directory, kind, "error", CUBICLE_ERR_UNSUPPORTED);
    run_error_scenario(directory, kind, "malformed", CUBICLE_ERR_PROTOCOL);
    run_error_scenario(directory, kind, "mismatch", CUBICLE_ERR_PROTOCOL);
}

int main(int argc, char **argv)
{
    const char *selector = "all";
    if (argc > 2) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 2) {
        selector = argv[1];
    }
    if (strcmp(selector, "all") != 0 && strcmp(selector, "unix") != 0 &&
        strcmp(selector, "tcp") != 0) {
        usage(argv[0]);
        return 2;
    }

    char directory_template[] = "/tmp/libcubicle-mock-test-XXXXXX";
    char *directory = mkdtemp(directory_template);
    assert(directory != NULL);

    if (strcmp(selector, "all") == 0 || strcmp(selector, "unix") == 0) {
        run_transport_suite(directory, TEST_TRANSPORT_UNIX);
    }
    if (strcmp(selector, "all") == 0 || strcmp(selector, "tcp") == 0) {
        run_transport_suite(directory, TEST_TRANSPORT_TCP);
    }

    rmdir(directory);
    return 0;
}
