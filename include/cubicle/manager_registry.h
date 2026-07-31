#ifndef CUBICLE_MANAGER_REGISTRY_H
#define CUBICLE_MANAGER_REGISTRY_H

#include "cubicle/util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CUBICLE_MANAGER_ID_LENGTH 32

typedef struct cubicle_workspace_record {
    char id[CUBICLE_MANAGER_ID_LENGTH + 1];
    char name[128];
} cubicle_workspace_record_t;

typedef struct cubicle_process_record {
    char process_id[CUBICLE_MANAGER_ID_LENGTH + 1];
    char workspace_id[CUBICLE_MANAGER_ID_LENGTH + 1];
    char friendly_name[128];
    char mode[32];
    char state[32];
    char controller_id[CUBICLE_MANAGER_ID_LENGTH + 1];
    char control_socket[CUBICLE_PATH_MAX];
} cubicle_process_record_t;

typedef struct cubicle_cursor_record {
    char process_id[CUBICLE_MANAGER_ID_LENGTH + 1];
    long long sequence;
} cubicle_cursor_record_t;

int cubicle_parse_workspace_record(char *line, cubicle_workspace_record_t *record);
int cubicle_parse_process_record(char *line, cubicle_process_record_t *record);
int cubicle_parse_cursor_record(char *line, cubicle_cursor_record_t *record);
int cubicle_parse_event_sequence(const char *line, long long *sequence);

#ifdef __cplusplus
}
#endif

#endif
