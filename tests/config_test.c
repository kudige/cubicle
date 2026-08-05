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
    assert(config.cube_debug_library == 0);
    assert(config.desk_debug_library == 0);
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
               "debug=input\n"
               "\n"
               "[cube]\n"
               "debug=library\n"
               "\n"
               "[desk]\n"
               "debug=library\n"
               "\n"
               "[client]\n"
               "manager=unix:///tmp/cubicle-run/manager.sock\n"
               "\n"
               "[defaults]\n"
               "launch=background\n"
               "mode=stream\n"
               "kill_cleanup=true\n");

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
    assert(config.cube_debug_library == 1);
    assert(config.desk_debug_library == 1);
    assert(config.default_launch == CUBICLE_LAUNCH_BACKGROUND);
    assert(config.default_mode == CUBICLE_PROCESS_STREAM);
    assert(config.default_kill_cleanup == 1);
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
               "debug=input\n"
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
    assert(config.cube_debug_library == 1);
    assert(config.desk_debug_library == 0);
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
               "[cube]\n"
               "debug=input\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "cube.debug") != NULL);

    write_file(path,
               "[desk]\n"
               "debug=input\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "desk.debug") != NULL);
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
    test_invalid_values();
    test_tcp_endpoints();
    test_unix_uri_path();
    return 0;
}
