#define _POSIX_C_SOURCE 200809L

#include "cubicle/config.h"

#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(content, file) >= 0);
    assert(fclose(file) == 0);
}

static void temp_path(char *path, size_t path_size, const char *name)
{
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }
    int length = snprintf(path, path_size, "%s/cubicle-config-test-XXXXXX",
                          tmpdir);
    assert(length >= 0 && (size_t)length < path_size);
    assert(mkdtemp(path) != NULL);
    size_t used = strlen(path);
    length = snprintf(path + used, path_size - used, "/%s", name);
    assert(length >= 0 && (size_t)length < path_size - used);
}

static void temp_dir(char *path, size_t path_size)
{
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }
    int length = snprintf(path, path_size, "%s/cubicle-config-test-XXXXXX",
                          tmpdir);
    assert(length >= 0 && (size_t)length < path_size);
    assert(mkdtemp(path) != NULL);
}

static void test_defaults(void)
{
    cubicle_config_t config;
    cubicle_config_defaults(&config);
    if (geteuid() == 0) {
        assert(strcmp(config.manager_state_dir, "/var/lib/cubicle") == 0);
        assert(strcmp(config.manager_runtime_dir, "/run/cubicle") == 0);
        assert(strcmp(config.manager_log_dir, "/var/log/cubicle") == 0);
        assert(strcmp(config.manager_listen_uri,
                      "unix:///run/cubicle/manager.sock") == 0);
    } else {
        assert(strstr(config.manager_state_dir, "/cubicle") != NULL);
        assert(strstr(config.manager_runtime_dir, "/cubicle") != NULL);
        assert(strstr(config.manager_log_dir, "/cubicle/log") != NULL);
        assert(strncmp(config.manager_listen_uri, "unix://", 7) == 0);
        assert(strstr(config.manager_listen_uri, "/manager.sock") != NULL);
        assert(strcmp(config.client_manager_uri,
                      config.manager_listen_uri) == 0);
    }
    assert(strcmp(config.controller_binary,
                  "/usr/libexec/cubicle/cubicle-controller") == 0);
    assert(config.manager_socket_mode == 0660);
    assert(strcmp(config.manager_socket_group, "") == 0);
    assert(config.controller_debug_input == 0);
    assert(config.controller_debug_library == 0);
    assert(config.controller_debug_terminal == 0);
    assert(config.cube_debug_library == 0);
    assert(config.desk_debug_library == 0);
    assert(config.desk_debug_terminal == 0);
    assert(config.desk_prefix_key == 0x18);
    assert(config.desk_key_binding_count == 9);
    assert(config.desk_key_bindings[0].uses_prefix == 1);
    assert(config.desk_key_bindings[0].key == 'n');
    assert(strcmp(config.desk_key_bindings[0].command, "pane.next") == 0);
    assert(config.default_launch == CUBICLE_LAUNCH_FOREGROUND);
    assert(config.default_mode == CUBICLE_PROCESS_TTY);
    assert(config.default_kill_cleanup == 0);
}

