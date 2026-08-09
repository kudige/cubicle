#include "cubicle/manager_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *next_field(char **cursor)
{
    if (*cursor == NULL) {
        return NULL;
    }
    char *field = *cursor;
    char *separator = strpbrk(field, "\t\n");
    if (separator == NULL) {
        *cursor = NULL;
    } else {
        *separator = '\0';
        *cursor = separator + 1;
    }
    return field;
}

int cubicle_parse_workspace_record(char *line, cubicle_workspace_record_t *record)
{
    char *id = strtok(line, "\t\n");
    char *name = strtok(NULL, "\t\n");
    char *directory = strtok(NULL, "\t\n");
    if (id == NULL || name == NULL) {
        return -1;
    }

    snprintf(record->id, sizeof(record->id), "%s", id);
    snprintf(record->name, sizeof(record->name), "%s", name);
    snprintf(record->directory, sizeof(record->directory), "%s",
             directory == NULL ? "" : directory);
    return 0;
}

int cubicle_parse_process_record(char *line, cubicle_process_record_t *record)
{
    char *cursor = line;
    char *process_id = next_field(&cursor);
    char *workspace_id = next_field(&cursor);
    char *friendly_name = next_field(&cursor);
    char *mode = next_field(&cursor);
    char *state = next_field(&cursor);
    char *controller_id = next_field(&cursor);
    char *control_socket = next_field(&cursor);
    char *cwd = next_field(&cursor);
    char *saved = next_field(&cursor);
    char *argv_json = next_field(&cursor);
    char *restart = next_field(&cursor);
    char *stdin_policy = next_field(&cursor);
    char *workspace_name = next_field(&cursor);
    char *workspace_directory = next_field(&cursor);
    if (process_id == NULL || workspace_id == NULL || friendly_name == NULL ||
        mode == NULL || state == NULL || controller_id == NULL ||
        control_socket == NULL) {
        return -1;
    }

    snprintf(record->process_id, sizeof(record->process_id), "%s", process_id);
    snprintf(record->workspace_id, sizeof(record->workspace_id), "%s", workspace_id);
    snprintf(record->friendly_name, sizeof(record->friendly_name), "%s", friendly_name);
    snprintf(record->mode, sizeof(record->mode), "%s", mode);
    snprintf(record->state, sizeof(record->state), "%s", state);
    snprintf(record->controller_id, sizeof(record->controller_id), "%s", controller_id);
    snprintf(record->control_socket, sizeof(record->control_socket), "%s", control_socket);
    snprintf(record->cwd, sizeof(record->cwd), "%s", cwd == NULL ? "" : cwd);
    record->saved = saved != NULL && strcmp(saved, "1") == 0 ? 1 : 0;
    snprintf(record->argv_json, sizeof(record->argv_json), "%s",
             argv_json == NULL ? "" : argv_json);
    record->restart = restart != NULL && strcmp(restart, "1") == 0 ? 1 : 0;
    snprintf(record->stdin_policy, sizeof(record->stdin_policy), "%s",
             stdin_policy == NULL ? "open" : stdin_policy);
    snprintf(record->workspace_name, sizeof(record->workspace_name), "%s",
             workspace_name == NULL ? "" : workspace_name);
    snprintf(record->workspace_directory, sizeof(record->workspace_directory),
             "%s", workspace_directory == NULL ? "" : workspace_directory);
    return 0;
}

int cubicle_parse_cursor_record(char *line, cubicle_cursor_record_t *record)
{
    char *process_id = strtok(line, "\t\n");
    char *sequence = strtok(NULL, "\t\n");
    char *offset = strtok(NULL, "\t\n");
    if (process_id == NULL || sequence == NULL) {
        return -1;
    }

    snprintf(record->process_id, sizeof(record->process_id), "%s", process_id);
    record->sequence = atoll(sequence);
    record->offset = offset == NULL ? 0 : atoll(offset);
    return 0;
}

int cubicle_parse_event_sequence(const char *line, long long *sequence)
{
    return sscanf(line, "seq=%lld", sequence) == 1 ? 0 : -1;
}
