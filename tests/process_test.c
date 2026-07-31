#include "cubicle/process.h"

#include <stdio.h>
#include <string.h>

static int expect_string(const char *name, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) == 0) {
        return 0;
    }

    fprintf(stderr, "%s: expected '%s', got '%s'\n", name, expected, actual);
    return 1;
}

int main(void)
{
    int failures = 0;

    failures += expect_string("stream mode",
                              cubicle_process_mode_name(CUBICLE_PROCESS_STREAM),
                              "stream");
    failures += expect_string("tty mode",
                              cubicle_process_mode_name(CUBICLE_PROCESS_TTY),
                              "tty");
    failures += expect_string("tty captured stderr mode",
                              cubicle_process_mode_name(CUBICLE_PROCESS_TTY_CAPTURED_STDERR),
                              "tty-captured-stderr");
    failures += expect_string("unknown mode",
                              cubicle_process_mode_name((cubicle_process_mode_t)99),
                              "unknown");

    failures += expect_string("starting state",
                              cubicle_process_state_name(CUBICLE_PROCESS_STARTING),
                              "starting");
    failures += expect_string("running state",
                              cubicle_process_state_name(CUBICLE_PROCESS_RUNNING),
                              "running");
    failures += expect_string("allocated state",
                              cubicle_process_state_name(CUBICLE_PROCESS_ALLOCATED),
                              "allocated");
    failures += expect_string("stopping state",
                              cubicle_process_state_name(CUBICLE_PROCESS_STOPPING),
                              "stopping");
    failures += expect_string("draining state",
                              cubicle_process_state_name(CUBICLE_PROCESS_DRAINING),
                              "draining");
    failures += expect_string("completed state",
                              cubicle_process_state_name(CUBICLE_PROCESS_COMPLETED),
                              "completed");
    failures += expect_string("failed state",
                              cubicle_process_state_name(CUBICLE_PROCESS_FAILED),
                              "failed");
    failures += expect_string("lost state",
                              cubicle_process_state_name(CUBICLE_PROCESS_LOST),
                              "lost");
    failures += expect_string("removed state",
                              cubicle_process_state_name(CUBICLE_PROCESS_REMOVED),
                              "removed");
    failures += expect_string("unknown state",
                              cubicle_process_state_name((cubicle_process_state_t)99),
                              "unknown");

    return failures == 0 ? 0 : 1;
}
