#define _POSIX_C_SOURCE 200809L

#include "cubicle/cubicle.h"
#include "cubicle/transport_unix.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char temp_dir[256];
static pid_t manager_pid = -1;

static void join_path(char *buffer, size_t size, const char *a, const char *b)
{
    int length = snprintf(buffer, size, "%s/%s", a, b);
    assert(length > 0 && (size_t)length < size);
}

static void wait_for_socket(const char *path)
{
    for (int i = 0; i < 100; ++i) {
        struct stat status;
        if (stat(path, &status) == 0 && S_ISSOCK(status.st_mode)) {
            return;
        }
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000L };
        nanosleep(&delay, NULL);
    }
    assert(!"manager socket was not created");
}

static void run_manager_capture(const char *manager, const char *state_dir,
                                char *const extra_args[], char *output,
                                size_t output_size)
{
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[1]);

        size_t extra_count = 0;
        while (extra_args[extra_count] != NULL) {
            ++extra_count;
        }
        char **argv = calloc(extra_count + 4, sizeof(*argv));
        if (argv == NULL) {
            _exit(127);
        }
        argv[0] = (char *)manager;
        argv[1] = "--state-dir";
        argv[2] = (char *)state_dir;
        for (size_t i = 0; i < extra_count; ++i) {
            argv[i + 3] = extra_args[i];
        }
        argv[extra_count + 3] = NULL;
        execv(manager, argv);
        _exit(127);
    }

    close(pipe_fds[1]);
    size_t used = 0;
    while (used + 1 < output_size) {
        ssize_t nread = read(pipe_fds[0], output + used,
                             output_size - used - 1);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            assert(!"failed to read manager command output");
        }
        if (nread == 0) {
            break;
        }
        used += (size_t)nread;
    }
    output[used] = '\0';
    close(pipe_fds[0]);

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static void parse_between(char *output, const char *prefix,
                          const char *suffix, char *value, size_t value_size)
{
    char *start = strstr(output, prefix);
    assert(start != NULL);
    start += strlen(prefix);
    char *end = suffix == NULL ? strchr(start, '\n') : strstr(start, suffix);
    if (end == NULL) {
        end = start + strlen(start);
    }
    size_t length = (size_t)(end - start);
    assert(length > 0 && length < value_size);
    memcpy(value, start, length);
    value[length] = '\0';
}

static void write_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(content, file) >= 0);
    assert(fclose(file) == 0);
}

static cubicle_error_code_t reconcile_with_retry(cubicle_client_t *client)
{
    cubicle_error_code_t result = CUBICLE_ERR_INTERNAL;
    for (int attempt = 0; attempt < 250; ++attempt) {
        result = cubicle_manager_reconcile(client);
        if (result == CUBICLE_OK) {
            return result;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
        nanosleep(&delay, NULL);
    }
    return result;
}

static cubicle_error_code_t process_wait_with_retry(cubicle_client_t *client,
                                                    const char *process_id,
                                                    uint64_t timeout_ms,
                                                    cubicle_process_info_t *process)
{
    cubicle_error_code_t result = CUBICLE_ERR_INTERNAL;
    for (int attempt = 0; attempt < 250; ++attempt) {
        result = cubicle_process_wait(client, process_id, timeout_ms, process);
        if (result == CUBICLE_OK &&
            (process->state == CUBICLE_PROCESS_COMPLETED ||
             process->state == CUBICLE_PROCESS_FAILED ||
             process->state == CUBICLE_PROCESS_LOST)) {
            return result;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
        nanosleep(&delay, NULL);
    }
    const cubicle_error_t *error = cubicle_client_last_error(client);
    fprintf(stderr, "process_wait_with_retry failed for %s: code=%d state=%d error=%s\n",
            process_id, result, process->state,
            error == NULL ? "" : error->message);
    return result;
}

static cubicle_client_t *connect_client(const char *socket_path)
{
    cubicle_client_t *client = NULL;
    // Endpoint test for cubicle_client_connect_uri
    assert(cubicle_client_connect_uri(socket_path, NULL, &client) ==
           CUBICLE_OK);
    return client;
}

static void write_all(int fd, const void *buffer, size_t length)
{
    const char *cursor = buffer;
    while (length > 0) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            assert(!"failed to write socket");
        }
        assert(written > 0);
        cursor += written;
        length -= (size_t)written;
    }
}

