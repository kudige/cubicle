#define _POSIX_C_SOURCE 200809L

#include "cubicle/transport_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CUBICLE_TCP_MAX_FRAME (16U * 1024U * 1024U)
#define CUBICLE_TCP_URI_PREFIX "tcp://"

typedef struct cubicle_tcp_transport_context { int fd; } cubicle_tcp_transport_context_t;

static cubicle_error_code_t set_error(cubicle_error_t *error, cubicle_error_code_t code, int system_errno, const char *message)
{
    if (error != NULL) {
        error->code = code;
        error->system_errno = system_errno;
        error->retryable = (code == CUBICLE_ERR_MANAGER_UNAVAILABLE || code == CUBICLE_ERR_IO || code == CUBICLE_ERR_TIMEOUT);
        snprintf(error->message, sizeof(error->message), "%s", message == NULL ? "" : message);
    }
    return code;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = send(fd, cursor, length, MSG_NOSIGNAL);
        if (result < 0) { if (errno == EINTR) continue; return -1; }
        if (result == 0) { errno = EPIPE; return -1; }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = recv(fd, cursor, length, 0);
        if (result < 0) { if (errno == EINTR) continue; return -1; }
        if (result == 0) { errno = ECONNRESET; return -1; }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int split_tcp_uri(const char *uri, char *host, size_t host_size, char *port, size_t port_size)
{
    size_t prefix_length = strlen(CUBICLE_TCP_URI_PREFIX);
    if (uri == NULL || strncmp(uri, CUBICLE_TCP_URI_PREFIX, prefix_length) != 0) return -1;

    const char *authority = uri + prefix_length;
    const char *port_start = NULL;
    size_t host_length = 0;
    if (authority[0] == '[') {
        const char *end = strchr(authority, ']');
        if (end == NULL || end[1] != ':') return -1;
        host_length = (size_t)(end - authority - 1);
        authority += 1;
        port_start = end + 2;
    } else {
        const char *colon = strrchr(authority, ':');
        if (colon == NULL) return -1;
        host_length = (size_t)(colon - authority);
        port_start = colon + 1;
    }

    if (host_length == 0 || port_start == NULL || port_start[0] == '\0' ||
        host_length >= host_size || strlen(port_start) >= port_size) {
        return -1;
    }
    memcpy(host, authority, host_length);
    host[host_length] = '\0';
    strcpy(port, port_start);
    return 0;
}

static cubicle_error_code_t tcp_connect(cubicle_transport_t *transport, const cubicle_endpoint_t *endpoint, cubicle_error_t *error)
{
    if (transport == NULL || transport->context == NULL || endpoint == NULL) return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0, "invalid TCP transport endpoint");

    cubicle_tcp_transport_context_t *context = transport->context;
    if (context->fd >= 0) return set_error(error, CUBICLE_ERR_INVALID_STATE, 0, "TCP transport is already connected");

    char host[256];
    char port[32];
    if (split_tcp_uri(endpoint->uri, host, sizeof(host), port, sizeof(port)) < 0) {
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0, "endpoint URI must use tcp://host:port syntax");
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *addresses = NULL;
    int gai_result = getaddrinfo(host, port, &hints, &addresses);
    if (gai_result != 0) {
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0, "failed to resolve TCP endpoint");
    }

    int fd = -1;
    int saved_errno = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);

    if (fd < 0) return set_error(error, CUBICLE_ERR_MANAGER_UNAVAILABLE, saved_errno, "failed to connect to TCP endpoint");

    context->fd = fd;
    if (error != NULL) memset(error, 0, sizeof(*error));
    return CUBICLE_OK;
}

static cubicle_error_code_t tcp_request(cubicle_transport_t *transport, const void *request, size_t request_length, void **response_out, size_t *response_length_out, cubicle_error_t *error)
{
    if (transport == NULL || transport->context == NULL || (request == NULL && request_length != 0) || response_out == NULL || response_length_out == NULL || request_length > CUBICLE_TCP_MAX_FRAME)
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0, "invalid TCP transport request");
    cubicle_tcp_transport_context_t *context = transport->context;
    if (context->fd < 0) return set_error(error, CUBICLE_ERR_INVALID_STATE, 0, "TCP transport is not connected");

    uint32_t request_size = htonl((uint32_t)request_length);
    if (write_all(context->fd, &request_size, sizeof(request_size)) < 0 || (request_length > 0 && write_all(context->fd, request, request_length) < 0))
        return set_error(error, CUBICLE_ERR_IO, errno, "failed to write TCP transport request");

    uint32_t response_size_network = 0;
    if (read_all(context->fd, &response_size_network, sizeof(response_size_network)) < 0)
        return set_error(error, CUBICLE_ERR_IO, errno, "failed to read TCP transport response header");
    uint32_t response_size = ntohl(response_size_network);
    if (response_size > CUBICLE_TCP_MAX_FRAME) return set_error(error, CUBICLE_ERR_PROTOCOL, EMSGSIZE, "TCP transport response exceeds frame limit");

    void *response = NULL;
    if (response_size > 0) {
        response = malloc(response_size);
        if (response == NULL) return set_error(error, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to allocate response buffer");
        if (read_all(context->fd, response, response_size) < 0) {
            int saved_errno = errno;
            free(response);
            return set_error(error, CUBICLE_ERR_IO, saved_errno, "failed to read TCP transport response");
        }
    }
    *response_out = response;
    *response_length_out = response_size;
    if (error != NULL) memset(error, 0, sizeof(*error));
    return CUBICLE_OK;
}

static void tcp_response_free(cubicle_transport_t *transport, void *response) { (void)transport; free(response); }
static void tcp_close(cubicle_transport_t *transport) { if (transport && transport->context) { cubicle_tcp_transport_context_t *context = transport->context; if (context->fd >= 0) { close(context->fd); context->fd = -1; } } }
static void tcp_destroy(cubicle_transport_t *transport) { if (transport) { tcp_close(transport); free(transport->context); free(transport); } }

static const cubicle_transport_vtable_t TCP_TRANSPORT_VTABLE = { .connect = tcp_connect, .request = tcp_request, .response_free = tcp_response_free, .close = tcp_close, .destroy = tcp_destroy };

cubicle_error_code_t cubicle_transport_tcp_create(cubicle_transport_t **transport_out)
{
    if (transport_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_transport_t *transport = calloc(1, sizeof(*transport));
    cubicle_tcp_transport_context_t *context = calloc(1, sizeof(*context));
    if (transport == NULL || context == NULL) { free(context); free(transport); return CUBICLE_ERR_INTERNAL; }
    context->fd = -1;
    transport->vtable = &TCP_TRANSPORT_VTABLE;
    transport->context = context;
    *transport_out = transport;
    return CUBICLE_OK;
}
