#include "internal.h"

#include "../common/auth_crypto.h"
#include "../common/auth_protocol.h"

#include "cubicle/transport_tcp.h"
#include "cubicle/transport_unix.h"
#include "cubicle/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct client_cached_session {
    cubicle_session_info_t session;
    unsigned char resume_secret[CUBICLE_AUTH_SECRET_BYTES];
    uint64_t manager_generation;
    uid_t peer_uid;
    gid_t peer_gid;
} client_cached_session_t;

static int client_auth_key_dir(char *path, size_t path_size)
{
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (config_home != NULL && config_home[0] != '\0') {
        int length = snprintf(path, path_size, "%s/cubicle/keys",
                              config_home);
        return length < 0 || (size_t)length >= path_size ? -1 : 0;
    }

    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        int length = snprintf(path, path_size, "%s/.config/cubicle/keys",
                              home);
        return length < 0 || (size_t)length >= path_size ? -1 : 0;
    }

    int length = snprintf(path, path_size, ".cubicle/keys");
    return length < 0 || (size_t)length >= path_size ? -1 : 0;
}

static int client_private_key_path(char *path, size_t path_size,
                                   const char *key_dir)
{
    int length = snprintf(path, path_size, "%s/client.key", key_dir);
    return length < 0 || (size_t)length >= path_size ? -1 : 0;
}

