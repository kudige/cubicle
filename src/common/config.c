#define _POSIX_C_SOURCE 200809L

#include "cubicle/config.h"

#include <dirent.h>
#include <ctype.h>
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

static void set_origin(cubicle_config_t *config,
                       cubicle_config_key_t key,
                       cubicle_config_source_kind_t kind,
                       const char *source)
{
    if (config == NULL || key < 0 || key >= CUBICLE_CONFIG_KEY_COUNT) {
        return;
    }
    config->origins[key].kind = kind;
    config->origins[key].line_number = 0;
    snprintf(config->origins[key].source_path,
             sizeof(config->origins[key].source_path), "%s",
             source != NULL && source[0] != '\0' ? source : "unknown");
}

static void set_all_origins(cubicle_config_t *config,
                            cubicle_config_source_kind_t kind,
                            const char *source)
{
    for (int i = 0; i < CUBICLE_CONFIG_KEY_COUNT; ++i) {
        set_origin(config, (cubicle_config_key_t)i, kind, source);
    }
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

static int string_equals_case(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return 0;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int parse_control_key_char(char ch, unsigned char *key)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch == ' ' || ch == '@') {
        *key = 0;
        return 0;
    }
    if (ch < 'A' || ch > '_') {
        return -1;
    }
    *key = (unsigned char)(ch - '@');
    return 0;
}

static int parse_desk_key_name(const char *text,
                               int *uses_prefix,
                               unsigned char *key,
                               char *normalized,
                               size_t normalized_size,
                               char *error,
                               size_t error_size)
{
    if (text == NULL || text[0] == '\0') {
        set_error(error, error_size, "desk key name must not be empty");
        return -1;
    }
    *uses_prefix = 0;
    const char *name = text;
    if (strncmp(name, "Prefix-", 7) == 0 ||
        strncmp(name, "prefix-", 7) == 0) {
        *uses_prefix = 1;
        name += 7;
    }

    unsigned char parsed = 0;
    if (name[0] == '^' && name[1] != '\0' && name[2] == '\0') {
        if (parse_control_key_char(name[1], &parsed) < 0) {
            goto invalid;
        }
    } else if ((strncmp(name, "Control-", 8) == 0 ||
                strncmp(name, "control-", 8) == 0)) {
        const char *ch = name + 8;
        if (string_equals_case(ch, "Space")) {
            parsed = 0;
        } else if (ch[0] != '\0' && ch[1] == '\0' &&
                   parse_control_key_char(ch[0], &parsed) == 0) {
            /* parsed */
        } else {
            goto invalid;
        }
    } else if ((strncmp(name, "Ctrl-", 5) == 0 ||
                strncmp(name, "ctrl-", 5) == 0)) {
        const char *ch = name + 5;
        if (string_equals_case(ch, "Space")) {
            parsed = 0;
        } else if (ch[0] != '\0' && ch[1] == '\0' &&
                   parse_control_key_char(ch[0], &parsed) == 0) {
            /* parsed */
        } else {
            goto invalid;
        }
    } else if ((strncmp(name, "C-", 2) == 0 ||
                strncmp(name, "c-", 2) == 0)) {
        const char *ch = name + 2;
        if (string_equals_case(ch, "Space")) {
            parsed = 0;
        } else if (ch[0] != '\0' && ch[1] == '\0' &&
                   parse_control_key_char(ch[0], &parsed) == 0) {
            /* parsed */
        } else {
            goto invalid;
        }
    } else if (string_equals_case(name, "Space")) {
        parsed = ' ';
    } else if (string_equals_case(name, "Enter") ||
               string_equals_case(name, "Return")) {
        parsed = '\r';
    } else if (string_equals_case(name, "Escape") ||
               string_equals_case(name, "Esc")) {
        parsed = 0x1b;
    } else if (string_equals_case(name, "Backspace")) {
        parsed = 0x7f;
    } else if (string_equals_case(name, "Tab")) {
        parsed = '\t';
    } else if (name[0] != '\0' && name[1] == '\0') {
        parsed = (unsigned char)name[0];
    } else {
        goto invalid;
    }

    *key = parsed;
    int length = snprintf(normalized, normalized_size, "%s%s",
                          *uses_prefix ? "Prefix-" : "", name);
    return length < 0 || (size_t)length >= normalized_size ? -1 : 0;

invalid:
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "invalid desk key name '%s'", text);
    }
    return -1;
}

