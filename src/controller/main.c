#include "cubicle/log.h"

#include <stdio.h>
#include <string.h>

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --mode stream|tty|tty-captured-stderr -- command [args...]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *mode = NULL;
    int command_index = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--") == 0 && i + 1 < argc) {
            command_index = i + 1;
            break;
        }
    }

    if (mode == NULL || command_index < 0) {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(mode, "stream") != 0 &&
        strcmp(mode, "tty") != 0 &&
        strcmp(mode, "tty-captured-stderr") != 0) {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return 2;
    }

    char message[256];
    snprintf(message, sizeof(message),
             "would launch '%s' in %s mode", argv[command_index], mode);
    cubicle_log(CUBICLE_LOG_INFO, "controller", message);
    cubicle_log(CUBICLE_LOG_INFO, "controller",
                "placeholder controller does not spawn processes yet");
    return 0;
}