static uint64_t client_endpoint_hash(const char *text)
{
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        hash ^= (uint64_t)*cursor;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t client_now_ms(void)
{
    time_t now = time(NULL);
    if (now < 0) {
        return 0;
    }
    return (uint64_t)now * 1000ULL;
}

static int client_session_cache_path(const cubicle_endpoint_t *endpoint,
                                     char *directory,
                                     size_t directory_size,
                                     char *path,
                                     size_t path_size)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == NULL || runtime_dir[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    int length = snprintf(directory, directory_size,
                          "%s/cubicle/sessions/by-endpoint", runtime_dir);
    if (length < 0 || (size_t)length >= directory_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    uint64_t hash = client_endpoint_hash(endpoint->uri);
    length = snprintf(path, path_size, "%s/%016llx.session", directory,
                      (unsigned long long)hash);
    if (length < 0 || (size_t)length >= path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int cache_field(const char *data,
                       const char *key,
                       char *buffer,
                       size_t buffer_size)
{
    size_t key_length = strlen(key);
    const char *cursor = data;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_length =
            line_end == NULL ? strlen(cursor) : (size_t)(line_end - cursor);
        if (line_length > key_length && cursor[key_length] == '=' &&
            strncmp(cursor, key, key_length) == 0) {
            size_t value_length = line_length - key_length - 1;
            if (value_length >= buffer_size) {
                errno = ENOSPC;
                return -1;
            }
            memcpy(buffer, cursor + key_length + 1, value_length);
            buffer[value_length] = '\0';
            return 0;
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }
    errno = ENOENT;
    return -1;
}

static int cache_field_u64(const char *data, const char *key, uint64_t *out)
{
    char value[32];
    char *end = NULL;
    if (cache_field(data, key, value, sizeof(value)) < 0) {
        return -1;
    }
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        errno = EINVAL;
        return -1;
    }
    *out = (uint64_t)parsed;
    return 0;
}

static int load_cached_session(const cubicle_endpoint_t *endpoint,
                               client_cached_session_t *cached)
{
    char directory[CUBICLE_PATH_MAX];
    char path[CUBICLE_PATH_MAX];
    if (client_session_cache_path(endpoint, directory, sizeof(directory),
                                  path, sizeof(path)) < 0) {
        return -1;
    }

    struct stat status;
    if (stat(path, &status) < 0) {
        return -1;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != getuid() ||
        (status.st_mode & 0077) != 0 || status.st_size <= 0 ||
        status.st_size > 4096) {
        errno = EACCES;
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    char data[4097];
    size_t length = fread(data, 1, sizeof(data) - 1, file);
    int read_failed = ferror(file);
    fclose(file);
    if (read_failed) {
        errno = EIO;
        return -1;
    }
    data[length] = '\0';

    memset(cached, 0, sizeof(*cached));
    char resume_secret_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
    uint64_t value = 0;
    if (cache_field(data, "session_id", cached->session.session_id,
                    sizeof(cached->session.session_id)) < 0 ||
        cache_field(data, "manager_id", cached->session.manager_id,
                    sizeof(cached->session.manager_id)) < 0 ||
        cache_field(data, "client_key_id", cached->session.client_key_id,
                    sizeof(cached->session.client_key_id)) < 0 ||
        cache_field(data, "resume_secret", resume_secret_hex,
                    sizeof(resume_secret_hex)) < 0 ||
        cubicle_auth_hex_decode(resume_secret_hex, cached->resume_secret,
                                sizeof(cached->resume_secret)) < 0 ||
        cache_field_u64(data, "protocol_major", &value) < 0) {
        return -1;
    }
    cached->session.protocol_major = (uint32_t)value;
    if (cache_field_u64(data, "protocol_minor", &value) == 0) {
        cached->session.protocol_minor = (uint32_t)value;
    }
    (void)cache_field_u64(data, "negotiated_capabilities",
                          &cached->session.negotiated_capabilities);
    (void)cache_field_u64(data, "authenticated_at_ms",
                          &cached->session.authenticated_at_ms);
    (void)cache_field_u64(data, "expires_at_ms",
                          &cached->session.expires_at_ms);
    (void)cache_field_u64(data, "manager_generation",
                          &cached->manager_generation);
    if (cache_field_u64(data, "peer_uid", &value) == 0) {
        cached->peer_uid = (uid_t)value;
    }
    if (cache_field_u64(data, "peer_gid", &value) == 0) {
        cached->peer_gid = (gid_t)value;
    }
    if (cached->session.expires_at_ms > 0 &&
        cached->session.expires_at_ms <= client_now_ms()) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static void save_cached_session(const cubicle_endpoint_t *endpoint,
                                const cubicle_session_info_t *session,
                                const unsigned char *resume_secret,
                                uint64_t manager_generation,
                                uid_t peer_uid,
                                gid_t peer_gid)
{
    char directory[CUBICLE_PATH_MAX];
    char path[CUBICLE_PATH_MAX];
    if (client_session_cache_path(endpoint, directory, sizeof(directory),
                                  path, sizeof(path)) < 0 ||
        cubicle_mkdir_p(directory) < 0 ||
        chmod(directory, 0700) < 0) {
        return;
    }

    char resume_secret_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
    if (cubicle_auth_hex_encode(resume_secret, CUBICLE_AUTH_SECRET_BYTES,
                                resume_secret_hex,
                                sizeof(resume_secret_hex)) < 0) {
        return;
    }

    char temporary[CUBICLE_PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                          (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        return;
    }

    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return;
    }
    char content[1024];
    length = snprintf(
        content, sizeof(content),
        "session_id=%s\nmanager_id=%s\nclient_key_id=%s\nprotocol_major=%u\nprotocol_minor=%u\nnegotiated_capabilities=%llu\nauthenticated_at_ms=%llu\nexpires_at_ms=%llu\nresume_secret=%s\nmanager_generation=%llu\npeer_uid=%llu\npeer_gid=%llu\n",
        session->session_id, session->manager_id, session->client_key_id,
        session->protocol_major, session->protocol_minor,
        (unsigned long long)session->negotiated_capabilities,
        (unsigned long long)session->authenticated_at_ms,
        (unsigned long long)session->expires_at_ms, resume_secret_hex,
        (unsigned long long)manager_generation,
        (unsigned long long)peer_uid, (unsigned long long)peer_gid);
    int write_result =
        length < 0 || (size_t)length >= sizeof(content) ||
        cubicle_write_all(fd, content, (size_t)length) < 0 ||
        fsync(fd) < 0;
    int close_result = close(fd);
    if (write_result || close_result < 0) {
        unlink(temporary);
        return;
    }
    if (rename(temporary, path) < 0) {
        unlink(temporary);
    }
}

static cubicle_error_code_t parse_session_result(cubicle_client_t *client,
                                                 const char *result)
{
    uint64_t value = 0;
    memset(&client->session, 0, sizeof(client->session));
    if (json_string_field(result, "session_id",
                          client->session.session_id,
                          sizeof(client->session.session_id)) < 0 ||
        json_string_field(result, "manager_id",
                          client->session.manager_id,
                          sizeof(client->session.manager_id)) < 0 ||
        json_string_field(result, "client_key_id",
                          client->session.client_key_id,
                          sizeof(client->session.client_key_id)) < 0 ||
        json_u64_field(result, "protocol_major", &value) < 0) {
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "invalid session response");
    }
    client->session.protocol_major = (uint32_t)value;
    if (json_u64_field(result, "protocol_minor", &value) == 0) {
        client->session.protocol_minor = (uint32_t)value;
    }
    json_u64_field(result, "negotiated_capabilities",
                   &client->session.negotiated_capabilities);
    json_u64_field(result, "authenticated_at_ms",
                   &client->session.authenticated_at_ms);
    json_u64_field(result, "expires_at_ms",
                   &client->session.expires_at_ms);
    return CUBICLE_OK;
}

static cubicle_error_code_t bootstrap_local_session(cubicle_client_t *client)
{
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "session.local_bootstrap",
                                           "{}", &response);
    if (code != CUBICLE_OK) {
        return code;
    }
    const char *result = result_object(client, response);
    if (result == NULL) {
        free(response);
        return CUBICLE_ERR_PROTOCOL;
    }
    code = parse_session_result(client, result);
    free(response);
    return code;
}

static int endpoint_is_unix(const cubicle_endpoint_t *endpoint)
{
    return strncmp(endpoint->uri, "unix://", 7) == 0 ||
           strncmp(endpoint->uri, "tcp://", 6) != 0;
}

static cubicle_error_code_t resume_unix_session(cubicle_client_t *client)
{
    client_cached_session_t cached;
    if (load_cached_session(&client->endpoint, &cached) < 0) {
        return CUBICLE_ERR_SESSION_EXPIRED;
    }

    cubicle_auth_resume_t resume;
    memset(&resume, 0, sizeof(resume));
    snprintf(resume.manager_key_id, sizeof(resume.manager_key_id), "%s",
             cached.session.manager_id);
    snprintf(resume.session_id, sizeof(resume.session_id), "%s",
             cached.session.session_id);
    if (cubicle_auth_random_bytes(resume.client_nonce,
                                  sizeof(resume.client_nonce)) < 0 ||
        cubicle_auth_random_bytes(resume.connection_id,
                                  sizeof(resume.connection_id)) < 0) {
        return set_client_error(client, CUBICLE_ERR_INTERNAL, errno,
                                "failed to create resume nonce");
    }
    resume.manager_generation = cached.manager_generation;
    resume.peer_uid = cached.peer_uid;
    resume.peer_gid = cached.peer_gid;

    unsigned char resume_bytes[512];
    size_t resume_length = 0;
    unsigned char authenticator[CUBICLE_AUTH_SECRET_BYTES];
    char client_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
    char connection_id_hex[CUBICLE_AUTH_CONNECTION_ID_BYTES * 2 + 1];
    char authenticator_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
    if (cubicle_auth_encode_resume(&resume, resume_bytes,
                                   sizeof(resume_bytes),
                                   &resume_length) < 0 ||
        cubicle_auth_hmac_sha256(cached.resume_secret,
                                 sizeof(cached.resume_secret),
                                 resume_bytes, resume_length,
                                 authenticator) < 0 ||
        cubicle_auth_hex_encode(resume.client_nonce,
                                sizeof(resume.client_nonce),
                                client_nonce_hex,
                                sizeof(client_nonce_hex)) < 0 ||
        cubicle_auth_hex_encode(resume.connection_id,
                                sizeof(resume.connection_id),
                                connection_id_hex,
                                sizeof(connection_id_hex)) < 0 ||
        cubicle_auth_hex_encode(authenticator, sizeof(authenticator),
                                authenticator_hex,
                                sizeof(authenticator_hex)) < 0) {
        return set_client_error(client, CUBICLE_ERR_INTERNAL, errno,
                                "failed to create resume authenticator");
    }

    char params[512];
    int length = snprintf(
        params, sizeof(params),
        "{\"session_id\":\"%s\",\"client_nonce\":\"%s\",\"connection_id\":\"%s\",\"authenticator\":\"%s\"}",
        cached.session.session_id, client_nonce_hex, connection_id_hex,
        authenticator_hex);
    if (length < 0 || (size_t)length >= sizeof(params)) {
        return set_client_error(client, CUBICLE_ERR_RESOURCE_LIMIT, 0,
                                "auth resume request is too large");
    }

    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "auth.resume", params,
                                           &response);
    if (code != CUBICLE_OK) {
        return code;
    }

    const char *result = result_object(client, response);
    if (result == NULL) {
        free(response);
        return CUBICLE_ERR_PROTOCOL;
    }
    code = parse_session_result(client, result);
    if (code == CUBICLE_OK &&
        strcmp(client->session.session_id, cached.session.session_id) != 0) {
        code = set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "resumed session id changed");
    }
    free(response);
    return code;
}

static cubicle_error_code_t authenticate_unix_session(cubicle_client_t *client)
{
    if (!endpoint_is_unix(&client->endpoint)) {
        return bootstrap_local_session(client);
    }

    cubicle_error_code_t resume_code = resume_unix_session(client);
    if (resume_code == CUBICLE_OK) {
        return CUBICLE_OK;
    }
    memset(&client->last_error, 0, sizeof(client->last_error));

    char key_dir[CUBICLE_PATH_MAX];
    char private_key_path[CUBICLE_PATH_MAX];
    cubicle_auth_identity_t identity;
    if (client_auth_key_dir(key_dir, sizeof(key_dir)) < 0 ||
        client_private_key_path(private_key_path, sizeof(private_key_path),
                                key_dir) < 0 ||
        cubicle_auth_ensure_identity(key_dir, "client.key", "client.pub",
                                     &identity) < 0) {
        return set_client_error(client, CUBICLE_ERR_IO, errno,
                                "failed to initialize client identity");
    }

    unsigned char client_nonce[CUBICLE_AUTH_NONCE_BYTES];
    char client_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
    if (cubicle_auth_random_bytes(client_nonce, sizeof(client_nonce)) < 0 ||
        cubicle_auth_hex_encode(client_nonce, sizeof(client_nonce),
                                client_nonce_hex,
                                sizeof(client_nonce_hex)) < 0) {
        return set_client_error(client, CUBICLE_ERR_INTERNAL, errno,
                                "failed to create auth nonce");
    }

    char params[512];
    int length = snprintf(
        params, sizeof(params),
        "{\"client_public_key\":\"%s\",\"client_nonce\":\"%s\"}",
        identity.public_key_hex, client_nonce_hex);
    if (length < 0 || (size_t)length >= sizeof(params)) {
        return set_client_error(client, CUBICLE_ERR_RESOURCE_LIMIT, 0,
                                "auth challenge request is too large");
    }

    char *challenge_response = NULL;
    cubicle_error_code_t code = rpc_object(client, "auth.challenge", params,
                                           &challenge_response);
    if (code != CUBICLE_OK) {
        if (client->last_error.code == CUBICLE_ERR_UNSUPPORTED ||
            client->last_error.code == CUBICLE_ERR_PROTOCOL) {
            return bootstrap_local_session(client);
        }
        return code;
    }

    const char *challenge = result_object(client, challenge_response);
    if (challenge == NULL) {
        free(challenge_response);
        return CUBICLE_ERR_PROTOCOL;
    }

    char manager_public_key_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
    char manager_nonce_hex[CUBICLE_AUTH_NONCE_BYTES * 2 + 1];
    char connection_id_hex[CUBICLE_AUTH_CONNECTION_ID_BYTES * 2 + 1];
    cubicle_auth_transcript_t transcript;
    memset(&transcript, 0, sizeof(transcript));
    uint64_t value = 0;
    if (json_string_field(challenge, "manager_public_key",
                          manager_public_key_hex,
                          sizeof(manager_public_key_hex)) < 0 ||
        json_string_field(challenge, "manager_nonce", manager_nonce_hex,
                          sizeof(manager_nonce_hex)) < 0 ||
        json_string_field(challenge, "connection_id", connection_id_hex,
                          sizeof(connection_id_hex)) < 0 ||
        json_u64_field(challenge, "protocol_major", &value) < 0) {
        free(challenge_response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "invalid auth challenge response");
    }
    transcript.protocol_major = (uint32_t)value;
    if (json_u64_field(challenge, "protocol_minor", &value) == 0) {
        transcript.protocol_minor = (uint32_t)value;
    }
    if (cubicle_auth_hex_decode(manager_public_key_hex,
                                transcript.manager_public_key,
                                sizeof(transcript.manager_public_key)) < 0 ||
        cubicle_auth_hex_decode(manager_nonce_hex,
                                transcript.manager_nonce,
                                sizeof(transcript.manager_nonce)) < 0 ||
        cubicle_auth_hex_decode(connection_id_hex,
                                transcript.connection_id,
                                sizeof(transcript.connection_id)) < 0) {
        free(challenge_response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, errno,
                                "invalid auth challenge encoding");
    }
    memcpy(transcript.client_public_key, identity.public_key,
           sizeof(transcript.client_public_key));
    memcpy(transcript.client_nonce, client_nonce,
           sizeof(transcript.client_nonce));
    json_u64_field(challenge, "capabilities", &transcript.capabilities);
    json_u64_field(challenge, "manager_generation",
                   &transcript.manager_generation);
    if (json_u64_field(challenge, "peer_uid", &value) == 0) {
        transcript.peer_uid = (uid_t)value;
    }
    if (json_u64_field(challenge, "peer_gid", &value) == 0) {
        transcript.peer_gid = (gid_t)value;
    }
    free(challenge_response);

    unsigned char transcript_bytes[512];
    size_t transcript_length = 0;
    unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES];
    char signature_hex[CUBICLE_AUTH_SIGNATURE_BYTES * 2 + 1];
    if (cubicle_auth_encode_transcript(&transcript, transcript_bytes,
                                       sizeof(transcript_bytes),
                                       &transcript_length) < 0 ||
        cubicle_auth_sign_file_key(private_key_path, transcript_bytes,
                                   transcript_length, signature) < 0 ||
        cubicle_auth_hex_encode(signature, sizeof(signature), signature_hex,
                                sizeof(signature_hex)) < 0) {
        return set_client_error(client, CUBICLE_ERR_AUTHENTICATION_FAILED,
                                errno, "failed to sign auth transcript");
    }

    length = snprintf(params, sizeof(params), "{\"signature\":\"%s\"}",
                      signature_hex);
    if (length < 0 || (size_t)length >= sizeof(params)) {
        return set_client_error(client, CUBICLE_ERR_RESOURCE_LIMIT, 0,
                                "auth signature request is too large");
    }

    char *session_response = NULL;
    code = rpc_object(client, "auth.authenticate", params, &session_response);
    if (code != CUBICLE_OK) {
        return code;
    }
    const char *session = result_object(client, session_response);
    if (session == NULL) {
        free(session_response);
        return CUBICLE_ERR_PROTOCOL;
    }
    code = parse_session_result(client, session);
    if (code == CUBICLE_OK) {
        unsigned char resume_secret[CUBICLE_AUTH_SECRET_BYTES];
        char resume_secret_hex[CUBICLE_AUTH_SECRET_BYTES * 2 + 1];
        if (json_string_field(session, "resume_secret", resume_secret_hex,
                              sizeof(resume_secret_hex)) == 0 &&
            cubicle_auth_hex_decode(resume_secret_hex, resume_secret,
                                    sizeof(resume_secret)) == 0) {
            save_cached_session(&client->endpoint, &client->session,
                                resume_secret,
                                transcript.manager_generation,
                                transcript.peer_uid,
                                transcript.peer_gid);
        }
    }
    free(session_response);
    return code;
}