static void read_all(int fd, void *buffer, size_t length)
{
    char *cursor = buffer;
    while (length > 0) {
        ssize_t nread = read(fd, cursor, length);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            assert(!"failed to read socket");
        }
        assert(nread > 0);
        cursor += nread;
        length -= (size_t)nread;
    }
}

static void raw_api_request(const char *socket_path, const char *json,
                            char *response, size_t response_size)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    int length = snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                          socket_path);
    assert(length > 0 && (size_t)length < sizeof(address.sun_path));
    assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);

    size_t request_length = strlen(json);
    assert(request_length < UINT32_MAX);
    uint32_t frame_length = htonl((uint32_t)request_length);
    write_all(fd, &frame_length, sizeof(frame_length));
    write_all(fd, json, request_length);

    uint32_t response_length_network = 0;
    read_all(fd, &response_length_network, sizeof(response_length_network));
    uint32_t response_length = ntohl(response_length_network);
    assert(response_length > 0 && response_length < response_size);
    read_all(fd, response, response_length);
    response[response_length] = '\0';
    close(fd);
}

static void expect_invalid_argument_response(const char *socket_path,
                                             const char *json)
{
    char response[4096];
    raw_api_request(socket_path, json, response, sizeof(response));
    assert(strstr(response, "\"success\":false") != NULL);
    assert(strstr(response, "\"code\":\"invalid_argument\"") != NULL);
}

static void cleanup(void)
{
    if (manager_pid > 0) {
        kill(manager_pid, SIGTERM);
        waitpid(manager_pid, NULL, 0);
    }
    if (temp_dir[0] != '\0') {
        char command[512];
        snprintf(command, sizeof(command), "rm -rf '%s'", temp_dir);
        (void)system(command);
    }
}

