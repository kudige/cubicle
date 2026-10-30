#ifndef CUBICLE_CONFIG_PARSER_H
#define CUBICLE_CONFIG_PARSER_H

#include <stddef.h>

typedef struct cubicle_config_file cubicle_config_file_t;

typedef enum cubicle_config_parser_result {
    CUBICLE_CONFIG_PARSER_SUCCESS = 0,
    CUBICLE_CONFIG_PARSER_NO_FILE,
    CUBICLE_CONFIG_PARSER_NOT_FOUND,
    CUBICLE_CONFIG_PARSER_ERROR
} cubicle_config_parser_result_t;

cubicle_config_parser_result_t cubicle_config_file_read(
    cubicle_config_file_t **file_out,
    const char *path,
    char *error,
    size_t error_size);

void cubicle_config_file_free(cubicle_config_file_t *file);

cubicle_config_parser_result_t cubicle_config_file_get_string(
    const cubicle_config_file_t *file,
    const char *group,
    const char *key,
    char **value_out,
    char *error,
    size_t error_size);

cubicle_config_parser_result_t cubicle_config_file_get_keys(
    const cubicle_config_file_t *file,
    const char *group,
    size_t *count_out,
    char ***keys_out,
    char *error,
    size_t error_size);

void cubicle_config_file_free_keys(char **keys, size_t count);

#endif
