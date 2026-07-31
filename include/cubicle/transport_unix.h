#ifndef CUBICLE_TRANSPORT_UNIX_H
#define CUBICLE_TRANSPORT_UNIX_H

#include "cubicle/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Creates a Unix-domain-socket transport.
 *
 * The returned transport is owned by the caller until it is passed to
 * cubicle_client_connect(). A successfully connected cubicle_client_t assumes
 * ownership and destroys the transport during cubicle_client_disconnect().
 */
cubicle_error_code_t cubicle_transport_unix_create(
    cubicle_transport_t **transport_out);

#ifdef __cplusplus
}
#endif

#endif
