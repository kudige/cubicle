#ifndef CUBICLE_AUTH_H
#define CUBICLE_AUTH_H

#include "cubicle/client_error.h"
#include "cubicle/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_signer cubicle_signer_t;

typedef cubicle_error_code_t (*cubicle_sign_fn)(
    void *context,
    const unsigned char *message,
    size_t message_length,
    unsigned char *signature,
    size_t *signature_length,
    cubicle_error_t *error);

typedef void (*cubicle_signer_destroy_fn)(void *context);

typedef struct cubicle_signer_callbacks {
    cubicle_sign_fn sign;
    cubicle_signer_destroy_fn destroy;
} cubicle_signer_callbacks_t;

typedef struct cubicle_auth_options {
    cubicle_signer_t *signer;
    const unsigned char *public_key;
    size_t public_key_length;
    const char *expected_server_identity;
    cubicle_protocol_capability_mask_t required_capabilities;
} cubicle_auth_options_t;

typedef struct cubicle_session_info {
    cubicle_session_id_t session_id;
    cubicle_manager_id_t manager_id;
    cubicle_key_id_t client_key_id;
    uint32_t protocol_major;
    uint32_t protocol_minor;
    cubicle_protocol_capability_mask_t negotiated_capabilities;
    uint64_t authenticated_at_ms;
    uint64_t expires_at_ms;
    char manager_public_key[CUBICLE_SERVER_ID_MAX];
} cubicle_session_info_t;

cubicle_error_code_t cubicle_signer_create(const cubicle_signer_callbacks_t *callbacks, void *context, cubicle_signer_t **signer_out);
cubicle_error_code_t cubicle_signer_from_private_key_file(const char *path, cubicle_signer_t **signer_out, cubicle_error_t *error);
cubicle_error_code_t cubicle_signer_from_ssh_agent(const char *public_key_fingerprint, cubicle_signer_t **signer_out, cubicle_error_t *error);
void cubicle_signer_destroy(cubicle_signer_t *signer);

#ifdef __cplusplus
}
#endif

#endif
