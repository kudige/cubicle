#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cubicle_error_code_t cubicle_events_list(cubicle_client_t *client,
    const cubicle_event_query_t *query, cubicle_event_t **events_out,
    size_t *count_out)
{
    if (client == NULL || events_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0}; cubicle_json_builder_append(&params, "{"); bool comma = false;
    if (query != NULL && query->workspace_id != NULL) { cubicle_json_builder_append(&params, "\"workspace_id\":"); cubicle_json_builder_append_string(&params, query->workspace_id); comma = true; }
    if (query != NULL && query->process_id != NULL) { cubicle_json_builder_append(&params, comma ? ",\"process_id\":" : "\"process_id\":"); cubicle_json_builder_append_string(&params, query->process_id); comma = true; }
    if (query != NULL) cubicle_json_builder_appendf(&params, "%s\"after_sequence\":%llu,\"limit\":%zu", comma ? "," : "", (unsigned long long)query->after_sequence, query->limit);
    cubicle_json_builder_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "events.list", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    const char *array = json_array_field(result_object(client, response), "events");
    if (array == NULL) { free(response); return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing events array"); }
    size_t count = count_array_objects(array);
    cubicle_event_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    const char *cursor = array;
    for (size_t i = 0; i < count; ++i) {
        size_t length = 0; const char *object = next_array_object(cursor, &length);
        char *copy = copy_object_slice(object, length);
        if (copy == NULL) { free(items); free(response); return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to parse events"); }
        parse_event(copy, &items[i]); free(copy); cursor = object + length;
    }
    *events_out = items; *count_out = count; free(response); return CUBICLE_OK;
}

cubicle_error_code_t cubicle_events_subscribe(cubicle_client_t *client,
    const cubicle_event_query_t *query,
    cubicle_event_subscription_t **subscription_out)
{
    (void)query;
    if (client == NULL || subscription_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return unsupported_client(client, "event subscriptions are not implemented");
}

cubicle_error_code_t cubicle_events_next(cubicle_event_subscription_t *subscription,
                                         int timeout_ms,
                                         cubicle_event_t *event_out)
{
    (void)timeout_ms; (void)event_out;
    if (subscription == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(&subscription->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "event subscriptions are not implemented");
}

const cubicle_error_t *cubicle_events_subscription_last_error(
    const cubicle_event_subscription_t *subscription)
{
    return subscription == NULL ? NULL : &subscription->last_error;
}

void cubicle_events_unsubscribe(cubicle_event_subscription_t *subscription)
{
    free(subscription);
}

void cubicle_events_free(cubicle_event_t *events) { free(events); }