cubicle_error_code_t cubicle_client_connect(const cubicle_client_options_t *options,
                                            cubicle_client_t **client_out)
{
    if (options == NULL || client_out == NULL || options->transport == NULL ||
        options->transport->vtable == NULL ||
        options->transport->vtable->connect == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    cubicle_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return CUBICLE_ERR_INTERNAL;
    }
    client->endpoint = options->endpoint;
    client->connect_timeout_ms = options->connect_timeout_ms;
    client->request_timeout_ms = options->request_timeout_ms;
    client->transport = options->transport;
    client->next_request_id = 0;
    cubicle_error_code_t result = client->transport->vtable->connect(
        client->transport, &client->endpoint, &client->last_error);
    if (result != CUBICLE_OK) {
        free(client);
        return result;
    }
    result = authenticate_unix_session(client);
    if (result != CUBICLE_OK) {
        if (client->transport->vtable->close != NULL) {
            client->transport->vtable->close(client->transport);
        }
        free(client);
        return result;
    }
    *client_out = client;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_client_connect_uri(
    const char *uri,
    const cubicle_auth_options_t *auth,
    cubicle_client_t **client_out)
{
    if (uri == NULL || uri[0] == '\0' || client_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    cubicle_endpoint_t endpoint;
    memset(&endpoint, 0, sizeof(endpoint));
    if (uri[0] == '/') {
        int length = snprintf(endpoint.uri, sizeof(endpoint.uri),
                              "unix://%s", uri);
        if (length < 0 || (size_t)length >= sizeof(endpoint.uri)) {
            return CUBICLE_ERR_INVALID_ARGUMENT;
        }
    } else {
        int length = snprintf(endpoint.uri, sizeof(endpoint.uri), "%s", uri);
        if (length < 0 || (size_t)length >= sizeof(endpoint.uri)) {
            return CUBICLE_ERR_INVALID_ARGUMENT;
        }
    }

    cubicle_transport_t *transport = NULL;
    cubicle_error_code_t code;
    if (strncmp(endpoint.uri, "tcp://", 6) == 0) {
        code = cubicle_transport_tcp_create(&transport);
    } else {
        code = cubicle_transport_unix_create(&transport);
    }
    if (code != CUBICLE_OK) {
        return code;
    }

    cubicle_client_options_t options;
    memset(&options, 0, sizeof(options));
    options.endpoint = endpoint;
    options.transport = transport;
    if (auth != NULL) {
        options.auth = *auth;
    }

    code = cubicle_client_connect(&options, client_out);
    if (code != CUBICLE_OK && transport->vtable != NULL &&
        transport->vtable->destroy != NULL) {
        transport->vtable->destroy(transport);
    }
    return code;
}

void cubicle_client_disconnect(cubicle_client_t *client)
{
    if (client == NULL) return;
    if (client->transport != NULL && client->transport->vtable != NULL) {
        if (client->transport->vtable->close != NULL) client->transport->vtable->close(client->transport);
        if (client->transport->vtable->destroy != NULL) client->transport->vtable->destroy(client->transport);
    }
    free(client);
}

const cubicle_error_t *cubicle_client_last_error(const cubicle_client_t *client)
{
    return client == NULL ? NULL : &client->last_error;
}

cubicle_error_code_t cubicle_client_session_info(const cubicle_client_t *client,
                                                 cubicle_session_info_t *session_out)
{
    if (client == NULL || session_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    *session_out = client->session;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_client_call_json(
    cubicle_client_t *client,
    const char *method,
    const char *params_json,
    char **result_json_out)
{
    if (client == NULL || method == NULL || result_json_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, method, params_json,
                                           &response);
    if (code != CUBICLE_OK) {
        return code;
    }
    const char *result = result_object(client, response);
    if (result == NULL) {
        free(response);
        return CUBICLE_ERR_PROTOCOL;
    }
    size_t length = strlen(result);
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to allocate result JSON");
    }
    memcpy(copy, result, length + 1);
    free(response);
    *result_json_out = copy;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_ping(cubicle_client_t *client,
                                          cubicle_manager_ping_result_t *result_out)
{
    if (client == NULL || result_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.ping", "{}", &response);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(result_out, 0, sizeof(*result_out));
    if (json_string_field(result, "manager_id", result_out->manager_id,
                          sizeof(result_out->manager_id)) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "manager.ping result missing manager_id");
    }
    uint64_t value = 0;
    if (json_u64_field(result, "protocol_major", &value) == 0) result_out->protocol_major = (uint32_t)value;
    if (json_u64_field(result, "protocol_minor", &value) == 0) result_out->protocol_minor = (uint32_t)value;
    json_u64_field(result, "server_time_ms", &result_out->server_time_ms);
    json_u64_field(result, "uptime_ms", &result_out->uptime_ms);
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_status(cubicle_client_t *client,
                                            cubicle_manager_status_t *status_out)
{
    if (client == NULL || status_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.status", "{}", &response);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(status_out, 0, sizeof(*status_out));
    if (json_string_field(result, "manager_id", status_out->manager_id,
                          sizeof(status_out->manager_id)) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "manager.status result missing manager_id");
    }
    uint64_t value = 0;
    if (json_u64_field(result, "protocol_major", &value) == 0) status_out->protocol_major = (uint32_t)value;
    if (json_u64_field(result, "protocol_minor", &value) == 0) status_out->protocol_minor = (uint32_t)value;
    json_u64_field(result, "capabilities", &status_out->capabilities);
    json_u64_field(result, "started_at_ms", &status_out->started_at_ms);
    json_u64_field(result, "server_time_ms", &status_out->server_time_ms);
    json_u64_field(result, "workspace_count", &status_out->workspace_count);
    json_u64_field(result, "process_count", &status_out->process_count);
    json_u64_field(result, "controller_count", &status_out->controller_count);
    json_u64_field(result, "active_client_sessions", &status_out->active_client_sessions);
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_cleanup(
    cubicle_client_t *client,
    const char *workspace_id,
    cubicle_manager_cleanup_result_t *result_out)
{
    if (client == NULL || result_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{");
    if (workspace_id != NULL && workspace_id[0] != '\0') {
        cubicle_json_builder_append(&params, "\"workspace_id\":");
        cubicle_json_builder_append_string(&params, workspace_id);
    }
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.cleanup",
                                           params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(result_out, 0, sizeof(*result_out));
    if (json_u64_field(result, "removed_count",
                       &result_out->removed_count) < 0 ||
        json_u64_field(result, "skipped_live_count",
                       &result_out->skipped_live_count) < 0 ||
        json_u64_field(result, "skipped_saved_count",
                       &result_out->skipped_saved_count) < 0 ||
        json_u64_field(result, "failed_count",
                       &result_out->failed_count) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "invalid manager.cleanup result");
    }
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_reconcile(cubicle_client_t *client)
{
    if (client == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.reconcile", "{}", &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_manager_shutdown(cubicle_client_t *client,
                                              bool stop_managed_processes)
{
    if (client == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[64];
    snprintf(params, sizeof(params), "{\"stop_managed_processes\":%s}",
             stop_managed_processes ? "true" : "false");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.shutdown", params, &response);
    free(response);
    return code;
}
