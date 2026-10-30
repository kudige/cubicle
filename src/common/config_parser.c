#define _POSIX_C_SOURCE 200809L

#include "config_parser.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CUBICLE_HAVE_LIBECONF
#include <libeconf.h>
#endif

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

#ifdef CUBICLE_HAVE_LIBECONF

struct cubicle_config_file {
    econf_file *file;
};

cubicle_config_parser_result_t cubicle_config_file_read(
    cubicle_config_file_t **file_out, const char *path, char *error,
    size_t error_size)
{
    *file_out = NULL;
    econf_file *file = NULL;
    econf_err result = econf_readFile(&file, path, "=", "#");
    if (result == ECONF_NOFILE) {
        return CUBICLE_CONFIG_PARSER_NO_FILE;
    }
    if (result != ECONF_SUCCESS) {
        set_error(error, error_size, econf_errString(result));
        return CUBICLE_CONFIG_PARSER_ERROR;
    }
    cubicle_config_file_t *parsed = calloc(1, sizeof(*parsed));
    if (parsed == NULL) {
        econf_free(file);
        set_error(error, error_size, "out of memory");
        return CUBICLE_CONFIG_PARSER_ERROR;
    }
    parsed->file = file;
    *file_out = parsed;
    return CUBICLE_CONFIG_PARSER_SUCCESS;
}

void cubicle_config_file_free(cubicle_config_file_t *file)
{
    if (file != NULL) {
        econf_free(file->file);
        free(file);
    }
}

cubicle_config_parser_result_t cubicle_config_file_get_string(
    const cubicle_config_file_t *file, const char *group, const char *key,
    char **value_out, char *error, size_t error_size)
{
    *value_out = NULL;
    econf_err result = econf_getStringValue(file->file, group, key, value_out);
    if (result == ECONF_NOKEY || result == ECONF_NOGROUP) {
        return CUBICLE_CONFIG_PARSER_NOT_FOUND;
    }
    if (result != ECONF_SUCCESS) {
        set_error(error, error_size, econf_errString(result));
        return CUBICLE_CONFIG_PARSER_ERROR;
    }
    return CUBICLE_CONFIG_PARSER_SUCCESS;
}

cubicle_config_parser_result_t cubicle_config_file_get_keys(
    const cubicle_config_file_t *file, const char *group, size_t *count_out,
    char ***keys_out, char *error, size_t error_size)
{
    *count_out = 0;
    *keys_out = NULL;
    econf_err result = econf_getKeys(file->file, group, count_out, keys_out);
    if (result == ECONF_NOKEY || result == ECONF_NOGROUP) {
        return CUBICLE_CONFIG_PARSER_NOT_FOUND;
    }
    if (result != ECONF_SUCCESS) {
        set_error(error, error_size, econf_errString(result));
        return CUBICLE_CONFIG_PARSER_ERROR;
    }
    return CUBICLE_CONFIG_PARSER_SUCCESS;
}

void cubicle_config_file_free_keys(char **keys, size_t count)
{
    (void)count;
    if (keys != NULL) {
        econf_free(keys);
    }
}

#else

static void set_line_error(char *error, size_t error_size, size_t line_number,
                           const char *message)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "line %zu: %s", line_number, message);
    }
}

typedef struct cubicle_config_entry {
    char *group;
    char *key;
    char *value;
} cubicle_config_entry_t;