static int desk_command_is_known(const char *command)
{
    return strcmp(command, "pane.next") == 0 ||
           strcmp(command, "pane.previous") == 0 ||
           strcmp(command, "layout.zoom") == 0 ||
           strcmp(command, "layout.resize.toggle") == 0 ||
           strcmp(command, "layout.save") == 0 ||
           strcmp(command, "layout.load") == 0 ||
           strcmp(command, "menu.open") == 0 ||
           strcmp(command, "mouse.toggle") == 0 ||
           strcmp(command, "quit") == 0;
}

static int add_desk_key_binding(cubicle_config_t *config,
                                const char *key_name,
                                const char *command,
                                int builtin,
                                char *error,
                                size_t error_size)
{
    if (config->desk_key_binding_count >= CUBICLE_DESK_KEY_BINDING_MAX) {
        set_error(error, error_size, "desk.keys has too many bindings");
        return -1;
    }
    cubicle_desk_key_binding_t binding;
    memset(&binding, 0, sizeof(binding));
    if (parse_desk_key_name(key_name, &binding.uses_prefix, &binding.key,
                            binding.key_name, sizeof(binding.key_name),
                            error, error_size) < 0) {
        return -1;
    }
    binding.unbind = strcmp(command, "none") == 0;
    binding.builtin = builtin;
    if (!binding.unbind && !desk_command_is_known(command)) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size,
                     "unknown desk command '%s' in desk.keys", command);
        }
        return -1;
    }
    int length = snprintf(binding.command, sizeof(binding.command), "%s",
                          command);
    if (length < 0 || (size_t)length >= sizeof(binding.command)) {
        set_error(error, error_size, "desk command name is too long");
        return -1;
    }
    for (size_t i = 0; i < config->desk_key_binding_count; ++i) {
        cubicle_desk_key_binding_t *existing =
            &config->desk_key_bindings[i];
        if (existing->uses_prefix == binding.uses_prefix &&
            existing->key == binding.key) {
            if (binding.unbind) {
                memmove(existing, existing + 1,
                        (config->desk_key_binding_count - i - 1) *
                            sizeof(*existing));
                config->desk_key_binding_count--;
                return 0;
            }
            if (existing->builtin) {
                *existing = binding;
                return 0;
            }
            if (error != NULL && error_size > 0) {
                snprintf(error, error_size,
                         "duplicate desk.keys binding for %s",
                         binding.key_name);
            }
            return -1;
        }
    }
    if (binding.unbind) {
        return 0;
    }
    config->desk_key_bindings[config->desk_key_binding_count++] = binding;
    return 0;
}

static int parse_desk_binding_value(cubicle_config_t *config,
                                    const char *value,
                                    char *error,
                                    size_t error_size)
{
    char copy[160];
    int length = snprintf(copy, sizeof(copy), "%s", value);
    if (length < 0 || (size_t)length >= sizeof(copy)) {
        set_error(error, error_size, "desk.keys binding is too long");
        return -1;
    }
    char *cursor = copy;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    char *key = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        set_error(error, error_size,
                  "desk.keys binding must be KEY COMMAND");
        return -1;
    }
    *cursor++ = '\0';
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    char *command = cursor;
    char *end = command + strlen(command);
    while (end > command && (end[-1] == ' ' || end[-1] == '\t' ||
                             end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    if (key[0] == '\0' || command[0] == '\0') {
        set_error(error, error_size,
                  "desk.keys binding must be KEY COMMAND");
        return -1;
    }
    return add_desk_key_binding(config, key, command, 0, error, error_size);
}

static int validate_desk_key_conflicts(const cubicle_config_t *config,
                                       char *error,
                                       size_t error_size)
{
    for (size_t i = 0; i < config->desk_key_binding_count; ++i) {
        const cubicle_desk_key_binding_t *left =
            &config->desk_key_bindings[i];
        if (left->unbind) {
            continue;
        }
        for (size_t j = i + 1; j < config->desk_key_binding_count; ++j) {
            const cubicle_desk_key_binding_t *right =
                &config->desk_key_bindings[j];
            if (right->unbind) {
                continue;
            }
            if (left->uses_prefix == right->uses_prefix &&
                left->key == right->key) {
                if (error != NULL && error_size > 0) {
                    snprintf(error, error_size,
                             "duplicate desk.keys binding for %s",
                             left->key_name);
                }
                return -1;
            }
        }
    }
    return 0;
}

static int parse_library_debug(const char *value,
                               int *enabled,
                               const char *name,
                               char *error,
                               size_t error_size)
{
    if (strcmp(value, "library") == 0) {
        *enabled = 1;
        return 0;
    }
    if (strcmp(value, "none") == 0 || strcmp(value, "off") == 0 ||
        strcmp(value, "false") == 0) {
        *enabled = 0;
        return 0;
    }
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size,
                 "%s must be library, none, off, or false", name);
    }
    return -1;
}

