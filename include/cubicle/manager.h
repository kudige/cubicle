#ifndef CUBICLE_MANAGER_H
#define CUBICLE_MANAGER_H

#include "cubicle/client_error.h"
#include "cubicle/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_client cubicle_client_t;

typedef struct cubicle_manager_status {
    cubicle_manager_id_t manager_id;
    uint32_t protocol_major;
    uint32_t protocol_minor;
    uint64_t started_at_ms;
    uint64_t server_time_ms;
    uint64_t workspace_count;
    uint64_t process_count;
    uint64_t controller_count;
    uint64_t active_client_sessions;
} cubicle_manager_status_t;

cubicle_error_code_t cubicle_manager_ping(
    cubicle_client_t *client);

cubicle_error_code_t cubicle_manager_status(
    cubicle_client_t *client,
    cubicle_manager_status_t *status_out);

cubicle_error_code_t cubicle_manager_reconcile(
    cubicle_client_t *client);

cubicle_error_code_t cubicle_manager_shutdown(
    cubicle_client_t *client,
    bool stop_managed_processes);

#ifdef __cplusplus
}
#endif

#endif
