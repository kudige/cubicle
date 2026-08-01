#ifndef CUBICLE_AUTH_PROTOCOL_H
#define CUBICLE_AUTH_PROTOCOL_H

#include "auth_crypto.h"

#include "cubicle/types.h"

#include <stdint.h>
#include <sys/types.h>

#define CUBICLE_AUTH_NONCE_BYTES 32
#define CUBICLE_AUTH_CONNECTION_ID_BYTES 16

typedef struct cubicle_auth_transcript {
    uint32_t protocol_major;
    uint32_t protocol_minor;
    unsigned char manager_public_key[CUBICLE_AUTH_PUBLIC_KEY_BYTES];
    unsigned char client_public_key[CUBICLE_AUTH_PUBLIC_KEY_BYTES];
    unsigned char client_nonce[CUBICLE_AUTH_NONCE_BYTES];
    unsigned char manager_nonce[CUBICLE_AUTH_NONCE_BYTES];
    unsigned char connection_id[CUBICLE_AUTH_CONNECTION_ID_BYTES];
    cubicle_protocol_capability_mask_t capabilities;
    uint64_t manager_generation;
    uid_t peer_uid;
    gid_t peer_gid;
    char workspace_ref[CUBICLE_NAME_MAX];
} cubicle_auth_transcript_t;

typedef struct cubicle_auth_resume {
    char manager_key_id[CUBICLE_ID_STRING_LENGTH];
    char session_id[CUBICLE_ID_STRING_LENGTH];
    unsigned char client_nonce[CUBICLE_AUTH_NONCE_BYTES];
    unsigned char server_nonce[CUBICLE_AUTH_NONCE_BYTES];
    unsigned char connection_id[CUBICLE_AUTH_CONNECTION_ID_BYTES];
    uint64_t manager_generation;
    uid_t peer_uid;
    gid_t peer_gid;
} cubicle_auth_resume_t;

int cubicle_auth_encode_transcript(const cubicle_auth_transcript_t *transcript,
                                   unsigned char *buffer,
                                   size_t buffer_size,
                                   size_t *length_out);
int cubicle_auth_encode_resume(const cubicle_auth_resume_t *resume,
                               unsigned char *buffer,
                               size_t buffer_size,
                               size_t *length_out);

#endif
