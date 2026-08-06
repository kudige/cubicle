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

typedef enum cubicle_config_source_kind {
    CUBICLE_CONFIG_SOURCE_BUILTIN = 1,
    CUBICLE_CONFIG_SOURCE_SYSTEM = 2,
    CUBICLE_CONFIG_SOURCE_USER = 3,
    CUBICLE_CONFIG_SOURCE_OVERRIDE = 4
} cubicle_config_source_kind_t;

typedef enum cubicle_config_key {
    CUBICLE_CONFIG_INSTALLATION_BINDIR = 0,
    CUBICLE_CONFIG_INSTALLATION_LIBEXECDIR,
    CUBICLE_CONFIG_MANAGER_STATE_DIR,
    CUBICLE_CONFIG_MANAGER_RUNTIME_DIR,
    CUBICLE_CONFIG_MANAGER_LOG_DIR,
    CUBICLE_CONFIG_MANAGER_LISTEN,
    CUBICLE_CONFIG_MANAGER_SOCKET_MODE,
    CUBICLE_CONFIG_MANAGER_SOCKET_GROUP,
    CUBICLE_CONFIG_MANAGER_CONTROLLER_BINARY,
    CUBICLE_CONFIG_CONTROLLER_DEBUG,
    CUBICLE_CONFIG_CUBE_DEBUG,
    CUBICLE_CONFIG_DESK_DEBUG,
    CUBICLE_CONFIG_CLIENT_MANAGER,
    CUBICLE_CONFIG_CLIENT_SERVER_IDENTITY,
    CUBICLE_CONFIG_DEFAULTS_LAUNCH,
    CUBICLE_CONFIG_DEFAULTS_MODE,
    CUBICLE_CONFIG_DEFAULTS_KILL_CLEANUP,
    CUBICLE_CONFIG_DESK_PREFIX,
    CUBICLE_CONFIG_DESK_KEYS,
    CUBICLE_CONFIG_KEY_COUNT
} cubicle_config_key_t;

#define CUBICLE_DESK_KEY_BINDING_MAX 64
#define CUBICLE_DESK_KEY_NAME_MAX 64
#define CUBICLE_DESK_COMMAND_NAME_MAX 64

typedef struct cubicle_desk_key_binding {
    int uses_prefix;
    unsigned char key;
    char key_name[CUBICLE_DESK_KEY_NAME_MAX];
    char command[CUBICLE_DESK_COMMAND_NAME_MAX];
    int unbind;
    int builtin;
} cubicle_desk_key_binding_t;

typedef struct cubicle_config_origin {
    cubicle_config_source_kind_t kind;
    char source_path[CUBICLE_PATH_MAX];
    unsigned int line_number;
} cubicle_config_origin_t;

typedef struct cubicle_config {
    char bindir[CUBICLE_PATH_MAX];
    char libexecdir[CUBICLE_PATH_MAX];
    char manager_state_dir[CUBICLE_PATH_MAX];
    char manager_runtime_dir[CUBICLE_PATH_MAX];
    char manager_log_dir[CUBICLE_PATH_MAX];
    char manager_listen_uri[CUBICLE_ENDPOINT_URI_MAX];
    unsigned int manager_socket_mode;
    char manager_socket_group[256];
    char controller_binary[CUBICLE_PATH_MAX];
    int controller_debug_input;
    int controller_debug_library;
    int controller_debug_terminal;
    int cube_debug_library;
    int desk_debug_library;
    int desk_debug_terminal;
    unsigned char desk_prefix_key;
    cubicle_desk_key_binding_t desk_key_bindings[CUBICLE_DESK_KEY_BINDING_MAX];
    size_t desk_key_binding_count;
    char client_manager_uri[CUBICLE_ENDPOINT_URI_MAX];
    char client_server_identity[256];
    cubicle_launch_default_t default_launch;
    cubicle_process_mode_t default_mode;
    int default_kill_cleanup;
    char source[CUBICLE_PATH_MAX];
    cubicle_config_origin_t origins[CUBICLE_CONFIG_KEY_COUNT];
} cubicle_config_t;

void cubicle_config_defaults(cubicle_config_t *config);
int cubicle_config_load(cubicle_config_t *config, char *error, size_t error_size);
int cubicle_config_load_client(cubicle_config_t *config,
                               char *error,
                               size_t error_size);
int cubicle_config_validate(const cubicle_config_t *config,
                            char *error,
                            size_t error_size);
int cubicle_config_unix_uri_path(const char *uri,
                                 char *path,
                                 size_t path_size);
const char *cubicle_launch_default_name(cubicle_launch_default_t launch);
const char *cubicle_config_key_name(cubicle_config_key_t key);
const cubicle_config_origin_t *cubicle_config_origin(
    const cubicle_config_t *config,
    cubicle_config_key_t key);
const char *cubicle_config_source_kind_name(cubicle_config_source_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif
