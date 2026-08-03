#define _POSIX_C_SOURCE 200809L

#include "cubicle/config.h"

#include <errno.h>
#include <libeconf.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

static int copy_string(char *destination,
                       size_t destination_size,
                       const char *value,
                       const char *name,
                       char *error,
                       size_t error_size)
{
    if (value == NULL || value[0] == '\0') {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "%s must not be empty", name);
        }
        return -1;
    }

    int length = snprintf(destination, destination_size, "%s", value);
    if (length < 0 || (size_t)length >= destination_size) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "%s is too long", name);
        }
        return -1;
    }

    return 0;
}

static int is_absolute_path(const char *path)
{
    return path != NULL && path[0] == '/';
}

static int validate_absolute_path(const char *path,
                                  const char *name,
                                  char *error,
                                  size_t error_size)
{
    if (!is_absolute_path(path)) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "%s must be an absolute path", name);
        }
        return -1;
    }
    return 0;
}

static int parse_launch(const char *value,
                        cubicle_launch_default_t *launch,
                        char *error,
                        size_t error_size)
{
    if (strcmp(value, "foreground") == 0) {
        *launch = CUBICLE_LAUNCH_FOREGROUND;
        return 0;
    }
    if (strcmp(value, "background") == 0) {
        *launch = CUBICLE_LAUNCH_BACKGROUND;
        return 0;
    }
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size,
                 "defaults.launch must be foreground or background");
    }
    return -1;
}

static int parse_mode(const char *value,
                      cubicle_process_mode_t *mode,
                      char *error,
                      size_t error_size)
{
    if (strcmp(value, "stream") == 0) {
        *mode = CUBICLE_PROCESS_STREAM;
        return 0;
    }
    if (strcmp(value, "tty") == 0) {
        *mode = CUBICLE_PROCESS_TTY;
        return 0;
    }
    if (strcmp(value, "term") == 0) {
        *mode = CUBICLE_PROCESS_TTY_CAPTURED_STDERR;
        return 0;
    }
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size,
                 "defaults.mode must be stream, tty, or term");
    }
    return -1;
}

static int parse_bool(const char *value,
                      int *parsed,
                      const char *name,
                      char *error,
                      size_t error_size)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "1") == 0 || strcmp(value, "on") == 0) {
        *parsed = 1;
        return 0;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "0") == 0 || strcmp(value, "off") == 0) {
        *parsed = 0;
        return 0;
    }
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size,
                 "%s must be true or false", name);
    }
    return -1;
}

static int parse_socket_mode(const char *value,
                             unsigned int *mode,
                             char *error,
                             size_t error_size)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 8);
    if (errno != 0 || end == value || *end != '\0' || parsed > 0777) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size,
                     "manager.socket_mode must be an octal mode such as 0660");
        }
        return -1;
    }
    *mode = (unsigned int)parsed;
    return 0;
}

static const char *user_home_directory(void)
{
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        return home;
    }

    struct passwd *entry = getpwuid(geteuid());
    return entry != NULL ? entry->pw_dir : NULL;
}

