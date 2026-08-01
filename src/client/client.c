#include "internal.h"

#include "../common/auth_crypto.h"
#include "../common/auth_protocol.h"

#include "cubicle/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static cubicle_error_code_t authenticate_unix_session(cubicle_client_t *client)
{
    if (!endpoint_is_unix(&client->endpoint)) {
        return bootstrap_local_session(client);
    }

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
