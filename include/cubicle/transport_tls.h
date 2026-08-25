#ifndef CUBICLE_TRANSPORT_TLS_H
#define CUBICLE_TRANSPORT_TLS_H

#include "cubicle/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Creates a TLS-over-TCP transport.
 *
 * Endpoint URIs use tls://host:port syntax. The transport uses the same
 * length-prefixed JSON-RPC framing as the Unix and plain TCP transports.
 */
cubicle_error_code_t cubicle_transport_tls_create(
    cubicle_transport_t **transport_out);

#ifdef __cplusplus
}
#endif

#endif
