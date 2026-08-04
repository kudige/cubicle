#define _POSIX_C_SOURCE 200809L

#include "cubeui.h"

#include "cubicle/util.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int cubeui_write_all(int fd, const char *buffer, size_t length)
{
    return cubicle_write_all(fd, buffer, length);
}

int cubeui_terminal_query_size(cubeui_terminal_t *terminal)
{
    if (terminal == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct winsize size;
    memset(&size, 0, sizeof(size));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
        return -1;
    }

    terminal->rows = size.ws_row > 0 ? size.ws_row : 24;
    terminal->cols = size.ws_col > 0 ? size.ws_col : 80;
    return 0;
}

int cubeui_terminal_enter_alt_raw(cubeui_terminal_t *terminal)
{
    if (terminal == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(terminal, 0, sizeof(*terminal));

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &terminal->original) < 0) {
        return -1;
    }

    struct termios raw = terminal->original;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= (tcflag_t) ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        return -1;
    }
    terminal->raw_enabled = true;

    if (cubeui_terminal_query_size(terminal) < 0) {
        return -1;
    }

    return cubeui_write_all(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 14);
}

void cubeui_terminal_leave_alt_raw(cubeui_terminal_t *terminal)
{
    (void)cubeui_write_all(STDOUT_FILENO, "\x1b[?25h\x1b[?1049l", 14);
    if (terminal != NULL && terminal->raw_enabled) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal->original);
        terminal->raw_enabled = false;
    }
}
