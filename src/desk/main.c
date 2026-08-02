#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cubicle/attachment.h"
#include "cubicle/client.h"
#include "cubicle/config.h"
#include "cubicle/process.h"
#include "cubicle/transport_tcp.h"
#include "cubicle/transport_unix.h"
#include "cubicle/types.h"
#include "cubicle/workspace.h"

#ifndef PATH_MAX
#define PATH_MAX CUBICLE_PATH_MAX
#endif

// Border glyphs
#define DESK_ACTIVE_HORIZONTAL "─"
#define DESK_ACTIVE_VERTICAL "│"
#define DESK_ACTIVE_TOP_JUNCTION "┬"
#define DESK_ACTIVE_MIDDLE_JUNCTION "┼"
#define DESK_ACTIVE_BOTTOM_JUNCTION "┴"
#define DESK_INACTIVE_HORIZONTAL "╌"
#define DESK_INACTIVE_VERTICAL "┊"
#define DESK_INACTIVE_TOP_JUNCTION "┄"
#define DESK_INACTIVE_MIDDLE_JUNCTION "┄"
#define DESK_INACTIVE_BOTTOM_JUNCTION "┄"

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

typedef enum desk_active_cube {
    DESK_ACTIVE_CUBE_ONE = 1,
    DESK_ACTIVE_CUBE_TWO = 2,
    DESK_ACTIVE_CUBE_THREE = 3
} desk_active_cube_t;

typedef struct desk_grid {
    char *cells;
    int rows;
    int cols;
    int cursor_row;
    int cursor_col;
    bool escape_active;
    bool csi_active;
    char csi[64];
    size_t csi_length;
} desk_grid_t;

typedef struct desk_attachment {
    cubicle_client_t *manager;
    cubicle_attachment_t *attachment;
    cubicle_process_info_t process;
    char workspace[CUBICLE_NAME_MAX];
    char workspace_id[CUBICLE_ID_STRING_LENGTH];
} desk_attachment_t;

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

