#define _POSIX_C_SOURCE 200809L

#include "cubeui.h"

#include "cubicle/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ensure_parent_dir(const char *path)
{
    char parent[CUBICLE_PATH_MAX];
    int length = snprintf(parent, sizeof(parent), "%s", path);
    if (length < 0 || (size_t)length >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (slash == NULL) {
        return 0;
    }
    *slash = '\0';
    return cubicle_mkdir_p(parent);
}

int cubeui_state_dir(char path[CUBICLE_PATH_MAX])
{
    const char *state_home = getenv("XDG_STATE_HOME");
    if (state_home != NULL && state_home[0] != '\0') {
        int length = snprintf(path, CUBICLE_PATH_MAX, "%s/cubicle",
                              state_home);
        return length < 0 || length >= CUBICLE_PATH_MAX ? -1 : 0;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        int length = snprintf(path, CUBICLE_PATH_MAX, ".cubicle");
        return length < 0 || length >= CUBICLE_PATH_MAX ? -1 : 0;
    }
    int length = snprintf(path, CUBICLE_PATH_MAX, "%s/.local/state/cubicle",
                          home);
    return length < 0 || length >= CUBICLE_PATH_MAX ? -1 : 0;
}

int cubeui_selected_workspace_path(char path[CUBICLE_PATH_MAX])
{
    char state_dir[CUBICLE_PATH_MAX];
    if (cubeui_state_dir(state_dir) < 0) {
        return -1;
    }
    int length = snprintf(path, CUBICLE_PATH_MAX, "%s/current-workspace",
                          state_dir);
    return length < 0 || length >= CUBICLE_PATH_MAX ? -1 : 0;
}

int cubeui_read_selected_workspace(char *buffer, size_t buffer_size)
{
    char path[CUBICLE_PATH_MAX];
    if (cubeui_selected_workspace_path(path) < 0) {
        return -1;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    if (fgets(buffer, (int)buffer_size, file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);
    buffer[strcspn(buffer, "\n")] = '\0';
    return buffer[0] == '\0' ? -1 : 0;
}

int cubeui_store_selected_workspace(const char *workspace_name)
{
    char path[CUBICLE_PATH_MAX];
    if (cubeui_selected_workspace_path(path) < 0 ||
        ensure_parent_dir(path) < 0) {
        return -1;
    }

    char line[CUBICLE_NAME_MAX + 2];
    int length = snprintf(line, sizeof(line), "%s\n", workspace_name);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    int result = cubicle_write_all(fd, line, strlen(line));
    close(fd);
    return result;
}

void cubeui_clear_selected_workspace_if_matches(const char *workspace)
{
    char current[CUBICLE_NAME_MAX];
    char path[CUBICLE_PATH_MAX];
    if (workspace == NULL ||
        cubeui_read_selected_workspace(current, sizeof(current)) < 0 ||
        strcmp(current, workspace) != 0 ||
        cubeui_selected_workspace_path(path) < 0) {
        return;
    }
    (void)unlink(path);
}
