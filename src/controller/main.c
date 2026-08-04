#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include "cubicle/log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--daemon] [--completed-retention-ms N] [--debug input] [--stdin-policy open|eof] [--cwd dir] [--state-dir dir] [--log-dir dir] [--control-socket path] --mode stream|tty|term -- command [args...]\n",
            program);
}

static int parse_stdin_policy(const char *name, stdin_policy_t *policy)
{
    if (strcmp(name, "open") == 0) {
        *policy = STDIN_POLICY_OPEN;
        return 0;
    }

    if (strcmp(name, "eof") == 0) {
        *policy = STDIN_POLICY_EOF;
        return 0;
    }

    return -1;
}

static int daemonize_controller(void)
{
    pid_t child_pid = fork();
    if (child_pid < 0) {
        return -1;
    }

    if (child_pid > 0) {
        _exit(0);
    }

    if (setsid() < 0) {
        return -1;
    }

    signal(SIGHUP, SIG_IGN);

    child_pid = fork();
    if (child_pid < 0) {
        return -1;
    }

    if (child_pid > 0) {
        _exit(0);
    }

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0) {
        return -1;
    }

    if (dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(null_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0) {
        close(null_fd);
        return -1;
    }

    if (null_fd > STDERR_FILENO) {
        close(null_fd);
    }

    return 0;
}

static int parse_mode(const char *name, cubicle_process_mode_t *mode)
{
    if (strcmp(name, "stream") == 0) {
        *mode = CUBICLE_PROCESS_STREAM;
        return 0;
    }

    if (strcmp(name, "tty") == 0) {
        *mode = CUBICLE_PROCESS_TTY;
        return 0;
    }

    if (strcmp(name, "term") == 0 ||
        strcmp(name, "tty-captured-stderr") == 0) {
        *mode = CUBICLE_PROCESS_TTY_CAPTURED_STDERR;
        return 0;
    }

    return -1;
}

static int parse_nonnegative_int(const char *value, int *number)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < 0 || parsed > 2147483647L) {
        return -1;
    }

    *number = (int)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    const char *mode = NULL;
    const char *state_dir = NULL;
    const char *log_dir = NULL;
    const char *control_socket = NULL;
    const char *cwd = NULL;
    stdin_policy_t stdin_policy = STDIN_POLICY_OPEN;
    int daemon = 0;
    int completed_retention_ms = 0;
    int debug_input = 0;
    int command_index = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "--daemon") == 0) {
            daemon = 1;
            continue;
        }

        if (strcmp(argv[i], "--completed-retention-ms") == 0 && i + 1 < argc) {
            if (parse_nonnegative_int(argv[++i], &completed_retention_ms) < 0) {
                fprintf(stderr, "Invalid completed retention: %s\n", argv[i]);
                return 2;
            }
            continue;
        }

        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            state_dir = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            log_dir = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--control-socket") == 0 && i + 1 < argc) {
            control_socket = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
            cwd = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--stdin-policy") == 0 && i + 1 < argc) {
            if (parse_stdin_policy(argv[++i], &stdin_policy) < 0) {
                fprintf(stderr, "Unknown stdin policy: %s\n", argv[i]);
                return 2;
            }
            continue;
        }

        if (strcmp(argv[i], "--debug") == 0 && i + 1 < argc) {
            const char *debug = argv[++i];
            if (strcmp(debug, "input") == 0) {
                debug_input = 1;
            } else if (strcmp(debug, "none") == 0 ||
                       strcmp(debug, "off") == 0) {
                debug_input = 0;
            } else {
                fprintf(stderr, "Unknown debug option: %s\n", debug);
                return 2;
            }
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

    cubicle_process_mode_t process_mode = CUBICLE_PROCESS_STREAM;
    if (parse_mode(mode, &process_mode) < 0) {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return 2;
    }

    if (daemon && daemonize_controller() < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return 1;
    }

    if (process_mode == CUBICLE_PROCESS_TTY) {
        return run_tty(&argv[command_index], state_dir, log_dir, control_socket,
                       cwd, stdin_policy, completed_retention_ms, debug_input);
    }

    if (process_mode == CUBICLE_PROCESS_TTY_CAPTURED_STDERR) {
        return run_term(&argv[command_index], state_dir, log_dir, control_socket,
                        cwd, stdin_policy, completed_retention_ms, debug_input);
    }

    return run_stream(&argv[command_index], state_dir, log_dir, control_socket,
                      cwd, stdin_policy, completed_retention_ms, debug_input);
}
