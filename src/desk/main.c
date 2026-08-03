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
#define DESK_ACTIVE_MIDDLE_JUNCTION "┼"
#define DESK_INACTIVE_HORIZONTAL "╌"
#define DESK_INACTIVE_VERTICAL "┊"
#define DESK_INACTIVE_MIDDLE_JUNCTION "┄"

static volatile sig_atomic_t g_resize_requested = 1;
static volatile sig_atomic_t g_stop_requested = 0;

typedef struct desk_terminal {
    struct termios original;
    bool raw_enabled;
    int rows;
    int cols;
} desk_terminal_t;

typedef struct desk_layout {
    int rows;
    int cols;
} desk_layout_t;

typedef enum desk_active_cube {
    DESK_ACTIVE_CUBE_ONE = 1,
    DESK_ACTIVE_CUBE_TWO = 2,
    DESK_ACTIVE_CUBE_THREE = 3,
    DESK_ACTIVE_CUBE_FOUR = 4
} desk_active_cube_t;

typedef enum desk_split {
    DESK_SPLIT_NONE = 0,
    DESK_SPLIT_HORIZONTAL,
    DESK_SPLIT_VERTICAL
} desk_split_t;

typedef enum desk_zoom {
    DESK_ZOOM_NONE = 0,
    DESK_ZOOM_HORIZONTAL,
    DESK_ZOOM_VERTICAL
} desk_zoom_t;

typedef struct desk_rect {
    int row;
    int col;
    int rows;
    int cols;
} desk_rect_t;

typedef struct desk_pane_node {
    bool used;
    int pane_id;
    desk_split_t split;
    int first;
    int second;
} desk_pane_node_t;

typedef struct desk_pane_layout {
    desk_pane_node_t nodes[32];
    int root;
    int active_pane_id;
    int next_pane_id;
    desk_zoom_t zoom;
} desk_pane_layout_t;

typedef struct desk_cell {
    char ch;
    char sgr[64];
} desk_cell_t;