struct cubicle_config_file {
    cubicle_config_entry_t *entries;
    size_t count;
    size_t capacity;
};

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static int add_entry(cubicle_config_file_t *file, const char *group,
                     const char *key, const char *value)
{
    for (size_t i = 0; i < file->count; ++i) {
        cubicle_config_entry_t *entry = &file->entries[i];
        if (strcmp(entry->group, group) == 0 && strcmp(entry->key, key) == 0) {
            char *replacement = strdup(value);
            if (replacement == NULL) {
                return -1;
            }
            free(entry->value);
            entry->value = replacement;
            return 0;
        }
    }
    if (file->count == file->capacity) {
        size_t capacity = file->capacity == 0 ? 16 : file->capacity * 2;
        cubicle_config_entry_t *entries = realloc(
            file->entries, capacity * sizeof(*file->entries));
        if (entries == NULL) {
            return -1;
        }
        file->entries = entries;
        file->capacity = capacity;
    }
    cubicle_config_entry_t *entry = &file->entries[file->count];
    memset(entry, 0, sizeof(*entry));
    entry->group = strdup(group);
    entry->key = strdup(key);
    entry->value = strdup(value);
    if (entry->group == NULL || entry->key == NULL || entry->value == NULL) {
        free(entry->group);
        free(entry->key);
        free(entry->value);
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    ++file->count;
    return 0;
}

cubicle_config_parser_result_t cubicle_config_file_read(
    cubicle_config_file_t **file_out, const char *path, char *error,
    size_t error_size)
{
    *file_out = NULL;
    FILE *stream = fopen(path, "r");
    if (stream == NULL) {
        if (errno == ENOENT) {
            return CUBICLE_CONFIG_PARSER_NO_FILE;
        }
        set_error(error, error_size, strerror(errno));
        return CUBICLE_CONFIG_PARSER_ERROR;
    }
    cubicle_config_file_t *file = calloc(1, sizeof(*file));
    if (file == NULL) {
        fclose(stream);
        set_error(error, error_size, "out of memory");
        return CUBICLE_CONFIG_PARSER_ERROR;
    }
    char group[256] = "";
    char *line = NULL;
    size_t line_capacity = 0;
    size_t line_number = 0;
    cubicle_config_parser_result_t result = CUBICLE_CONFIG_PARSER_SUCCESS;
    while (getline(&line, &line_capacity, stream) >= 0) {
        ++line_number;
        char *text = trim(line);
        if (text[0] == '\0' || text[0] == '#' || text[0] == ';') {
            continue;
        }
        if (text[0] == '[') {
            char *end = strchr(text + 1, ']');
            if (end == NULL || trim(end + 1)[0] != '\0') {
                set_line_error(error, error_size, line_number,
                               "invalid section");
                result = CUBICLE_CONFIG_PARSER_ERROR;
                break;
            }
            *end = '\0';
            char *name = trim(text + 1);
            if (name[0] == '\0' || strlen(name) >= sizeof(group)) {
                set_line_error(error, error_size, line_number,
                               "invalid section");
                result = CUBICLE_CONFIG_PARSER_ERROR;
                break;
            }
            snprintf(group, sizeof(group), "%s", name);
            continue;
        }
        char *separator = strchr(text, '=');
        if (separator == NULL) {
            set_line_error(error, error_size, line_number,
                           "expected key=value");
            result = CUBICLE_CONFIG_PARSER_ERROR;
            break;
        }
        *separator = '\0';
        char *key = trim(text);
        char *value = trim(separator + 1);
        if (group[0] == '\0' || key[0] == '\0') {
            set_line_error(error, error_size, line_number,
                           "key must be in a section");
            result = CUBICLE_CONFIG_PARSER_ERROR;
            break;
        }
        if (add_entry(file, group, key, value) < 0) {
            set_error(error, error_size, "out of memory");
            result = CUBICLE_CONFIG_PARSER_ERROR;
            break;
        }
    }
    if (ferror(stream) != 0 && result == CUBICLE_CONFIG_PARSER_SUCCESS) {
        set_error(error, error_size, strerror(errno));
        result = CUBICLE_CONFIG_PARSER_ERROR;
    }
    free(line);
    fclose(stream);
    if (result != CUBICLE_CONFIG_PARSER_SUCCESS) {
        cubicle_config_file_free(file);
        return result;
    }
    *file_out = file;
    return CUBICLE_CONFIG_PARSER_SUCCESS;
}

void cubicle_config_file_free(cubicle_config_file_t *file)
{
    if (file == NULL) {
        return;
    }
    for (size_t i = 0; i < file->count; ++i) {
        free(file->entries[i].group);
        free(file->entries[i].key);
        free(file->entries[i].value);
    }
    free(file->entries);
    free(file);
}

cubicle_config_parser_result_t cubicle_config_file_get_string(
    const cubicle_config_file_t *file, const char *group, const char *key,
    char **value_out, char *error, size_t error_size)
{
    *value_out = NULL;
    for (size_t i = 0; i < file->count; ++i) {
        const cubicle_config_entry_t *entry = &file->entries[i];
        if (strcmp(entry->group, group) == 0 && strcmp(entry->key, key) == 0) {
            *value_out = strdup(entry->value);
            if (*value_out == NULL) {
                set_error(error, error_size, "out of memory");
                return CUBICLE_CONFIG_PARSER_ERROR;
            }
            return CUBICLE_CONFIG_PARSER_SUCCESS;
        }
    }
    return CUBICLE_CONFIG_PARSER_NOT_FOUND;
}

cubicle_config_parser_result_t cubicle_config_file_get_keys(
    const cubicle_config_file_t *file, const char *group, size_t *count_out,
    char ***keys_out, char *error, size_t error_size)
{
    *count_out = 0;
    *keys_out = NULL;
    size_t count = 0;
    for (size_t i = 0; i < file->count; ++i) {
        if (strcmp(file->entries[i].group, group) == 0) {
            ++count;
        }
    }
    if (count == 0) {
        return CUBICLE_CONFIG_PARSER_NOT_FOUND;
    }
    char **keys = calloc(count, sizeof(*keys));
    if (keys == NULL) {
        set_error(error, error_size, "out of memory");
        return CUBICLE_CONFIG_PARSER_ERROR;
    }
    size_t index = 0;
    for (size_t i = 0; i < file->count; ++i) {
        if (strcmp(file->entries[i].group, group) != 0) {
            continue;
        }
        keys[index] = strdup(file->entries[i].key);
        if (keys[index] == NULL) {
            cubicle_config_file_free_keys(keys, index);
            set_error(error, error_size, "out of memory");
            return CUBICLE_CONFIG_PARSER_ERROR;
        }
        ++index;
    }
    *count_out = count;
    *keys_out = keys;
    return CUBICLE_CONFIG_PARSER_SUCCESS;
}

void cubicle_config_file_free_keys(char **keys, size_t count)
{
    if (keys == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(keys[i]);
    }
    free(keys);
}

#endif
