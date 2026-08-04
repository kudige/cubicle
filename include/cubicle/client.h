#ifndef CUBICLE_CLIENT_H
#define CUBICLE_CLIENT_H

#include "cubicle/auth.h"
#include "cubicle/client_error.h"
#include "cubicle/types.h"
#include "cubicle/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_client cubicle_client_t;

typedef struct cubicle_client_options {
    cubicle_endpoint_t endpoint;
    int connect_timeout_ms;
    int request_timeout_ms;
    cubicle_transport_t *transport;
    cubicle_auth_options_t auth;
} cubicle_client_options_t;

cubicle_error_code_t cubicle_client_connect(
    const cubicle_client_options_t *options,
    cubicle_client_t **client_out);

cubicle_error_code_t cubicle_client_connect_uri(
    const char *uri,
    const cubicle_auth_options_t *auth,
    cubicle_client_t **client_out);

void cubicle_client_disconnect(cubicle_client_t *client);

const cubicle_error_t *cubicle_client_last_error(
    const cubicle_client_t *client);

cubicle_error_code_t cubicle_client_session_info(
    const cubicle_client_t *client,
    cubicle_session_info_t *session_out);

cubicle_error_code_t cubicle_client_call_json(
    cubicle_client_t *client,
    const char *method,
    const char *params_json,
    char **result_json_out);

#ifdef __cplusplus
}
#endif

#endif