static int set_user_defaults(cubicle_config_t *config)
{
    uid_t uid = geteuid();
    const char *xdg_state_home = getenv("XDG_STATE_HOME");
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char *home = user_home_directory();

    int result;
    if (xdg_state_home != NULL && xdg_state_home[0] != '\0') {
        result = snprintf(config->manager_state_dir,
                          sizeof(config->manager_state_dir),
                          "%s/cubicle", xdg_state_home);
    } else if (home != NULL && home[0] != '\0') {
        result = snprintf(config->manager_state_dir,
                          sizeof(config->manager_state_dir),
                          "%s/.local/state/cubicle", home);
    } else {
        result = snprintf(config->manager_state_dir,
                          sizeof(config->manager_state_dir),
                          "/tmp/cubicle-state-%ld", (long)uid);
    }
    if (result < 0 || (size_t)result >= sizeof(config->manager_state_dir)) {
        return -1;
    }

    result = snprintf(config->manager_log_dir,
                      sizeof(config->manager_log_dir),
                      "%s/log", config->manager_state_dir);
    if (result < 0 || (size_t)result >= sizeof(config->manager_log_dir)) {
        return -1;
    }

    if (runtime_dir != NULL && runtime_dir[0] != '\0') {
        result = snprintf(config->manager_runtime_dir,
                          sizeof(config->manager_runtime_dir),
                          "%s/cubicle", runtime_dir);
    } else {
        result = snprintf(config->manager_runtime_dir,
                          sizeof(config->manager_runtime_dir),
                          "/run/user/%ld/cubicle", (long)uid);
    }
    if (result < 0 || (size_t)result >= sizeof(config->manager_runtime_dir)) {
        return -1;
    }

    result = snprintf(config->manager_listen_uri,
                      sizeof(config->manager_listen_uri),
                      "unix://%s/manager.sock", config->manager_runtime_dir);
    if (result < 0 || (size_t)result >= sizeof(config->manager_listen_uri)) {
        return -1;
    }

    result = snprintf(config->client_manager_uri,
                      sizeof(config->client_manager_uri),
                      "%s", config->manager_listen_uri);
    if (result < 0 || (size_t)result >= sizeof(config->client_manager_uri)) {
        return -1;
    }

    return 0;
}

void cubicle_config_defaults(cubicle_config_t *config)
{
    memset(config, 0, sizeof(*config));
    snprintf(config->bindir, sizeof(config->bindir), "/usr/bin");
    snprintf(config->libexecdir, sizeof(config->libexecdir),
             "/usr/libexec/cubicle");
    snprintf(config->manager_state_dir, sizeof(config->manager_state_dir),
             "/var/lib/cubicle");
    snprintf(config->manager_runtime_dir, sizeof(config->manager_runtime_dir),
             "/run/cubicle");
    snprintf(config->manager_log_dir, sizeof(config->manager_log_dir),
             "/var/log/cubicle");
    snprintf(config->manager_listen_uri, sizeof(config->manager_listen_uri),
             "unix:///run/cubicle/manager.sock");
    config->manager_socket_mode = 0660;
    config->manager_socket_group[0] = '\0';
    snprintf(config->controller_binary, sizeof(config->controller_binary),
             "/usr/libexec/cubicle/cubicle-controller");
    snprintf(config->client_manager_uri, sizeof(config->client_manager_uri),
             "unix:///run/cubicle/manager.sock");
    if (geteuid() != 0 && set_user_defaults(config) < 0) {
        snprintf(config->manager_state_dir, sizeof(config->manager_state_dir),
                 "/tmp/cubicle-state-%ld", (long)geteuid());
        snprintf(config->manager_runtime_dir,
                 sizeof(config->manager_runtime_dir),
                 "/run/user/%ld/cubicle", (long)geteuid());
        snprintf(config->manager_log_dir, sizeof(config->manager_log_dir),
                 "/tmp/cubicle-log-%ld", (long)geteuid());
        snprintf(config->manager_listen_uri,
                 sizeof(config->manager_listen_uri),
                 "unix:///run/user/%ld/cubicle/manager.sock",
                 (long)geteuid());
        snprintf(config->client_manager_uri,
                 sizeof(config->client_manager_uri), "%s",
                 config->manager_listen_uri);
    }
    config->default_launch = CUBICLE_LAUNCH_FOREGROUND;
    config->default_mode = CUBICLE_PROCESS_TTY;
    config->default_kill_cleanup = 0;
    snprintf(config->source, sizeof(config->source), "built-in defaults");
}

