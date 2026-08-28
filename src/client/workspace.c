#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cubicle_error_code_t cubicle_workspace_create(cubicle_client_t *client,
    const cubicle_workspace_create_options_t *options,
    cubicle_workspace_info_t *workspace_out)
{
    if (client == NULL || options == NULL || options->name == NULL ||
        options->name[0] == '\0' || workspace_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    if (cubicle_json_builder_append(&params, "{\"name\":") < 0 ||
        cubicle_json_builder_append_string(&params, options->name) < 0 ||
        (options->directory != NULL &&
         (cubicle_json_builder_append(&params, ",\"directory\":") < 0 ||
          cubicle_json_builder_append_string(&params, options->directory) < 0)) ||
        (options->initial_owner_label != NULL &&
         (cubicle_json_builder_append(&params, ",\"initial_owner_label\":") < 0 ||
          cubicle_json_builder_append_string(&params, options->initial_owner_label) < 0)) ||
        append_common_options(&params, &options->request) < 0 ||
        cubicle_json_builder_append(&params, "}") < 0) {
        cubicle_json_builder_cleanup(&params);
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to build request");
    }
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.create", params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    code = parse_workspace_info(result, workspace_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid workspace result");
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_get(cubicle_client_t *client,
    const char *workspace_id_or_name, cubicle_workspace_info_t *workspace_out)
{
    if (client == NULL || workspace_id_or_name == NULL ||
        workspace_id_or_name[0] == '\0' || workspace_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace\":");
    cubicle_json_builder_append_string(&params, workspace_id_or_name);
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.get", params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    code = parse_workspace_info(result, workspace_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid workspace result");
    free(response);
    return code;
}