static void test_override_file(void)
{
    char path[CUBICLE_PATH_MAX];
    temp_path(path, sizeof(path), "config.cfg");
    write_file(path,
               "[manager]\n"
               "state_dir=/tmp/cubicle-state\n"
               "runtime_dir=/tmp/cubicle-run\n"
               "log_dir=/tmp/cubicle-log\n"
               "listen=unix:///tmp/cubicle-run/manager.sock\n"
               "socket_mode=0664\n"
               "socket_group=cubicle\n"
               "controller_binary=/tmp/cubicle-controller\n"
               "\n"
               "[controller]\n"
               "debug=input,library,terminal\n"
               "\n"
               "[cube]\n"
               "debug=library\n"
               "\n"
               "[desk]\n"
               "debug=library,terminal\n"
               "prefix=Control-Space\n"
               "\n"
               "[desk.keys]\n"
               "bind.1=Prefix-n pane.previous\n"
               "bind.2=Control-G quit\n"
               "bind.3=Prefix-m none\n"
               "\n"
               "[client]\n"
               "manager=unix:///tmp/cubicle-run/manager.sock\n"
               "\n"
               "[defaults]\n"
               "launch=background\n"
               "mode=stream\n"
               "kill_cleanup=true\n");

    char dropin_dir[CUBICLE_PATH_MAX];
    int length = snprintf(dropin_dir, sizeof(dropin_dir), "%s.d", path);
    assert(length >= 0 && (size_t)length < sizeof(dropin_dir));
    assert(mkdir(dropin_dir, 0700) == 0);
    char dropin_path[CUBICLE_PATH_MAX];
    length = snprintf(dropin_path, sizeof(dropin_path),
                      "%s/90-mode.cfg", dropin_dir);
    assert(length >= 0 && (size_t)length < sizeof(dropin_path));
    write_file(dropin_path,
               "[defaults]\n"
               "mode=term\n");

    assert(setenv("CUBICLE_CONFIG", path, 1) == 0);
    cubicle_config_t config;
    char error[256];
    assert(cubicle_config_load(&config, error, sizeof(error)) == 0);
    assert(strcmp(config.manager_state_dir, "/tmp/cubicle-state") == 0);
    assert(strcmp(config.manager_runtime_dir, "/tmp/cubicle-run") == 0);
    assert(strcmp(config.manager_log_dir, "/tmp/cubicle-log") == 0);
    assert(config.manager_socket_mode == 0664);
    assert(strcmp(config.manager_socket_group, "cubicle") == 0);
    assert(strcmp(config.controller_binary, "/tmp/cubicle-controller") == 0);
    assert(config.controller_debug_input == 1);
    assert(config.controller_debug_library == 1);
    assert(config.controller_debug_terminal == 1);
    assert(config.cube_debug_library == 1);
    assert(config.desk_debug_library == 1);
    assert(config.desk_debug_terminal == 1);
    assert(config.desk_prefix_key == 0);
    assert(config.desk_key_binding_count == 9);
    int saw_rebound_prefix_n = 0;
    int saw_direct_quit = 0;
    int saw_prefix_m = 0;
    for (size_t i = 0; i < config.desk_key_binding_count; ++i) {
        if (config.desk_key_bindings[i].uses_prefix &&
            config.desk_key_bindings[i].key == 'n' &&
            strcmp(config.desk_key_bindings[i].command,
                   "pane.previous") == 0) {
            saw_rebound_prefix_n = 1;
        }
        if (!config.desk_key_bindings[i].uses_prefix &&
            config.desk_key_bindings[i].key == 7 &&
            strcmp(config.desk_key_bindings[i].command, "quit") == 0) {
            saw_direct_quit = 1;
        }
        if (config.desk_key_bindings[i].uses_prefix &&
            config.desk_key_bindings[i].key == 'm') {
            saw_prefix_m = 1;
        }
    }
    assert(saw_rebound_prefix_n);
    assert(saw_direct_quit);
    assert(!saw_prefix_m);
    assert(config.default_launch == CUBICLE_LAUNCH_BACKGROUND);
    assert(config.default_mode == CUBICLE_PROCESS_TTY_CAPTURED_STDERR);
    assert(config.default_kill_cleanup == 1);
    const cubicle_config_origin_t *origin =
        cubicle_config_origin(&config, CUBICLE_CONFIG_MANAGER_STATE_DIR);
    assert(origin != NULL);
    assert(origin->kind == CUBICLE_CONFIG_SOURCE_OVERRIDE);
    assert(strcmp(origin->source_path, path) == 0);
    origin = cubicle_config_origin(&config, CUBICLE_CONFIG_DEFAULTS_MODE);
    assert(origin != NULL);
    assert(origin->kind == CUBICLE_CONFIG_SOURCE_OVERRIDE);
    assert(strcmp(origin->source_path, dropin_path) == 0);
    assert(strcmp(cubicle_config_key_name(CUBICLE_CONFIG_DEFAULTS_MODE),
                  "defaults.mode") == 0);
    assert(strcmp(cubicle_config_source_kind_name(origin->kind),
                  "override") == 0);
    origin = cubicle_config_origin(&config,
                                   CUBICLE_CONFIG_CLIENT_SERVER_IDENTITY);
    assert(origin != NULL);
    assert(origin->kind == CUBICLE_CONFIG_SOURCE_BUILTIN);
    assert(strcmp(origin->source_path, "built-in defaults") == 0);
    assert(unsetenv("CUBICLE_CONFIG") == 0);
}