static int parse_desk_debug(const char *value,
                            cubicle_config_t *config,
                            char *error,
                            size_t error_size)
{
    if (strcmp(value, "none") == 0 || strcmp(value, "off") == 0 ||
        strcmp(value, "false") == 0) {
        config->desk_debug_library = 0;
        config->desk_debug_terminal = 0;
        return 0;
    }

    config->desk_debug_library = 0;
    config->desk_debug_terminal = 0;

    char copy[128];
    int length = snprintf(copy, sizeof(copy), "%s", value);
    if (length < 0 || (size_t)length >= sizeof(copy)) {
        set_error(error, error_size, "desk.debug is too long");
        return -1;
    }

    char *save = NULL;
    for (char *part = strtok_r(copy, ",", &save); part != NULL;
         part = strtok_r(NULL, ",", &save)) {
        while (*part == ' ' || *part == '\t') {
            ++part;
        }
        char *end = part + strlen(part);
        while (end > part && (end[-1] == ' ' || end[-1] == '\t' ||
                              end[-1] == '\r' || end[-1] == '\n')) {
            *--end = '\0';
        }
        if (strcmp(part, "library") == 0) {
            config->desk_debug_library = 1;
        } else if (strcmp(part, "terminal") == 0) {
            config->desk_debug_terminal = 1;
        } else {
            if (error != NULL && error_size > 0) {
                snprintf(error, error_size,
                         "desk.debug must contain library, terminal, none, off, or false");
            }
            return -1;
        }
    }
    return 0;
}

