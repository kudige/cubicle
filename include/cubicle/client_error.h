#ifndef CUBICLE_CLIENT_ERROR_H
#define CUBICLE_CLIENT_ERROR_H

#include "cubicle/client_types.h"

typedef enum cubicle_error_code {
    CUBICLE_OK = 0,
    CUBICLE_ERR_INVALID_ARGUMENT,
    CUBICLE_ERR_NOT_FOUND,
    CUBICLE_ERR_ALREADY_EXISTS,
    CUBICLE_ERR_AMBIGUOUS_NAME,
    CUBICLE_ERR_PERMISSION_DENIED,
    CUBICLE_ERR_UNSUPPORTED,
    CUBICLE_ERR_INVALID_STATE,
    CUBICLE_ERR_TIMEOUT,
    CUBICLE_ERR_MANAGER_UNAVAILABLE,
    CUBICLE_ERR_CONTROLLER_UNAVAILABLE,
    CUBICLE_ERR_AUTHENTICATION,
    CUBICLE_ERR_PROTOCOL,
    CUBICLE_ERR_IO,
    CUBICLE_ERR_INTERNAL
} cubicle_error_code_t;

typedef struct cubicle_error {
    cubicle_error_code_t code;
    int system_errno;
    char message[CUBICLE_ERROR_MESSAGE_MAX];
} cubicle_error_t;

const char *cubicle_error_code_name(cubicle_error_code_t code);

#endif
