#ifndef CUBICLE_CONFIG_H
#define CUBICLE_CONFIG_H

#include "cubicle/process.h"
#include "cubicle/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cubicle_launch_default {
    CUBICLE_LAUNCH_FOREGROUND = 1,
    CUBICLE_LAUNCH_BACKGROUND = 2
} cubicle_launch_default_t;

typedef struct cubicle_config {
    char bindir[CUBICLE_PATH_MAX];
    char libexecdir[CUBICLE_PATH_MAX];
    char manager_state_dir[CUBICLE_PATH_MAX];
    char manager_runtime_dir[CUBICLE_PATH_MAX];
    char manager_log_dir[CUBICLE_PATH_MAX];
    char manager_listen_uri[CUBICLE_ENDPOINT_URI_MAX];
    char controller_binary[CUBICLE_PATH_MAX];
    char client_manager_uri[CUBICLE_ENDPOINT_URI_MAX];
    char client_server_identity[256];
    cubicle_launch_default_t default_launch;
    cubicle_process_mode_t default_mode;
    char source[CUBICLE_PATH_MAX];
} cubicle_config_t;

void cubicle_config_defaults(cubicle_config_t *config);
int cubicle_config_load(cubicle_config_t *config, char *error, size_t error_size);
int cubicle_config_validate(const cubicle_config_t *config,
                            char *error,
                            size_t error_size);
int cubicle_config_unix_uri_path(const char *uri,
                                 char *path,
                                 size_t path_size);
const char *cubicle_launch_default_name(cubicle_launch_default_t launch);

#ifdef __cplusplus
}
#endif

#endif
