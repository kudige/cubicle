#define _POSIX_C_SOURCE 200809L

#include "config_parser.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
}

int main(void)
{
    char path[] = "/tmp/cubicle-config-parser-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
               "# a comment\n"
               "[manager]\n"
               "state_dir = /tmp/state\n"
               "state_dir = /tmp/override\n"
               "\n"
               "[desk.keys]\n"
               "bind.1=Prefix-n pane.next\n"
               "bind.2=Prefix-p pane.previous\n");

    cubicle_config_file_t *file = NULL;
    char error[128] = "";
    assert(cubicle_config_file_read(&file, path, error, sizeof(error)) ==
           CUBICLE_CONFIG_PARSER_SUCCESS);

    char *value = NULL;
    assert(cubicle_config_file_get_string(file, "manager", "state_dir",
                                          &value, error, sizeof(error)) ==
           CUBICLE_CONFIG_PARSER_SUCCESS);
    assert(strcmp(value, "/tmp/override") == 0);
    free(value);

    assert(cubicle_config_file_get_string(file, "manager", "missing",
                                          &value, error, sizeof(error)) ==
           CUBICLE_CONFIG_PARSER_NOT_FOUND);

    size_t key_count = 0;
    char **keys = NULL;
    assert(cubicle_config_file_get_keys(file, "desk.keys", &key_count, &keys,
                                        error, sizeof(error)) ==
           CUBICLE_CONFIG_PARSER_SUCCESS);
    assert(key_count == 2);
    assert(strcmp(keys[0], "bind.1") == 0);
    assert(strcmp(keys[1], "bind.2") == 0);
    cubicle_config_file_free_keys(keys, key_count);
    cubicle_config_file_free(file);

    write_file(path, "[manager\nstate_dir=/tmp/state\n");
    assert(cubicle_config_file_read(&file, path, error, sizeof(error)) ==
           CUBICLE_CONFIG_PARSER_ERROR);
    assert(strstr(error, "line 1") != NULL);

    assert(unlink(path) == 0);
    return 0;
}
