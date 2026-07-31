#include "cubicle/log.h"

#include <stdio.h>
#include <string.h>

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--help] [--foreground]\n", program);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "--foreground") != 0) {
            print_usage(argv[0]);
            return 2;
        }
    }

    cubicle_log(CUBICLE_LOG_INFO, "manager",
                "starting workspace namespace and controller registry");
    cubicle_log(CUBICLE_LOG_INFO, "manager",
                "placeholder manager has no event loop yet");
    return 0;
}
