#ifndef CUBICLE_LOG_H
#define CUBICLE_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cubicle_log_level {
    CUBICLE_LOG_DEBUG = 0,
    CUBICLE_LOG_INFO = 1,
    CUBICLE_LOG_WARN = 2,
    CUBICLE_LOG_ERROR = 3
} cubicle_log_level_t;

void cubicle_log(cubicle_log_level_t level, const char *component, const char *message);

#ifdef __cplusplus
}
#endif

#endif
