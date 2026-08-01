#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cubicle_error_code_t parse_process_list(cubicle_client_t *client,
    const char *result, cubicle_process_info_t **processes_out,
    size_t *count_out, cubicle_page_info_t *page_out)
{
    const char *array = json_array_field(result, "processes");
    if (array == NULL) return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing processes array");
    size_t count = json_array_field_count(result, "processes");
    cubicle_process_info_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    for (size_t i = 0; i < count; ++i) {
        char *copy = json_array_field_object_copy(result, "processes", i);
        if (copy == NULL || parse_process_info(copy, &items[i]) < 0) {
            free(copy); free(items); return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid process item");
        }
        free(copy);
    }
    parse_page(result, page_out);
    *processes_out = items; *count_out = count; return CUBICLE_OK;
}

cubicle_error_code_t cubicle_process_start(cubicle_client_t *client,
    const cubicle_process_start_options_t *options,
    cubicle_process_info_t *process_out)
{
    if (client == NULL || options == NULL || process_out == NULL ||
        options->workspace_id == NULL || options->argv == NULL ||
        options->argc == 0) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":"); cubicle_json_builder_append_string(&params, options->workspace_id);
    if (options->friendly_name != NULL) { cubicle_json_builder_append(&params, ",\"friendly_name\":"); cubicle_json_builder_append_string(&params, options->friendly_name); }
    cubicle_json_builder_append(&params, ",\"mode\":"); cubicle_json_builder_append_string(&params, mode_name(options->mode));
    cubicle_json_builder_append(&params, ",\"stdin_policy\":"); cubicle_json_builder_append_string(&params, options->stdin_policy == CUBICLE_STDIN_EOF ? "eof" : "open");
    if (options->cwd != NULL) { cubicle_json_builder_append(&params, ",\"cwd\":"); cubicle_json_builder_append_string(&params, options->cwd); }
    cubicle_json_builder_append(&params, ",\"argv\":[");
    for (size_t i = 0; i < options->argc; ++i) {
        if (i > 0) cubicle_json_builder_append(&params, ",");
        cubicle_json_builder_append_string(&params, options->argv[i]);
    }
    cubicle_json_builder_append(&params, "]");
    if (options->tty_rows > 0) cubicle_json_builder_appendf(&params, ",\"tty_rows\":%u", options->tty_rows);
    if (options->tty_cols > 0) cubicle_json_builder_appendf(&params, ",\"tty_cols\":%u", options->tty_cols);
    append_common_options(&params, &options->request);
    cubicle_json_builder_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.start", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    code = parse_process_info(result_object(client, response), process_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid process result");
    free(response); return code;
}

cubicle_error_code_t cubicle_process_get(cubicle_client_t *client,
    const char *process_id_or_name, const char *workspace_id,
    cubicle_process_info_t *process_out)
{
    if (client == NULL || process_id_or_name == NULL || process_id_or_name[0] == '\0' || process_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0}; cubicle_json_builder_append(&params, "{\"process\":"); cubicle_json_builder_append_string(&params, process_id_or_name);
    if (workspace_id != NULL) { cubicle_json_builder_append(&params, ",\"workspace_id\":"); cubicle_json_builder_append_string(&params, workspace_id); }
    cubicle_json_builder_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.get", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    code = parse_process_info(result_object(client, response), process_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid process result");
    free(response); return code;
}

cubicle_error_code_t cubicle_process_list(cubicle_client_t *client,
    const cubicle_process_filter_t *filter, cubicle_process_info_t **processes_out,
    size_t *count_out, cubicle_page_info_t *page_out)
{
    if (client == NULL || processes_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0}; cubicle_json_builder_append(&params, "{"); bool comma = false;
    if (filter != NULL && filter->workspace_id != NULL) { cubicle_json_builder_append(&params, "\"workspace_id\":"); cubicle_json_builder_append_string(&params, filter->workspace_id); comma = true; }
    if (filter != NULL && filter->name_prefix != NULL) { cubicle_json_builder_append(&params, comma ? ",\"name_prefix\":" : "\"name_prefix\":"); cubicle_json_builder_append_string(&params, filter->name_prefix); comma = true; }
    if (filter != NULL && filter->include_completed) { cubicle_json_builder_append(&params, comma ? ",\"include_completed\":true" : "\"include_completed\":true"); }
    cubicle_json_builder_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.list", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    code = parse_process_list(client, result_object(client, response), processes_out, count_out, page_out);
    free(response); return code;
}

