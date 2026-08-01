#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cubicle_error_code_t cubicle_signer_create(const cubicle_signer_callbacks_t *callbacks,
                                           void *context,
                                           cubicle_signer_t **signer_out)
{
    if (callbacks == NULL || callbacks->sign == NULL || signer_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_signer_t *signer = calloc(1, sizeof(*signer));
    if (signer == NULL) return CUBICLE_ERR_INTERNAL;
    signer->callbacks = *callbacks;
    signer->context = context;
    *signer_out = signer;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_signer_from_private_key_file(const char *path,
    cubicle_signer_t **signer_out, cubicle_error_t *error)
{
    if (path == NULL || signer_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(error, CUBICLE_ERR_UNSUPPORTED, 0, false,
                     "private key file signers are not implemented");
}

cubicle_error_code_t cubicle_signer_from_ssh_agent(const char *public_key_fingerprint,
    cubicle_signer_t **signer_out, cubicle_error_t *error)
{
    if (public_key_fingerprint == NULL || signer_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(error, CUBICLE_ERR_UNSUPPORTED, 0, false,
                     "SSH agent signers are not implemented");
}

void cubicle_signer_destroy(cubicle_signer_t *signer)
{
    if (signer == NULL) return;
    if (signer->callbacks.destroy != NULL) signer->callbacks.destroy(signer->context);
    free(signer);
}

