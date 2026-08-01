#ifndef CUBICLE_AUTH_CRYPTO_H
#define CUBICLE_AUTH_CRYPTO_H

#include "cubicle/types.h"

#include <stddef.h>

#define CUBICLE_AUTH_PUBLIC_KEY_BYTES 32
#define CUBICLE_AUTH_PRIVATE_KEY_MAX_BYTES 2048
#define CUBICLE_AUTH_SIGNATURE_BYTES 64
#define CUBICLE_AUTH_SECRET_BYTES 32
#define CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH \
    (CUBICLE_AUTH_PUBLIC_KEY_BYTES * 2 + 1)
#define CUBICLE_AUTH_FINGERPRINT_LENGTH 65

typedef struct cubicle_auth_identity {
    unsigned char public_key[CUBICLE_AUTH_PUBLIC_KEY_BYTES];
    char public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
    char key_id[CUBICLE_ID_STRING_LENGTH];
    char fingerprint[CUBICLE_AUTH_FINGERPRINT_LENGTH];
} cubicle_auth_identity_t;

int cubicle_auth_random_bytes(unsigned char *buffer, size_t length);
int cubicle_auth_hex_encode(const unsigned char *bytes, size_t length,
                            char *hex, size_t hex_size);
int cubicle_auth_hex_decode(const char *hex, unsigned char *bytes,
                            size_t bytes_size);
int cubicle_auth_key_fingerprint(const unsigned char *public_key,
                                 size_t public_key_length,
                                 char key_id[CUBICLE_ID_STRING_LENGTH],
                                 char fingerprint[CUBICLE_AUTH_FINGERPRINT_LENGTH]);
int cubicle_auth_ensure_identity(const char *key_dir,
                                 const char *private_key_name,
                                 const char *public_key_name,
                                 cubicle_auth_identity_t *identity);
int cubicle_auth_sign_file_key(const char *private_key_path,
                               const unsigned char *message,
                               size_t message_length,
                               unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES]);
int cubicle_auth_verify(const unsigned char public_key[CUBICLE_AUTH_PUBLIC_KEY_BYTES],
                        const unsigned char *message,
                        size_t message_length,
                        const unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES]);
int cubicle_auth_hmac_sha256(const unsigned char *key, size_t key_length,
                             const unsigned char *message,
                             size_t message_length,
                             unsigned char out[CUBICLE_AUTH_SECRET_BYTES]);

#endif
