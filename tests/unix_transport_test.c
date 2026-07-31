#define _POSIX_C_SOURCE 200809L

#include "cubicle/client_types.h"
#include "cubicle/transport.h"
#include "cubicle/transport_unix.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_all(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = read(fd, cursor, length);
        if (result < 0) { if (errno == EINTR) continue; return -1; }
        if (result == 0) return -1;
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
        if (result < 0) { if (errno == EINTR) continue; return -1; }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static void run_server(const char *path, int ready_fd)
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
    assert(listen(listen_fd, 1) == 0);
    assert(write_all(ready_fd, "R", 1) == 0);
    close(ready_fd);

    int client_fd = accept(listen_fd, NULL, NULL);
    assert(client_fd >= 0);
    uint32_t size_network = 0;
    assert(read_all(client_fd, &size_network, sizeof(size_network)) == 0);
    uint32_t size = ntohl(size_network);
    assert(size == 4);
    char request[4];
    assert(read_all(client_fd, request, sizeof(request)) == 0);
    assert(memcmp(request, "ping", 4) == 0);
    const char response[] = "pong";
    size_network = htonl((uint32_t)(sizeof(response) - 1));
    assert(write_all(client_fd, &size_network, sizeof(size_network)) == 0);
    assert(write_all(client_fd, response, sizeof(response) - 1) == 0);
    close(client_fd);
    close(listen_fd);
    unlink(path);
    _exit(0);
}

int main(void)
{
    char directory_template[] = "/tmp/libcubicle-unix-test-XXXXXX";
    char *directory = mkdtemp(directory_template);
    assert(directory != NULL);
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int length = snprintf(socket_path, sizeof(socket_path), "%s/manager.sock", directory);
    assert(length > 0 && (size_t)length < sizeof(socket_path));

    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);
    pid_t server_pid = fork();
    assert(server_pid >= 0);
    if (server_pid == 0) { close(ready_pipe[0]); run_server(socket_path, ready_pipe[1]); }
    close(ready_pipe[1]);
    char ready = 0;
    assert(read_all(ready_pipe[0], &ready, 1) == 0 && ready == 'R');
    close(ready_pipe[0]);

    cubicle_transport_t *transport = NULL;
    assert(cubicle_transport_unix_create(&transport) == CUBICLE_OK);
    cubicle_endpoint_t endpoint;
    memset(&endpoint, 0, sizeof(endpoint));
    length = snprintf(endpoint.uri, sizeof(endpoint.uri), "unix://%s", socket_path);
    assert(length > 0 && (size_t)length < sizeof(endpoint.uri));

    cubicle_error_t error;
    memset(&error, 0, sizeof(error));
    assert(transport->vtable->connect(transport, &endpoint, &error) == CUBICLE_OK);
    void *response = NULL;
    size_t response_length = 0;
    assert(transport->vtable->request(transport, "ping", 4, &response, &response_length, &error) == CUBICLE_OK);
    assert(response_length == 4 && memcmp(response, "pong", 4) == 0);
    transport->vtable->response_free(transport, response);
    transport->vtable->destroy(transport);

    int status = 0;
    assert(waitpid(server_pid, &status, 0) == server_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(rmdir(directory) == 0);
    puts("unix transport test passed");
    return 0;
}
