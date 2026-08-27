#define _POSIX_C_SOURCE 200809L

#include "cubicle/transport_tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CUBICLE_TLS_MAX_FRAME (16U * 1024U * 1024U)
#define CUBICLE_TLS_URI_PREFIX "tls://"

typedef struct cubicle_tls_transport_context {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
} cubicle_tls_transport_context_t;

static void tls_set_low_latency(int fd)
{
    int enabled = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                     sizeof(enabled));
}

static cubicle_error_code_t set_error(cubicle_error_t *error,
                                      cubicle_error_code_t code,
                                      int system_errno,
                                      const char *message)
{
    if (error != NULL) {
        error->code = code;
        error->system_errno = system_errno;
        error->retryable =
            code == CUBICLE_ERR_MANAGER_UNAVAILABLE ||
            code == CUBICLE_ERR_IO ||
            code == CUBICLE_ERR_TIMEOUT;
        snprintf(error->message, sizeof(error->message), "%s",
                 message == NULL ? "" : message);
    }
    return code;
}

static int tls_write_all(SSL *ssl, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length > 0) {
        int chunk = length > INT32_MAX ? INT32_MAX : (int)length;
        int result = SSL_write(ssl, cursor, chunk);
        if (result <= 0) {
            int error = SSL_get_error(ssl, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            errno = EIO;
            return -1;
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int tls_read_all(SSL *ssl, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    while (length > 0) {
        int chunk = length > INT32_MAX ? INT32_MAX : (int)length;
        int result = SSL_read(ssl, cursor, chunk);
        if (result <= 0) {
            int error = SSL_get_error(ssl, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            errno = ECONNRESET;
            return -1;
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int split_tls_uri(const char *uri, char *host, size_t host_size,
                         char *port, size_t port_size)
{
    size_t prefix_length = strlen(CUBICLE_TLS_URI_PREFIX);
    if (uri == NULL ||
        strncmp(uri, CUBICLE_TLS_URI_PREFIX, prefix_length) != 0) {
        return -1;
    }

    const char *authority = uri + prefix_length;
    const char *port_start = NULL;
    size_t host_length = 0;
    if (authority[0] == '[') {
        const char *end = strchr(authority, ']');
        if (end == NULL || end[1] != ':') {
            return -1;
        }
        host_length = (size_t)(end - authority - 1);
        authority += 1;
        port_start = end + 2;
    } else {
        const char *colon = strrchr(authority, ':');
        if (colon == NULL) {
            return -1;
        }
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

static int connect_tcp_socket(const char *host, const char *port,
                              int *fd_out, int *saved_errno_out)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *addresses = NULL;
    int gai_result = getaddrinfo(host, port, &hints, &addresses);
    if (gai_result != 0) {
        *saved_errno_out = EINVAL;
        return -1;
    }

    int fd = -1;
    int saved_errno = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);

    if (fd < 0) {
        *saved_errno_out = saved_errno;
        return -1;
    }
    *fd_out = fd;
    return 0;
}

static cubicle_error_code_t tls_connect(cubicle_transport_t *transport,
                                        const cubicle_endpoint_t *endpoint,
                                        cubicle_error_t *error)
{
    if (transport == NULL || transport->context == NULL || endpoint == NULL) {
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0,
                         "invalid TLS transport endpoint");
    }

    cubicle_tls_transport_context_t *context = transport->context;
    if (context->fd >= 0) {
        return set_error(error, CUBICLE_ERR_INVALID_STATE, 0,
                         "TLS transport is already connected");
    }

    char host[256];
    char port[32];
    if (split_tls_uri(endpoint->uri, host, sizeof(host), port,
                      sizeof(port)) < 0) {
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0,
                         "endpoint URI must use tls://host:port syntax");
    }

    int fd = -1;
    int saved_errno = 0;
    if (connect_tcp_socket(host, port, &fd, &saved_errno) < 0) {
        return set_error(error, CUBICLE_ERR_MANAGER_UNAVAILABLE,
                         saved_errno, "failed to connect to TLS endpoint");
    }
    tls_set_low_latency(fd);

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = ctx == NULL ? NULL : SSL_new(ctx);
    if (ctx == NULL || ssl == NULL) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return set_error(error, CUBICLE_ERR_INTERNAL, errno,
                         "failed to initialize TLS client");
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    (void)SSL_set_tlsext_host_name(ssl, host);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return set_error(error, CUBICLE_ERR_MANAGER_UNAVAILABLE, EPROTO,
                         "TLS handshake failed");
    }

    context->fd = fd;
    context->ctx = ctx;
    context->ssl = ssl;
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    return CUBICLE_OK;
}

static cubicle_error_code_t tls_request(cubicle_transport_t *transport,
                                        const void *request,
                                        size_t request_length,
                                        void **response_out,
                                        size_t *response_length_out,
                                        cubicle_error_t *error)
{
    if (transport == NULL || transport->context == NULL ||
        (request == NULL && request_length != 0) || response_out == NULL ||
        response_length_out == NULL ||
        request_length > CUBICLE_TLS_MAX_FRAME) {
        return set_error(error, CUBICLE_ERR_INVALID_ARGUMENT, 0,
                         "invalid TLS transport request");
    }
    cubicle_tls_transport_context_t *context = transport->context;
    if (context->ssl == NULL) {
        return set_error(error, CUBICLE_ERR_INVALID_STATE, 0,
                         "TLS transport is not connected");
    }

    uint32_t request_size = htonl((uint32_t)request_length);
    if (tls_write_all(context->ssl, &request_size, sizeof(request_size)) < 0 ||
        (request_length > 0 &&
         tls_write_all(context->ssl, request, request_length) < 0)) {
        return set_error(error, CUBICLE_ERR_IO, errno,
                         "failed to write TLS transport request");
    }

    uint32_t response_size_network = 0;
    if (tls_read_all(context->ssl, &response_size_network,
                     sizeof(response_size_network)) < 0) {
        return set_error(error, CUBICLE_ERR_IO, errno,
                         "failed to read TLS transport response header");
    }
    uint32_t response_size = ntohl(response_size_network);
    if (response_size > CUBICLE_TLS_MAX_FRAME) {
        return set_error(error, CUBICLE_ERR_PROTOCOL, EMSGSIZE,
                         "TLS transport response exceeds frame limit");
    }

    void *response = NULL;
    if (response_size > 0) {
        response = malloc(response_size);
        if (response == NULL) {
            return set_error(error, CUBICLE_ERR_INTERNAL, ENOMEM,
                             "failed to allocate response buffer");
        }
        if (tls_read_all(context->ssl, response, response_size) < 0) {
            int saved_errno = errno;
            free(response);
            return set_error(error, CUBICLE_ERR_IO, saved_errno,
                             "failed to read TLS transport response");
        }
    }
    *response_out = response;
    *response_length_out = response_size;
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    return CUBICLE_OK;
}

static void tls_response_free(cubicle_transport_t *transport, void *response)
{
    (void)transport;
    free(response);
}

static void tls_close(cubicle_transport_t *transport)
{
    if (transport == NULL || transport->context == NULL) {
        return;
    }
    cubicle_tls_transport_context_t *context = transport->context;
    if (context->ssl != NULL) {
        (void)SSL_shutdown(context->ssl);
        SSL_free(context->ssl);
        context->ssl = NULL;
    }
    if (context->fd >= 0) {
        close(context->fd);
        context->fd = -1;
    }
    SSL_CTX_free(context->ctx);
    context->ctx = NULL;
}

static void tls_destroy(cubicle_transport_t *transport)
{
    if (transport != NULL) {
        tls_close(transport);
        free(transport->context);
        free(transport);
    }
}

static const cubicle_transport_vtable_t TLS_TRANSPORT_VTABLE = {
    .connect = tls_connect,
    .request = tls_request,
    .response_free = tls_response_free,
    .close = tls_close,
    .destroy = tls_destroy,
};

cubicle_error_code_t cubicle_transport_tls_create(
    cubicle_transport_t **transport_out)
{
    if (transport_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    cubicle_transport_t *transport = calloc(1, sizeof(*transport));
    cubicle_tls_transport_context_t *context = calloc(1, sizeof(*context));
    if (transport == NULL || context == NULL) {
        free(context);
        free(transport);
        return CUBICLE_ERR_INTERNAL;
    }
    context->fd = -1;
    transport->vtable = &TLS_TRANSPORT_VTABLE;
    transport->context = context;
    *transport_out = transport;
    return CUBICLE_OK;
}
