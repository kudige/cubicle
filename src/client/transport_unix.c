#define _POSIX_C_SOURCE 200809L

#include "cubicle/transport_unix.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CUBICLE_UNIX_MAX_FRAME (16U * 1024U * 1024U)

typedef struct cubicle_unix_transport_context {
    int fd;
} cubicle_unix_transport_context_t;

static cubicle_error_code_t set_error(cubicle_error_t *error,
                                      cubicle_error_code_t code,
                                      int system_errno,
                                      const char *message)
{
    if (error != NULL) {
        error->code = code;
        error->system_errno = system_errno;
        snprintf(error->message, sizeof(error->message), "%s",
                 message == NULL ? "" : message);
    }
    return code;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = send(fd, cursor, length, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = EPIPE;
            return -1;
        }
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
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = ECONNRESET;
            return -1;
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static cubicle_error_code_t unix_connect(cubicle_transport_t *transport,
                                         const cubicle_endpoint_t *endpoint,
                                         cubicle_error_t *error)
{
    if (transport == NULL || transport->context == NULL || endpoint == NULL ||
        endpoint->transport != CUBICLE_TRANSPORT_UNIX ||
        endpoint->address[0] == '\0') {
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0,
                         "invalid Unix transport endpoint");
    }

    cubicle_unix_transport_context_t *context = transport->context;
    if (context->fd >= 0) {
        return set_error(error, CUBICLE_ERR_INVALID_STATE, 0,
                         "Unix transport is already connected");
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;

    size_t path_length = strlen(endpoint->address);
    if (path_length >= sizeof(address.sun_path)) {
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, ENAMETOOLONG,
                         "Unix socket path is too long");
    }
    memcpy(address.sun_path, endpoint->address, path_length + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return set_error(error, CUBICLE_ERR_IO, errno,
                         "failed to create Unix socket");
    }

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(fd);
        return set_error(error, CUBICLE_ERR_MANAGER_UNAVAILABLE, saved_errno,
                         "failed to connect to Unix socket");
    }

    context->fd = fd;
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    return CUBICLE_OK;
}

static cubicle_error_code_t unix_request(cubicle_transport_t *transport,
                                         const void *request,
                                         size_t request_length,
                                         void **response_out,
                                         size_t *response_length_out,
                                         cubicle_error_t *error)
{
    if (transport == NULL || transport->context == NULL ||
        (request == NULL && request_length != 0) || response_out == NULL ||
        response_length_out == NULL || request_length > CUBICLE_UNIX_MAX_FRAME) {
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0,
                         "invalid Unix transport request");
    }

    cubicle_unix_transport_context_t *context = transport->context;
    if (context->fd < 0) {
        return set_error(error, CUBICLE_ERR_INVALID_STATE, 0,
                         "Unix transport is not connected");
    }

    uint32_t request_size = htonl((uint32_t)request_length);
    if (write_all(context->fd, &request_size, sizeof(request_size)) < 0 ||
        (request_length > 0 &&
         write_all(context->fd, request, request_length) < 0)) {
        return set_error(error, CUBICLE_ERR_IO, errno,
                         "failed to write Unix transport request");
    }

    uint32_t response_size_network = 0;
    if (read_all(context->fd, &response_size_network,
                 sizeof(response_size_network)) < 0) {
        return set_error(error, CUBICLE_ERR_IO, errno,
                         "failed to read Unix transport response header");
    }

    uint32_t response_size = ntohl(response_size_network);
    if (response_size > CUBICLE_UNIX_MAX_FRAME) {
        return set_error(error, CUBICLE_ERR_PROTOCOL, EMSGSIZE,
                         "Unix transport response exceeds frame limit");
    }

    void *response = NULL;
    if (response_size > 0) {
        response = malloc(response_size);
        if (response == NULL) {
            return set_error(error, CUBICLE_ERR_INTERNAL, ENOMEM,
                             "failed to allocate response buffer");
        }
        if (read_all(context->fd, response, response_size) < 0) {
            int saved_errno = errno;
            free(response);
            return set_error(error, CUBICLE_ERR_IO, saved_errno,
                             "failed to read Unix transport response");
        }
    }

    *response_out = response;
    *response_length_out = response_size;
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    return CUBICLE_OK;
}

static void unix_response_free(cubicle_transport_t *transport, void *response)
{
    (void)transport;
    free(response);
}

static void unix_close(cubicle_transport_t *transport)
{
    if (transport == NULL || transport->context == NULL) {
        return;
    }
    cubicle_unix_transport_context_t *context = transport->context;
    if (context->fd >= 0) {
        close(context->fd);
        context->fd = -1;
    }
}

static void unix_destroy(cubicle_transport_t *transport)
{
    if (transport == NULL) {
        return;
    }
    unix_close(transport);
    free(transport->context);
    free(transport);
}

static const cubicle_transport_vtable_t UNIX_TRANSPORT_VTABLE = {
    .connect = unix_connect,
    .request = unix_request,
    .response_free = unix_response_free,
    .close = unix_close,
    .destroy = unix_destroy,
};

cubicle_error_code_t cubicle_transport_unix_create(
    cubicle_transport_t **transport_out)
{
    if (transport_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    cubicle_transport_t *transport = calloc(1, sizeof(*transport));
    cubicle_unix_transport_context_t *context = calloc(1, sizeof(*context));
    if (transport == NULL || context == NULL) {
        free(context);
        free(transport);
        return CUBICLE_ERR_INTERNAL;
    }

    context->fd = -1;
    transport->vtable = &UNIX_TRANSPORT_VTABLE;
    transport->context = context;
    *transport_out = transport;
    return CUBICLE_OK;
}