cubicle_error_code_t cubicle_process_signal(cubicle_client_t *client,
    const char *process_id, int signal_number)
{
    if (client == NULL || process_id == NULL || process_id[0] == '\0' || signal_number <= 0) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0}; cubicle_json_builder_append(&params, "{\"process_id\":"); cubicle_json_builder_append_string(&params, process_id);
    cubicle_json_builder_appendf(&params, ",\"signal_number\":%d}", signal_number);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.signal", params.data, &response);
    cubicle_json_builder_cleanup(&params); free(response); return code;
}

cubicle_error_code_t cubicle_process_terminate(cubicle_client_t *client,
    const char *process_id, const cubicle_process_terminate_options_t *options)
{
    if (client == NULL || process_id == NULL || process_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[256]; snprintf(params, sizeof(params), "{\"process_id\":\"%s\",\"grace_period_ms\":%d,\"force_after_grace\":%s}",
                               process_id, options == NULL ? 0 : options->grace_period_ms,
                               options != NULL && options->force_after_grace ? "true" : "false");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.terminate", params, &response);
    free(response); return code;
}

cubicle_error_code_t cubicle_process_kill(cubicle_client_t *client,
                                          const char *process_id)
{
    return simple_string_rpc(client, "process.kill", "process_id", process_id, NULL);
}

cubicle_error_code_t cubicle_process_wait(cubicle_client_t *client,
    const char *process_id, int timeout_ms, cubicle_process_info_t *process_out)
{
    if (client == NULL || process_id == NULL || process_id[0] == '\0' || process_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0}; cubicle_json_builder_append(&params, "{\"process_id\":"); cubicle_json_builder_append_string(&params, process_id);
    cubicle_json_builder_appendf(&params, ",\"timeout_ms\":%d}", timeout_ms);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.wait", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    code = parse_process_info(result_object(client, response), process_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid process result");
    free(response); return code;
}

cubicle_error_code_t cubicle_process_remove(cubicle_client_t *client,
                                            const char *process_id)
{
    return simple_string_rpc(client, "process.remove", "process_id", process_id, NULL);
}

cubicle_error_code_t cubicle_process_read_output(cubicle_client_t *client,
    const char *process_id, cubicle_stream_kind_t stream, uint64_t offset,
    size_t maximum_length, cubicle_output_chunk_t *chunk_out)
{
    if (client == NULL || process_id == NULL || process_id[0] == '\0' || chunk_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0}; cubicle_json_builder_append(&params, "{\"process_id\":"); cubicle_json_builder_append_string(&params, process_id);
    cubicle_json_builder_append(&params, ",\"stream\":"); cubicle_json_builder_append_string(&params, stream_name(stream));
    cubicle_json_builder_appendf(&params, ",\"offset\":%llu,\"maximum_length\":%zu}", (unsigned long long)offset, maximum_length);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.read_output", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(chunk_out, 0, sizeof(*chunk_out));
    json_u64_field(result, "start_offset", &chunk_out->start_offset);
    json_u64_field(result, "next_offset", &chunk_out->next_offset);
    json_bool_field(result, "end_of_stream", &chunk_out->end_of_stream);
    char data[4096];
    if (json_string_field(result, "data", data, sizeof(data)) == 0) {
        chunk_out->length = strlen(data);
        chunk_out->data = malloc(chunk_out->length);
        if (chunk_out->length > 0 && chunk_out->data == NULL) { free(response); return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to allocate output"); }
        memcpy(chunk_out->data, data, chunk_out->length);
    }
    free(response); return CUBICLE_OK;
}

void cubicle_process_list_free(cubicle_process_info_t *processes) { free(processes); }

void cubicle_output_chunk_free(cubicle_output_chunk_t *chunk) { if (chunk != NULL) { free(chunk->data); memset(chunk, 0, sizeof(*chunk)); } }
