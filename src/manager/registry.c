#include "cubicle/manager_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cubicle_parse_workspace_record(char *line, cubicle_workspace_record_t *record)
{
    char *id = strtok(line, "\t\n");
    char *name = strtok(NULL, "\t\n");
    if (id == NULL || name == NULL) {
        return -1;
    }

    snprintf(record->id, sizeof(record->id), "%s", id);
    snprintf(record->name, sizeof(record->name), "%s", name);
    return 0;
}

int cubicle_parse_process_record(char *line, cubicle_process_record_t *record)
{
    char *process_id = strtok(line, "\t\n");
    char *workspace_id = strtok(NULL, "\t\n");
    char *friendly_name = strtok(NULL, "\t\n");
    char *mode = strtok(NULL, "\t\n");
    char *state = strtok(NULL, "\t\n");
    char *controller_id = strtok(NULL, "\t\n");
    char *control_socket = strtok(NULL, "\t\n");
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
