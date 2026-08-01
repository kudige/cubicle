#define _POSIX_C_SOURCE 200809L

#include "../src/common/auth_crypto.h"
#include "../src/common/auth_protocol.h"
#include "../src/common/json.h"
#include "../src/common/rpc_internal.h"

#include "cubicle/rpc.h"
#include "cubicle/types.h"
#include "cubicle/util.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int read_all(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t count = read(fd, cursor, length);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        assert(count != 0);
        cursor += (size_t)count;
        length -= (size_t)count;
    }
    return 0;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t count = write(fd, cursor, length);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        cursor += (size_t)count;
        length -= (size_t)count;
    }
    return 0;
}

static int connect_unix_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    assert(strlen(path) < sizeof(address.sun_path));
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}

static char *rpc_call(int fd,
                      const char *request_id,
                      const char *session_id,
                      const char *method,
                      const char *params)
{
    char request[4096];
    assert(cubicle_rpc_request(request, sizeof(request), request_id,
                               session_id, method, params) == 0);

    uint32_t request_size = htonl((uint32_t)strlen(request));
    assert(write_all(fd, &request_size, sizeof(request_size)) == 0);
    assert(write_all(fd, request, strlen(request)) == 0);

    uint32_t response_size_network = 0;
    assert(read_all(fd, &response_size_network,
                    sizeof(response_size_network)) == 0);
    uint32_t response_size = ntohl(response_size_network);
    assert(response_size > 0 && response_size <= 65536);

    char *response = calloc((size_t)response_size + 1, 1);
    assert(response != NULL);
    assert(read_all(fd, response, response_size) == 0);

    cubicle_rpc_response_envelope_t envelope;
    assert(cubicle_rpc_decode_response(&envelope, response, request_id) == 0);
    assert(envelope.success);
    cubicle_rpc_response_envelope_cleanup(&envelope);
    return response;
}

static void get_result_string(yyjson_val *result,
                              const char *field,
                              char *buffer,
                              size_t buffer_size)
{
    cubicle_validation_error_t validation_error;
    assert(cubicle_json_get_required_string(result, field, buffer,
                                            buffer_size,
                                            &validation_error) == 0);
}

static uint64_t get_result_u64(yyjson_val *result, const char *field)
{
    uint64_t value = 0;
    cubicle_validation_error_t validation_error;
    assert(cubicle_json_get_required_u64(result, field, &value,
                                         &validation_error) == 0);
    return value;
}