static void test_user_config_file(void)
{
    char xdg_home[CUBICLE_PATH_MAX];
    temp_dir(xdg_home, sizeof(xdg_home));

    char cubicle_dir[CUBICLE_PATH_MAX];
    int length = snprintf(cubicle_dir, sizeof(cubicle_dir), "%s/cubicle",
                          xdg_home);
    assert(length >= 0 && (size_t)length < sizeof(cubicle_dir));
    assert(mkdir(cubicle_dir, 0700) == 0);

    char path[CUBICLE_PATH_MAX];
    length = snprintf(path, sizeof(path), "%s/config.cfg", cubicle_dir);
    assert(length >= 0 && (size_t)length < sizeof(path));
    write_file(path,
               "[manager]\n"
               "state_dir=/tmp/user-cubicle-state\n"
               "runtime_dir=/tmp/user-cubicle-run\n"
               "log_dir=/tmp/user-cubicle-log\n"
               "listen=unix:///tmp/user-cubicle-run/manager.sock\n"
               "\n"
               "[controller]\n"
               "debug=input,terminal\n"
               "\n"
               "[cube]\n"
               "debug=library\n"
               "\n"
               "[desk]\n"
               "debug=none\n"
               "\n"
               "[client]\n"
               "manager=unix:///tmp/user-cubicle-run/manager.sock\n");

    assert(unsetenv("CUBICLE_CONFIG") == 0);
    assert(setenv("XDG_CONFIG_HOME", xdg_home, 1) == 0);

    cubicle_config_t config;
    char error[256];
    assert(cubicle_config_load(&config, error, sizeof(error)) == 0);
    assert(strcmp(config.source, path) == 0);
    assert(strcmp(config.manager_state_dir, "/tmp/user-cubicle-state") == 0);
    assert(strcmp(config.manager_runtime_dir, "/tmp/user-cubicle-run") == 0);
    assert(strcmp(config.manager_log_dir, "/tmp/user-cubicle-log") == 0);
    assert(strcmp(config.manager_listen_uri,
                  "unix:///tmp/user-cubicle-run/manager.sock") == 0);
    assert(strcmp(config.client_manager_uri,
                  "unix:///tmp/user-cubicle-run/manager.sock") == 0);
    assert(config.controller_debug_input == 1);
    assert(config.controller_debug_library == 0);
    assert(config.controller_debug_terminal == 1);
    assert(config.cube_debug_library == 1);
    assert(config.desk_debug_library == 0);
    assert(config.desk_debug_terminal == 0);
    const cubicle_config_origin_t *origin =
        cubicle_config_origin(&config, CUBICLE_CONFIG_MANAGER_STATE_DIR);
    assert(origin != NULL);
    assert(origin->kind == CUBICLE_CONFIG_SOURCE_USER);
    assert(strcmp(origin->source_path, path) == 0);
    origin = cubicle_config_origin(&config, CUBICLE_CONFIG_DEFAULTS_MODE);
    assert(origin != NULL);
    assert(strcmp(origin->source_path, path) != 0);
    assert(unsetenv("XDG_CONFIG_HOME") == 0);
}

static void test_client_user_policy(void)
{
    char xdg_home[CUBICLE_PATH_MAX];
    temp_dir(xdg_home, sizeof(xdg_home));

    char cubicle_dir[CUBICLE_PATH_MAX];
    int length = snprintf(cubicle_dir, sizeof(cubicle_dir), "%s/cubicle",
                          xdg_home);
    assert(length >= 0 && (size_t)length < sizeof(cubicle_dir));
    assert(mkdir(cubicle_dir, 0700) == 0);

    char path[CUBICLE_PATH_MAX];
    length = snprintf(path, sizeof(path), "%s/config.cfg", cubicle_dir);
    assert(length >= 0 && (size_t)length < sizeof(path));
    write_file(path,
               "[manager]\n"
               "state_dir=relative-state\n"
               "runtime_dir=relative-runtime\n"
               "listen=relative.sock\n"
               "socket_mode=9999\n"
               "controller_binary=relative-controller\n"
               "log_dir=/tmp/client-log\n"
               "\n"
               "[controller]\n"
               "debug=invalid-controller-debug\n"
               "\n"
               "[client]\n"
               "manager=unix:///tmp/client-manager.sock\n"
               "\n"
               "[cube]\n"
               "debug=library\n"
               "\n"
               "[defaults]\n"
               "launch=background\n"
               "mode=stream\n");

    assert(unsetenv("CUBICLE_CONFIG") == 0);
    assert(setenv("XDG_CONFIG_HOME", xdg_home, 1) == 0);

    cubicle_config_t config;
    char error[256];
    assert(cubicle_config_load_client(&config, error, sizeof(error)) == 0);
    assert(strcmp(config.manager_state_dir, "relative-state") != 0);
    assert(strcmp(config.manager_runtime_dir, "relative-runtime") != 0);
    assert(strcmp(config.manager_listen_uri, "relative.sock") != 0);
    assert(strcmp(config.controller_binary, "relative-controller") != 0);
    assert(config.manager_socket_mode != 9999);
    assert(config.controller_debug_input == 0);
    assert(strcmp(config.manager_log_dir, "/tmp/client-log") == 0);
    assert(strcmp(config.client_manager_uri,
                  "unix:///tmp/client-manager.sock") == 0);
    assert(config.cube_debug_library == 1);
    assert(config.default_launch == CUBICLE_LAUNCH_BACKGROUND);
    assert(config.default_mode == CUBICLE_PROCESS_STREAM);

    const cubicle_config_origin_t *origin =
        cubicle_config_origin(&config, CUBICLE_CONFIG_MANAGER_STATE_DIR);
    assert(origin != NULL);
    assert(origin->kind != CUBICLE_CONFIG_SOURCE_USER);
    origin = cubicle_config_origin(&config, CUBICLE_CONFIG_MANAGER_LOG_DIR);
    assert(origin != NULL);
    assert(origin->kind == CUBICLE_CONFIG_SOURCE_USER);
    origin = cubicle_config_origin(&config, CUBICLE_CONFIG_CLIENT_MANAGER);
    assert(origin != NULL);
    assert(origin->kind == CUBICLE_CONFIG_SOURCE_USER);

    assert(unsetenv("XDG_CONFIG_HOME") == 0);
}

