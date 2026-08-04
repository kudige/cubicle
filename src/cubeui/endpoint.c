#include "cubeui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *cubeui_resolve_manager_endpoint(
    const char *override_endpoint,
    const cubicle_config_t *config,
    char *configured_endpoint,
    size_t configured_endpoint_size)
{
    if (override_endpoint != NULL && override_endpoint[0] != '\0') {
        return override_endpoint;
    }
    const char *environment = getenv("CUBICLE_MANAGER_SOCKET");
    if (environment != NULL && environment[0] != '\0') {
        return environment;
    }
    if (config == NULL || configured_endpoint == NULL ||
        configured_endpoint_size == 0) {
        return NULL;
    }
    int result = snprintf(configured_endpoint, configured_endpoint_size, "%s",
                          config->client_manager_uri);
    if (result < 0 || (size_t)result >= configured_endpoint_size) {
        return NULL;
    }
    return configured_endpoint;
}

int cubeui_endpoint_from_uri(cubicle_endpoint_t *endpoint, const char *uri)
{
    if (endpoint == NULL || uri == NULL || uri[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    if (uri[0] == '/') {
        int length = snprintf(endpoint->uri, sizeof(endpoint->uri),
                              "unix://%s", uri);
        return length < 0 || (size_t)length >= sizeof(endpoint->uri) ? -1 : 0;
    }
    int length = snprintf(endpoint->uri, sizeof(endpoint->uri), "%s", uri);
    return length < 0 || (size_t)length >= sizeof(endpoint->uri) ? -1 : 0;
}