static void path_join(char *path, size_t path_size, const char *dir,
                      const char *name)
{
    int length = snprintf(path, path_size, "%s/%s", dir, name);
    assert(length >= 0 && (size_t)length < path_size);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *socket_path = argv[1];
    const char *key_dir = argv[2];

    cubicle_auth_identity_t identity;
    assert(cubicle_auth_ensure_identity(key_dir, "client.key", "client.pub",
                                        &identity) == 0);

    unsigned char client_nonce[CUBICLE_AUTH_NONCE_BYTES];
    char client_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
    assert(cubicle_auth_random_bytes(client_nonce, sizeof(client_nonce)) == 0);
    assert(cubicle_auth_hex_encode(client_nonce, sizeof(client_nonce),
                                   client_nonce_hex,
                                   sizeof(client_nonce_hex)) == 0);

    int fd = connect_unix_socket(socket_path);

    char params[512];
    int length = snprintf(
        params, sizeof(params),
        "{\"client_public_key\":\"%s\",\"client_nonce\":\"%s\",\"workspace\":\"Project A\"}",
        identity.public_key_hex, client_nonce_hex);
    assert(length >= 0 && (size_t)length < sizeof(params));

    char *challenge_json = rpc_call(fd, "auth-1", "", "auth.challenge",
                                    params);
    cubicle_json_doc_t challenge_doc;
    assert(cubicle_json_parse(&challenge_doc, challenge_json) == 0);
    yyjson_val *challenge = yyjson_obj_get(challenge_doc.root, "result");
    assert(yyjson_is_obj(challenge));

    char manager_public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
    char manager_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
    char connection_id_hex[CUBICLE_AUTH_CONNECTION_ID_BYTES * 2 + 1];
    get_result_string(challenge, "manager_public_key", manager_public_key_hex,
                      sizeof(manager_public_key_hex));
    get_result_string(challenge, "manager_nonce", manager_nonce_hex,
                      sizeof(manager_nonce_hex));
    get_result_string(challenge, "connection_id", connection_id_hex,
                      sizeof(connection_id_hex));

    cubicle_auth_transcript_t transcript;
    memset(&transcript, 0, sizeof(transcript));
    transcript.protocol_major = (uint32_t)get_result_u64(challenge,
                                                         "protocol_major");
    transcript.protocol_minor = (uint32_t)get_result_u64(challenge,
                                                         "protocol_minor");
    assert(cubicle_auth_hex_decode(manager_public_key_hex,
                                   transcript.manager_public_key,
                                   sizeof(transcript.manager_public_key)) == 0);
    memcpy(transcript.client_public_key, identity.public_key,
           sizeof(transcript.client_public_key));
    memcpy(transcript.client_nonce, client_nonce,
           sizeof(transcript.client_nonce));
    assert(cubicle_auth_hex_decode(manager_nonce_hex,
                                   transcript.manager_nonce,
                                   sizeof(transcript.manager_nonce)) == 0);
    assert(cubicle_auth_hex_decode(connection_id_hex,
                                   transcript.connection_id,
                                   sizeof(transcript.connection_id)) == 0);
    transcript.capabilities = get_result_u64(challenge, "capabilities");
    transcript.manager_generation = get_result_u64(challenge,
                                                   "manager_generation");
    transcript.peer_uid = (uid_t)get_result_u64(challenge, "peer_uid");
    transcript.peer_gid = (gid_t)get_result_u64(challenge, "peer_gid");
    snprintf(transcript.workspace_ref, sizeof(transcript.workspace_ref),
             "Project A");

    unsigned char transcript_bytes[512];
    size_t transcript_length = 0;
    assert(cubicle_auth_encode_transcript(&transcript, transcript_bytes,
                                          sizeof(transcript_bytes),
                                          &transcript_length) == 0);

    char private_key_path[CUBICLE_PATH_MAX];
    path_join(private_key_path, sizeof(private_key_path), key_dir,
              "client.key");
    unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES];
    char signature_hex[CUBICLE_AUTH_SIGNATURE_BYTES * 2 + 1];
    assert(cubicle_auth_sign_file_key(private_key_path, transcript_bytes,
                                      transcript_length, signature) == 0);
    assert(cubicle_auth_hex_encode(signature, sizeof(signature),
                                   signature_hex, sizeof(signature_hex)) == 0);

    length = snprintf(params, sizeof(params), "{\"signature\":\"%s\"}",
                      signature_hex);
    assert(length >= 0 && (size_t)length < sizeof(params));
    char *session_json = rpc_call(fd, "auth-2", "", "auth.authenticate",
                                  params);

    cubicle_json_doc_t session_doc;
    assert(cubicle_json_parse(&session_doc, session_json) == 0);
    yyjson_val *session = yyjson_obj_get(session_doc.root, "result");
    assert(yyjson_is_obj(session));
    char session_id[CUBICLE_ID_STRING_LENGTH];
    char client_key_id[CUBICLE_ID_STRING_LENGTH];
    get_result_string(session, "session_id", session_id, sizeof(session_id));
    get_result_string(session, "client_key_id", client_key_id,
                      sizeof(client_key_id));
    assert(strcmp(session_id, "local-session") != 0);
    assert(strcmp(client_key_id, identity.key_id) == 0);

    char *bootstrap_json = rpc_call(fd, "auth-3", session_id,
                                    "session.local_bootstrap", "{}");
    cubicle_json_doc_t bootstrap_doc;
    assert(cubicle_json_parse(&bootstrap_doc, bootstrap_json) == 0);
    yyjson_val *bootstrap = yyjson_obj_get(bootstrap_doc.root, "result");
    assert(yyjson_is_obj(bootstrap));
    char bootstrap_session_id[CUBICLE_ID_STRING_LENGTH];
    get_result_string(bootstrap, "session_id", bootstrap_session_id,
                      sizeof(bootstrap_session_id));
    assert(strcmp(bootstrap_session_id, session_id) == 0);

    cubicle_json_cleanup(&challenge_doc);
    cubicle_json_cleanup(&session_doc);
    cubicle_json_cleanup(&bootstrap_doc);
    free(challenge_json);
    free(session_json);
    free(bootstrap_json);
    close(fd);
    return 0;
}
