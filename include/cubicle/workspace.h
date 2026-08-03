#ifndef CUBICLE_WORKSPACE_H
#define CUBICLE_WORKSPACE_H

#include "cubicle/client_error.h"
#include "cubicle/types.h"
#include "cubicle/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_client cubicle_client_t;

typedef struct cubicle_workspace_info {
    cubicle_manager_id_t manager_id;
    cubicle_workspace_id_t id;
    char name[CUBICLE_NAME_MAX];
    char directory[CUBICLE_PATH_MAX];
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
    uint64_t process_count;
    uint64_t running_process_count;
} cubicle_workspace_info_t;

typedef struct cubicle_workspace_create_options {
    const char *name;
    const char *directory;
    const unsigned char *initial_owner_public_key;
    size_t initial_owner_public_key_length;
    const char *initial_owner_label;
    cubicle_request_options_t request;
} cubicle_workspace_create_options_t;

typedef struct cubicle_workspace_list_options {
    const char *name_prefix;
    cubicle_page_options_t page;
} cubicle_workspace_list_options_t;

typedef struct cubicle_workspace_stop_options {
    int grace_period_ms;
    bool force_after_grace;
} cubicle_workspace_stop_options_t;

typedef struct cubicle_workspace_delete_options {
    bool stop_running_processes;
    bool remove_retained_processes;
} cubicle_workspace_delete_options_t;

typedef uint64_t cubicle_capability_mask_t;

#define CUBICLE_CAP_WORKSPACE_READ        (UINT64_C(1) << 0)
#define CUBICLE_CAP_WORKSPACE_RENAME      (UINT64_C(1) << 1)
#define CUBICLE_CAP_WORKSPACE_STOP        (UINT64_C(1) << 2)
#define CUBICLE_CAP_WORKSPACE_DELETE      (UINT64_C(1) << 3)
#define CUBICLE_CAP_WORKSPACE_MANAGE_KEYS (UINT64_C(1) << 4)
#define CUBICLE_CAP_PROCESS_START         (UINT64_C(1) << 8)
#define CUBICLE_CAP_PROCESS_READ          (UINT64_C(1) << 9)
#define CUBICLE_CAP_PROCESS_OBSERVE       (UINT64_C(1) << 10)
#define CUBICLE_CAP_PROCESS_INPUT         (UINT64_C(1) << 11)
#define CUBICLE_CAP_PROCESS_SIGNAL        (UINT64_C(1) << 12)
#define CUBICLE_CAP_PROCESS_REMOVE        (UINT64_C(1) << 13)
#define CUBICLE_CAP_EVENTS_READ           (UINT64_C(1) << 16)

typedef struct cubicle_workspace_key_info {
    cubicle_key_id_t key_id;
    char fingerprint[CUBICLE_NAME_MAX];
    char label[CUBICLE_KEY_LABEL_MAX];
    cubicle_capability_mask_t capabilities;
    uint64_t created_at_ms;
    uint64_t revoked_at_ms;
} cubicle_workspace_key_info_t;

cubicle_error_code_t cubicle_workspace_create(cubicle_client_t *client, const cubicle_workspace_create_options_t *options, cubicle_workspace_info_t *workspace_out);
cubicle_error_code_t cubicle_workspace_get(cubicle_client_t *client, const char *workspace_id_or_name, cubicle_workspace_info_t *workspace_out);
cubicle_error_code_t cubicle_workspace_list(cubicle_client_t *client, const cubicle_workspace_list_options_t *options, cubicle_workspace_info_t **workspaces_out, size_t *count_out, cubicle_page_info_t *page_out);
cubicle_error_code_t cubicle_workspace_rename(cubicle_client_t *client, const char *workspace_id, const char *new_name, const cubicle_request_options_t *request_options);
cubicle_error_code_t cubicle_workspace_stop(cubicle_client_t *client, const char *workspace_id, const cubicle_workspace_stop_options_t *options);
cubicle_error_code_t cubicle_workspace_delete(cubicle_client_t *client, const char *workspace_id, const cubicle_workspace_delete_options_t *options);
cubicle_error_code_t cubicle_workspace_key_add(cubicle_client_t *client, const char *workspace_id, const unsigned char *public_key, size_t public_key_length, const char *label, cubicle_capability_mask_t capabilities, cubicle_workspace_key_info_t *key_out);
cubicle_error_code_t cubicle_workspace_key_list(cubicle_client_t *client, const char *workspace_id, cubicle_workspace_key_info_t **keys_out, size_t *count_out);
cubicle_error_code_t cubicle_workspace_key_set_capabilities(cubicle_client_t *client, const char *workspace_id, const char *key_id, cubicle_capability_mask_t capabilities);
cubicle_error_code_t cubicle_workspace_key_revoke(cubicle_client_t *client, const char *workspace_id, const char *key_id);

void cubicle_workspace_list_free(cubicle_workspace_info_t *workspaces);
void cubicle_workspace_key_list_free(cubicle_workspace_key_info_t *keys);

#ifdef __cplusplus
}
#endif

#endif
