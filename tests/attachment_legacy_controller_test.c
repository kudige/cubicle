#define _POSIX_C_SOURCE 200809L

#include "cubicle/cubicle.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_all(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = read(fd, cursor, length);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            return -1;
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = write(fd, cursor, length);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static void request_id_from_json(const char *request, char *buffer,
                                 size_t buffer_size)
{
    const char *key = "\"request_id\":\"";
    const char *start = strstr(request, key);
    assert(start != NULL);
    start += strlen(key);
    const char *end = strchr(start, '"');
    assert(end != NULL && (size_t)(end - start) < buffer_size);
    memcpy(buffer, start, (size_t)(end - start));
    buffer[end - start] = '\0';
}

static void respond(int fd, const char *request)
{
    char request_id[64];
    request_id_from_json(request, request_id, sizeof(request_id));

    const char *result = "{}";
    if (strstr(request, "\"method\":\"controller.attach\"") != NULL) {
        result = "{\"accepted_channels\":2,\"stdout_offset\":0,"
                 "\"stderr_offset\":0,\"tty_offset\":0}";
    } else if (strstr(request, "\"method\":\"controller.read\"") != NULL) {
        result = "{\"stream\":\"stdout\",\"offset\":0,\"next_offset\":5,"
                 "\"end_of_stream\":false,\"data\":\"hello\"}";
    } else {
        assert(!"unexpected controller request");
    }

    char response[512];
    int length = snprintf(
        response, sizeof(response),
        "{\"request_id\":\"%s\",\"success\":true,\"result\":%s}",
        request_id, result);
    assert(length > 0 && (size_t)length < sizeof(response));
    uint32_t response_size = htonl((uint32_t)length);
    assert(write_all(fd, &response_size, sizeof(response_size)) == 0);
    assert(write_all(fd, response, (size_t)length) == 0);
}

static void run_legacy_controller(const char *path, int ready_fd)
{
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listen_fd >= 0);

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    assert(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    unlink(path);
    assert(bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listen_fd, 4) == 0);
    assert(write_all(ready_fd, "R", 1) == 0);
    close(ready_fd);

    for (int i = 0; i < 3; ++i) {
        int client_fd = accept(listen_fd, NULL, NULL);
        assert(client_fd >= 0);

        uint32_t size_network = 0;
        assert(read_all(client_fd, &size_network, sizeof(size_network)) == 0);
        uint32_t size = ntohl(size_network);
        assert(size > 0 && size < 4096);
        char request[4096];
        assert(read_all(client_fd, request, size) == 0);
        request[size] = '\0';
        respond(client_fd, request);
        close(client_fd);
    }

    close(listen_fd);
    unlink(path);
    _exit(0);
}

static void make_endpoint(cubicle_endpoint_t *endpoint, const char *socket_path)
{
    memset(endpoint, 0, sizeof(*endpoint));
    int length = snprintf(endpoint->uri, sizeof(endpoint->uri), "unix://%s",
                          socket_path);
    assert(length > 0 && (size_t)length < sizeof(endpoint->uri));
}

int main(void)
{
    char directory_template[] = "/tmp/cubicle-legacy-controller-XXXXXX";
    char *directory = mkdtemp(directory_template);
    assert(directory != NULL);

    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int length = snprintf(socket_path, sizeof(socket_path),
                          "%s/control.sock", directory);
    assert(length > 0 && (size_t)length < sizeof(socket_path));

    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);
    pid_t server_pid = fork();
    assert(server_pid >= 0);
    if (server_pid == 0) {
        close(ready_pipe[0]);
        run_legacy_controller(socket_path, ready_pipe[1]);
    }
    close(ready_pipe[1]);
    char ready = 0;
    assert(read_all(ready_pipe[0], &ready, 1) == 0 && ready == 'R');
    close(ready_pipe[0]);

    cubicle_attachment_grant_t grant;
    memset(&grant, 0, sizeof(grant));
    snprintf(grant.grant_id, sizeof(grant.grant_id),
             "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    snprintf(grant.token, sizeof(grant.token), "local:test:process");
    make_endpoint(&grant.endpoint, socket_path);
    grant.granted_channels = CUBICLE_CHANNEL_STDOUT;
    grant.mode = CUBICLE_ATTACHMENT_OBSERVER;

    cubicle_attachment_t *attachment = NULL;
    assert(cubicle_attachment_connect(&grant, NULL, &attachment) ==
           CUBICLE_OK);

    char buffer[16];
    bool end_of_stream = true;
    assert(cubicle_attachment_read_stream(
               attachment, CUBICLE_STREAM_STDOUT, buffer, sizeof(buffer),
               &end_of_stream) == 5);
    assert(memcmp(buffer, "hello", 5) == 0);
    assert(!end_of_stream);
    cubicle_attachment_disconnect(attachment);

    int status = 0;
    assert(waitpid(server_pid, &status, 0) == server_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(rmdir(directory) == 0);
    return 0;
}
