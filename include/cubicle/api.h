#ifndef CUBICLE_API_H
#define CUBICLE_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUBICLE_PROTOCOL_MAJOR 0
#define CUBICLE_PROTOCOL_MINOR 1
#define CUBICLE_ENDPOINT_URI_MAX 256
#define CUBICLE_SERVER_ID_MAX 128

typedef enum cubicle_error_code {
    CUBICLE_OK = 0,
    CUBICLE_ERR_INVALID_ARGUMENT,
    CUBICLE_ERR_NOT_FOUND,
    CUBICLE_ERR_ALREADY_EXISTS,
    CUBICLE_ERR_AMBIGUOUS_NAME,
    CUBICLE_ERR_PERMISSION_DENIED,
    CUBICLE_ERR_AUTHENTICATION_FAILED,
    CUBICLE_ERR_SESSION_EXPIRED,
    CUBICLE_ERR_UNSUPPORTED,
    CUBICLE_ERR_INVALID_STATE,
    CUBICLE_ERR_CONFLICT,
    CUBICLE_ERR_TIMEOUT,
    CUBICLE_ERR_MANAGER_UNAVAILABLE,
    CUBICLE_ERR_CONTROLLER_UNAVAILABLE,
    CUBICLE_ERR_PROTOCOL,
    CUBICLE_ERR_IO,
    CUBICLE_ERR_RESOURCE_LIMIT,
    CUBICLE_ERR_INTERNAL
} cubicle_error_code_t;

typedef struct cubicle_error {
    cubicle_error_code_t code;
    int system_errno;
    int retryable;
    char message[256];
} cubicle_error_t;

typedef struct cubicle_endpoint {
    char uri[CUBICLE_ENDPOINT_URI_MAX];
    char server_identity[CUBICLE_SERVER_ID_MAX];
} cubicle_endpoint_t;

const char *cubicle_error_code_name(cubicle_error_code_t code);
int cubicle_endpoint_parse(cubicle_endpoint_t *endpoint,
                           const char *uri,
                           const char *server_identity);
int cubicle_endpoint_is_unix(const cubicle_endpoint_t *endpoint);
int cubicle_endpoint_unix_path(const cubicle_endpoint_t *endpoint,
                               char *path,
                               size_t path_size);

#ifdef __cplusplus
}
#endif

#endif
