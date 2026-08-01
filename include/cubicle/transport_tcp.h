#ifndef CUBICLE_TRANSPORT_TCP_H
#define CUBICLE_TRANSPORT_TCP_H

#include "cubicle/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Creates a plain TCP transport.
 *
 * Endpoint URIs use tcp://host:port syntax. The transport uses the same
 * length-prefixed JSON-RPC framing as the Unix-domain-socket transport.
 */
cubicle_error_code_t cubicle_transport_tcp_create(
    cubicle_transport_t **transport_out);

#ifdef __cplusplus
}
#endif

#endif
