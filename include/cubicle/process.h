#ifndef CUBICLE_PROCESS_H
#define CUBICLE_PROCESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cubicle_process_mode {
    CUBICLE_PROCESS_STREAM = 0,
    CUBICLE_PROCESS_TTY = 1,
    CUBICLE_PROCESS_TTY_CAPTURED_STDERR = 2
} cubicle_process_mode_t;

typedef enum cubicle_process_state {
    CUBICLE_PROCESS_STARTING = 0,
    CUBICLE_PROCESS_RUNNING = 1,
    CUBICLE_PROCESS_EXITED = 2,
    CUBICLE_PROCESS_FAILED = 3
} cubicle_process_state_t;

typedef struct cubicle_process_ref {
    char process_id[37];
    char workspace_id[37];
    char friendly_name[128];
    cubicle_process_mode_t mode;
    cubicle_process_state_t state;
    int64_t pid;
    int64_t process_group_id;
} cubicle_process_ref_t;

const char *cubicle_process_mode_name(cubicle_process_mode_t mode);
const char *cubicle_process_state_name(cubicle_process_state_t state);

#ifdef __cplusplus
}
#endif

#endif