static int get_optional_string(econf_file *file,
                               const char *group,
                               const char *key,
                               char *destination,
                               size_t destination_size,
                               char *error,
                               size_t error_size)
{
    char *value = NULL;
    econf_err result = econf_getStringValue(file, group, key, &value);
    if (result == ECONF_NOKEY || result == ECONF_NOGROUP) {
        return 0;
    }
    if (result != ECONF_SUCCESS) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "%s.%s: %s", group, key,
                     econf_errString(result));
        }
        return -1;
    }

    int copy_result = copy_string(destination, destination_size, value, key,
                                  error, error_size);
    free(value);
    return copy_result;
}

static int apply_econf_file(cubicle_config_t *config,
                            econf_file *file,
                            char *error,
                            size_t error_size)
{
    if (get_optional_string(file, "installation", "bindir",
                            config->bindir, sizeof(config->bindir),
                            error, error_size) < 0 ||
        get_optional_string(file, "installation", "libexecdir",
                            config->libexecdir, sizeof(config->libexecdir),
                            error, error_size) < 0 ||
        get_optional_string(file, "manager", "state_dir",
                            config->manager_state_dir,
                            sizeof(config->manager_state_dir),
                            error, error_size) < 0 ||
        get_optional_string(file, "manager", "runtime_dir",
                            config->manager_runtime_dir,
                            sizeof(config->manager_runtime_dir),
                            error, error_size) < 0 ||
        get_optional_string(file, "manager", "log_dir",
                            config->manager_log_dir,
                            sizeof(config->manager_log_dir),
                            error, error_size) < 0 ||
        get_optional_string(file, "manager", "listen",
                            config->manager_listen_uri,
                            sizeof(config->manager_listen_uri),
                            error, error_size) < 0 ||
        get_optional_string(file, "manager", "socket_group",
                            config->manager_socket_group,
                            sizeof(config->manager_socket_group),
                            error, error_size) < 0 ||
        get_optional_string(file, "manager", "controller_binary",
                            config->controller_binary,
                            sizeof(config->controller_binary),
                            error, error_size) < 0 ||
        get_optional_string(file, "client", "manager",
                            config->client_manager_uri,
                            sizeof(config->client_manager_uri),
                            error, error_size) < 0 ||
        get_optional_string(file, "client", "server_identity",
                            config->client_server_identity,
                            sizeof(config->client_server_identity),
                            error, error_size) < 0) {
        return -1;
    }

    char value[64];
    value[0] = '\0';
    if (get_optional_string(file, "defaults", "launch", value, sizeof(value),
                            error, error_size) < 0 ||
        (value[0] != '\0' &&
         parse_launch(value, &config->default_launch, error, error_size) < 0)) {
        return -1;
    }

    value[0] = '\0';
    if (get_optional_string(file, "manager", "socket_mode", value,
                            sizeof(value), error, error_size) < 0 ||
        (value[0] != '\0' &&
         parse_socket_mode(value, &config->manager_socket_mode,
                           error, error_size) < 0)) {
        return -1;
    }

    value[0] = '\0';
    if (get_optional_string(file, "defaults", "mode", value, sizeof(value),
                            error, error_size) < 0 ||
        (value[0] != '\0' &&
         parse_mode(value, &config->default_mode, error, error_size) < 0)) {
        return -1;
    }

    value[0] = '\0';
    if (get_optional_string(file, "defaults", "kill_cleanup", value,
                            sizeof(value), error, error_size) < 0 ||
        (value[0] != '\0' &&
         parse_bool(value, &config->default_kill_cleanup,
                    "defaults.kill_cleanup", error, error_size) < 0)) {
        return -1;
    }

    return 0;
}

