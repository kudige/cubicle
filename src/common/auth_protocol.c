#define _POSIX_C_SOURCE 200809L

#include "auth_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static int append_bytes(unsigned char *buffer, size_t buffer_size,
                        size_t *used, const void *data, size_t length)
{
    if (buffer == NULL || used == NULL || data == NULL ||
        length > buffer_size - *used) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(buffer + *used, data, length);
    *used += length;
    return 0;
}

static int append_u32(unsigned char *buffer, size_t buffer_size, size_t *used,
                      uint32_t value)
{
    uint32_t encoded = htonl(value);
    return append_bytes(buffer, buffer_size, used, &encoded, sizeof(encoded));
}

static int append_u64(unsigned char *buffer, size_t buffer_size, size_t *used,
                      uint64_t value)
{
    uint32_t high = (uint32_t)(value >> 32);
    uint32_t low = (uint32_t)(value & UINT32_C(0xffffffff));
    return append_u32(buffer, buffer_size, used, high) == 0 &&
                   append_u32(buffer, buffer_size, used, low) == 0
               ? 0
               : -1;
}

static int append_string(unsigned char *buffer, size_t buffer_size,
                         size_t *used, const char *value)
{
    const char *text = value == NULL ? "" : value;
    size_t length = strlen(text);
    if (length > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return append_u32(buffer, buffer_size, used, (uint32_t)length) == 0 &&
                   (length == 0 ||
                    append_bytes(buffer, buffer_size, used, text, length) == 0)
               ? 0
               : -1;
}

int cubicle_auth_encode_transcript(const cubicle_auth_transcript_t *transcript,
                                   unsigned char *buffer,
                                   size_t buffer_size,
                                   size_t *length_out)
{
    static const unsigned char domain[] = "CUBICLE-AUTH-V0";
    if (transcript == NULL || buffer == NULL || length_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t used = 0;
    if (append_bytes(buffer, buffer_size, &used, domain, sizeof(domain)) < 0 ||
        append_u32(buffer, buffer_size, &used, transcript->protocol_major) < 0 ||
        append_u32(buffer, buffer_size, &used, transcript->protocol_minor) < 0 ||
        append_bytes(buffer, buffer_size, &used, transcript->manager_public_key,
                     sizeof(transcript->manager_public_key)) < 0 ||
        append_bytes(buffer, buffer_size, &used, transcript->client_public_key,
                     sizeof(transcript->client_public_key)) < 0 ||
        append_bytes(buffer, buffer_size, &used, transcript->client_nonce,
                     sizeof(transcript->client_nonce)) < 0 ||
        append_bytes(buffer, buffer_size, &used, transcript->manager_nonce,
                     sizeof(transcript->manager_nonce)) < 0 ||
        append_bytes(buffer, buffer_size, &used, transcript->connection_id,
                     sizeof(transcript->connection_id)) < 0 ||
        append_u64(buffer, buffer_size, &used,
                   transcript->capabilities) < 0 ||
        append_u64(buffer, buffer_size, &used,
                   transcript->manager_generation) < 0 ||
        append_u64(buffer, buffer_size, &used,
                   (uint64_t)transcript->peer_uid) < 0 ||
        append_u64(buffer, buffer_size, &used,
                   (uint64_t)transcript->peer_gid) < 0 ||
        append_string(buffer, buffer_size, &used,
                      transcript->workspace_ref) < 0) {
        return -1;
    }

    *length_out = used;
    return 0;
}

int cubicle_auth_encode_resume(const cubicle_auth_resume_t *resume,
                               unsigned char *buffer,
                               size_t buffer_size,
                               size_t *length_out)
{
    static const unsigned char domain[] = "CUBICLE-RESUME-V0";
    if (resume == NULL || buffer == NULL || length_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t used = 0;
    if (append_bytes(buffer, buffer_size, &used, domain, sizeof(domain)) < 0 ||
        append_string(buffer, buffer_size, &used, resume->manager_key_id) < 0 ||
        append_string(buffer, buffer_size, &used, resume->session_id) < 0 ||
        append_bytes(buffer, buffer_size, &used, resume->client_nonce,
                     sizeof(resume->client_nonce)) < 0 ||
        append_bytes(buffer, buffer_size, &used, resume->server_nonce,
                     sizeof(resume->server_nonce)) < 0 ||
        append_bytes(buffer, buffer_size, &used, resume->connection_id,
                     sizeof(resume->connection_id)) < 0 ||
        append_u64(buffer, buffer_size, &used, resume->manager_generation) < 0 ||
        append_u64(buffer, buffer_size, &used, (uint64_t)resume->peer_uid) < 0 ||
        append_u64(buffer, buffer_size, &used, (uint64_t)resume->peer_gid) < 0) {
        return -1;
    }

    *length_out = used;
    return 0;
}