static void test_invalid_values(void)
{
    char path[CUBICLE_PATH_MAX];
    temp_path(path, sizeof(path), "invalid.cfg");
    write_file(path,
               "[manager]\n"
               "state_dir=relative\n");
    assert(setenv("CUBICLE_CONFIG", path, 1) == 0);
    cubicle_config_t config;
    char error[256];
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "manager.state_dir") != NULL);

    write_file(path,
               "[defaults]\n"
               "mode=interactive\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "defaults.mode") != NULL);

    write_file(path,
               "[client]\n"
               "manager=relative.sock\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "client.manager") != NULL);

    write_file(path,
               "[manager]\n"
               "listen=tcp://127.0.0.1\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "manager.listen") != NULL);

    write_file(path,
               "[manager]\n"
               "socket_mode=9999\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "manager.socket_mode") != NULL);

    write_file(path,
               "[defaults]\n"
               "kill_cleanup=maybe\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "defaults.kill_cleanup") != NULL);

    write_file(path,
               "[controller]\n"
               "debug=input,nope\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "controller.debug") != NULL);

    write_file(path,
               "[cube]\n"
               "debug=input\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "cube.debug") != NULL);

    write_file(path,
               "[desk]\n"
               "debug=input\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "desk.debug") != NULL);

    write_file(path,
               "[desk]\n"
               "prefix=Prefix-x\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "desk.prefix") != NULL);

    write_file(path,
               "[desk.keys]\n"
               "bind.1=Control-NotAKey quit\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "desk key") != NULL);

    write_file(path,
               "[desk.keys]\n"
               "bind.1=Control-G missing.command\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "desk command") != NULL);

    write_file(path,
               "[desk.keys]\n"
               "bind.1=Control-G quit\n"
               "bind.2=Control-G layout.zoom\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "duplicate desk.keys") != NULL);
    assert(unsetenv("CUBICLE_CONFIG") == 0);
}

static void test_tcp_endpoints(void)
{
    char path[CUBICLE_PATH_MAX];
    temp_path(path, sizeof(path), "tcp.cfg");
    write_file(path,
               "[manager]\n"
               "listen=tcp://127.0.0.1:7777\n"
               "\n"
               "[client]\n"
               "manager=tcp://127.0.0.1:7777\n");

    assert(setenv("CUBICLE_CONFIG", path, 1) == 0);
    cubicle_config_t config;
    char error[256];
    assert(cubicle_config_load(&config, error, sizeof(error)) == 0);
    assert(strcmp(config.manager_listen_uri, "tcp://127.0.0.1:7777") == 0);
    assert(strcmp(config.client_manager_uri, "tcp://127.0.0.1:7777") == 0);
    assert(unsetenv("CUBICLE_CONFIG") == 0);
}

static void test_unix_uri_path(void)
{
    char path[CUBICLE_PATH_MAX];
    assert(cubicle_config_unix_uri_path("unix:///tmp/manager.sock",
                                        path, sizeof(path)) == 0);
    assert(strcmp(path, "/tmp/manager.sock") == 0);
    assert(cubicle_config_unix_uri_path("tcp://127.0.0.1:1",
                                        path, sizeof(path)) < 0);
    assert(cubicle_config_unix_uri_path("unix://relative.sock",
                                        path, sizeof(path)) < 0);
}

int main(void)
{
    test_defaults();
    assert(strcmp(cubicle_launch_default_name(CUBICLE_LAUNCH_FOREGROUND),
                  "foreground") == 0);
    assert(strcmp(cubicle_launch_default_name(CUBICLE_LAUNCH_BACKGROUND),
                  "background") == 0);
    test_override_file();
    test_user_config_file();
    test_client_user_policy();
    test_invalid_values();
    test_tcp_endpoints();
    test_unix_uri_path();
    return 0;
}
