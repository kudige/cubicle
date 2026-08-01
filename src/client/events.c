#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static cubicle_error_code_t build_events_params(
    cubicle_client_t *client,
    const cubicle_event_query_t *query,
    cubicle_json_builder_t *params)
{
    if (cubicle_json_builder_append(params, "{") < 0) {
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to build events request");
    }
    bool comma = false;
    if (query != NULL && query->workspace_id != NULL) {
        if (cubicle_json_builder_append(params, "\"workspace_id\":") < 0 ||
            cubicle_json_builder_append_string(params, query->workspace_id) < 0) {
            return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                    "failed to build events request");
        }
        comma = true;
    }
    if (query != NULL && query->process_id != NULL) {
        if (cubicle_json_builder_append(params,
                comma ? ",\"process_id\":" : "\"process_id\":") < 0 ||
            cubicle_json_builder_append_string(params, query->process_id) < 0) {
            return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                    "failed to build events request");
        }
        comma = true;
    }
    if (query != NULL &&
        cubicle_json_builder_appendf(
            params, "%s\"after_sequence\":%llu,\"limit\":%zu",
            comma ? "," : "", (unsigned long long)query->after_sequence,
            query->limit) < 0) {
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to build events request");
    }
    if (cubicle_json_builder_append(params, "}") < 0) {
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to build events request");
    }
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_events_list(cubicle_client_t *client,
    const cubicle_event_query_t *query, cubicle_event_t **events_out,
    size_t *count_out)
{
    if (client == NULL || events_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_error_code_t code = build_events_params(client, query, &params);
    if (code != CUBICLE_OK) {
        cubicle_json_builder_cleanup(&params);
        return code;
    }
    char *response = NULL; code = rpc_object(client, "events.list", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    const char *array = json_array_field(result, "events");
    if (array == NULL) { free(response); return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing events array"); }
    size_t count = json_array_field_count(result, "events");
    cubicle_event_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    for (size_t i = 0; i < count; ++i) {
        char *copy = json_array_field_object_copy(result, "events", i);
        if (copy == NULL) { free(items); free(response); return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to parse events"); }
        parse_event(copy, &items[i]); free(copy);
    }
    *events_out = items; *count_out = count; free(response); return CUBICLE_OK;
}

cubicle_error_code_t cubicle_events_subscribe(cubicle_client_t *client,
    const cubicle_event_query_t *query,
    cubicle_event_subscription_t **subscription_out)
{
    if (client == NULL || subscription_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0};
    cubicle_error_code_t code = build_events_params(client, query, &params);
    if (code != CUBICLE_OK) {
        cubicle_json_builder_cleanup(&params);
        return code;
    }
    char *response = NULL;
    code = rpc_object(client, "events.subscribe", params.data, &response);
    cubicle_json_builder_cleanup(&params);
    if (code != CUBICLE_OK) {
        return code;
    }
    free(response);

    cubicle_event_subscription_t *subscription =
        calloc(1, sizeof(*subscription));
    if (subscription == NULL) {
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to allocate event subscription");
    }
    subscription->client = client;
    subscription->after_sequence = query == NULL ? 0 : query->after_sequence;
    subscription->limit = query == NULL || query->limit == 0 ? 100
                                                            : query->limit;
    if (query != NULL && query->workspace_id != NULL) {
        snprintf(subscription->workspace_id, sizeof(subscription->workspace_id),
                 "%s", query->workspace_id);
    }
    if (query != NULL && query->process_id != NULL) {
        snprintf(subscription->process_id, sizeof(subscription->process_id),
                 "%s", query->process_id);
    }
    *subscription_out = subscription;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_events_next(cubicle_event_subscription_t *subscription,
                                         int timeout_ms,
                                         cubicle_event_t *event_out)
{
    if (subscription == NULL || event_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    int waited_ms = 0;
    for (;;) {
        if (subscription->pending_index < subscription->pending_count) {
            *event_out = subscription->pending[subscription->pending_index++];
            subscription->after_sequence = event_out->global_sequence;
            if (subscription->pending_index == subscription->pending_count) {
                cubicle_events_free(subscription->pending);
                subscription->pending = NULL;
                subscription->pending_count = 0;
                subscription->pending_index = 0;
            }
            memset(&subscription->last_error, 0,
                   sizeof(subscription->last_error));
            return CUBICLE_OK;
        }

        cubicle_event_query_t query;
        memset(&query, 0, sizeof(query));
        query.workspace_id = subscription->workspace_id[0] == '\0'
                                 ? NULL
                                 : subscription->workspace_id;
        query.process_id = subscription->process_id[0] == '\0'
                               ? NULL
                               : subscription->process_id;
        query.after_sequence = subscription->after_sequence;
        query.limit = subscription->limit;
        cubicle_error_code_t code = cubicle_events_list(
            subscription->client, &query, &subscription->pending,
            &subscription->pending_count);
        if (code != CUBICLE_OK) {
            const cubicle_error_t *error =
                cubicle_client_last_error(subscription->client);
            if (error != NULL) {
                subscription->last_error = *error;
            }
            return code;
        }
        if (subscription->pending_count > 0) {
            continue;
        }
        cubicle_events_free(subscription->pending);
        subscription->pending = NULL;

        if (waited_ms >= timeout_ms) {
            return set_error(&subscription->last_error,
                             CUBICLE_ERR_TIMEOUT, ETIMEDOUT, true,
                             "event subscription timed out");
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000L};
        nanosleep(&delay, NULL);
        waited_ms += 50;
    }
}

const cubicle_error_t *cubicle_events_subscription_last_error(
    const cubicle_event_subscription_t *subscription)
{
    return subscription == NULL ? NULL : &subscription->last_error;
}

void cubicle_events_unsubscribe(cubicle_event_subscription_t *subscription)
{
    if (subscription != NULL) {
        cubicle_events_free(subscription->pending);
    }
    free(subscription);
}

void cubicle_events_free(cubicle_event_t *events) { free(events); }
