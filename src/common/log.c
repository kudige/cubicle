#include "cubicle/log.h"

#include <stdio.h>

static const char *level_name(cubicle_log_level_t level)
{
    switch (level) {
    case CUBICLE_LOG_DEBUG:
        return "DEBUG";
    case CUBICLE_LOG_INFO:
        return "INFO";
    case CUBICLE_LOG_WARN:
        return "WARN";
    case CUBICLE_LOG_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

void cubicle_log(cubicle_log_level_t level, const char *component, const char *message)
{
    const char *safe_component = component != NULL ? component : "cubicle";
    const char *safe_message = message != NULL ? message : "";

    fprintf(stderr, "[%s] %s: %s\n", level_name(level), safe_component, safe_message);
}
