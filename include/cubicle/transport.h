#ifndef CUBICLE_TRANSPORT_H
#define CUBICLE_TRANSPORT_H

#include "cubicle/api.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_transport_vtable {
    cubicle_error_code_t (*connect)(void *context,
                                    const cubicle_endpoint_t *endpoint);
    cubicle_error_code_t (*send_frame)(void *context,
                                       const void *data,
                                       size_t length);
    cubicle_error_code_t (*receive_frame)(void *context,
                                          void *buffer,
                                          size_t capacity,
                                          size_t *length_out,
                                          int timeout_ms);
    void (*close)(void *context);
} cubicle_transport_vtable_t;

#ifdef __cplusplus
}
#endif

#endif
