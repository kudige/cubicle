#include "cubicle/process.h"

const char *cubicle_process_mode_name(cubicle_process_mode_t mode)
{
    switch (mode) {
    case CUBICLE_PROCESS_STREAM:
        return "stream";
    case CUBICLE_PROCESS_TTY:
        return "tty";
    case CUBICLE_PROCESS_TTY_CAPTURED_STDERR:
        return "term";
    default:
        return "unknown";
    }
}

const char *cubicle_process_state_name(cubicle_process_state_t state)
{
    switch (state) {
    case CUBICLE_PROCESS_ALLOCATED:
        return "allocated";
    case CUBICLE_PROCESS_STARTING:
        return "starting";
    case CUBICLE_PROCESS_RUNNING:
        return "running";
    case CUBICLE_PROCESS_STOPPING:
        return "stopping";
    case CUBICLE_PROCESS_DRAINING:
        return "draining";
    case CUBICLE_PROCESS_COMPLETED:
        return "completed";
    case CUBICLE_PROCESS_FAILED:
        return "failed";
    case CUBICLE_PROCESS_LOST:
        return "lost";
    case CUBICLE_PROCESS_REMOVED:
        return "removed";
    default:
        return "unknown";
    }
}