static int parse_controller_debug(const char *value,
                                  cubicle_config_t *config,
                                  char *error,
                                  size_t error_size)
{
    if (strcmp(value, "none") == 0 || strcmp(value, "off") == 0 ||
        strcmp(value, "false") == 0) {
        config->controller_debug_input = 0;
        config->controller_debug_library = 0;
        config->controller_debug_terminal = 0;
        return 0;
    }

    config->controller_debug_input = 0;
    config->controller_debug_library = 0;
    config->controller_debug_terminal = 0;

    char copy[128];
    int length = snprintf(copy, sizeof(copy), "%s", value);
    if (length < 0 || (size_t)length >= sizeof(copy)) {
        set_error(error, error_size, "controller.debug is too long");
        return -1;
    }

    char *save = NULL;
    for (char *part = strtok_r(copy, ",", &save); part != NULL;
         part = strtok_r(NULL, ",", &save)) {
        if (strcmp(part, "input") == 0) {
            config->controller_debug_input = 1;
        } else if (strcmp(part, "library") == 0) {
            config->controller_debug_library = 1;
        } else if (strcmp(part, "terminal") == 0) {
            config->controller_debug_terminal = 1;
        } else {
            if (error != NULL && error_size > 0) {
                snprintf(error, error_size,
                         "controller.debug must contain input, library, terminal, none, off, or false");
            }
            return -1;
        }
    }
    return 0;
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

static int user_config_path(char *path, size_t path_size)
{
    const char *config_home = getenv("XDG_CONFIG_HOME");
    int length;
    if (config_home != NULL && config_home[0] != '\0') {
        length = snprintf(path, path_size, "%s/cubicle/config.cfg",
                          config_home);
    } else {
        const char *home = user_home_directory();
        if (home == NULL || home[0] == '\0') {
            errno = ENOENT;
            return -1;
        }
        length = snprintf(path, path_size, "%s/.config/cubicle/config.cfg",
                          home);
    }
    if (length < 0 || (size_t)length >= path_size) {
        errno = ENAMETOOLONG;
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
    config->controller_debug_input = 0;
    config->controller_debug_library = 0;
    config->controller_debug_terminal = 0;
    config->cube_debug_library = 0;
    config->desk_debug_library = 0;
    config->desk_debug_terminal = 0;
    config->desk_prefix_key = 0x18;
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
    (void)add_desk_key_binding(config, "Prefix-n", "pane.next", 1, NULL, 0);
    (void)add_desk_key_binding(config, "Prefix-p", "pane.previous", 1, NULL, 0);
    (void)add_desk_key_binding(config, "Prefix-Space", "layout.zoom", 1, NULL, 0);
    (void)add_desk_key_binding(config, "Prefix-s", "layout.resize.toggle",
                               1, NULL, 0);
    (void)add_desk_key_binding(config, "Prefix-m", "mouse.toggle", 1, NULL, 0);
    (void)add_desk_key_binding(config, "Prefix-o", "menu.open", 1, NULL, 0);
    (void)add_desk_key_binding(config, "Prefix-:", "layout.save", 1, NULL, 0);
    (void)add_desk_key_binding(config, "Prefix-;", "layout.load", 1, NULL, 0);
    (void)add_desk_key_binding(config, "Prefix-q", "quit", 1, NULL, 0);
    snprintf(config->source, sizeof(config->source), "built-in defaults");
    set_all_origins(config, CUBICLE_CONFIG_SOURCE_BUILTIN,
                    "built-in defaults");
}

static int get_optional_string(econf_file *file,
                               const char *group,
                               const char *key,
                               char *destination,
                               size_t destination_size,
                               int *present_out,
                               char *error,
                               size_t error_size)
{
    if (present_out != NULL) {
        *present_out = 0;
    }
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
    if (copy_result == 0 && present_out != NULL) {
        *present_out = 1;
    }
    free(value);
    return copy_result;
}

static int apply_econf_file(cubicle_config_t *config,
                            econf_file *file,
                            cubicle_config_source_kind_t source_kind,
                            int allow_manager_control_keys,
                            const char *source,
                            char *error,
                            size_t error_size)
{
    int present = 0;
    if (allow_manager_control_keys &&
        get_optional_string(file, "installation", "bindir",
                            config->bindir, sizeof(config->bindir),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (allow_manager_control_keys && present) {
        set_origin(config, CUBICLE_CONFIG_INSTALLATION_BINDIR, source_kind,
                   source);
    }
    if (allow_manager_control_keys &&
        get_optional_string(file, "installation", "libexecdir",
                            config->libexecdir, sizeof(config->libexecdir),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (allow_manager_control_keys && present) {
        set_origin(config, CUBICLE_CONFIG_INSTALLATION_LIBEXECDIR,
                   source_kind, source);
    }
    if (allow_manager_control_keys &&
        get_optional_string(file, "manager", "state_dir",
                            config->manager_state_dir,
                            sizeof(config->manager_state_dir),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (allow_manager_control_keys && present) {
        set_origin(config, CUBICLE_CONFIG_MANAGER_STATE_DIR, source_kind,
                   source);
    }
    if (allow_manager_control_keys &&
        get_optional_string(file, "manager", "runtime_dir",
                            config->manager_runtime_dir,
                            sizeof(config->manager_runtime_dir),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (allow_manager_control_keys && present) {
        set_origin(config, CUBICLE_CONFIG_MANAGER_RUNTIME_DIR, source_kind,
                   source);
    }
    if (get_optional_string(file, "manager", "log_dir",
                            config->manager_log_dir,
                            sizeof(config->manager_log_dir),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (present) set_origin(config, CUBICLE_CONFIG_MANAGER_LOG_DIR,
                            source_kind, source);
    if (allow_manager_control_keys &&
        get_optional_string(file, "manager", "listen",
                            config->manager_listen_uri,
                            sizeof(config->manager_listen_uri),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (allow_manager_control_keys && present) {
        set_origin(config, CUBICLE_CONFIG_MANAGER_LISTEN, source_kind,
                   source);
    }
    if (allow_manager_control_keys &&
        get_optional_string(file, "manager", "socket_group",
                            config->manager_socket_group,
                            sizeof(config->manager_socket_group),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (allow_manager_control_keys && present) {
        set_origin(config, CUBICLE_CONFIG_MANAGER_SOCKET_GROUP, source_kind,
                   source);
    }
    if (allow_manager_control_keys &&
        get_optional_string(file, "manager", "controller_binary",
                            config->controller_binary,
                            sizeof(config->controller_binary),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (allow_manager_control_keys && present) {
        set_origin(config, CUBICLE_CONFIG_MANAGER_CONTROLLER_BINARY,
                   source_kind, source);
    }
    if (get_optional_string(file, "client", "manager",
                            config->client_manager_uri,
                            sizeof(config->client_manager_uri),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (present) set_origin(config, CUBICLE_CONFIG_CLIENT_MANAGER,
                            source_kind, source);
    if (get_optional_string(file, "client", "server_identity",
                            config->client_server_identity,
                            sizeof(config->client_server_identity),
                            &present, error, error_size) < 0) {
        return -1;
    }
    if (present) set_origin(config, CUBICLE_CONFIG_CLIENT_SERVER_IDENTITY,
                            source_kind, source);

    char controller_debug[64];
    controller_debug[0] = '\0';
    if (allow_manager_control_keys &&
        get_optional_string(file, "controller", "debug", controller_debug,
                            sizeof(controller_debug), &present, error,
                            error_size) < 0) {
        return -1;
    }
    if (controller_debug[0] != '\0') {
        if (parse_controller_debug(controller_debug, config, error,
                                   error_size) < 0) {
            return -1;
        }
        set_origin(config, CUBICLE_CONFIG_CONTROLLER_DEBUG, source_kind,
                   source);
    }

    char cube_debug[64];
    cube_debug[0] = '\0';
    if (get_optional_string(file, "cube", "debug", cube_debug,
                            sizeof(cube_debug), &present, error,
                            error_size) < 0 ||
        (cube_debug[0] != '\0' &&
         parse_library_debug(cube_debug, &config->cube_debug_library,
                             "cube.debug", error, error_size) < 0)) {
        return -1;
    }
    if (cube_debug[0] != '\0') {
        set_origin(config, CUBICLE_CONFIG_CUBE_DEBUG, source_kind, source);
    }

    char desk_debug[64];
    desk_debug[0] = '\0';
    if (get_optional_string(file, "desk", "debug", desk_debug,
                            sizeof(desk_debug), &present, error,
                            error_size) < 0) {
        return -1;
    }
    if (desk_debug[0] != '\0' &&
        parse_desk_debug(desk_debug, config, error, error_size) < 0) {
        return -1;
    }
    if (desk_debug[0] != '\0') {
        set_origin(config, CUBICLE_CONFIG_DESK_DEBUG, source_kind, source);
    }

    char desk_prefix[64];
    desk_prefix[0] = '\0';
    if (get_optional_string(file, "desk", "prefix", desk_prefix,
                            sizeof(desk_prefix), &present, error,
                            error_size) < 0) {
        return -1;
    }
    if (desk_prefix[0] != '\0') {
        int prefixed = 0;
        char normalized[CUBICLE_DESK_KEY_NAME_MAX];
        if (parse_desk_key_name(desk_prefix, &prefixed,
                                &config->desk_prefix_key, normalized,
                                sizeof(normalized), error, error_size) < 0 ||
            prefixed) {
            if (prefixed) {
                set_error(error, error_size,
                          "desk.prefix must not include Prefix-");
            }
            return -1;
        }
        set_origin(config, CUBICLE_CONFIG_DESK_PREFIX, source_kind, source);
    }

    size_t desk_key_count = 0;
    char **desk_keys = NULL;
    econf_err keys_result = econf_getKeys(file, "desk.keys",
                                          &desk_key_count, &desk_keys);
    if (keys_result != ECONF_SUCCESS && keys_result != ECONF_NOGROUP &&
        keys_result != ECONF_NOKEY) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "desk.keys: %s",
                     econf_errString(keys_result));
        }
        return -1;
    }
    for (size_t i = 0; keys_result == ECONF_SUCCESS && i < desk_key_count;
         ++i) {
        if (strncmp(desk_keys[i], "bind.", 5) != 0) {
            continue;
        }
        char binding_value[160];
        binding_value[0] = '\0';
        if (get_optional_string(file, "desk.keys", desk_keys[i],
                                binding_value, sizeof(binding_value),
                                &present, error, error_size) < 0 ||
            (binding_value[0] != '\0' &&
             parse_desk_binding_value(config, binding_value, error,
                                      error_size) < 0)) {
            econf_free(desk_keys);
            return -1;
        }
        if (binding_value[0] != '\0') {
            set_origin(config, CUBICLE_CONFIG_DESK_KEYS, source_kind, source);
        }
    }
    if (desk_keys != NULL) {
        econf_free(desk_keys);
    }

    char value[64];
    value[0] = '\0';
    if (get_optional_string(file, "defaults", "launch", value, sizeof(value),
                            &present, error, error_size) < 0 ||
        (value[0] != '\0' &&
         parse_launch(value, &config->default_launch, error, error_size) < 0)) {
        return -1;
    }
    if (value[0] != '\0') {
        set_origin(config, CUBICLE_CONFIG_DEFAULTS_LAUNCH, source_kind,
                   source);
    }

    value[0] = '\0';
    if (allow_manager_control_keys) {
        if (get_optional_string(file, "manager", "socket_mode", value,
                                sizeof(value), &present, error,
                                error_size) < 0 ||
            (value[0] != '\0' &&
             parse_socket_mode(value, &config->manager_socket_mode,
                               error, error_size) < 0)) {
            return -1;
        }
    }
    if (allow_manager_control_keys && value[0] != '\0') {
        set_origin(config, CUBICLE_CONFIG_MANAGER_SOCKET_MODE, source_kind,
                   source);
    }

    value[0] = '\0';
    if (get_optional_string(file, "defaults", "mode", value, sizeof(value),
                            &present, error, error_size) < 0 ||
        (value[0] != '\0' &&
         parse_mode(value, &config->default_mode, error, error_size) < 0)) {
        return -1;
    }
    if (value[0] != '\0') {
        set_origin(config, CUBICLE_CONFIG_DEFAULTS_MODE, source_kind, source);
    }

    value[0] = '\0';
    if (get_optional_string(file, "defaults", "kill_cleanup", value,
                            sizeof(value), &present, error, error_size) < 0 ||
        (value[0] != '\0' &&
         parse_bool(value, &config->default_kill_cleanup,
                    "defaults.kill_cleanup", error, error_size) < 0)) {
        return -1;
    }
    if (value[0] != '\0') {
        set_origin(config, CUBICLE_CONFIG_DEFAULTS_KILL_CLEANUP,
                   source_kind, source);
    }

    return 0;
}

static int has_cfg_suffix(const char *name)
{
    size_t length = name == NULL ? 0 : strlen(name);
    return length > 4 && strcmp(name + length - 4, ".cfg") == 0;
}

static int compare_names(const void *left, const void *right)
{
    const char *const *left_name = left;
    const char *const *right_name = right;
    return strcmp(*left_name, *right_name);
}

static void free_names(char **names, size_t count)
{
    if (names == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(names[i]);
    }
    free(names);
}

static int apply_config_path(cubicle_config_t *config,
                             const char *path,
                             cubicle_config_source_kind_t source_kind,
                             int required,
                             int allow_manager_control_keys,
                             int *loaded_out,
                             char *error,
                             size_t error_size)
{
    if (loaded_out != NULL) {
        *loaded_out = 0;
    }

    econf_file *file = NULL;
    econf_err result = econf_readFile(&file, path, "=", "#");
    if (result == ECONF_NOFILE && !required) {
        return 0;
    }
    if (result != ECONF_SUCCESS) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "%s: %s", path,
                     econf_errString(result));
        }
        return -1;
    }

    if (apply_econf_file(config, file, source_kind,
                         allow_manager_control_keys, path, error,
                         error_size) < 0) {
        econf_free(file);
        return -1;
    }
    econf_free(file);
    snprintf(config->source, sizeof(config->source), "%s", path);
    if (loaded_out != NULL) {
        *loaded_out = 1;
    }
    return 0;
}

static int load_dropin_dir(cubicle_config_t *config,
                           const char *directory,
                           cubicle_config_source_kind_t source_kind,
                           int allow_manager_control_keys,
                           char *error,
                           size_t error_size)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "%s: %s", directory,
                     strerror(errno));
        }
        return -1;
    }

    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !has_cfg_suffix(entry->d_name)) {
            continue;
        }
        if (count == capacity) {
            size_t next_capacity = capacity == 0 ? 8 : capacity * 2;
            char **next = realloc(names, next_capacity * sizeof(*next));
            if (next == NULL) {
                closedir(dir);
                free_names(names, count);
                return -1;
            }
            names = next;
            capacity = next_capacity;
        }
        names[count] = strdup(entry->d_name);
        if (names[count] == NULL) {
            closedir(dir);
            free_names(names, count);
            return -1;
        }
        ++count;
    }
    closedir(dir);

    qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count; ++i) {
        char path[CUBICLE_PATH_MAX];
        int length = snprintf(path, sizeof(path), "%s/%s", directory,
                              names[i]);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            free_names(names, count);
            set_error(error, error_size, "config drop-in path is too long");
            return -1;
        }
        if (apply_config_path(config, path, source_kind, 1,
                              allow_manager_control_keys, NULL, error,
                              error_size) < 0) {
            free_names(names, count);
            return -1;
        }
    }

    free_names(names, count);
    return 0;
}

static int apply_config_path_with_dropins(
    cubicle_config_t *config,
    const char *path,
    cubicle_config_source_kind_t source_kind,
    int required,
    int allow_manager_control_keys,
    char *error,
    size_t error_size)
{
    if (apply_config_path(config, path, source_kind, required,
                          allow_manager_control_keys, NULL, error,
                          error_size) < 0) {
        return -1;
    }

    char directory[CUBICLE_PATH_MAX];
    int length = snprintf(directory, sizeof(directory), "%s.d", path);
    if (length < 0 || (size_t)length >= sizeof(directory)) {
        set_error(error, error_size, "config drop-in directory is too long");
        return -1;
    }
    return load_dropin_dir(config, directory, source_kind,
                           allow_manager_control_keys, error, error_size);
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

    if (validate_desk_key_conflicts(config, error, error_size) < 0) {
        return -1;
    }

    return 0;
}

static int cubicle_config_load_with_user_policy(cubicle_config_t *config,
                                                int user_manager_control_keys,
                                                char *error,
                                                size_t error_size)
{
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    cubicle_config_defaults(config);

    const char *override_path = getenv("CUBICLE_CONFIG");
    if (override_path != NULL && override_path[0] != '\0') {
        if (apply_config_path_with_dropins(config, override_path,
                                           CUBICLE_CONFIG_SOURCE_OVERRIDE, 1, 1,
                                           error, error_size) < 0) {
            return -1;
        }
        return cubicle_config_validate(config, error, error_size);
    }

    const char *system_paths[] = {
        "/usr/lib/cubicle/config.cfg",
        "/etc/cubicle/config.cfg",
        "/run/cubicle/config.cfg",
    };
    for (size_t i = 0; i < sizeof(system_paths) / sizeof(system_paths[0]);
         ++i) {
        if (apply_config_path_with_dropins(config, system_paths[i],
                                           CUBICLE_CONFIG_SOURCE_SYSTEM, 0, 1,
                                           error, error_size) < 0) {
            return -1;
        }
    }

    char path[CUBICLE_PATH_MAX];
    if (user_config_path(path, sizeof(path)) == 0) {
        if (apply_config_path_with_dropins(config, path,
                                           CUBICLE_CONFIG_SOURCE_USER, 0,
                                           user_manager_control_keys,
                                           error, error_size) < 0) {
            return -1;
        }
    } else if (errno != ENOENT) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size,
                     "user config path could not be resolved: %s",
                     strerror(errno));
        }
        return -1;
    }

    return cubicle_config_validate(config, error, error_size);
}

int cubicle_config_load(cubicle_config_t *config, char *error, size_t error_size)
{
    return cubicle_config_load_with_user_policy(config, 1, error, error_size);
}

int cubicle_config_load_client(cubicle_config_t *config,
                               char *error,
                               size_t error_size)
{
    return cubicle_config_load_with_user_policy(config, 0, error, error_size);
}

const char *cubicle_launch_default_name(cubicle_launch_default_t launch)
{
    return launch == CUBICLE_LAUNCH_BACKGROUND ? "background" : "foreground";
}

const char *cubicle_config_source_kind_name(cubicle_config_source_kind_t kind)
{
    switch (kind) {
    case CUBICLE_CONFIG_SOURCE_SYSTEM:
        return "system";
    case CUBICLE_CONFIG_SOURCE_USER:
        return "user";
    case CUBICLE_CONFIG_SOURCE_OVERRIDE:
        return "override";
    case CUBICLE_CONFIG_SOURCE_BUILTIN:
    default:
        return "built-in";
    }
}

const char *cubicle_config_key_name(cubicle_config_key_t key)
{
    switch (key) {
    case CUBICLE_CONFIG_INSTALLATION_BINDIR:
        return "installation.bindir";
    case CUBICLE_CONFIG_INSTALLATION_LIBEXECDIR:
        return "installation.libexecdir";
    case CUBICLE_CONFIG_MANAGER_STATE_DIR:
        return "manager.state_dir";
    case CUBICLE_CONFIG_MANAGER_RUNTIME_DIR:
        return "manager.runtime_dir";
    case CUBICLE_CONFIG_MANAGER_LOG_DIR:
        return "manager.log_dir";
    case CUBICLE_CONFIG_MANAGER_LISTEN:
        return "manager.listen";
    case CUBICLE_CONFIG_MANAGER_SOCKET_MODE:
        return "manager.socket_mode";
    case CUBICLE_CONFIG_MANAGER_SOCKET_GROUP:
        return "manager.socket_group";
    case CUBICLE_CONFIG_MANAGER_CONTROLLER_BINARY:
        return "manager.controller_binary";
    case CUBICLE_CONFIG_CONTROLLER_DEBUG:
        return "controller.debug";
    case CUBICLE_CONFIG_CUBE_DEBUG:
        return "cube.debug";
    case CUBICLE_CONFIG_DESK_DEBUG:
        return "desk.debug";
    case CUBICLE_CONFIG_CLIENT_MANAGER:
        return "client.manager";
    case CUBICLE_CONFIG_CLIENT_SERVER_IDENTITY:
        return "client.server_identity";
    case CUBICLE_CONFIG_DEFAULTS_LAUNCH:
        return "defaults.launch";
    case CUBICLE_CONFIG_DEFAULTS_MODE:
        return "defaults.mode";
    case CUBICLE_CONFIG_DEFAULTS_KILL_CLEANUP:
        return "defaults.kill_cleanup";
    case CUBICLE_CONFIG_DESK_PREFIX:
        return "desk.prefix";
    case CUBICLE_CONFIG_DESK_KEYS:
        return "desk.keys";
    case CUBICLE_CONFIG_KEY_COUNT:
    default:
        return "unknown";
    }
}

const cubicle_config_origin_t *cubicle_config_origin(
    const cubicle_config_t *config,
    cubicle_config_key_t key)
{
    if (config == NULL || key < 0 || key >= CUBICLE_CONFIG_KEY_COUNT) {
        return NULL;
    }
    return &config->origins[key];
}