typedef struct desk_grid {
    desk_cell_t *cells;
    int rows;
    int cols;
    int cursor_row;
    int cursor_col;
    int scroll_top;
    int scroll_bottom;
    char current_sgr[64];
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

static int pane_alloc_node(desk_pane_layout_t *panes)
{
    for (size_t i = 0; i < sizeof(panes->nodes) / sizeof(panes->nodes[0]);
         ++i) {
        if (!panes->nodes[i].used) {
            memset(&panes->nodes[i], 0, sizeof(panes->nodes[i]));
            panes->nodes[i].used = true;
            return (int)i;
        }
    }
    return -1;
}

static int pane_create_leaf(desk_pane_layout_t *panes, int pane_id)
{
    int node = pane_alloc_node(panes);
    if (node >= 0) {
        panes->nodes[node].pane_id = pane_id;
        panes->nodes[node].split = DESK_SPLIT_NONE;
    }
    return node;
}

static int pane_create_split(desk_pane_layout_t *panes, desk_split_t split,
                             int first, int second)
{
    int node = pane_alloc_node(panes);
    if (node >= 0) {
        panes->nodes[node].split = split;
        panes->nodes[node].first = first;
        panes->nodes[node].second = second;
    }
    return node;
}

static void pane_layout_reset(desk_pane_layout_t *panes)
{
    memset(panes, 0, sizeof(*panes));
    int one = pane_create_leaf(panes, 1);
    int two = pane_create_leaf(panes, 2);
    int three = pane_create_leaf(panes, 3);
    int four = pane_create_leaf(panes, 4);
    int top = pane_create_split(panes, DESK_SPLIT_HORIZONTAL, one, two);
    int bottom = pane_create_split(panes, DESK_SPLIT_HORIZONTAL, three, four);
    panes->root = pane_create_split(panes, DESK_SPLIT_VERTICAL, top, bottom);
    panes->active_pane_id = 1;
    panes->next_pane_id = 5;
    panes->zoom = DESK_ZOOM_NONE;
}

static bool pane_subtree_contains(const desk_pane_layout_t *panes, int node,
                                  int pane_id)
{
    if (node < 0 || !panes->nodes[node].used) {
        return false;
    }
    const desk_pane_node_t *entry = &panes->nodes[node];
    if (entry->split == DESK_SPLIT_NONE) {
        return entry->pane_id == pane_id;
    }
    return pane_subtree_contains(panes, entry->first, pane_id) ||
           pane_subtree_contains(panes, entry->second, pane_id);
}

static int pane_first_leaf(const desk_pane_layout_t *panes, int node)
{
    if (node < 0 || !panes->nodes[node].used) {
        return 0;
    }
    const desk_pane_node_t *entry = &panes->nodes[node];
    if (entry->split == DESK_SPLIT_NONE) {
        return entry->pane_id;
    }
    int first = pane_first_leaf(panes, entry->first);
    return first != 0 ? first : pane_first_leaf(panes, entry->second);
}

static int pane_find_leaf_node(const desk_pane_layout_t *panes, int node,
                               int pane_id)
{
    if (node < 0 || !panes->nodes[node].used) {
        return -1;
    }
    const desk_pane_node_t *entry = &panes->nodes[node];
    if (entry->split == DESK_SPLIT_NONE) {
        return entry->pane_id == pane_id ? node : -1;
    }
    int found = pane_find_leaf_node(panes, entry->first, pane_id);
    return found >= 0 ? found
                      : pane_find_leaf_node(panes, entry->second, pane_id);
}

static int pane_find_parent(const desk_pane_layout_t *panes, int node,
                            int child)
{
    if (node < 0 || !panes->nodes[node].used) {
        return -1;
    }
    const desk_pane_node_t *entry = &panes->nodes[node];
    if (entry->split == DESK_SPLIT_NONE) {
        return -1;
    }
    if (entry->first == child || entry->second == child) {
        return node;
    }
    int found = pane_find_parent(panes, entry->first, child);
    return found >= 0 ? found : pane_find_parent(panes, entry->second, child);
}

static void pane_layout_next(desk_pane_layout_t *panes)
{
    int best = 0;
    int fallback = 0;
    for (size_t i = 0; i < sizeof(panes->nodes) / sizeof(panes->nodes[0]);
         ++i) {
        if (!panes->nodes[i].used ||
            panes->nodes[i].split != DESK_SPLIT_NONE) {
            continue;
        }
        int pane_id = panes->nodes[i].pane_id;
        if (fallback == 0 || pane_id < fallback) {
            fallback = pane_id;
        }
        if (pane_id > panes->active_pane_id &&
            (best == 0 || pane_id < best)) {
            best = pane_id;
        }
    }
    panes->active_pane_id = best != 0 ? best : fallback;
    panes->zoom = DESK_ZOOM_NONE;
}

static int pane_layout_split(desk_pane_layout_t *panes, desk_split_t split)
{
    int leaf = pane_find_leaf_node(panes, panes->root, panes->active_pane_id);
    if (leaf < 0) {
        return -1;
    }
    int new_leaf = pane_create_leaf(panes, panes->next_pane_id++);
    if (new_leaf < 0) {
        return -1;
    }
    panes->nodes[leaf].split = split;
    panes->nodes[leaf].first = pane_create_leaf(panes, panes->active_pane_id);
    panes->nodes[leaf].second = new_leaf;
    panes->nodes[leaf].pane_id = 0;
    if (panes->nodes[leaf].first < 0) {
        panes->nodes[new_leaf].used = false;
        panes->nodes[leaf].used = false;
        return -1;
    }
    panes->active_pane_id = panes->nodes[new_leaf].pane_id;
    panes->zoom = DESK_ZOOM_NONE;
    return 0;
}

static int pane_layout_delete_active(desk_pane_layout_t *panes)
{
    int leaf = pane_find_leaf_node(panes, panes->root, panes->active_pane_id);
    if (leaf < 0 || leaf == panes->root) {
        return -1;
    }
    int parent = pane_find_parent(panes, panes->root, leaf);
    if (parent < 0) {
        return -1;
    }
    int sibling = panes->nodes[parent].first == leaf
                      ? panes->nodes[parent].second
                      : panes->nodes[parent].first;
    panes->active_pane_id = pane_first_leaf(panes, sibling);
    panes->nodes[parent] = panes->nodes[sibling];
    panes->nodes[leaf].used = false;
    panes->nodes[sibling].used = false;
    panes->zoom = DESK_ZOOM_NONE;
    return 0;
}

static void split_rect(desk_rect_t rect, desk_split_t split,
                       desk_rect_t *first, desk_rect_t *second,
                       desk_rect_t *divider)
{
    *first = rect;
    *second = rect;
    memset(divider, 0, sizeof(*divider));
    if (split == DESK_SPLIT_HORIZONTAL) {
        int left = rect.cols / 2;
        first->cols = left;
        second->col = rect.col + left + 1;
        second->cols = rect.cols - left - 1;
        divider->row = rect.row;
        divider->col = rect.col + left;
        divider->rows = rect.rows;
        divider->cols = 1;
    } else {
        int top = rect.rows / 2;
        first->rows = top;
        second->row = rect.row + top + 1;
        second->rows = rect.rows - top - 1;
        divider->row = rect.row + top;
        divider->col = rect.col;
        divider->rows = 1;
        divider->cols = rect.cols;
    }
}

static bool pane_layout_rect_for_node(const desk_pane_layout_t *panes,
                                      int node, int pane_id, desk_rect_t rect,
                                      desk_rect_t *out)
{
    if (node < 0 || !panes->nodes[node].used || rect.rows <= 0 ||
        rect.cols <= 0) {
        return false;
    }
    const desk_pane_node_t *entry = &panes->nodes[node];
    if (entry->split == DESK_SPLIT_NONE) {
        if (entry->pane_id == pane_id) {
            *out = rect;
            return true;
        }
        return false;
    }

    bool first_has_active =
        pane_subtree_contains(panes, entry->first, panes->active_pane_id);
    bool second_has_active =
        pane_subtree_contains(panes, entry->second, panes->active_pane_id);
    bool zooms_this_split =
        (panes->zoom == DESK_ZOOM_HORIZONTAL &&
         entry->split == DESK_SPLIT_HORIZONTAL) ||
        (panes->zoom == DESK_ZOOM_VERTICAL &&
         entry->split == DESK_SPLIT_VERTICAL);
    if (zooms_this_split && (first_has_active || second_has_active)) {
        int child = first_has_active ? entry->first : entry->second;
        return pane_layout_rect_for_node(panes, child, pane_id, rect, out);
    }

    desk_rect_t first;
    desk_rect_t second;
    desk_rect_t divider;
    split_rect(rect, entry->split, &first, &second, &divider);
    return pane_layout_rect_for_node(panes, entry->first, pane_id, first, out) ||
           pane_layout_rect_for_node(panes, entry->second, pane_id, second, out);
}

static bool pane_layout_rect_for_pane(const desk_pane_layout_t *panes,
                                      const desk_terminal_t *terminal,
                                      int pane_id, desk_rect_t *out)
{
    desk_rect_t root = {0, 0, terminal->rows, terminal->cols};
    return pane_layout_rect_for_node(panes, panes->root, pane_id, root, out);
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

    if (rows < 5 || cols < 24) {
        return false;
    }

    layout->rows = rows;
    layout->cols = cols;
    return true;
}

static void pane_canvas_put(const char **canvas, const desk_terminal_t *terminal,
                            int row, int col, const char *glyph)
{
    if (row < 0 || row >= terminal->rows || col < 0 || col >= terminal->cols) {
        return;
    }
    canvas[(size_t)row * (size_t)terminal->cols + (size_t)col] = glyph;
}

static const char *ascii_glyph(char ch)
{
    static char glyphs[128][2];
    unsigned char value = (unsigned char)ch;
    if (value >= 128) {
        return " ";
    }
    if (glyphs[value][0] == '\0') {
        glyphs[value][0] = ch;
        glyphs[value][1] = '\0';
    }
    return glyphs[value];
}

static bool pane_divider_crosses(const char **canvas,
                                 const desk_terminal_t *terminal, int row,
                                 int col, desk_split_t split)
{
    const char *existing =
        canvas[(size_t)row * (size_t)terminal->cols + (size_t)col];
    if (split == DESK_SPLIT_HORIZONTAL) {
        return strcmp(existing, DESK_ACTIVE_HORIZONTAL) == 0 ||
               strcmp(existing, DESK_INACTIVE_HORIZONTAL) == 0;
    }
    return strcmp(existing, DESK_ACTIVE_VERTICAL) == 0 ||
           strcmp(existing, DESK_INACTIVE_VERTICAL) == 0;
}

static void pane_canvas_write_label(const char **canvas,
                                    const desk_terminal_t *terminal,
                                    desk_rect_t rect, int pane_id)
{
    if (rect.rows <= 0 || rect.cols <= 0) {
        return;
    }
    char label[32];
    int length = snprintf(label, sizeof(label), "cube %d", pane_id);
    if (length < 0) {
        return;
    }
    int limit = length < rect.cols ? length : rect.cols;
    for (int i = 0; i < limit; ++i) {
        pane_canvas_put(canvas, terminal, rect.row, rect.col + i,
                        ascii_glyph(label[i]));
    }
}

static void pane_render_node(const desk_pane_layout_t *panes,
                             const desk_terminal_t *terminal,
                             const char **canvas, int node, desk_rect_t rect)
{
    if (node < 0 || !panes->nodes[node].used || rect.rows <= 0 ||
        rect.cols <= 0) {
        return;
    }
    const desk_pane_node_t *entry = &panes->nodes[node];
    if (entry->split == DESK_SPLIT_NONE) {
        pane_canvas_write_label(canvas, terminal, rect, entry->pane_id);
        return;
    }

    bool first_has_active =
        pane_subtree_contains(panes, entry->first, panes->active_pane_id);
    bool second_has_active =
        pane_subtree_contains(panes, entry->second, panes->active_pane_id);
    bool zooms_this_split =
        (panes->zoom == DESK_ZOOM_HORIZONTAL &&
         entry->split == DESK_SPLIT_HORIZONTAL) ||
        (panes->zoom == DESK_ZOOM_VERTICAL &&
         entry->split == DESK_SPLIT_VERTICAL);
    if (zooms_this_split && (first_has_active || second_has_active)) {
        pane_render_node(panes, terminal, canvas,
                         first_has_active ? entry->first : entry->second,
                         rect);
        return;
    }

    desk_rect_t first;
    desk_rect_t second;
    desk_rect_t divider;
    split_rect(rect, entry->split, &first, &second, &divider);
    pane_render_node(panes, terminal, canvas, entry->first, first);
    pane_render_node(panes, terminal, canvas, entry->second, second);

    bool active_divider = first_has_active || second_has_active;
    const char *line = entry->split == DESK_SPLIT_HORIZONTAL
                           ? (active_divider ? DESK_ACTIVE_VERTICAL
                                             : DESK_INACTIVE_VERTICAL)
                           : (active_divider ? DESK_ACTIVE_HORIZONTAL
                                             : DESK_INACTIVE_HORIZONTAL);
    const char *junction = active_divider ? DESK_ACTIVE_MIDDLE_JUNCTION
                                          : DESK_INACTIVE_MIDDLE_JUNCTION;
    for (int row = 0; row < divider.rows; ++row) {
        for (int col = 0; col < divider.cols; ++col) {
            int target_row = divider.row + row;
            int target_col = divider.col + col;
            pane_canvas_put(
                canvas, terminal, target_row, target_col,
                pane_divider_crosses(canvas, terminal, target_row, target_col,
                                     entry->split)
                    ? junction
                    : line);
        }
    }
}

static int desk_build_layout_frame(const desk_terminal_t *terminal,
                                   const desk_pane_layout_t *panes,
                                   char *frame,
                                   size_t frame_size,
                                   size_t *used,
                                   bool include_clear)
{
    desk_layout_t layout;
    *used = 0;

    if (!desk_get_layout(terminal, &layout)) {
        if (include_clear) {
            append_text(frame, frame_size, used, "\x1b[H\x1b[2J");
        }
        append_text(frame, frame_size, used,
                    "Terminal too small for desk. Press q to quit.");
        return 0;
    }

    const char **canvas = calloc((size_t)terminal->rows * (size_t)terminal->cols,
                                 sizeof(*canvas));
    if (canvas == NULL) {
        return -1;
    }
    for (int row = 0; row < terminal->rows; ++row) {
        for (int col = 0; col < terminal->cols; ++col) {
            canvas[(size_t)row * (size_t)terminal->cols + (size_t)col] = " ";
        }
    }

    if (include_clear) {
        append_text(frame, frame_size, used, "\x1b[H\x1b[2J");
    }
    desk_rect_t root = {0, 0, terminal->rows, terminal->cols};
    pane_render_node(panes, terminal, canvas, panes->root, root);
    for (int row = 0; row < terminal->rows; ++row) {
        for (int col = 0; col < terminal->cols; ++col) {
            append_text(frame, frame_size, used,
                        canvas[(size_t)row * (size_t)terminal->cols +
                               (size_t)col]);
        }
        if (row + 1 < terminal->rows) {
            append_text(frame, frame_size, used, "\r\n");
        }
    }
    free(canvas);
    return 0;
}

static void desk_render_layout(const desk_terminal_t *terminal,
                               const desk_pane_layout_t *panes)
{
    char frame[262144];
    size_t used = 0;
    if (desk_build_layout_frame(terminal, panes, frame, sizeof(frame), &used,
                                true) < 0) {
        return;
    }
    (void)write_all(STDOUT_FILENO, frame, used);
}

static int desk_dump_layout(const desk_terminal_t *terminal,
                            const desk_pane_layout_t *panes)
{
    char frame[262144];
    size_t used = 0;
    if (desk_build_layout_frame(terminal, panes, frame, sizeof(frame), &used,
                                false) < 0) {
        return -1;
    }
    FILE *file = fopen("desk.bin", "wb");
    if (file == NULL) {
        return -1;
    }
    size_t written = fwrite(frame, 1, used, file);
    int close_result = fclose(file);
    return written == used && close_result == 0 ? 0 : -1;
}

static void desk_render_cube_one(const desk_terminal_t *terminal,
                                 const desk_pane_layout_t *panes,
                                 unsigned long long counter)
{
    char frame[16384];
    size_t used = 0;
    desk_rect_t rect;

    if (!pane_layout_rect_for_pane(panes, terminal, 1, &rect)) {
        return;
    }

    for (int row = 0; row < rect.rows; ++row) {
        char line[128];
        const char *text = "";
        int terminal_row = rect.row + row + 1;

        if (row == 0) {
            text = "cube 1: counter";
        } else {
            int scroll_rows = rect.rows - 1;
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
        int cursor_length = snprintf(cursor, sizeof(cursor), "\x1b[%d;%dH",
                                     terminal_row, rect.col + 1);
        if (cursor_length > 0 && (size_t)cursor_length < sizeof(cursor)) {
            append_text(frame, sizeof(frame), &used, cursor);
        }
        append_cell_text(frame, sizeof(frame), &used, text, rect.cols);
    }

    (void)write_all(STDOUT_FILENO, frame, used);
}

static void grid_clear(desk_grid_t *grid)
{
    if (grid->cells != NULL) {
        size_t count = (size_t)grid->rows * (size_t)grid->cols;
        for (size_t i = 0; i < count; ++i) {
            grid->cells[i].ch = ' ';
            grid->cells[i].sgr[0] = '\0';
        }
    }
    grid->cursor_row = 0;
    grid->cursor_col = 0;
    grid->scroll_top = 0;
    grid->scroll_bottom = grid->rows > 0 ? grid->rows - 1 : 0;
    grid->current_sgr[0] = '\0';
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

    desk_cell_t *cells = malloc((size_t)rows * (size_t)cols * sizeof(*cells));
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

static void grid_clear_row(desk_grid_t *grid, int row)
{
    if (row < 0 || row >= grid->rows) {
        return;
    }
    desk_cell_t *line = grid->cells + (size_t)row * (size_t)grid->cols;
    for (int col = 0; col < grid->cols; ++col) {
        line[col].ch = ' ';
        line[col].sgr[0] = '\0';
    }
}

static void grid_scroll_up_region(desk_grid_t *grid, int top, int bottom,
                                  int amount)
{
    if (top < 0) top = 0;
    if (bottom >= grid->rows) bottom = grid->rows - 1;
    if (top > bottom || amount <= 0) {
        return;
    }

    int height = bottom - top + 1;
    if (amount >= height) {
        for (int row = top; row <= bottom; ++row) {
            grid_clear_row(grid, row);
        }
        return;
    }

    desk_cell_t *start = grid->cells + (size_t)top * (size_t)grid->cols;
    desk_cell_t *source =
        grid->cells + (size_t)(top + amount) * (size_t)grid->cols;
    size_t move_cells = (size_t)(height - amount) * (size_t)grid->cols;
    memmove(start, source, move_cells * sizeof(*grid->cells));
    for (int row = bottom - amount + 1; row <= bottom; ++row) {
        grid_clear_row(grid, row);
    }
}

static void grid_scroll_down_region(desk_grid_t *grid, int top, int bottom,
                                    int amount)
{
    if (top < 0) top = 0;
    if (bottom >= grid->rows) bottom = grid->rows - 1;
    if (top > bottom || amount <= 0) {
        return;
    }

    int height = bottom - top + 1;
    if (amount >= height) {
        for (int row = top; row <= bottom; ++row) {
            grid_clear_row(grid, row);
        }
        return;
    }

    desk_cell_t *dest =
        grid->cells + (size_t)(top + amount) * (size_t)grid->cols;
    desk_cell_t *source = grid->cells + (size_t)top * (size_t)grid->cols;
    size_t move_cells = (size_t)(height - amount) * (size_t)grid->cols;
    memmove(dest, source, move_cells * sizeof(*grid->cells));
    for (int row = top; row < top + amount; ++row) {
        grid_clear_row(grid, row);
    }
}

static void grid_index(desk_grid_t *grid)
{
    int bottom = grid->scroll_bottom;
    if (bottom < grid->scroll_top || bottom >= grid->rows) {
        bottom = grid->rows - 1;
    }
    if (grid->cursor_row >= bottom) {
        grid_scroll_up_region(grid, grid->scroll_top, bottom, 1);
        grid->cursor_row = bottom;
    } else if (grid->cursor_row + 1 < grid->rows) {
        grid->cursor_row++;
    }
}

static void grid_reverse_index(desk_grid_t *grid)
{
    int top = grid->scroll_top;
    int bottom = grid->scroll_bottom;
    if (top < 0) top = 0;
    if (bottom < top || bottom >= grid->rows) bottom = grid->rows - 1;
    if (grid->cursor_row <= top) {
        grid_scroll_down_region(grid, top, bottom, 1);
        grid->cursor_row = top;
    } else if (grid->cursor_row > 0) {
        grid->cursor_row--;
    }
}

static void grid_newline(desk_grid_t *grid)
{
    grid->cursor_col = 0;
    grid_index(grid);
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
    desk_cell_t *cell =
        &grid->cells[(size_t)grid->cursor_row * (size_t)grid->cols +
                     (size_t)grid->cursor_col];
    cell->ch = (char)ch;
    snprintf(cell->sgr, sizeof(cell->sgr), "%s", grid->current_sgr);
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

static void parse_csi_pair(const char *text, int default_first,
                           int default_second, int *first, int *second)
{
    const char *separator = strchr(text, ';');
    *first = default_first;
    *second = default_second;
    if (separator == NULL) {
        *first = parse_csi_number(text, default_first);
        return;
    }

    char first_text[32];
    size_t first_length = (size_t)(separator - text);
    if (first_length >= sizeof(first_text)) {
        first_length = sizeof(first_text) - 1;
    }
    memcpy(first_text, text, first_length);
    first_text[first_length] = '\0';
    *first = parse_csi_number(first_text, default_first);
    *second = parse_csi_number(separator + 1, default_second);
}

static void grid_clear_cell(desk_grid_t *grid, int row, int col)
{
    if (row < 0 || row >= grid->rows || col < 0 || col >= grid->cols) {
        return;
    }
    desk_cell_t *cell =
        &grid->cells[(size_t)row * (size_t)grid->cols + (size_t)col];
    cell->ch = ' ';
    cell->sgr[0] = '\0';
}

static void grid_clear_line_range(desk_grid_t *grid, int start_col,
                                  int end_col)
{
    if (grid->cursor_row < 0 || grid->cursor_row >= grid->rows) {
        return;
    }
    if (start_col < 0) {
        start_col = 0;
    }
    if (end_col >= grid->cols) {
        end_col = grid->cols - 1;
    }
    for (int col = start_col; col <= end_col; ++col) {
        grid_clear_cell(grid, grid->cursor_row, col);
    }
}

static void grid_set_sgr(desk_grid_t *grid)
{
    if (grid->csi[0] == '\0' || strcmp(grid->csi, "0") == 0) {
        grid->current_sgr[0] = '\0';
        return;
    }

    int length = snprintf(grid->current_sgr, sizeof(grid->current_sgr),
                          "\x1b[%sm", grid->csi);
    if (length < 0 || (size_t)length >= sizeof(grid->current_sgr)) {
        grid->current_sgr[0] = '\0';
    }
}

static void grid_apply_csi(desk_grid_t *grid)
{
    if (grid->csi_length == 0) {
        return;
    }
    char command = grid->csi[grid->csi_length - 1];
    grid->csi[grid->csi_length - 1] = '\0';

    if (command == 'J') {
        int mode = parse_csi_number(grid->csi, 0);
        if (mode == 1 || mode == 2 || mode == 3) {
            int end_row = mode == 1 ? grid->cursor_row : grid->rows - 1;
            for (int row = 0; row <= end_row; ++row) {
                int start_col = 0;
                int end_col = grid->cols - 1;
                if (mode == 1 && row == grid->cursor_row) {
                    end_col = grid->cursor_col;
                }
                for (int col = start_col; col <= end_col; ++col) {
                    grid_clear_cell(grid, row, col);
                }
            }
        } else {
            for (int row = grid->cursor_row; row < grid->rows; ++row) {
                int start_col = row == grid->cursor_row ? grid->cursor_col : 0;
                for (int col = start_col; col < grid->cols; ++col) {
                    grid_clear_cell(grid, row, col);
                }
            }
        }
        return;
    }
    if (command == 'K') {
        int mode = parse_csi_number(grid->csi, 0);
        if (mode == 1) {
            grid_clear_line_range(grid, 0, grid->cursor_col);
        } else if (mode == 2) {
            grid_clear_line_range(grid, 0, grid->cols - 1);
        } else {
            grid_clear_line_range(grid, grid->cursor_col, grid->cols - 1);
        }
        return;
    }
    if (command == 'm') {
        grid_set_sgr(grid);
        return;
    }
    if (command == 'H' || command == 'f') {
        int row = 1;
        int col = 1;
        parse_csi_pair(grid->csi, 1, 1, &row, &col);
        if (row < 1) row = 1;
        if (col < 1) col = 1;
        if (row > grid->rows) row = grid->rows;
        if (col > grid->cols) col = grid->cols;
        grid->cursor_row = row - 1;
        grid->cursor_col = col - 1;
        return;
    }
    if (command == 'r') {
        int top = 1;
        int bottom = grid->rows;
        parse_csi_pair(grid->csi, 1, grid->rows, &top, &bottom);
        if (top < 1) top = 1;
        if (bottom > grid->rows) bottom = grid->rows;
        if (top < bottom) {
            grid->scroll_top = top - 1;
            grid->scroll_bottom = bottom - 1;
        } else {
            grid->scroll_top = 0;
            grid->scroll_bottom = grid->rows - 1;
        }
        grid->cursor_row = 0;
        grid->cursor_col = 0;
        return;
    }
    if (command == 'L') {
        int amount = parse_csi_number(grid->csi, 1);
        int bottom = grid->scroll_bottom;
        if (grid->cursor_row >= grid->scroll_top && grid->cursor_row <= bottom) {
            grid_scroll_down_region(grid, grid->cursor_row, bottom, amount);
        }
        return;
    }
    if (command == 'M') {
        int amount = parse_csi_number(grid->csi, 1);
        int bottom = grid->scroll_bottom;
        if (grid->cursor_row >= grid->scroll_top && grid->cursor_row <= bottom) {
            grid_scroll_up_region(grid, grid->cursor_row, bottom, amount);
        }
        return;
    }
    if (command == 'S') {
        int amount = parse_csi_number(grid->csi, 1);
        grid_scroll_up_region(grid, grid->scroll_top, grid->scroll_bottom,
                              amount);
        return;
    }
    if (command == 'T') {
        int amount = parse_csi_number(grid->csi, 1);
        grid_scroll_down_region(grid, grid->scroll_top, grid->scroll_bottom,
                                amount);
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
            } else if (ch == 'D') {
                grid_index(grid);
                grid->escape_active = false;
            } else if (ch == 'E') {
                grid->cursor_col = 0;
                grid_index(grid);
                grid->escape_active = false;
            } else if (ch == 'M') {
                grid_reverse_index(grid);
                grid->escape_active = false;
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
                                  const desk_pane_layout_t *panes,
                                  const desk_grid_t *grid,
                                  const char *title)
{
    (void)title;
    char frame[65536];
    size_t used = 0;
    desk_rect_t rect;

    if (!pane_layout_rect_for_pane(panes, terminal, 1, &rect)) {
        return;
    }

    for (int row = 0; row < grid->rows; ++row) {
        char cursor[32];
        char active_sgr[64] = "";
        int terminal_row = rect.row + row + 1;
        int cursor_length = snprintf(cursor, sizeof(cursor), "\x1b[%d;%dH",
                                     terminal_row, rect.col + 1);
        if (cursor_length > 0 && (size_t)cursor_length < sizeof(cursor)) {
            append_text(frame, sizeof(frame), &used, cursor);
        }
        for (int col = 0; col < rect.cols; ++col) {
            char ch = ' ';
            const char *sgr = "";
            if (row < grid->rows && col < grid->cols) {
                const desk_cell_t *cell =
                    &grid->cells[(size_t)row * (size_t)grid->cols +
                                 (size_t)col];
                ch = cell->ch;
                sgr = cell->sgr;
            }
            if (strcmp(active_sgr, sgr) != 0) {
                append_text(frame, sizeof(frame), &used,
                            sgr[0] == '\0' ? "\x1b[0m" : sgr);
                snprintf(active_sgr, sizeof(active_sgr), "%s", sgr);
            }
            if (used + 1 < sizeof(frame)) {
                frame[used++] = ch;
                frame[used] = '\0';
            }
        }
        if (active_sgr[0] != '\0') {
            append_text(frame, sizeof(frame), &used, "\x1b[0m");
        }
    }

    append_text(frame, sizeof(frame), &used, "\x1b[0m");
    (void)write_all(STDOUT_FILENO, frame, used);
}

static int cube_one_content_size(const desk_terminal_t *terminal,
                                 const desk_pane_layout_t *panes,
                                 unsigned int *rows,
                                 unsigned int *cols)
{
    desk_rect_t rect;
    if (!pane_layout_rect_for_pane(panes, terminal, 1, &rect) ||
        rect.rows <= 0 || rect.cols <= 0) {
        return -1;
    }
    *rows = (unsigned int)rect.rows;
    *cols = (unsigned int)rect.cols;
    return 0;
}

static int desk_attach_after_terminal_enter(desk_attachment_t *target,
                                            const desk_terminal_t *terminal,
                                            const desk_pane_layout_t *panes,
                                            char *error,
                                            size_t error_size)
{
    unsigned int rows = 0;
    unsigned int cols = 0;
    if (cube_one_content_size(terminal, panes, &rows, &cols) < 0) {
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

static bool handle_pane_key(desk_pane_layout_t *panes, unsigned char key)
{
    switch (key) {
    case 0:
        pane_layout_next(panes);
        return true;
    case 'h':
        panes->zoom = DESK_ZOOM_HORIZONTAL;
        return true;
    case 'v':
        panes->zoom = DESK_ZOOM_VERTICAL;
        return true;
    case 'r':
        pane_layout_reset(panes);
        return true;
    case 'H':
        (void)pane_layout_split(panes, DESK_SPLIT_HORIZONTAL);
        return true;
    case 'V':
        (void)pane_layout_split(panes, DESK_SPLIT_VERTICAL);
        return true;
    case 'D':
        (void)pane_layout_delete_active(panes);
        return true;
    default:
        return false;
    }
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
    desk_pane_layout_t panes;
    pane_layout_reset(&panes);
    int detach_requested = 0;
    int escape_pending = 0;
    int result = 0;

    if (cube_one_content_size(&terminal, &panes, &content_rows,
                              &content_cols) < 0 ||
        grid_resize(&grid, (int)content_rows, (int)content_cols) < 0) {
        result = 2;
        goto cleanup;
    }

    desk_render_layout(&terminal, &panes);
    char title[CUBICLE_NAME_MAX + 32];
    snprintf(title, sizeof(title), "cube 1: %s", process_name);
    desk_render_cube_grid(&terminal, &panes, &grid, title);

    code = desk_attach_after_terminal_enter(&target, &terminal, &panes, error,
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
                cube_one_content_size(&terminal, &panes, &content_rows,
                                      &content_cols) == 0) {
                desk_render_layout(&terminal, &panes);
                if (grid_resize(&grid, (int)content_rows,
                                (int)content_cols) == 0) {
                    (void)cubicle_attachment_resize(target.attachment,
                                                    content_rows,
                                                    content_cols);
                    desk_render_cube_grid(&terminal, &panes, &grid, title);
                }
            }
        }

        unsigned char output[4096];
        ssize_t nread = cubicle_attachment_read(target.attachment, output,
                                                sizeof(output));
        if (nread > 0) {
            grid_feed(&grid, output, (size_t)nread);
            desk_render_cube_grid(&terminal, &panes, &grid, title);
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
                    if (input[i] == 'P') {
                        if (panes.active_pane_id == 1 && i > (ssize_t)start &&
                            forward_input(target.attachment, input + start,
                                          (size_t)i - start, &escape_pending,
                                          &detach_requested) < 0) {
                            result = 2;
                            break;
                        }
                        (void)desk_dump_layout(&terminal, &panes);
                        start = (size_t)i + 1;
                        continue;
                    }
                    if (!handle_pane_key(&panes, input[i])) {
                        continue;
                    }
                    if (panes.active_pane_id == 1 && i > (ssize_t)start &&
                        forward_input(target.attachment, input + start,
                                      (size_t)i - start, &escape_pending,
                                      &detach_requested) < 0) {
                        result = 2;
                        break;
                    }
                    if (cube_one_content_size(&terminal, &panes,
                                              &content_rows,
                                              &content_cols) == 0 &&
                        grid_resize(&grid, (int)content_rows,
                                    (int)content_cols) == 0) {
                        (void)cubicle_attachment_resize(target.attachment,
                                                        content_rows,
                                                        content_cols);
                    }
                    desk_render_layout(&terminal, &panes);
                    desk_render_cube_grid(&terminal, &panes, &grid, title);
                    start = (size_t)i + 1;
                }
                if (result != 0) {
                    break;
                }
                if (panes.active_pane_id == 1 &&
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
    desk_pane_layout_t panes;
    pane_layout_reset(&panes);

    while (!g_stop_requested) {
        if (g_resize_requested) {
            g_resize_requested = 0;
            if (terminal_query_size(&terminal) == 0) {
                desk_render_layout(&terminal, &panes);
                desk_render_cube_one(&terminal, &panes, counter);
            }
        }

        time_t now = time(NULL);
        if (now != (time_t)-1 && now != last_tick) {
            last_tick = now;
            counter++;
            desk_render_cube_one(&terminal, &panes, counter);
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
            if (input[i] == 'P') {
                (void)desk_dump_layout(&terminal, &panes);
                continue;
            }
            if (handle_pane_key(&panes, input[i])) {
                desk_render_layout(&terminal, &panes);
                desk_render_cube_one(&terminal, &panes, counter);
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
