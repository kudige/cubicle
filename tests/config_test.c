#define _POSIX_C_SOURCE 200809L

#include "cubicle/config.h"

#include <assert.h>
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
    assert(config.default_launch == CUBICLE_LAUNCH_FOREGROUND);
    assert(config.default_mode == CUBICLE_PROCESS_TTY_CAPTURED_STDERR);
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
               "controller_binary=/tmp/cubicle-controller\n"
               "\n"
               "[client]\n"
               "manager=unix:///tmp/cubicle-run/manager.sock\n"
               "\n"
               "[defaults]\n"
               "launch=background\n"
               "mode=stream\n");

    assert(setenv("CUBICLE_CONFIG", path, 1) == 0);
    cubicle_config_t config;
    char error[256];
    assert(cubicle_config_load(&config, error, sizeof(error)) == 0);
    assert(strcmp(config.manager_state_dir, "/tmp/cubicle-state") == 0);
    assert(strcmp(config.manager_runtime_dir, "/tmp/cubicle-run") == 0);
    assert(strcmp(config.manager_log_dir, "/tmp/cubicle-log") == 0);
    assert(strcmp(config.controller_binary, "/tmp/cubicle-controller") == 0);
    assert(config.default_launch == CUBICLE_LAUNCH_BACKGROUND);
    assert(config.default_mode == CUBICLE_PROCESS_STREAM);
    assert(unsetenv("CUBICLE_CONFIG") == 0);
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
               "manager=tcp://127.0.0.1:1234\n");
    assert(cubicle_config_load(&config, error, sizeof(error)) < 0);
    assert(strstr(error, "client.manager") != NULL);
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
    test_invalid_values();
    test_unix_uri_path();
    return 0;
}
