#include "cubicle/client_error.h"

const char *cubicle_error_code_name(cubicle_error_code_t code)
{
    switch (code) {
    case CUBICLE_OK: return "ok";
    case CUBICLE_ERR_INVALID_ARGUMENT: return "invalid_argument";
    case CUBICLE_ERR_NOT_FOUND: return "not_found";
    case CUBICLE_ERR_ALREADY_EXISTS: return "already_exists";
    case CUBICLE_ERR_AMBIGUOUS_NAME: return "ambiguous_name";
    case CUBICLE_ERR_PERMISSION_DENIED: return "permission_denied";
    case CUBICLE_ERR_UNSUPPORTED: return "unsupported";
    case CUBICLE_ERR_INVALID_STATE: return "invalid_state";
    case CUBICLE_ERR_TIMEOUT: return "timeout";
    case CUBICLE_ERR_MANAGER_UNAVAILABLE: return "manager_unavailable";
    case CUBICLE_ERR_CONTROLLER_UNAVAILABLE: return "controller_unavailable";
    case CUBICLE_ERR_AUTHENTICATION: return "authentication";
    case CUBICLE_ERR_PROTOCOL: return "protocol";
    case CUBICLE_ERR_IO: return "io";
    case CUBICLE_ERR_INTERNAL: return "internal";
    }

    return "unknown";
}
