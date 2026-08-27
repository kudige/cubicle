#ifndef CUBEUI_H
#define CUBEUI_H

#include "cubicle/config.h"
#include "cubicle/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

int cubeui_state_dir(char path[CUBICLE_PATH_MAX]);
int cubeui_selected_workspace_path(char path[CUBICLE_PATH_MAX]);
int cubeui_read_selected_workspace(char *buffer, size_t buffer_size);
int cubeui_store_selected_workspace(const char *workspace_name);
void cubeui_clear_selected_workspace_if_matches(const char *workspace);
int cubeui_read_desk_workspace(char *buffer, size_t buffer_size);
int cubeui_store_desk_workspace(const char *workspace_name);
void cubeui_clear_desk_workspace_if_matches(const char *workspace);
int cubeui_read_desk_layout(char *buffer, size_t buffer_size);
int cubeui_store_desk_layout(const char *layout_name);
void cubeui_clear_desk_layout(void);

const char *cubeui_resolve_manager_endpoint(
    const char *override_endpoint,
    const cubicle_config_t *config,
    char *configured_endpoint,
    size_t configured_endpoint_size);
int cubeui_endpoint_from_uri(cubicle_endpoint_t *endpoint, const char *uri);
int cubeui_autostart_manager(const char *manager_uri,
                             const cubicle_config_t *config,
                             int enabled,
                             char *error,
                             size_t error_size);

typedef struct cubeui_terminal {
    struct termios original;
    bool raw_enabled;
    int rows;
    int cols;
} cubeui_terminal_t;

int cubeui_write_all(int fd, const char *buffer, size_t length);
int cubeui_terminal_query_size(cubeui_terminal_t *terminal);
int cubeui_terminal_enter_alt_raw(cubeui_terminal_t *terminal);
void cubeui_terminal_leave_alt_raw(cubeui_terminal_t *terminal);

#endif
