#ifndef CUBICLE_TRANSPORT_H
#define CUBICLE_TRANSPORT_H

#include "cubicle/client_error.h"

#include <stddef.h>
#include <stdint.h>

typedef struct cubicle_transport cubicle_transport_t;

typedef struct cubicle_transport_vtable {
    cubicle_error_code_t (*connect)(cubicle_transport_t *transport,
                                     const cubicle_endpoint_t *endpoint,
                                     cubicle_error_t *error);
    cubicle_error_code_t (*request)(cubicle_transport_t *transport,
                                     const void *request,
                                     size_t request_length,
                                     void **response_out,
                                     size_t *response_length_out,
                                     cubicle_error_t *error);
    void (*response_free)(cubicle_transport_t *transport, void *response);
    void (*close)(cubicle_transport_t *transport);
    void (*destroy)(cubicle_transport_t *transport);
} cubicle_transport_vtable_t;

struct cubicle_transport {
    const cubicle_transport_vtable_t *vtable;
    void *context;
};

#endif