int main(void)
{
    const char *manager = getenv("CUBICLE_MANAGER");
    const char *controller = getenv("CUBICLE_CONTROLLER");
    assert(manager != NULL && manager[0] != '\0');
    assert(controller != NULL && controller[0] != '\0');
    atexit(cleanup);

    snprintf(temp_dir, sizeof(temp_dir), "/tmp/libcubicle-real-manager-XXXXXX");
    assert(mkdtemp(temp_dir) != NULL);
    assert(setenv("XDG_RUNTIME_DIR", temp_dir, 1) == 0);

    char state_dir[256];
    char socket_path[256];
    char controller_socket[256];
    join_path(state_dir, sizeof(state_dir), temp_dir, "state");
    join_path(socket_path, sizeof(socket_path), temp_dir, "manager.sock");
    join_path(controller_socket, sizeof(controller_socket), temp_dir,
              "controller.sock");

    char output[1024];
    char *workspace_args[] = { "workspace", "create", "Project A", NULL };
    run_manager_capture(manager, state_dir, workspace_args, output,
                        sizeof(output));
    char workspace_id[64];
    parse_between(output, "workspace id=", " name=", workspace_id,
                  sizeof(workspace_id));

    char *process_args[] = {
        "process", "register",
        "--workspace", workspace_id,
        "--friendly-name", "daemon-1",
        "--mode", "stream",
        "--controller-id", "controller-1",
        "--control-socket", controller_socket,
        NULL,
    };
    run_manager_capture(manager, state_dir, process_args, output,
                        sizeof(output));
    char process_id[64];
    parse_between(output, "process id=", " workspace_id=", process_id,
                  sizeof(process_id));

    char controller_dir[512];
    snprintf(controller_dir, sizeof(controller_dir), "%s/controllers/%s",
             state_dir, process_id);
    assert(mkdir(state_dir, 0777) == 0 || errno == EEXIST);
    char controllers_dir[512];
    join_path(controllers_dir, sizeof(controllers_dir), state_dir,
              "controllers");
    assert(mkdir(controllers_dir, 0777) == 0 || errno == EEXIST);
    assert(mkdir(controller_dir, 0777) == 0);

    char stdout_path[512];
    char stderr_path[512];
    char events_path[512];
    join_path(stdout_path, sizeof(stdout_path), controller_dir, "stdout.log");
    join_path(stderr_path, sizeof(stderr_path), controller_dir, "stderr.log");
    join_path(events_path, sizeof(events_path), controller_dir, "events.log");
    write_file(stdout_path, "hello\n");
    write_file(stderr_path, "error\n");
    write_file(events_path,
               "seq=1 type=process_started controller_id=controller-1 pid=1 pgid=1 mode=stream\n"
               "seq=2 type=output stream=stdout start=0 length=6\n"
               "seq=3 type=process_exited status=exited exit_code=0\n");

    manager_pid = fork();
    assert(manager_pid >= 0);
    if (manager_pid == 0) {
        execl(manager, manager, "--state-dir", state_dir,
              "--controller-bin", controller, "daemon", "--foreground",
              "--control-socket", socket_path, "--event-interval-ms", "50",
              (char *)NULL);
        _exit(127);
    }
    wait_for_socket(socket_path);

    cubicle_client_t *client = connect_client(socket_path);

    cubicle_manager_ping_result_t ping;
    memset(&ping, 0, sizeof(ping));
    // Endpoint test for manager.ping
    assert(cubicle_manager_ping(client, &ping) == CUBICLE_OK);
    assert(strcmp(ping.manager_id, "") != 0);

    cubicle_manager_status_t status;
    memset(&status, 0, sizeof(status));
    // Endpoint test for manager.status
    assert(cubicle_manager_status(client, &status) == CUBICLE_OK);
    assert(status.workspace_count == 1);
    assert(status.process_count == 1);

    cubicle_workspace_create_options_t create;
    memset(&create, 0, sizeof(create));
    create.name = "Project B";
    cubicle_workspace_info_t workspace;
    memset(&workspace, 0, sizeof(workspace));
    // Endpoint test for workspace.create
    assert(cubicle_workspace_create(client, &create, &workspace) ==
           CUBICLE_OK);
    assert(strcmp(workspace.name, "Project B") == 0);

    memset(&workspace, 0, sizeof(workspace));
    // Endpoint test for workspace.get
    assert(cubicle_workspace_get(client, "Project B", &workspace) ==
           CUBICLE_OK);
    assert(strcmp(workspace.name, "Project B") == 0);

    cubicle_workspace_info_t *workspaces = NULL;
    size_t workspace_count = 0;
    // Endpoint test for workspace.list
    assert(cubicle_workspace_list(client, NULL, &workspaces,
                                  &workspace_count, NULL) == CUBICLE_OK);
    assert(workspace_count == 2);
    cubicle_workspace_list_free(workspaces);

    // Endpoint test for workspace.rename
    assert(cubicle_workspace_rename(client, workspace.id, "Project C",
                                    NULL) == CUBICLE_OK);
    memset(&workspace, 0, sizeof(workspace));
    assert(cubicle_workspace_get(client, "Project C", &workspace) ==
           CUBICLE_OK);
    assert(strcmp(workspace.name, "Project C") == 0);

    unsigned char owner_key[] = { 0xab, 0xcd, 0xef };
    cubicle_workspace_key_info_t key;
    memset(&key, 0, sizeof(key));
    // Endpoint test for workspace.key.add
    assert(cubicle_workspace_key_add(client, workspace_id, owner_key,
                                     sizeof(owner_key), "owner",
                                     CUBICLE_CAP_WORKSPACE_READ |
                                         CUBICLE_CAP_PROCESS_READ,
                                     &key) == CUBICLE_OK);
    assert(strcmp(key.label, "owner") == 0);

    // Endpoint test for workspace.key.update
    assert(cubicle_workspace_key_set_capabilities(
               client, workspace_id, key.key_id,
               CUBICLE_CAP_WORKSPACE_READ | CUBICLE_CAP_PROCESS_START) ==
           CUBICLE_OK);

    cubicle_workspace_key_info_t *keys = NULL;
    size_t key_count = 0;
    // Endpoint test for workspace.key.list
    assert(cubicle_workspace_key_list(client, workspace_id, &keys,
                                      &key_count) == CUBICLE_OK);
    assert(key_count == 1);
    assert(keys[0].capabilities ==
           (CUBICLE_CAP_WORKSPACE_READ | CUBICLE_CAP_PROCESS_START));
    cubicle_workspace_key_list_free(keys);

    // Endpoint test for workspace.key.revoke
    assert(cubicle_workspace_key_revoke(client, workspace_id, key.key_id) ==
           CUBICLE_OK);

    const char *started_argv[] = {
        "sh",
        "-c",
        "trap 'echo gotusr1' USR1; while true; do sleep 1; done",
    };
    cubicle_process_start_options_t start_options;
    memset(&start_options, 0, sizeof(start_options));
    start_options.workspace_id = workspace_id;
    start_options.friendly_name = "api-started";
    start_options.mode = CUBICLE_PROCESS_STREAM;
    start_options.stdin_policy = CUBICLE_STDIN_EOF;
    start_options.argv = started_argv;
    start_options.argc = sizeof(started_argv) / sizeof(started_argv[0]);
    cubicle_process_info_t started;
    memset(&started, 0, sizeof(started));
    // Endpoint test for process.start
    assert(cubicle_process_start(client, &start_options, &started) ==
           CUBICLE_OK);
    assert(strcmp(started.friendly_name, "api-started") == 0);

    cubicle_process_update_options_t update_options;
    memset(&update_options, 0, sizeof(update_options));
    update_options.workspace_id = workspace_id;
    update_options.friendly_name = "api-updated";
    update_options.has_restart = true;
    update_options.restart = true;
    cubicle_process_info_t updated;
    memset(&updated, 0, sizeof(updated));
    // Endpoint test for process.update
    assert(cubicle_process_update(client, started.id, &update_options,
                                  &updated) == CUBICLE_OK);
    assert(strcmp(updated.friendly_name, "api-updated") == 0);
    assert(updated.restart);
    assert(cubicle_process_get(client, "api-updated", workspace_id,
                               &started) == CUBICLE_OK);
    assert(strcmp(started.id, updated.id) == 0);

    // Endpoint test for process.signal
    assert(cubicle_process_signal(client, started.id, SIGUSR1) ==
           CUBICLE_OK);

    cubicle_attachment_request_t running_attachment_request;
    memset(&running_attachment_request, 0, sizeof(running_attachment_request));
    running_attachment_request.process_id = started.id;
    running_attachment_request.channels = CUBICLE_CHANNEL_STDOUT;
    running_attachment_request.mode = CUBICLE_ATTACHMENT_OBSERVER;
    cubicle_attachment_grant_t running_grant;
    memset(&running_grant, 0, sizeof(running_grant));
    // Endpoint test for attachment.request
    assert(cubicle_attachment_request(client, &running_attachment_request,
                                      &running_grant) == CUBICLE_OK);

    cubicle_attachment_t *attachment = NULL;
    cubicle_attachment_options_t attachment_options;
    memset(&attachment_options, 0, sizeof(attachment_options));
    // Endpoint test for controller.attach
    assert(cubicle_attachment_connect(&running_grant, &attachment_options,
                                      &attachment) == CUBICLE_OK);
    assert(attachment != NULL);

    cubicle_attachment_status_t attachment_status;
    memset(&attachment_status, 0, sizeof(attachment_status));
    // Endpoint test for controller.status
    assert(cubicle_attachment_status(attachment, &attachment_status) ==
           CUBICLE_OK);
    assert(attachment_status.state == CUBICLE_PROCESS_RUNNING);

    char attachment_buffer[32];
    bool end_of_stream = false;
    // Endpoint test for controller.read
    assert(cubicle_attachment_read_stream(
               attachment, CUBICLE_STREAM_STDOUT, attachment_buffer,
               sizeof(attachment_buffer), &end_of_stream) >= 0);

    // Endpoint test for controller.detach
    assert(cubicle_attachment_detach(attachment) == CUBICLE_OK);
    cubicle_attachment_disconnect(attachment);

    // Endpoint test for process.terminate
    assert(cubicle_process_terminate(client, started.id, NULL) ==
           CUBICLE_OK);

    // Endpoint test for manager.reconcile
    assert(reconcile_with_retry(client) == CUBICLE_OK);

    cubicle_process_info_t process;
    memset(&process, 0, sizeof(process));
    // Endpoint test for process.get by process ID
    assert(cubicle_process_get(client, process_id, NULL, &process) ==
           CUBICLE_OK);
    assert(strcmp(process.id, process_id) == 0);

    memset(&process, 0, sizeof(process));
    // Endpoint test for process.get by workspace-local friendly name
    assert(cubicle_process_get(client, "daemon-1", workspace_id, &process) ==
           CUBICLE_OK);
    assert(strcmp(process.id, process_id) == 0);

    const char *other_argv[] = {"sh", "-c", "exit 0"};
    cubicle_process_start_options_t other_start_options;
    memset(&other_start_options, 0, sizeof(other_start_options));
    other_start_options.workspace_id = workspace.id;
    other_start_options.friendly_name = "other-workspace";
    other_start_options.mode = CUBICLE_PROCESS_STREAM;
    other_start_options.stdin_policy = CUBICLE_STDIN_EOF;
    other_start_options.argv = other_argv;
    other_start_options.argc = sizeof(other_argv) / sizeof(other_argv[0]);
    cubicle_process_info_t other_started;
    memset(&other_started, 0, sizeof(other_started));
    // Endpoint test for process.start in a second workspace
    assert(cubicle_process_start(client, &other_start_options,
                                 &other_started) == CUBICLE_OK);
    assert(strcmp(other_started.workspace_id, workspace.id) == 0);
    assert(reconcile_with_retry(client) == CUBICLE_OK);

    cubicle_process_info_t *processes = NULL;
    size_t process_count = 0;
    cubicle_process_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.workspace_id = workspace_id;
    // Endpoint test for process.list
    assert(cubicle_process_list(client, &filter, &processes,
                                &process_count, NULL) == CUBICLE_OK);
    assert(process_count == 2);
    for (size_t i = 0; i < process_count; ++i) {
        assert(strcmp(processes[i].workspace_id, workspace_id) == 0);
    }
    cubicle_process_list_free(processes);

    processes = NULL;
    process_count = 0;
    memset(&filter, 0, sizeof(filter));
    filter.workspace_id = workspace.id;
    // Endpoint test for process.list scoped to a second workspace
    assert(cubicle_process_list(client, &filter, &processes,
                                &process_count, NULL) == CUBICLE_OK);
    assert(process_count == 1);
    assert(strcmp(processes[0].workspace_id, workspace.id) == 0);
    assert(strcmp(processes[0].friendly_name, "other-workspace") == 0);
    cubicle_process_list_free(processes);

    cubicle_output_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    // Endpoint test for process.read_output
    assert(cubicle_process_read_output(client, process_id,
                                       CUBICLE_STREAM_STDOUT, 0, 16,
                                       &chunk) == CUBICLE_OK);
    assert(chunk.length == 6);
    assert(memcmp(chunk.data, "hello\n", 6) == 0);
    cubicle_output_chunk_free(&chunk);

    cubicle_attachment_request_t attachment_request;
    memset(&attachment_request, 0, sizeof(attachment_request));
    attachment_request.process_id = process_id;
    attachment_request.channels = CUBICLE_CHANNEL_STDOUT;
    attachment_request.mode = CUBICLE_ATTACHMENT_OBSERVER;
    cubicle_attachment_grant_t grant;
    memset(&grant, 0, sizeof(grant));
    // Endpoint test for attachment.request
    assert(cubicle_attachment_request(client, &attachment_request, &grant) ==
           CUBICLE_OK);
    assert(strcmp(grant.process_id, process_id) == 0);
    assert((grant.granted_channels & CUBICLE_CHANNEL_STDOUT) != 0);
    assert(strncmp(grant.endpoint.uri, "unix://", 7) == 0);

    cubicle_event_query_t query;
    memset(&query, 0, sizeof(query));
    query.workspace_id = workspace_id;
    query.process_id = process_id;
    query.limit = 10;

    cubicle_event_t *events = NULL;
    size_t event_count = 0;
    for (int i = 0; i < 100; ++i) {
        // Endpoint test for events.list
        if (cubicle_events_list(client, &query, &events, &event_count) ==
                CUBICLE_OK &&
            event_count == 3) {
            break;
        }
        free(events);
        events = NULL;
        event_count = 0;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000L };
        nanosleep(&delay, NULL);
    }
    assert(event_count == 3);
    cubicle_events_free(events);

    cubicle_event_subscription_t *subscription = NULL;
    query.limit = 1;
    // Endpoint test for events.subscribe
    assert(cubicle_events_subscribe(client, &query, &subscription) ==
           CUBICLE_OK);
    cubicle_event_t event;
    memset(&event, 0, sizeof(event));
    assert(cubicle_events_next(subscription, 1000, &event) == CUBICLE_OK);
    assert(event.global_sequence > 0);
    assert(strcmp(event.process_id, process_id) == 0);
    cubicle_events_unsubscribe(subscription);

    cubicle_process_info_t waited;
    memset(&waited, 0, sizeof(waited));
    // Endpoint test for process.wait
    assert(process_wait_with_retry(client, started.id, 5000, &waited) ==
           CUBICLE_OK);
    assert(strcmp(waited.id, started.id) == 0);
    assert(waited.state == CUBICLE_PROCESS_COMPLETED);

    // Endpoint test for process.remove
    assert(cubicle_process_remove(client, started.id) == CUBICLE_OK);

    const char *kill_argv[] = {
        "sh",
        "-c",
        "while true; do sleep 1; done",
    };
    cubicle_process_start_options_t kill_start_options;
    memset(&kill_start_options, 0, sizeof(kill_start_options));
    kill_start_options.workspace_id = workspace_id;
    kill_start_options.friendly_name = "api-killed";
    kill_start_options.mode = CUBICLE_PROCESS_STREAM;
    kill_start_options.stdin_policy = CUBICLE_STDIN_EOF;
    kill_start_options.argv = kill_argv;
    kill_start_options.argc = sizeof(kill_argv) / sizeof(kill_argv[0]);
    cubicle_process_info_t killed;
    memset(&killed, 0, sizeof(killed));
    assert(cubicle_process_start(client, &kill_start_options, &killed) ==
           CUBICLE_OK);

    // Endpoint test for process.kill
    assert(cubicle_process_kill(client, killed.id) == CUBICLE_OK);
    memset(&waited, 0, sizeof(waited));
    assert(process_wait_with_retry(client, killed.id, 5000, &waited) ==
           CUBICLE_OK);
    assert(waited.state == CUBICLE_PROCESS_COMPLETED);
    assert(cubicle_process_remove(client, killed.id) == CUBICLE_OK);

    cubicle_workspace_create_options_t cleanup_workspace_create;
    memset(&cleanup_workspace_create, 0, sizeof(cleanup_workspace_create));
    cleanup_workspace_create.name = "Cleanup Workspace";
    cubicle_workspace_info_t cleanup_workspace;
    memset(&cleanup_workspace, 0, sizeof(cleanup_workspace));
    assert(cubicle_workspace_create(client, &cleanup_workspace_create,
                                    &cleanup_workspace) == CUBICLE_OK);

    const char *completed_cleanup_argv[] = {"/bin/true"};
    cubicle_process_start_options_t cleanup_completed_options;
    memset(&cleanup_completed_options, 0, sizeof(cleanup_completed_options));
    cleanup_completed_options.workspace_id = cleanup_workspace.id;
    cleanup_completed_options.friendly_name = "api-cleanup-completed";
    cleanup_completed_options.mode = CUBICLE_PROCESS_STREAM;
    cleanup_completed_options.stdin_policy = CUBICLE_STDIN_EOF;
    cleanup_completed_options.argv = completed_cleanup_argv;
    cleanup_completed_options.argc = 1;
    cubicle_process_info_t cleanup_completed;
    memset(&cleanup_completed, 0, sizeof(cleanup_completed));
    assert(cubicle_process_start(client, &cleanup_completed_options,
                                 &cleanup_completed) == CUBICLE_OK);
    memset(&waited, 0, sizeof(waited));
    assert(process_wait_with_retry(client, cleanup_completed.id, 5000,
                                &waited) == CUBICLE_OK);

    cubicle_process_start_options_t cleanup_live_options;
    memset(&cleanup_live_options, 0, sizeof(cleanup_live_options));
    cleanup_live_options.workspace_id = cleanup_workspace.id;
    cleanup_live_options.friendly_name = "api-cleanup-live";
    cleanup_live_options.mode = CUBICLE_PROCESS_STREAM;
    cleanup_live_options.stdin_policy = CUBICLE_STDIN_EOF;
    cleanup_live_options.argv = kill_argv;
    cleanup_live_options.argc = sizeof(kill_argv) / sizeof(kill_argv[0]);
    cubicle_process_info_t cleanup_live;
    memset(&cleanup_live, 0, sizeof(cleanup_live));
    assert(cubicle_process_start(client, &cleanup_live_options,
                                 &cleanup_live) == CUBICLE_OK);

    cubicle_manager_cleanup_result_t cleanup_result;
    memset(&cleanup_result, 0, sizeof(cleanup_result));
    // Endpoint test for manager.cleanup
    assert(cubicle_manager_cleanup(client, cleanup_workspace.id,
                                   &cleanup_result) == CUBICLE_OK);
    assert(cleanup_result.removed_count == 1);
    assert(cleanup_result.skipped_live_count == 1);
    assert(cleanup_result.skipped_saved_count == 0);
    assert(cleanup_result.failed_count == 0);
    assert(cubicle_process_get(client, cleanup_completed.id, NULL,
                               &process) == CUBICLE_ERR_NOT_FOUND);
    memset(&process, 0, sizeof(process));
    assert(cubicle_process_get(client, cleanup_live.id, NULL,
                               &process) == CUBICLE_OK);
    assert(process.state == CUBICLE_PROCESS_RUNNING);

    assert(cubicle_process_kill(client, cleanup_live.id) == CUBICLE_OK);
    memset(&waited, 0, sizeof(waited));
    assert(process_wait_with_retry(client, cleanup_live.id, 5000,
                                &waited) == CUBICLE_OK);
    memset(&cleanup_result, 0, sizeof(cleanup_result));
    assert(cubicle_manager_cleanup(client, cleanup_workspace.id,
                                   &cleanup_result) == CUBICLE_OK);
    assert(cleanup_result.removed_count == 1);
    assert(cleanup_result.skipped_live_count == 0);
    assert(cleanup_result.skipped_saved_count == 0);

    cubicle_workspace_delete_options_t cleanup_delete_options;
    memset(&cleanup_delete_options, 0, sizeof(cleanup_delete_options));
    cleanup_delete_options.remove_retained_processes = true;
    assert(cubicle_workspace_delete(client, cleanup_workspace.id,
                                    &cleanup_delete_options) == CUBICLE_OK);

    cubicle_workspace_create_options_t stop_workspace_create;
    memset(&stop_workspace_create, 0, sizeof(stop_workspace_create));
    stop_workspace_create.name = "Stop Workspace";
    cubicle_workspace_info_t stop_workspace;
    memset(&stop_workspace, 0, sizeof(stop_workspace));
    assert(cubicle_workspace_create(client, &stop_workspace_create,
                                    &stop_workspace) == CUBICLE_OK);

    cubicle_process_start_options_t stop_start_options;
    memset(&stop_start_options, 0, sizeof(stop_start_options));
    stop_start_options.workspace_id = stop_workspace.id;
    stop_start_options.friendly_name = "api-stopped";
    stop_start_options.mode = CUBICLE_PROCESS_STREAM;
    stop_start_options.stdin_policy = CUBICLE_STDIN_EOF;
    stop_start_options.argv = kill_argv;
    stop_start_options.argc = sizeof(kill_argv) / sizeof(kill_argv[0]);
    cubicle_process_info_t stopped;
    memset(&stopped, 0, sizeof(stopped));
    assert(cubicle_process_start(client, &stop_start_options, &stopped) ==
           CUBICLE_OK);

    // Endpoint test for workspace.stop
    assert(cubicle_workspace_stop(client, stop_workspace.id, NULL) ==
           CUBICLE_OK);
    memset(&waited, 0, sizeof(waited));
    assert(process_wait_with_retry(client, stopped.id, 5000, &waited) ==
           CUBICLE_OK);
    assert(waited.state == CUBICLE_PROCESS_COMPLETED);
    assert(cubicle_process_remove(client, stopped.id) == CUBICLE_OK);

    cubicle_workspace_delete_options_t delete_options;
    memset(&delete_options, 0, sizeof(delete_options));
    delete_options.remove_retained_processes = true;
    // Endpoint test for workspace.delete
    assert(cubicle_workspace_delete(client, stop_workspace.id,
                                    &delete_options) == CUBICLE_OK);
    assert(cubicle_workspace_delete(client, workspace.id,
                                    &delete_options) == CUBICLE_OK);

    cubicle_client_disconnect(client);

    // Endpoint test for process.list invalid typed params
    expect_invalid_argument_response(
        socket_path,
        "{\"protocol_major\":0,\"protocol_minor\":1,\"request_id\":\"bad-1\","
        "\"session_id\":\"local-session\",\"method\":\"process.list\","
        "\"params\":{\"workspace_id\":7}}");

    // Endpoint test for events.list invalid typed params
    expect_invalid_argument_response(
        socket_path,
        "{\"protocol_major\":0,\"protocol_minor\":1,\"request_id\":\"bad-2\","
        "\"session_id\":\"local-session\",\"method\":\"events.list\","
        "\"params\":{\"limit\":\"many\"}}");

    // Endpoint test for process.read_output invalid typed params
    expect_invalid_argument_response(
        socket_path,
        "{\"protocol_major\":0,\"protocol_minor\":1,\"request_id\":\"bad-3\","
        "\"session_id\":\"local-session\",\"method\":\"process.read_output\","
        "\"params\":{\"process_id\":\"x\",\"stream\":\"stdout\","
        "\"offset\":0,\"maximum_length\":\"16\"}}");

    // Endpoint test for workspace.create invalid typed params
    expect_invalid_argument_response(
        socket_path,
        "{\"protocol_major\":0,\"protocol_minor\":1,\"request_id\":\"bad-4\","
        "\"session_id\":\"local-session\",\"method\":\"workspace.create\","
        "\"params\":{\"name\":null}}");

    client = connect_client(socket_path);
    memset(&status, 0, sizeof(status));
    assert(cubicle_manager_status(client, &status) == CUBICLE_OK);
    assert(status.active_client_sessions == 1);

    // Endpoint test for manager.shutdown
    assert(cubicle_manager_shutdown(client, false) == CUBICLE_OK);
    cubicle_client_disconnect(client);

    int status_code = 0;
    assert(waitpid(manager_pid, &status_code, 0) == manager_pid);
    manager_pid = -1;
    assert(WIFEXITED(status_code));
    assert(WEXITSTATUS(status_code) == 0);
    return 0;
}
