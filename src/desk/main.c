#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_resize_requested = 1;
static volatile sig_atomic_t g_stop_requested = 0;

typedef struct desk_terminal {
    struct termios original;
    bool raw_enabled;
    int rows;
    int cols;
} desk_terminal_t;

typedef struct desk_layout {
    int left_width;
    int right_width;
    int body_rows;
    int top_rows;
} desk_layout_t;

static void handle_signal(int signo)
{
    if (signo == SIGWINCH) {
        g_resize_requested = 1;
    } else {
        g_stop_requested = 1;
    }
}

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t rc = write(fd, buffer + written, length - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)rc;
    }
    return 0;
}

static int terminal_query_size(desk_terminal_t *terminal)
{
    struct winsize size;
    memset(&size, 0, sizeof(size));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
        return -1;
    }

    terminal->rows = size.ws_row > 0 ? size.ws_row : 24;
    terminal->cols = size.ws_col > 0 ? size.ws_col : 80;
    return 0;
}

static int terminal_enter(desk_terminal_t *terminal)
{
    memset(terminal, 0, sizeof(*terminal));

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &terminal->original) < 0) {
        return -1;
    }

    struct termios raw = terminal->original;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= (tcflag_t) ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        return -1;
    }
    terminal->raw_enabled = true;

    if (terminal_query_size(terminal) < 0) {
        return -1;
    }

    return write_all(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 14);
}

static void terminal_leave(desk_terminal_t *terminal)
{
    (void)write_all(STDOUT_FILENO, "\x1b[?25h\x1b[?1049l", 14);
    if (terminal->raw_enabled) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal->original);
        terminal->raw_enabled = false;
    }
}

static void append_repeat(char *buffer, size_t buffer_size, size_t *used,
                          char ch, int count)
{
    for (int i = 0; i < count && *used + 1 < buffer_size; ++i) {
        buffer[(*used)++] = ch;
    }
    if (*used < buffer_size) {
        buffer[*used] = '\0';
    }
}

static void append_text(char *buffer, size_t buffer_size, size_t *used,
                        const char *text)
{
    while (*text != '\0' && *used + 1 < buffer_size) {
        buffer[(*used)++] = *text++;
    }
    if (*used < buffer_size) {
        buffer[*used] = '\0';
    }
}

static void append_cell_text(char *buffer, size_t buffer_size, size_t *used,
                             const char *text, int width)
{
    int emitted = 0;
    while (*text != '\0' && emitted < width && *used + 1 < buffer_size) {
        buffer[(*used)++] = *text++;
        emitted++;
    }
    append_repeat(buffer, buffer_size, used, ' ', width - emitted);
}

static bool desk_get_layout(const desk_terminal_t *terminal,
                            desk_layout_t *layout)
{
    int rows = terminal->rows;
    int cols = terminal->cols;

    if (rows < 6 || cols < 24) {
        return false;
    }

    layout->left_width = cols / 2;
    layout->right_width = cols - layout->left_width - 1;
    layout->body_rows = rows - 3;
    layout->top_rows = layout->body_rows / 2;
    return true;
}

