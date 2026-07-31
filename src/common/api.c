#include "cubicle/api.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

const char *cubicle_error_code_name(cubicle_error_code_t code)
{
    switch (code) {
    case CUBICLE_OK:
        return "ok";
    case CUBICLE_ERR_INVALID_ARGUMENT:
        return "invalid_argument";
    case CUBICLE_ERR_NOT_FOUND:
        return "not_found";
    case CUBICLE_ERR_ALREADY_EXISTS:
        return "already_exists";
    case CUBICLE_ERR_AMBIGUOUS_NAME:
        return "ambiguous_name";
    case CUBICLE_ERR_PERMISSION_DENIED:
        return "permission_denied";
    case CUBICLE_ERR_AUTHENTICATION_FAILED:
        return "authentication_failed";
    case CUBICLE_ERR_SESSION_EXPIRED:
        return "session_expired";
    case CUBICLE_ERR_UNSUPPORTED:
        return "unsupported";
    case CUBICLE_ERR_INVALID_STATE:
        return "invalid_state";
    case CUBICLE_ERR_CONFLICT:
        return "conflict";
    case CUBICLE_ERR_TIMEOUT:
        return "timeout";
    case CUBICLE_ERR_MANAGER_UNAVAILABLE:
        return "manager_unavailable";
    case CUBICLE_ERR_CONTROLLER_UNAVAILABLE:
        return "controller_unavailable";
    case CUBICLE_ERR_PROTOCOL:
        return "protocol";
    case CUBICLE_ERR_IO:
        return "io";
    case CUBICLE_ERR_RESOURCE_LIMIT:
        return "resource_limit";
    case CUBICLE_ERR_INTERNAL:
        return "internal";
    default:
        return "unknown";
    }
}

int cubicle_endpoint_parse(cubicle_endpoint_t *endpoint,
                           const char *uri,
                           const char *server_identity)
{
    if (endpoint == NULL || uri == NULL || uri[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (strncmp(uri, "unix://", 7) != 0 || uri[7] == '\0') {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    int uri_length = snprintf(endpoint->uri, sizeof(endpoint->uri), "%s", uri);
    if (uri_length < 0 || (size_t)uri_length >= sizeof(endpoint->uri)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    const char *identity = server_identity == NULL ? "" : server_identity;
    int identity_length = snprintf(endpoint->server_identity,
                                   sizeof(endpoint->server_identity), "%s",
                                   identity);
    if (identity_length < 0 ||
        (size_t)identity_length >= sizeof(endpoint->server_identity)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

int cubicle_endpoint_is_unix(const cubicle_endpoint_t *endpoint)
{
    return endpoint != NULL && strncmp(endpoint->uri, "unix://", 7) == 0;
}

int cubicle_endpoint_unix_path(const cubicle_endpoint_t *endpoint,
                               char *path,
                               size_t path_size)
{
    if (!cubicle_endpoint_is_unix(endpoint) || path == NULL ||
        path_size == 0) {
        errno = EINVAL;
        return -1;
    }

    const char *unix_path = endpoint->uri + 7;
    int length = snprintf(path, path_size, "%s", unix_path);
    if (length < 0 || (size_t)length >= path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}
