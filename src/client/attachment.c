#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cubicle_error_code_t cubicle_attachment_request(cubicle_client_t *client,
    const cubicle_attachment_request_t *request,
    cubicle_attachment_grant_t *grant_out)
{
    if (client == NULL || request == NULL || request->process_id == NULL ||
        request->process_id[0] == '\0' || grant_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0}; cubicle_json_builder_append(&params, "{\"process_id\":"); cubicle_json_builder_append_string(&params, request->process_id);
    cubicle_json_builder_appendf(&params, ",\"channels\":%u,\"mode\":", (unsigned)request->channels);
    cubicle_json_builder_append_string(&params, attachment_mode_name(request->mode));
    cubicle_json_builder_appendf(&params, ",\"stdout_offset\":%llu,\"stderr_offset\":%llu,\"tty_offset\":%llu,\"rows\":%u,\"cols\":%u}",
                   (unsigned long long)request->stdout_offset, (unsigned long long)request->stderr_offset,
                   (unsigned long long)request->tty_offset, request->rows, request->cols);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "attachment.request", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(grant_out, 0, sizeof(*grant_out));
    json_string_field(result, "grant_id", grant_out->grant_id, sizeof(grant_out->grant_id));
    json_string_field(result, "manager_id", grant_out->manager_id, sizeof(grant_out->manager_id));
    json_string_field(result, "workspace_id", grant_out->workspace_id, sizeof(grant_out->workspace_id));
    json_string_field(result, "process_id", grant_out->process_id, sizeof(grant_out->process_id));
    json_string_field(result, "client_key_id", grant_out->client_key_id, sizeof(grant_out->client_key_id));
    json_string_field(result, "token", grant_out->token, sizeof(grant_out->token));
    json_u64_field(result, "issued_at_ms", &grant_out->issued_at_ms);
    json_u64_field(result, "expires_at_ms", &grant_out->expires_at_ms);
    uint64_t value = 0; if (json_u64_field(result, "connection_limit", &value) == 0) grant_out->connection_limit = (uint32_t)value;
    char channels[128]; if (json_string_field(result, "granted_channels", channels, sizeof(channels)) == 0) channel_mask_from_string(channels, &grant_out->granted_channels);
    char mode[32]; if (json_string_field(result, "mode", mode, sizeof(mode)) == 0 && strcmp(mode, "interactive") == 0) grant_out->mode = CUBICLE_ATTACHMENT_INTERACTIVE;
    const char *endpoint = json_object_field(result, "endpoint"); if (endpoint != NULL) parse_endpoint(endpoint, &grant_out->endpoint);
    free(response); return grant_out->grant_id[0] == '\0' ? set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid attachment grant") : CUBICLE_OK;
}

cubicle_error_code_t cubicle_attachment_connect(const cubicle_attachment_grant_t *grant,
    const cubicle_attachment_options_t *options, cubicle_attachment_t **attachment_out)
{
    (void)options;
    if (grant == NULL || attachment_out == NULL || grant->grant_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    return CUBICLE_ERR_UNSUPPORTED;
}

ssize_t cubicle_attachment_read(cubicle_attachment_t *attachment, void *buffer, size_t length)
{
    (void)buffer; (void)length;
    if (attachment == NULL) return -1;
    set_error(&attachment->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "attachment read is not implemented");
    return -1;
}

ssize_t cubicle_attachment_write(cubicle_attachment_t *attachment, const void *buffer, size_t length)
{
    (void)buffer; (void)length;
    if (attachment == NULL) return -1;
    set_error(&attachment->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "attachment write is not implemented");
    return -1;
}

cubicle_error_code_t cubicle_attachment_resize(cubicle_attachment_t *attachment,
                                               unsigned int rows, unsigned int cols)
{
    (void)rows; (void)cols;
    if (attachment == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(&attachment->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "attachment resize is not implemented");
}

cubicle_error_code_t cubicle_attachment_close_input(cubicle_attachment_t *attachment)
{
    if (attachment == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(&attachment->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "attachment close input is not implemented");
}

const cubicle_error_t *cubicle_attachment_last_error(const cubicle_attachment_t *attachment)
{
    return attachment == NULL ? NULL : &attachment->last_error;
}

void cubicle_attachment_disconnect(cubicle_attachment_t *attachment)
{
    free(attachment);
}