static void desk_render_layout(const desk_terminal_t *terminal)
{
    char frame[16384];
    size_t used = 0;
    desk_layout_t layout;

    if (!desk_get_layout(terminal, &layout)) {
        append_text(frame, sizeof(frame), &used, "\x1b[H\x1b[2J");
        append_text(frame, sizeof(frame), &used,
                    "Terminal too small for desk. Press q to quit.");
        (void)write_all(STDOUT_FILENO, frame, used);
        return;
    }

    append_text(frame, sizeof(frame), &used, "\x1b[H\x1b[2J");
    append_text(frame, sizeof(frame), &used, "Cubicle Desk");
    append_repeat(frame, sizeof(frame), &used, ' ', terminal->cols - 29);
    append_text(frame, sizeof(frame), &used, "q quit | resize aware\r\n");
    append_repeat(frame, sizeof(frame), &used, '-', terminal->cols);
    append_text(frame, sizeof(frame), &used, "\r\n");

    for (int row = 0; row < layout.body_rows; ++row) {
        const char *left = "";
        const char *right = "";

        if (row == 0) {
            right = "cube 2: editor";
        } else if (row == layout.top_rows + 1) {
            right = "cube 3: logs";
        }

        append_cell_text(frame, sizeof(frame), &used, left, layout.left_width);
        append_text(frame, sizeof(frame), &used, "|");

        if (row == layout.top_rows) {
            append_repeat(frame, sizeof(frame), &used, '-', layout.right_width);
        } else {
            append_cell_text(frame, sizeof(frame), &used, right,
                             layout.right_width);
        }
        append_text(frame, sizeof(frame), &used, "\r\n");
    }

    append_repeat(frame, sizeof(frame), &used, '-', terminal->cols);
    (void)write_all(STDOUT_FILENO, frame, used);
}

static void desk_render_cube_one(const desk_terminal_t *terminal,
                                 unsigned long long counter)
{
    char frame[16384];
    size_t used = 0;
    desk_layout_t layout;

    if (!desk_get_layout(terminal, &layout)) {
        return;
    }

    for (int row = 0; row < layout.body_rows; ++row) {
        char line[128];
        const char *text = "";
        int terminal_row = row + 3;

        if (row == 0) {
            text = "cube 1: counter";
        } else {
            int scroll_rows = layout.body_rows - 1;
            unsigned long long first_visible = 1;
            unsigned long long line_number = 0;

            if (counter > (unsigned long long)scroll_rows) {
                first_visible = counter - (unsigned long long)scroll_rows + 1;
            }
            line_number = first_visible + (unsigned long long)row - 1;
            if (line_number <= counter) {
                (void)snprintf(line, sizeof(line), "%llu", line_number);
                text = line;
            }
        }

        char cursor[32];
        int cursor_length = snprintf(cursor, sizeof(cursor), "\x1b[%d;1H",
                                     terminal_row);
        if (cursor_length > 0 && (size_t)cursor_length < sizeof(cursor)) {
            append_text(frame, sizeof(frame), &used, cursor);
        }
        append_cell_text(frame, sizeof(frame), &used, text, layout.left_width);
    }

    (void)write_all(STDOUT_FILENO, frame, used);
}

static int desk_run(void)
{
    desk_terminal_t terminal;
    if (terminal_enter(&terminal) < 0) {
        return -1;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGWINCH, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    unsigned long long counter = 0;
    time_t last_tick = 0;

    while (!g_stop_requested) {
        if (g_resize_requested) {
            g_resize_requested = 0;
            if (terminal_query_size(&terminal) == 0) {
                desk_render_layout(&terminal);
                desk_render_cube_one(&terminal, counter);
            }
        }

        time_t now = time(NULL);
        if (now != (time_t)-1 && now != last_tick) {
            last_tick = now;
            counter++;
            desk_render_cube_one(&terminal, counter);
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        int ready = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            terminal_leave(&terminal);
            return -1;
        }
        if (ready == 0 || !FD_ISSET(STDIN_FILENO, &read_set)) {
            continue;
        }

        unsigned char input[32];
        ssize_t length = read(STDIN_FILENO, input, sizeof(input));
        if (length < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            terminal_leave(&terminal);
            return -1;
        }
        for (ssize_t i = 0; i < length; ++i) {
            if (input[i] == 'q' || input[i] == 3) {
                g_stop_requested = 1;
            }
        }
    }

    terminal_leave(&terminal);
    return 0;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s\n", program);
    fprintf(stream, "Render the Cubicle desk terminal view.\n");
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        print_usage(stderr, argv[0]);
        return 2;
    }

    if (desk_run() < 0) {
        fprintf(stderr, "desk: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
