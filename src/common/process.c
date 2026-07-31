#include "cubicle/process.h"

const char *cubicle_process_mode_name(cubicle_process_mode_t mode)
{
    switch (mode) {
    case CUBICLE_PROCESS_STREAM:
        return "stream";
    case CUBICLE_PROCESS_TTY:
        return "tty";
    case CUBICLE_PROCESS_TTY_CAPTURED_STDERR:
        return "tty-captured-stderr";
    default:
        return "unknown";
    }
}

const char *cubicle_process_state_name(cubicle_process_state_t state)
{
    switch (state) {
    case CUBICLE_PROCESS_STARTING:
        return "starting";
    case CUBICLE_PROCESS_RUNNING:
        return "running";
    case CUBICLE_PROCESS_EXITED:
        return "exited";
    case CUBICLE_PROCESS_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
