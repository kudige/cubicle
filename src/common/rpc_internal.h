#ifndef CUBICLE_COMMON_RPC_INTERNAL_H
#define CUBICLE_COMMON_RPC_INTERNAL_H

#include "cubicle/rpc.h"
#include "json.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct cubicle_rpc_request_envelope {
    cubicle_json_doc_t document;
    uint32_t protocol_major;
    uint32_t protocol_minor;
    char request_id[64];
    char session_id[64];
    char method[128];
    yyjson_val *params;
} cubicle_rpc_request_envelope_t;

typedef struct cubicle_rpc_response_envelope {
    cubicle_json_doc_t document;
    char request_id[64];
    bool success;
    yyjson_val *result;
    yyjson_val *error;
} cubicle_rpc_response_envelope_t;

int cubicle_rpc_decode_request(cubicle_rpc_request_envelope_t *envelope,
                               const char *json);
void cubicle_rpc_request_envelope_cleanup(
    cubicle_rpc_request_envelope_t *envelope);

int cubicle_rpc_decode_response(cubicle_rpc_response_envelope_t *envelope,
                                const char *json,
                                const char *expected_request_id);
void cubicle_rpc_response_envelope_cleanup(
    cubicle_rpc_response_envelope_t *envelope);

int cubicle_rpc_decode_error_value(yyjson_val *error_value,
                                   cubicle_error_t *error);

#endif
