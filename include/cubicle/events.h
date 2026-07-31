#ifndef CUBICLE_EVENTS_H
#define CUBICLE_EVENTS_H

#include "cubicle/client_error.h"
#include "cubicle/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_client cubicle_client_t;
typedef struct cubicle_event_subscription cubicle_event_subscription_t;

typedef enum cubicle_event_type {
    CUBICLE_EVENT_WORKSPACE_CREATED = 0,
    CUBICLE_EVENT_WORKSPACE_UPDATED,
    CUBICLE_EVENT_WORKSPACE_STOPPING,
    CUBICLE_EVENT_WORKSPACE_STOPPED,
    CUBICLE_EVENT_WORKSPACE_DELETED,
    CUBICLE_EVENT_PROCESS_STARTED,
    CUBICLE_EVENT_PROCESS_STATE_CHANGED,
    CUBICLE_EVENT_PROCESS_EXITED,
    CUBICLE_EVENT_OUTPUT_AVAILABLE,
    CUBICLE_EVENT_CLIENT_ATTACHED,
    CUBICLE_EVENT_CLIENT_DETACHED,
    CUBICLE_EVENT_CONTROLLER_LOST,
    CUBICLE_EVENT_CONTROLLER_RECOVERED,
    CUBICLE_EVENT_MANAGER_RECOVERED
} cubicle_event_type_t;

typedef struct cubicle_event {
    uint64_t global_sequence;
    uint64_t workspace_sequence;
    uint64_t timestamp_ms;
    cubicle_event_type_t type;
    cubicle_workspace_id_t workspace_id;
    cubicle_process_id_t process_id;
    char payload[CUBICLE_EVENT_PAYLOAD_MAX];
} cubicle_event_t;

typedef struct cubicle_event_query {
    const char *workspace_id;
    const char *process_id;
    uint64_t after_sequence;
    size_t limit;
} cubicle_event_query_t;

cubicle_error_code_t cubicle_events_list(
    cubicle_client_t *client,
    const cubicle_event_query_t *query,
    cubicle_event_t **events_out,
    size_t *count_out);

cubicle_error_code_t cubicle_events_subscribe(
    cubicle_client_t *client,
    const cubicle_event_query_t *query,
    cubicle_event_subscription_t **subscription_out);

cubicle_error_code_t cubicle_events_next(
    cubicle_event_subscription_t *subscription,
    int timeout_ms,
    cubicle_event_t *event_out);

const cubicle_error_t *cubicle_events_subscription_last_error(
    const cubicle_event_subscription_t *subscription);

void cubicle_events_unsubscribe(
    cubicle_event_subscription_t *subscription);

void cubicle_events_free(cubicle_event_t *events);

#ifdef __cplusplus
}
#endif

#endif