static cubicle_error_code_t parse_workspace_list(cubicle_client_t *client,
    const char *result, cubicle_workspace_info_t **workspaces_out,
    size_t *count_out, cubicle_page_info_t *page_out)
{
    const char *array = json_array_field(result, "workspaces");
    if (array == NULL) return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing workspaces array");
    size_t count = json_array_field_count(result, "workspaces");
    cubicle_workspace_info_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count > 0 && items == NULL) return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to allocate workspaces");
    for (size_t i = 0; i < count; ++i) {
        char *copy = json_array_field_object_copy(result, "workspaces", i);
        if (copy == NULL || parse_workspace_info(copy, &items[i]) < 0) {
            free(copy); free(items);
            return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid workspace item");
        }
        free(copy);
    }
    parse_page(result, page_out);
    *workspaces_out = items;
    *count_out = count;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_workspace_list(cubicle_client_t *client,
    const cubicle_workspace_list_options_t *options,
    cubicle_workspace_info_t **workspaces_out, size_t *count_out,
    cubicle_page_info_t *page_out)
{
    if (client == NULL || workspaces_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{");
    bool comma = false;
    if (options != NULL && options->name_prefix != NULL) {
        cubicle_json_builder_append(&params, "\"name_prefix\":");
        cubicle_json_builder_append_string(&params, options->name_prefix);
        comma = true;
    }
    if (options != NULL && options->page.limit > 0) {
        cubicle_json_builder_appendf(&params, "%s\"limit\":%zu", comma ? "," : "", options->page.limit);
        comma = true;
    }
    if (options != NULL && options->page.continuation_token != NULL) {
        cubicle_json_builder_append(&params, comma ? ",\"continuation_token\":" : "\"continuation_token\":");
        cubicle_json_builder_append_string(&params, options->page.continuation_token);
    }
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.list", params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    code = parse_workspace_list(client, result_object(client, response), workspaces_out, count_out, page_out);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_rename(cubicle_client_t *client,
    const char *workspace_id, const char *new_name,
    const cubicle_request_options_t *request_options)
{
    if (client == NULL || workspace_id == NULL || new_name == NULL ||
        workspace_id[0] == '\0' || new_name[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":");
    cubicle_json_builder_append_string(&params, workspace_id);
    cubicle_json_builder_append(&params, ",\"new_name\":");
    cubicle_json_builder_append_string(&params, new_name);
    append_common_options(&params, request_options);
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.rename", params.data, &response);
    cubicle_json_builder_cleanup(&params); free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_stop(cubicle_client_t *client,
    const char *workspace_id, const cubicle_workspace_stop_options_t *options)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[256];
    snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\",\"grace_period_ms\":%d,\"force_after_grace\":%s}",
             workspace_id, options == NULL ? 0 : options->grace_period_ms,
             options != NULL && options->force_after_grace ? "true" : "false");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.stop", params, &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_delete(cubicle_client_t *client,
    const char *workspace_id, const cubicle_workspace_delete_options_t *options)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[256];
    snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\",\"stop_running_processes\":%s,\"remove_retained_processes\":%s}",
             workspace_id, options != NULL && options->stop_running_processes ? "true" : "false",
             options != NULL && options->remove_retained_processes ? "true" : "false");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.delete", params, &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_key_add(cubicle_client_t *client,
    const char *workspace_id, const unsigned char *public_key,
    size_t public_key_length, const char *label,
    cubicle_capability_mask_t capabilities, cubicle_workspace_key_info_t *key_out)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0' ||
        public_key == NULL || public_key_length == 0 || key_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":");
    cubicle_json_builder_append_string(&params, workspace_id);
    cubicle_json_builder_append(&params, ",\"public_key\":\"");
    for (size_t i = 0; i < public_key_length; ++i) cubicle_json_builder_appendf(&params, "%02x", public_key[i]);
    cubicle_json_builder_append(&params, "\",\"label\":");
    cubicle_json_builder_append_string(&params, label == NULL ? "" : label);
    cubicle_json_builder_appendf(&params, ",\"capabilities\":%llu}", (unsigned long long)capabilities);
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.key.add", params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    code = parse_key_info(result_object(client, response), key_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid key result");
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_key_list(cubicle_client_t *client,
    const char *workspace_id, cubicle_workspace_key_info_t **keys_out,
    size_t *count_out)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0' ||
        keys_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":");
    cubicle_json_builder_append_string(&params, workspace_id);
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.key.list", params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    const char *array = json_array_field(result, "keys");
    if (array == NULL) { free(response); return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing keys array"); }
    size_t count = json_array_field_count(result, "keys");
    cubicle_workspace_key_info_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    for (size_t i = 0; i < count; ++i) {
        char *copy = json_array_field_object_copy(result, "keys", i);
        if (copy == NULL || parse_key_info(copy, &items[i]) < 0) {
            free(copy); free(items); free(response);
            return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid key item");
        }
        free(copy);
    }
    *keys_out = items; *count_out = count; free(response); return CUBICLE_OK;
}

cubicle_error_code_t cubicle_workspace_key_set_capabilities(cubicle_client_t *client,
    const char *workspace_id, const char *key_id, cubicle_capability_mask_t capabilities)
{
    if (client == NULL || workspace_id == NULL || key_id == NULL ||
        workspace_id[0] == '\0' || key_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":"); cubicle_json_builder_append_string(&params, workspace_id);
    cubicle_json_builder_append(&params, ",\"key_id\":"); cubicle_json_builder_append_string(&params, key_id);
    cubicle_json_builder_appendf(&params, ",\"capabilities\":%llu}", (unsigned long long)capabilities);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "workspace.key.update", params.data, &response);
    cubicle_json_builder_cleanup(&params); free(response); return code;
}

cubicle_error_code_t cubicle_workspace_key_revoke(cubicle_client_t *client,
    const char *workspace_id, const char *key_id)
{
    if (client == NULL || workspace_id == NULL || key_id == NULL ||
        workspace_id[0] == '\0' || key_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":"); cubicle_json_builder_append_string(&params, workspace_id);
    cubicle_json_builder_append(&params, ",\"key_id\":"); cubicle_json_builder_append_string(&params, key_id);
    cubicle_json_builder_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "workspace.key.revoke", params.data, &response);
    cubicle_json_builder_cleanup(&params); free(response); return code;
}