int cubicle_config_unix_uri_path(const char *uri, char *path, size_t path_size)
{
    const char prefix[] = "unix://";
    if (uri == NULL || strncmp(uri, prefix, strlen(prefix)) != 0) {
        errno = EINVAL;
        return -1;
    }
    const char *socket_path = uri + strlen(prefix);
    if (!is_absolute_path(socket_path)) {
        errno = EINVAL;
        return -1;
    }
    int result = snprintf(path, path_size, "%s", socket_path);
    if (result < 0 || (size_t)result >= path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int validate_tcp_uri(const char *uri)
{
    const char prefix[] = "tcp://";
    if (uri == NULL || strncmp(uri, prefix, strlen(prefix)) != 0) {
        errno = EINVAL;
        return -1;
    }

    const char *authority = uri + strlen(prefix);
    const char *port = NULL;
    if (authority[0] == '[') {
        const char *end = strchr(authority, ']');
        if (end == NULL || end[1] != ':' || end == authority + 1) {
            errno = EINVAL;
            return -1;
        }
        port = end + 2;
    } else {
        const char *colon = strrchr(authority, ':');
        if (colon == NULL || colon == authority) {
            errno = EINVAL;
            return -1;
        }
        port = colon + 1;
    }

    if (port == NULL || port[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    for (const char *cursor = port; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static int validate_endpoint_uri(const char *uri)
{
    char endpoint_path[CUBICLE_PATH_MAX];
    if (cubicle_config_unix_uri_path(uri, endpoint_path,
                                     sizeof(endpoint_path)) == 0) {
        return 0;
    }
    return validate_tcp_uri(uri);
}

int cubicle_config_validate(const cubicle_config_t *config,
                            char *error,
                            size_t error_size)
{
    if (validate_absolute_path(config->bindir, "installation.bindir",
                               error, error_size) < 0 ||
        validate_absolute_path(config->libexecdir, "installation.libexecdir",
                               error, error_size) < 0 ||
        validate_absolute_path(config->manager_state_dir, "manager.state_dir",
                               error, error_size) < 0 ||
        validate_absolute_path(config->manager_runtime_dir,
                               "manager.runtime_dir",
                               error, error_size) < 0 ||
        validate_absolute_path(config->manager_log_dir, "manager.log_dir",
                               error, error_size) < 0 ||
        validate_absolute_path(config->controller_binary,
                               "manager.controller_binary",
                               error, error_size) < 0 ||
        validate_endpoint_uri(config->manager_listen_uri) < 0) {
        if (error != NULL && error_size > 0 && error[0] == '\0') {
            snprintf(error, error_size,
                     "manager.listen must be an absolute unix:// endpoint or tcp://host:port endpoint");
        }
        return -1;
    }

    if (validate_endpoint_uri(config->client_manager_uri) < 0) {
        set_error(error, error_size,
                  "client.manager must be an absolute unix:// endpoint or tcp://host:port endpoint");
        return -1;
    }

    return 0;
}

int cubicle_config_load(cubicle_config_t *config, char *error, size_t error_size)
{
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    cubicle_config_defaults(config);

    const char *override_path = getenv("CUBICLE_CONFIG");
    econf_file *file = NULL;
    econf_err result;
    if (override_path != NULL && override_path[0] != '\0') {
        result = econf_readFile(&file, override_path, "=", "#");
        if (result != ECONF_SUCCESS) {
            if (error != NULL && error_size > 0) {
                snprintf(error, error_size, "%s: %s", override_path,
                         econf_errString(result));
            }
            return -1;
        }
        snprintf(config->source, sizeof(config->source), "%s", override_path);
    } else {
        result = econf_readConfig(&file, "cubicle", "/usr/lib", "config",
                                  "cfg", "=", "#");
        if (result == ECONF_NOFILE) {
            return cubicle_config_validate(config, error, error_size);
        }
        if (result != ECONF_SUCCESS) {
            if (error != NULL && error_size > 0) {
                snprintf(error, error_size, "system config: %s",
                         econf_errString(result));
            }
            return -1;
        }
        snprintf(config->source, sizeof(config->source),
                 "system configuration");
    }

    if (apply_econf_file(config, file, error, error_size) < 0) {
        econf_free(file);
        return -1;
    }
    econf_free(file);
    return cubicle_config_validate(config, error, error_size);
}

const char *cubicle_launch_default_name(cubicle_launch_default_t launch)
{
    return launch == CUBICLE_LAUNCH_BACKGROUND ? "background" : "foreground";
}
