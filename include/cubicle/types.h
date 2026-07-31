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
#define CUBICLE_ENDPOINT_URI_MAX 512
#define CUBICLE_ENDPOINT_MAX CUBICLE_ENDPOINT_URI_MAX
#define CUBICLE_SERVER_ID_MAX 256
#define CUBICLE_TOKEN_MAX 512
#define CUBICLE_ERROR_MESSAGE_MAX 256
#define CUBICLE_EVENT_PAYLOAD_MAX 512
#define CUBICLE_KEY_LABEL_MAX 128
#define CUBICLE_CONTINUATION_TOKEN_MAX 256
#define CUBICLE_CAPABILITY_NAME_MAX 128
#define CUBICLE_MAX_PROTOCOL_CAPABILITIES 64

typedef char cubicle_manager_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_workspace_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_process_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_key_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_session_id_t[CUBICLE_ID_STRING_LENGTH];
typedef char cubicle_grant_id_t[CUBICLE_ID_STRING_LENGTH];

typedef struct cubicle_endpoint {
    char uri[CUBICLE_ENDPOINT_URI_MAX];
    char server_identity[CUBICLE_SERVER_ID_MAX];
} cubicle_endpoint_t;

typedef struct cubicle_request_options {
    const char *idempotency_key;
    int timeout_ms;
    uint64_t deadline_ms;
} cubicle_request_options_t;

typedef struct cubicle_page_options {
    size_t limit;
    const char *continuation_token;
} cubicle_page_options_t;

typedef struct cubicle_page_info {
    char continuation_token[CUBICLE_CONTINUATION_TOKEN_MAX];
    bool has_more;
} cubicle_page_info_t;

typedef uint64_t cubicle_protocol_capability_mask_t;

#define CUBICLE_PROTOCOL_CAP_AUTH_ED25519       (UINT64_C(1) << 0)
#define CUBICLE_PROTOCOL_CAP_TRANSPORT_UNIX     (UINT64_C(1) << 1)
#define CUBICLE_PROTOCOL_CAP_TRANSPORT_TLS      (UINT64_C(1) << 2)
#define CUBICLE_PROTOCOL_CAP_TRANSPORT_QUIC     (UINT64_C(1) << 3)
#define CUBICLE_PROTOCOL_CAP_PROCESS_STREAM     (UINT64_C(1) << 8)
#define CUBICLE_PROTOCOL_CAP_PROCESS_TTY        (UINT64_C(1) << 9)
#define CUBICLE_PROTOCOL_CAP_ATTACHMENT_DIRECT  (UINT64_C(1) << 16)
#define CUBICLE_PROTOCOL_CAP_ATTACHMENT_RELAY   (UINT64_C(1) << 17)
#define CUBICLE_PROTOCOL_CAP_EVENTS_SUBSCRIBE   (UINT64_C(1) << 24)

#ifdef __cplusplus
}
#endif

#endif