static void append_repeat_text(char *buffer, size_t buffer_size, size_t *used,
                               const char *text, int count)
{
    for (int i = 0; i < count; ++i) {
        append_text(buffer, buffer_size, used, text);
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

static desk_active_cube_t next_active_cube(desk_active_cube_t active)
{
    return active == DESK_ACTIVE_CUBE_THREE
               ? DESK_ACTIVE_CUBE_ONE
               : (desk_active_cube_t)(active + 1);
}

static const char *horizontal_border_glyph(desk_active_cube_t active,
                                           desk_active_cube_t cube)
{
    return active == cube ? DESK_ACTIVE_HORIZONTAL : DESK_INACTIVE_HORIZONTAL;
}

static const char *vertical_border_glyph(desk_active_cube_t active,
                                         desk_active_cube_t cube)
{
    return active == cube ? DESK_ACTIVE_VERTICAL : DESK_INACTIVE_VERTICAL;
}

static const char *top_junction_glyph(desk_active_cube_t active)
{
    return active == DESK_ACTIVE_CUBE_ONE || active == DESK_ACTIVE_CUBE_TWO
               ? DESK_ACTIVE_TOP_JUNCTION
               : DESK_INACTIVE_TOP_JUNCTION;
}

static const char *middle_junction_glyph(desk_active_cube_t active)
{
    return active == DESK_ACTIVE_CUBE_ONE ||
                   active == DESK_ACTIVE_CUBE_TWO ||
                   active == DESK_ACTIVE_CUBE_THREE
               ? DESK_ACTIVE_MIDDLE_JUNCTION
               : DESK_INACTIVE_MIDDLE_JUNCTION;
}

static const char *bottom_junction_glyph(desk_active_cube_t active)
{
    return active == DESK_ACTIVE_CUBE_ONE || active == DESK_ACTIVE_CUBE_THREE
               ? DESK_ACTIVE_BOTTOM_JUNCTION
               : DESK_INACTIVE_BOTTOM_JUNCTION;
}

static int selected_workspace_path(char path[PATH_MAX])
{
    const char *state_home = getenv("XDG_STATE_HOME");
    if (state_home != NULL && state_home[0] != '\0') {
        int length = snprintf(path, PATH_MAX,
                              "%s/cubicle/current-workspace", state_home);
        return length < 0 || length >= PATH_MAX ? -1 : 0;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        int length = snprintf(path, PATH_MAX, ".cubicle/current-workspace");
        return length < 0 || length >= PATH_MAX ? -1 : 0;
    }
    int length = snprintf(path, PATH_MAX,
                          "%s/.local/state/cubicle/current-workspace", home);
    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static int read_selected_workspace(char *buffer, size_t buffer_size)
{
    char path[PATH_MAX];
    if (selected_workspace_path(path) < 0) {
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    if (fgets(buffer, (int)buffer_size, file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);
    buffer[strcspn(buffer, "\n")] = '\0';
    return buffer[0] == '\0' ? -1 : 0;
}

static int endpoint_from_uri(cubicle_endpoint_t *endpoint,
                             const char *uri)
{
    memset(endpoint, 0, sizeof(*endpoint));
    if (uri == NULL || uri[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (uri[0] == '/') {
        int length = snprintf(endpoint->uri, sizeof(endpoint->uri),
                              "unix://%s", uri);
        return length < 0 || (size_t)length >= sizeof(endpoint->uri) ? -1 : 0;
    }
    int length = snprintf(endpoint->uri, sizeof(endpoint->uri), "%s", uri);
    return length < 0 || (size_t)length >= sizeof(endpoint->uri) ? -1 : 0;
}

static cubicle_error_code_t connect_client(cubicle_client_t **client_out,
                                           char *error,
                                           size_t error_size)
{
    cubicle_config_t config;
    char config_error[512];
    if (cubicle_config_load(&config, config_error, sizeof(config_error)) < 0) {
        snprintf(error, error_size, "configuration error: %s", config_error);
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    const char *manager_uri = getenv("CUBICLE_MANAGER_SOCKET");
    if (manager_uri == NULL || manager_uri[0] == '\0') {
        manager_uri = config.client_manager_uri;
    }

    cubicle_endpoint_t endpoint;
    if (endpoint_from_uri(&endpoint, manager_uri) < 0) {
        snprintf(error, error_size, "manager endpoint is invalid");
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    cubicle_transport_t *transport = NULL;
    cubicle_error_code_t code;
    if (strncmp(endpoint.uri, "tcp://", 6) == 0) {
        code = cubicle_transport_tcp_create(&transport);
    } else {
        code = cubicle_transport_unix_create(&transport);
    }
    if (code != CUBICLE_OK) {
        snprintf(error, error_size, "failed to create manager transport");
        return code;
    }

    cubicle_client_options_t options;
    memset(&options, 0, sizeof(options));
    options.endpoint = endpoint;
    options.transport = transport;

    code = cubicle_client_connect(&options, client_out);
    if (code != CUBICLE_OK) {
        if (transport->vtable != NULL && transport->vtable->destroy != NULL) {
            transport->vtable->destroy(transport);
        }
        snprintf(error, error_size, "failed to connect to manager");
    }
    return code;
}

static cubicle_error_code_t resolve_attachment_target(
    const char *process_name,
    desk_attachment_t *target,
    char *error,
    size_t error_size)
{
    memset(target, 0, sizeof(*target));
    if (read_selected_workspace(target->workspace,
                                sizeof(target->workspace)) < 0) {
        snprintf(error, error_size, "no workspace selected");
        return CUBICLE_ERR_NOT_FOUND;
    }

    cubicle_error_code_t code = connect_client(&target->manager, error,
                                               error_size);
    if (code != CUBICLE_OK) {
        return code;
    }

    cubicle_workspace_info_t *workspaces = NULL;
    size_t workspace_count = 0;
    cubicle_page_info_t page;
    memset(&page, 0, sizeof(page));
    code = cubicle_workspace_list(target->manager, NULL, &workspaces,
                                   &workspace_count, &page);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(target->manager);
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "workspace list failed");
        return code;
    }
    for (size_t i = 0; i < workspace_count; ++i) {
        if (strcmp(workspaces[i].name, target->workspace) == 0 ||
            strcmp(workspaces[i].id, target->workspace) == 0) {
            snprintf(target->workspace_id, sizeof(target->workspace_id), "%s",
                     workspaces[i].id);
            break;
        }
    }
    cubicle_workspace_list_free(workspaces);
    code = cubicle_process_get(
        target->manager, process_name,
        target->workspace_id[0] == '\0' ? NULL : target->workspace_id,
        &target->process);
    if (code != CUBICLE_OK) {
        cubicle_process_filter_t filter;
        memset(&filter, 0, sizeof(filter));
        filter.workspace_id =
            target->workspace_id[0] == '\0' ? NULL : target->workspace_id;
        filter.include_completed = true;

        cubicle_process_info_t *processes = NULL;
        size_t process_count = 0;
        cubicle_page_info_t process_page;
        memset(&process_page, 0, sizeof(process_page));
        code = cubicle_process_list(target->manager, &filter, &processes,
                                    &process_count, &process_page);
        if (code == CUBICLE_OK) {
            code = CUBICLE_ERR_NOT_FOUND;
            for (size_t i = 0; i < process_count; ++i) {
                if (strcmp(processes[i].id, process_name) == 0 ||
                    strcmp(processes[i].friendly_name, process_name) == 0) {
                    target->process = processes[i];
                    code = CUBICLE_OK;
                    break;
                }
            }
        }
        cubicle_process_list_free(processes);
        if (code != CUBICLE_OK) {
            const cubicle_error_t *last =
                cubicle_client_last_error(target->manager);
            snprintf(error, error_size, "%s",
                     last != NULL && last->message[0] != '\0'
                         ? last->message
                         : "process not found");
            return code;
        }
    }

    if (target->process.mode != CUBICLE_PROCESS_TTY &&
        target->process.mode != CUBICLE_PROCESS_TTY_CAPTURED_STDERR) {
        snprintf(error, error_size,
                 "process '%s' is not a TTY process", process_name);
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    return CUBICLE_OK;
}

static void desk_attachment_cleanup(desk_attachment_t *target)
{
    cubicle_attachment_disconnect(target->attachment);
    target->attachment = NULL;
    cubicle_client_disconnect(target->manager);
    target->manager = NULL;
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

static void desk_render_layout(const desk_terminal_t *terminal,
                               desk_active_cube_t active)
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
    append_repeat(frame, sizeof(frame), &used, ' ', terminal->cols - 44);
    append_text(frame, sizeof(frame), &used,
                "Ctrl-Space switch | q quit | resize aware\r\n");
    append_repeat_text(frame, sizeof(frame), &used,
                       horizontal_border_glyph(active, DESK_ACTIVE_CUBE_ONE),
                       layout.left_width);
    append_text(frame, sizeof(frame), &used, top_junction_glyph(active));
    append_repeat_text(frame, sizeof(frame), &used,
                       horizontal_border_glyph(active, DESK_ACTIVE_CUBE_TWO),
                       layout.right_width);
    append_text(frame, sizeof(frame), &used, "\r\n");

    for (int row = 0; row < layout.body_rows; ++row) {
        const char *left = "";
        const char *right = "";
        desk_active_cube_t right_cube =
            row <= layout.top_rows ? DESK_ACTIVE_CUBE_TWO
                                   : DESK_ACTIVE_CUBE_THREE;

        if (row == 0) {
            right = "cube 2: editor";
        } else if (row == layout.top_rows + 1) {
            right = "cube 3: logs";
        }

        if (row == layout.top_rows) {
            append_cell_text(frame, sizeof(frame), &used, left,
                             layout.left_width);
            append_text(frame, sizeof(frame), &used,
                        middle_junction_glyph(active));
            append_repeat_text(frame, sizeof(frame), &used,
                               active == DESK_ACTIVE_CUBE_TWO ||
                                       active == DESK_ACTIVE_CUBE_THREE
                                   ? DESK_ACTIVE_HORIZONTAL
                                   : DESK_INACTIVE_HORIZONTAL,
                               layout.right_width);
        } else {
            append_cell_text(frame, sizeof(frame), &used, left,
                             layout.left_width);
            append_text(frame, sizeof(frame), &used,
                        active == DESK_ACTIVE_CUBE_ONE || active == right_cube
                            ? vertical_border_glyph(
                                  active, active == DESK_ACTIVE_CUBE_ONE
                                              ? DESK_ACTIVE_CUBE_ONE
                                              : right_cube)
                            : DESK_INACTIVE_VERTICAL);
            append_cell_text(frame, sizeof(frame), &used, right,
                             layout.right_width);
        }
        append_text(frame, sizeof(frame), &used, "\r\n");
    }

    append_repeat_text(frame, sizeof(frame), &used,
                       horizontal_border_glyph(active, DESK_ACTIVE_CUBE_ONE),
                       layout.left_width);
    append_text(frame, sizeof(frame), &used, bottom_junction_glyph(active));
    append_repeat_text(frame, sizeof(frame), &used,
                       horizontal_border_glyph(active, DESK_ACTIVE_CUBE_THREE),
                       layout.right_width);
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

static void grid_clear(desk_grid_t *grid)
{
    if (grid->cells != NULL) {
        memset(grid->cells, ' ', (size_t)grid->rows * (size_t)grid->cols);
    }
    grid->cursor_row = 0;
    grid->cursor_col = 0;
}

static int grid_resize(desk_grid_t *grid, int rows, int cols)
{
    if (rows <= 0 || cols <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (grid->rows == rows && grid->cols == cols && grid->cells != NULL) {
        return 0;
    }

    char *cells = malloc((size_t)rows * (size_t)cols);
    if (cells == NULL) {
        return -1;
    }
    free(grid->cells);
    memset(grid, 0, sizeof(*grid));
    grid->cells = cells;
    grid->rows = rows;
    grid->cols = cols;
    grid_clear(grid);
    return 0;
}

static void grid_cleanup(desk_grid_t *grid)
{
    free(grid->cells);
    memset(grid, 0, sizeof(*grid));
}

static void grid_scroll(desk_grid_t *grid)
{
    if (grid->rows <= 1) {
        memset(grid->cells, ' ', (size_t)grid->cols);
        grid->cursor_row = 0;
        grid->cursor_col = 0;
        return;
    }
    memmove(grid->cells, grid->cells + grid->cols,
            (size_t)(grid->rows - 1) * (size_t)grid->cols);
    memset(grid->cells + (size_t)(grid->rows - 1) * (size_t)grid->cols,
           ' ', (size_t)grid->cols);
    grid->cursor_row = grid->rows - 1;
}

static void grid_newline(desk_grid_t *grid)
{
    grid->cursor_col = 0;
    if (grid->cursor_row + 1 >= grid->rows) {
        grid_scroll(grid);
    } else {
        grid->cursor_row++;
    }
}

static void grid_put_char(desk_grid_t *grid, unsigned char ch)
{
    if (ch == '\r') {
        grid->cursor_col = 0;
        return;
    }
    if (ch == '\n') {
        grid_newline(grid);
        return;
    }
    if (ch == '\b') {
        if (grid->cursor_col > 0) {
            grid->cursor_col--;
        }
        return;
    }
    if (ch == '\t') {
        int spaces = 8 - (grid->cursor_col % 8);
        for (int i = 0; i < spaces; ++i) {
            grid_put_char(grid, ' ');
        }
        return;
    }
    if (ch < 0x20) {
        return;
    }
    if (grid->cursor_row < 0 || grid->cursor_row >= grid->rows ||
        grid->cursor_col < 0 || grid->cursor_col >= grid->cols) {
        return;
    }
    grid->cells[(size_t)grid->cursor_row * (size_t)grid->cols +
                (size_t)grid->cursor_col] = (char)ch;
    if (grid->cursor_col + 1 >= grid->cols) {
        grid_newline(grid);
    } else {
        grid->cursor_col++;
    }
}

static int parse_csi_number(const char *text, int default_value)
{
    if (text == NULL || text[0] == '\0') {
        return default_value;
    }
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (end == text || parsed < 1 || parsed > 10000) {
        return default_value;
    }
    return (int)parsed;
}

static void grid_apply_csi(desk_grid_t *grid)
{
    if (grid->csi_length == 0) {
        return;
    }
    char command = grid->csi[grid->csi_length - 1];
    grid->csi[grid->csi_length - 1] = '\0';

    if (command == 'J') {
        grid_clear(grid);
        return;
    }
    if (command == 'K') {
        if (grid->cursor_row >= 0 && grid->cursor_row < grid->rows) {
            memset(grid->cells +
                       (size_t)grid->cursor_row * (size_t)grid->cols,
                   ' ', (size_t)grid->cols);
        }
        return;
    }
    if (command == 'H' || command == 'f') {
        char *separator = strchr(grid->csi, ';');
        int row = 1;
        int col = 1;
        if (separator != NULL) {
            *separator = '\0';
            row = parse_csi_number(grid->csi, 1);
            col = parse_csi_number(separator + 1, 1);
        } else {
            row = parse_csi_number(grid->csi, 1);
        }
        if (row < 1) row = 1;
        if (col < 1) col = 1;
        if (row > grid->rows) row = grid->rows;
        if (col > grid->cols) col = grid->cols;
        grid->cursor_row = row - 1;
        grid->cursor_col = col - 1;
        return;
    }
    if (command == 'A' && grid->cursor_row > 0) {
        int amount = parse_csi_number(grid->csi, 1);
        grid->cursor_row -= amount;
        if (grid->cursor_row < 0) grid->cursor_row = 0;
    } else if (command == 'B') {
        int amount = parse_csi_number(grid->csi, 1);
        grid->cursor_row += amount;
        if (grid->cursor_row >= grid->rows) grid->cursor_row = grid->rows - 1;
    } else if (command == 'C') {
        int amount = parse_csi_number(grid->csi, 1);
        grid->cursor_col += amount;
        if (grid->cursor_col >= grid->cols) grid->cursor_col = grid->cols - 1;
    } else if (command == 'D') {
        int amount = parse_csi_number(grid->csi, 1);
        grid->cursor_col -= amount;
        if (grid->cursor_col < 0) grid->cursor_col = 0;
    }
}

static void grid_feed(desk_grid_t *grid, const unsigned char *data,
                      size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = data[i];
        if (grid->csi_active) {
            if (grid->csi_length + 1 < sizeof(grid->csi)) {
                grid->csi[grid->csi_length++] = (char)ch;
                grid->csi[grid->csi_length] = '\0';
            }
            if (ch >= 0x40 && ch <= 0x7e) {
                grid_apply_csi(grid);
                grid->csi_active = false;
                grid->escape_active = false;
                grid->csi_length = 0;
            }
            continue;
        }
        if (grid->escape_active) {
            if (ch == '[') {
                grid->csi_active = true;
                grid->csi_length = 0;
            } else {
                grid->escape_active = false;
            }
            continue;
        }
        if (ch == 0x1b) {
            grid->escape_active = true;
            continue;
        }
        grid_put_char(grid, ch);
    }
}

static void desk_render_cube_grid(const desk_terminal_t *terminal,
                                  const desk_grid_t *grid,
                                  const char *title)
{
    char frame[16384];
    size_t used = 0;
    desk_layout_t layout;

    if (!desk_get_layout(terminal, &layout)) {
        return;
    }

    append_text(frame, sizeof(frame), &used, "\x1b[3;1H");
    append_cell_text(frame, sizeof(frame), &used, title, layout.left_width);

    for (int row = 0; row < grid->rows; ++row) {
        char cursor[32];
        int terminal_row = row + 4;
        int cursor_length = snprintf(cursor, sizeof(cursor), "\x1b[%d;1H",
                                     terminal_row);
        if (cursor_length > 0 && (size_t)cursor_length < sizeof(cursor)) {
            append_text(frame, sizeof(frame), &used, cursor);
        }
        for (int col = 0; col < layout.left_width; ++col) {
            char ch = ' ';
            if (row < grid->rows && col < grid->cols) {
                ch = grid->cells[(size_t)row * (size_t)grid->cols +
                                 (size_t)col];
            }
            if (used + 1 < sizeof(frame)) {
                frame[used++] = ch;
                frame[used] = '\0';
            }
        }
    }

    (void)write_all(STDOUT_FILENO, frame, used);
}

static int cube_one_content_size(const desk_terminal_t *terminal,
                                 unsigned int *rows,
                                 unsigned int *cols)
{
    desk_layout_t layout;
    if (!desk_get_layout(terminal, &layout) || layout.body_rows <= 1 ||
        layout.left_width <= 0) {
        return -1;
    }
    *rows = (unsigned int)(layout.body_rows - 1);
    *cols = (unsigned int)layout.left_width;
    return 0;
}

static int desk_attach_after_terminal_enter(desk_attachment_t *target,
                                            const desk_terminal_t *terminal,
                                            char *error,
                                            size_t error_size)
{
    unsigned int rows = 0;
    unsigned int cols = 0;
    if (cube_one_content_size(terminal, &rows, &cols) < 0) {
        snprintf(error, error_size, "terminal too small for attachment");
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    cubicle_attachment_request_t request;
    memset(&request, 0, sizeof(request));
    request.process_id = target->process.id;
    request.channels = CUBICLE_CHANNEL_TTY | CUBICLE_CHANNEL_STDOUT |
                       CUBICLE_CHANNEL_STDIN;
    request.mode = CUBICLE_ATTACHMENT_INTERACTIVE;
    request.rows = rows;
    request.cols = cols;

    cubicle_attachment_grant_t grant;
    cubicle_error_code_t code = cubicle_attachment_request(
        target->manager, &request, &grant);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(target->manager);
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "attachment request failed");
        return code;
    }

    cubicle_attachment_options_t options;
    memset(&options, 0, sizeof(options));
    code = cubicle_attachment_connect(&grant, &options, &target->attachment);
    if (code != CUBICLE_OK) {
        snprintf(error, error_size, "controller attachment failed");
        return code;
    }

    code = cubicle_attachment_resize(target->attachment, rows, cols);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last =
            cubicle_attachment_last_error(target->attachment);
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "attachment resize failed");
        return code;
    }
    return CUBICLE_OK;
}

static int forward_input(cubicle_attachment_t *attachment,
                         const unsigned char *buffer,
                         size_t length,
                         int *escape_pending,
                         int *detach_requested)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = buffer[i];
        if (*escape_pending) {
            *escape_pending = 0;
            if (ch == 'd') {
                *detach_requested = 1;
                continue;
            }
            unsigned char escape = 0x1c;
            if (cubicle_attachment_write(attachment, &escape, 1) < 0) {
                return -1;
            }
        } else if (ch == 0x1c) {
            *escape_pending = 1;
            continue;
        }
        if (cubicle_attachment_write(attachment, &ch, 1) < 0) {
            return -1;
        }
    }
    return 0;
}

static int desk_run_attached(const char *process_name)
{
    char error[256];
    desk_attachment_t target;
    cubicle_error_code_t code = resolve_attachment_target(
        process_name, &target, error, sizeof(error));
    if (code != CUBICLE_OK) {
        fprintf(stderr, "desk: %s\n", error);
        desk_attachment_cleanup(&target);
        return code == CUBICLE_ERR_NOT_FOUND ? 1 : 2;
    }

    desk_terminal_t terminal;
    if (terminal_enter(&terminal) < 0) {
        desk_attachment_cleanup(&target);
        return -1;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGWINCH, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    desk_grid_t grid;
    memset(&grid, 0, sizeof(grid));
    unsigned int content_rows = 0;
    unsigned int content_cols = 0;
    desk_active_cube_t active_cube = DESK_ACTIVE_CUBE_ONE;
    int detach_requested = 0;
    int escape_pending = 0;
    int result = 0;

    if (cube_one_content_size(&terminal, &content_rows, &content_cols) < 0 ||
        grid_resize(&grid, (int)content_rows, (int)content_cols) < 0) {
        result = 2;
        goto cleanup;
    }

    desk_render_layout(&terminal, active_cube);
    char title[CUBICLE_NAME_MAX + 32];
    snprintf(title, sizeof(title), "cube 1: %s", process_name);
    desk_render_cube_grid(&terminal, &grid, title);

    code = desk_attach_after_terminal_enter(&target, &terminal, error,
                                            sizeof(error));
    if (code != CUBICLE_OK) {
        terminal_leave(&terminal);
        fprintf(stderr, "desk: %s\n", error);
        grid_cleanup(&grid);
        desk_attachment_cleanup(&target);
        return code == CUBICLE_ERR_NOT_FOUND ? 1 : 2;
    }

    while (!g_stop_requested && !detach_requested) {
        if (g_resize_requested) {
            g_resize_requested = 0;
            if (terminal_query_size(&terminal) == 0 &&
                cube_one_content_size(&terminal, &content_rows,
                                      &content_cols) == 0) {
                desk_render_layout(&terminal, active_cube);
                if (grid_resize(&grid, (int)content_rows,
                                (int)content_cols) == 0) {
                    (void)cubicle_attachment_resize(target.attachment,
                                                    content_rows,
                                                    content_cols);
                    desk_render_cube_grid(&terminal, &grid, title);
                }
            }
        }

        unsigned char output[4096];
        ssize_t nread = cubicle_attachment_read(target.attachment, output,
                                                sizeof(output));
        if (nread > 0) {
            grid_feed(&grid, output, (size_t)nread);
            desk_render_cube_grid(&terminal, &grid, title);
        } else if (nread < 0) {
            const cubicle_error_t *last =
                cubicle_attachment_last_error(target.attachment);
            if (last == NULL || last->code != CUBICLE_ERR_IO) {
                result = 2;
                break;
            }
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;

        int ready = select(STDIN_FILENO + 1, &read_set, NULL, NULL,
                           &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = 2;
            break;
        }
        if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_set)) {
            unsigned char input[256];
            ssize_t input_length = read(STDIN_FILENO, input, sizeof(input));
            if (input_length < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                result = 2;
                break;
            }
            if (input_length == 0) {
                detach_requested = 1;
            } else {
                size_t start = 0;
                for (ssize_t i = 0; i < input_length; ++i) {
                    if (input[i] != 0) {
                        continue;
                    }
                    if (active_cube == DESK_ACTIVE_CUBE_ONE && i > (ssize_t)start &&
                        forward_input(target.attachment, input + start,
                                      (size_t)i - start, &escape_pending,
                                      &detach_requested) < 0) {
                        result = 2;
                        break;
                    }
                    active_cube = next_active_cube(active_cube);
                    desk_render_layout(&terminal, active_cube);
                    desk_render_cube_grid(&terminal, &grid, title);
                    start = (size_t)i + 1;
                }
                if (result != 0) {
                    break;
                }
                if (active_cube == DESK_ACTIVE_CUBE_ONE &&
                    start < (size_t)input_length &&
                    forward_input(target.attachment, input + start,
                                  (size_t)input_length - start,
                                  &escape_pending,
                                  &detach_requested) < 0) {
                    result = 2;
                    break;
                }
            }
        }
    }

cleanup:
    terminal_leave(&terminal);
    grid_cleanup(&grid);
    desk_attachment_cleanup(&target);
    return result;
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
    desk_active_cube_t active_cube = DESK_ACTIVE_CUBE_ONE;

    while (!g_stop_requested) {
        if (g_resize_requested) {
            g_resize_requested = 0;
            if (terminal_query_size(&terminal) == 0) {
                desk_render_layout(&terminal, active_cube);
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
            if (input[i] == 0) {
                active_cube = next_active_cube(active_cube);
                desk_render_layout(&terminal, active_cube);
                desk_render_cube_one(&terminal, counter);
                continue;
            }
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
    fprintf(stream, "Usage: %s [PROCESS]\n", program);
    fprintf(stream, "Render the Cubicle desk terminal view.\n");
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (argc == 2) {
            int result = desk_run_attached(argv[1]);
            if (result < 0) {
                fprintf(stderr, "desk: %s\n", strerror(errno));
                return 1;
            }
            return result;
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
