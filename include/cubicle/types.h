#ifndef CUBICLE_TYPES_H
#define CUBICLE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUBICLE_ID_STRING_LENGTH 33
#define CUBICLE_NAME_MAX 256
#define CUBICLE_ENDPOINT_MAX 512
#define CUBICLE_SERVER_ID_MAX 256
#define CUBICLE_TOKEN_MAX 512
#define CUBICLE_ERROR_MESSAGE_MAX 256
#define CUBICLE_EVENT_PAYLOAD_MAX 512
#define CUBICLE_KEY_LABEL_MAX 128

typedef char cubicle_manager_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_workspace_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_process_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_key_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_session_id_t[CUBICLE_ID_STRING_LENGTH];

typedef enum cubicle_transport_kind {
    CUBICLE_TRANSPORT_UNSPECIFIED = 0,
    CUBICLE_TRANSPORT_UNIX,
    CUBICLE_TRANSPORT_TLS,
    CUBICLE_TRANSPORT_QUIC,
    CUBICLE_TRANSPORT_RELAY
} cubicle_transport_kind_t;

typedef struct cubicle_endpoint {
    cubicle_transport_kind_t transport;
    char address[CUBICLE_ENDPOINT_MAX];
    char server_identity[CUBICLE_SERVER_ID_MAX];
} cubicle_endpoint_t;

typedef struct cubicle_request_options {
    const char *idempotency_key;
    int timeout_ms;
} cubicle_request_options_t;

#ifdef __cplusplus
}
#endif

#endif