static cubicle_error_code_t parse_macro_list(cubicle_client_t *client,
    const char *result, cubicle_workspace_macro_info_t **macros_out,
    size_t *count_out)
{
    const char *array = json_array_field(result, "macros");
    if (array == NULL) return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing macros array");
    size_t count = json_array_field_count(result, "macros");
    cubicle_workspace_macro_info_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count > 0 && items == NULL) return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to allocate macros");
    for (size_t i = 0; i < count; ++i) {
        char *copy = json_array_field_object_copy(result, "macros", i);
        if (copy == NULL || parse_macro_info(copy, &items[i]) < 0) {
            free(copy); free(items);
            return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid macro item");
        }
        free(copy);
    }
    *macros_out = items;
    *count_out = count;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_workspace_macro_list(cubicle_client_t *client,
    const char *workspace_id, cubicle_workspace_macro_info_t **macros_out,
    size_t *count_out)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0' ||
        macros_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":");
    cubicle_json_builder_append_string(&params, workspace_id);
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.macro.list", params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    code = parse_macro_list(client, result_object(client, response), macros_out, count_out);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_macro_save(cubicle_client_t *client,
    const cubicle_workspace_macro_save_options_t *options,
    cubicle_workspace_macro_info_t *macro_out)
{
    if (client == NULL || options == NULL || options->workspace_id == NULL ||
        options->workspace_id[0] == '\0' || options->ordinal == 0 ||
        options->name == NULL || options->name[0] == '\0' ||
        options->text == NULL || macro_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":");
    cubicle_json_builder_append_string(&params, options->workspace_id);
    cubicle_json_builder_appendf(&params, ",\"ordinal\":%llu,\"name\":", (unsigned long long)options->ordinal);
    cubicle_json_builder_append_string(&params, options->name);
    cubicle_json_builder_append(&params, ",\"text\":");
    cubicle_json_builder_append_string(&params, options->text);
    cubicle_json_builder_appendf(&params, ",\"target_pane\":%llu", (unsigned long long)options->target_pane);
    if (options->target_process_name != NULL) {
        cubicle_json_builder_append(&params, ",\"target_process_name\":");
        cubicle_json_builder_append_string(&params, options->target_process_name);
    }
    if (options->key_name != NULL) {
        cubicle_json_builder_append(&params, ",\"key_name\":");
        cubicle_json_builder_append_string(&params, options->key_name);
    }
    cubicle_json_builder_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.macro.save", params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) return code;
    code = parse_macro_info(result_object(client, response), macro_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid macro result");
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_macro_delete(cubicle_client_t *client,
    const char *workspace_id, uint64_t ordinal)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0' ||
        ordinal == 0) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":");
    cubicle_json_builder_append_string(&params, workspace_id);
    cubicle_json_builder_appendf(&params, ",\"ordinal\":%llu}", (unsigned long long)ordinal);
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.macro.delete", params.data, &response);
    cubicle_json_builder_cleanup(&params); free(response); return code;
}

cubicle_error_code_t cubicle_workspace_macro_reorder(cubicle_client_t *client,
    const char *workspace_id, uint64_t ordinal, uint64_t new_ordinal)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0' ||
        ordinal == 0 || new_ordinal == 0) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_json_builder_append(&params, "{\"workspace_id\":");
    cubicle_json_builder_append_string(&params, workspace_id);
    cubicle_json_builder_appendf(&params, ",\"ordinal\":%llu,\"new_ordinal\":%llu}",
                                 (unsigned long long)ordinal,
                                 (unsigned long long)new_ordinal);
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.macro.reorder", params.data, &response);
    cubicle_json_builder_cleanup(&params); free(response); return code;
}

void cubicle_workspace_list_free(cubicle_workspace_info_t *workspaces) { free(workspaces); }

void cubicle_workspace_key_list_free(cubicle_workspace_key_info_t *keys) { free(keys); }

void cubicle_workspace_macro_list_free(cubicle_workspace_macro_info_t *macros) { free(macros); }
