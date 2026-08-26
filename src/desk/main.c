#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cubicle/attachment.h"
#include "cubicle/client.h"
#include "cubicle/config.h"
#include "cubicle/process.h"
#include "cubicle/types.h"
#include "cubicle/util.h"
#include "cubicle/workspace.h"
#include "../common/terminal_model.h"
#include "../cubeui/cubeui.h"

#ifndef PATH_MAX
#define PATH_MAX CUBICLE_PATH_MAX
#endif

#if defined(__GNUC__)
#define DESK_UNUSED __attribute__((unused))
#else
#define DESK_UNUSED
#endif

// Border glyphs
#define DESK_ACTIVE_HORIZONTAL "─"
#define DESK_ACTIVE_VERTICAL "│"
#define DESK_ACTIVE_MIDDLE_JUNCTION "┼"
#define DESK_ACTIVE_T_DOWN "┬"
#define DESK_ACTIVE_T_UP "┴"
#define DESK_ACTIVE_T_RIGHT "├"
#define DESK_ACTIVE_T_LEFT "┤"
#define DESK_INACTIVE_HORIZONTAL "╌"
#define DESK_INACTIVE_VERTICAL "┊"
#define DESK_INACTIVE_MIDDLE_JUNCTION "┼"
#define DESK_INACTIVE_T_DOWN "┬"
#define DESK_INACTIVE_T_UP "┴"
#define DESK_INACTIVE_T_RIGHT "├"
#define DESK_INACTIVE_T_LEFT "┤"
#define DESK_MIN_PANE_COLS 4
#define DESK_MIN_PANE_ROWS 2
#define DESK_OUTPUT_READ_BURST 16
#define DESK_CURSOR_BLINK_MS 500
#define DESK_PENDING_INPUT_TIMEOUT_MS 30
#define DESK_PROCESS_REFRESH_MS 1000
#define DESK_NOTICE_MS 2500
#define DESK_PANE_TITLE_ROWS 1
#define DESK_MENU_MAX_ITEMS 128
#define DESK_NEW_COMMAND_MAX 512

static volatile sig_atomic_t g_resize_requested = 1;
static volatile sig_atomic_t g_stop_requested = 0;
static int g_desk_debug_terminal = 0;
static char g_desk_debug_log_path[CUBICLE_PATH_MAX];
static char g_desk_crash_log_path[CUBICLE_PATH_MAX];
static char g_desk_crash_phase[64] = "startup";
static char g_desk_crash_workspace[CUBICLE_NAME_MAX];
static char g_desk_crash_manager[CUBICLE_ENDPOINT_URI_MAX];

typedef cubeui_terminal_t desk_terminal_t;

static const char *desk_user_home_directory(void)
{
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        return home;
    }
    struct passwd *entry = getpwuid(geteuid());
    return entry != NULL ? entry->pw_dir : NULL;
}

static int desk_remote_registry_path(char path[CUBICLE_PATH_MAX])
{
    const char *config_home = getenv("XDG_CONFIG_HOME");
    int length;
    if (config_home != NULL && config_home[0] != '\0') {
        length = snprintf(path, CUBICLE_PATH_MAX, "%s/cubicle/remotes.tsv",
                          config_home);
    } else {
        const char *home = desk_user_home_directory();
        if (home == NULL || home[0] == '\0') {
            errno = ENOENT;
            return -1;
        }
        length = snprintf(path, CUBICLE_PATH_MAX,
                          "%s/.config/cubicle/remotes.tsv", home);
    }
    if (length < 0 || length >= CUBICLE_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int desk_remote_uri_for_name(const char *name,
                                    char uri[CUBICLE_ENDPOINT_URI_MAX])
{
    char path[CUBICLE_PATH_MAX];
    if (desk_remote_registry_path(path) < 0) {
        return -1;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    char line[4096];
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *remote_name = line;
        char *remote_uri = strchr(remote_name, '\t');
        if (remote_uri == NULL) {
            continue;
        }
        *remote_uri++ = '\0';
        char *tab = strchr(remote_uri, '\t');
        if (tab != NULL) {
            *tab = '\0';
        }
        if (strcmp(remote_name, name) == 0) {
            int length = snprintf(uri, CUBICLE_ENDPOINT_URI_MAX, "%s",
                                  remote_uri);
            fclose(file);
            if (length < 0 || (size_t)length >= CUBICLE_ENDPOINT_URI_MAX) {
                errno = ENAMETOOLONG;
                return -1;
            }
            return 0;
        }
    }
    fclose(file);
    errno = ENOENT;
    return -1;
}

static int desk_split_remote_workspace(const char *input,
                                       char workspace[CUBICLE_NAME_MAX],
                                       char remote[CUBICLE_NAME_MAX])
{
    const char *separator = strrchr(input, '@');
    if (separator == NULL) {
        return 0;
    }
    if (separator == input || separator[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    size_t workspace_length = (size_t)(separator - input);
    size_t remote_length = strlen(separator + 1);
    if (workspace_length >= CUBICLE_NAME_MAX ||
        remote_length >= CUBICLE_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(workspace, input, workspace_length);
    workspace[workspace_length] = '\0';
    memcpy(remote, separator + 1, remote_length + 1);
    return 1;
}

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
    DESK_ZOOM_VERTICAL,
    DESK_ZOOM_FULL
} desk_zoom_t;

typedef enum desk_resize_side {
    DESK_RESIZE_LEFT = 0,
    DESK_RESIZE_RIGHT,
    DESK_RESIZE_TOP,
    DESK_RESIZE_BOTTOM
} desk_resize_side_t;

typedef enum desk_pane_direction {
    DESK_PANE_LEFT = 0,
    DESK_PANE_RIGHT,
    DESK_PANE_ABOVE,
    DESK_PANE_BELOW
} desk_pane_direction_t;

typedef enum desk_pending_split {
    DESK_PENDING_SPLIT_NONE = 0,
    DESK_PENDING_SPLIT_HORIZONTAL,
    DESK_PENDING_SPLIT_VERTICAL
} desk_pending_split_t;

typedef struct desk_rect {
    int row;
    int col;
    int rows;
    int cols;
} desk_rect_t;

typedef struct desk_pane_node {
    bool used;
    int pane_id;
    char label[64];
    desk_split_t split;
    int first;
    int second;
    int split_size;
} desk_pane_node_t;

typedef struct desk_pane_layout {
    desk_pane_node_t nodes[32];
    int root;
    int active_pane_id;
    int next_pane_id;
    desk_zoom_t zoom;
    int zoom_pane_id;
    bool resize_mode;
    char status[128];
} desk_pane_layout_t;

typedef struct desk_cell {
    char text[32];
    char sgr[96];
} desk_cell_t;

typedef struct desk_grid {
    desk_cell_t *cells;
    bool *dirty_rows;
    int rows;
    int cols;
    int cursor_row;
    int cursor_col;
    bool cursor_visible;
    int scroll_top;
    int scroll_bottom;
    char current_sgr[96];
    bool escape_active;
    bool csi_active;
    char csi[64];
    size_t csi_length;
    char utf8[8];
    size_t utf8_length;
    size_t utf8_expected;
} desk_grid_t;

typedef struct desk_scrollback_line {
    int cols;
    desk_cell_t *cells;
} desk_scrollback_line_t;

typedef struct desk_attachment {
    cubicle_client_t *manager;
    cubicle_attachment_t *attachment;
    cubicle_process_info_t process;
    char workspace[CUBICLE_NAME_MAX];
    char workspace_id[CUBICLE_ID_STRING_LENGTH];
} desk_attachment_t;

typedef struct desk_pane {
    cubicle_attachment_t *attachment;
    cubicle_process_info_t process;
    desk_grid_t grid;
    cubicle_resize_tracker_t resize;
    cubicle_terminal_model_t *terminal_model;
    desk_scrollback_line_t *scrollback;
    size_t scrollback_head;
    size_t scrollback_count;
    size_t scrollback_capacity;
    size_t scrollback_offset;
    size_t scrollback_limit;
    unsigned int rows;
    unsigned int cols;
    bool ended_notice_shown;
} desk_pane_t;

typedef enum desk_menu_level {
    DESK_MENU_CLOSED = 0,
    DESK_MENU_ROOT,
    DESK_MENU_WORKSPACE,
    DESK_MENU_NEW_COMMAND,
    DESK_MENU_SAVE_LAYOUT,
    DESK_MENU_LAYOUT_PICKER,
    DESK_MENU_BINDINGS,
    DESK_MENU_BINDING_EDIT,
    DESK_MENU_MACROS,
    DESK_MENU_MACRO_NAME,
    DESK_MENU_MACRO_TEXT,
    DESK_MENU_MACRO_TARGET
} desk_menu_level_t;

typedef enum desk_menu_item_kind {
    DESK_MENU_ITEM_PROCESS = 1,
    DESK_MENU_ITEM_WORKSPACE,
    DESK_MENU_ITEM_NEW,
    DESK_MENU_ITEM_LAYOUT,
    DESK_MENU_ITEM_BINDING,
    DESK_MENU_ITEM_MACRO,
    DESK_MENU_ITEM_MACRO_NEW,
    DESK_MENU_ITEM_MACRO_TARGET
} desk_menu_item_kind_t;

typedef struct desk_menu_item {
    desk_menu_item_kind_t kind;
    bool disabled;
    cubicle_workspace_info_t workspace;
    cubicle_process_info_t process;
    char layout_name[CUBICLE_NAME_MAX];
    char command_name[CUBICLE_DESK_COMMAND_NAME_MAX];
    char description[96];
    char key_text[128];
    size_t macro_index;
    uint64_t target_pane;
} desk_menu_item_t;

typedef struct desk_open_menu {
    desk_menu_level_t level;
    desk_menu_level_t prompt_return_level;
    cubicle_workspace_info_t workspace;
    desk_menu_item_t items[DESK_MENU_MAX_ITEMS];
    size_t item_count;
    size_t selected;
    size_t scroll_offset;
    char command[DESK_NEW_COMMAND_MAX];
    size_t command_length;
    char edit_command[CUBICLE_DESK_COMMAND_NAME_MAX];
    size_t edit_macro_index;
    uint64_t edit_macro_ordinal;
    char macro_name[CUBICLE_WORKSPACE_MACRO_NAME_MAX];
    char macro_text[CUBICLE_WORKSPACE_MACRO_TEXT_MAX];
    char status[256];
} desk_open_menu_t;

typedef struct desk_menu_geometry {
    int row;
    int col;
    int rows;
    int cols;
    int item_row;
    int visible_items;
} desk_menu_geometry_t;

typedef struct desk_session {
    cubicle_client_t *manager;
    char manager_uri[CUBICLE_ENDPOINT_URI_MAX];
    bool manager_relay_attachments;
    cubicle_workspace_info_t workspace;
    desk_pane_t panes[32];
    size_t pane_count;
    desk_pane_layout_t layout;
    char layout_path[PATH_MAX];
    unsigned char prefix_key;
    unsigned char prefix_sequence[CUBICLE_DESK_KEY_SEQUENCE_MAX];
    size_t prefix_sequence_length;
    bool mouse_titles;
    long long mouse_suspended_until_ms;
    bool prefix_pending;
    bool zoomed;
    desk_pending_split_t pending_split;
    bool terminal_size_dirty;
    bool cursor_drawn;
    bool cursor_blink_visible;
    long long cursor_next_blink_ms;
    int cursor_pane_id;
    int cursor_row;
    int cursor_col;
    unsigned char pending_input[64];
    size_t pending_input_length;
    long long pending_input_since_ms;
    bool redraw_requested;
    desk_open_menu_t open_menu;
    cubicle_desk_key_binding_t key_bindings[CUBICLE_DESK_KEY_BINDING_MAX];
    size_t key_binding_count;
    cubicle_desk_key_binding_t startup_key_bindings[CUBICLE_DESK_KEY_BINDING_MAX];
    size_t startup_key_binding_count;
    cubicle_workspace_macro_info_t macros[CUBICLE_WORKSPACE_MACRO_MAX];
    size_t macro_count;
    char notice[256];
    long long notice_until_ms;
    long long next_process_refresh_ms;
    char last_opened_workspace_id[CUBICLE_ID_STRING_LENGTH];
    size_t scrollback_limit;
} desk_session_t;

static void desk_render_cube_grid(const desk_terminal_t *terminal,
                                  const desk_pane_layout_t *panes,
                                  int pane_id,
                                  desk_pane_t *pane,
                                  bool mouse_titles);
static long long desk_monotonic_ms(void);
static bool desk_string_equals_case(const char *left, const char *right);
static int desk_load_workspace_macros(desk_session_t *session,
                                      char *error,
                                      size_t error_size);
static int desk_save_macro(desk_session_t *session,
                           const cubicle_workspace_macro_info_t *macro,
                           char *error,
                           size_t error_size);
static cubicle_workspace_macro_info_t *desk_find_macro_by_command(
    desk_session_t *session,
    const char *command);
static void desk_macro_command_name(uint64_t ordinal,
                                    char *buffer,
                                    size_t size);
static int desk_run_macro(desk_session_t *session,
                          const cubicle_workspace_macro_info_t *macro);
static desk_pane_t *desk_pane_for_id(desk_session_t *session, int pane_id);
static int parse_prefix_key(const char *text, unsigned char *key);
static void desk_apply_pane_labels(desk_session_t *session);
static int resize_all_panes(desk_session_t *session,
                            const desk_terminal_t *terminal,
                            bool *changed);
static int desk_save_layout(desk_session_t *session);
static void desk_render_notice(const desk_terminal_t *terminal,
                               const desk_session_t *session);
static bool desk_execute_command(desk_session_t *session,
                                 desk_terminal_t *terminal,
                                 const char *command,
                                 bool *layout_changed,
                                 bool *quit_requested);

static void handle_signal(int signo)
{
    if (signo == SIGWINCH) {
        g_resize_requested = 1;
    } else {
        g_stop_requested = 1;
    }
}

static void desk_crash_write_string(int fd, const char *text)
{
    if (text == NULL) {
        return;
    }
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    if (length > 0) {
        (void)write(fd, text, length);
    }
}

static void desk_crash_write_long(int fd, long value)
{
    char buffer[32];
    size_t used = sizeof(buffer);
    unsigned long magnitude;
    int negative = value < 0;
    if (negative) {
        magnitude = (unsigned long)(-(value + 1)) + 1;
    } else {
        magnitude = (unsigned long)value;
    }
    buffer[--used] = '\0';
    do {
        buffer[--used] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude > 0 && used > 0);
    if (negative && used > 0) {
        buffer[--used] = '-';
    }
    desk_crash_write_string(fd, &buffer[used]);
}

static void handle_crash_signal(int signo)
{
    if (g_desk_crash_log_path[0] != '\0') {
        int fd = open(g_desk_crash_log_path,
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd >= 0) {
            desk_crash_write_string(fd, "event=crash pid=");
            desk_crash_write_long(fd, (long)getpid());
            desk_crash_write_string(fd, " signal=");
            desk_crash_write_long(fd, (long)signo);
            desk_crash_write_string(fd, " phase=");
            desk_crash_write_string(fd, g_desk_crash_phase);
            desk_crash_write_string(fd, " workspace=\"");
            desk_crash_write_string(fd, g_desk_crash_workspace);
            desk_crash_write_string(fd, "\" manager=\"");
            desk_crash_write_string(fd, g_desk_crash_manager);
            desk_crash_write_string(fd, "\"\n");
            close(fd);
        }
    }

    signal(signo, SIG_DFL);
    raise(signo);
    _exit(128 + signo);
}

static void desk_crash_set_phase(const char *phase)
{
    if (phase == NULL) {
        phase = "";
    }
    snprintf(g_desk_crash_phase, sizeof(g_desk_crash_phase), "%s", phase);
}

static void desk_crash_configure(const cubicle_config_t *config)
{
    g_desk_crash_log_path[0] = '\0';
    if (config == NULL || config->manager_log_dir[0] == '\0') {
        return;
    }
    int length = snprintf(g_desk_crash_log_path,
                          sizeof(g_desk_crash_log_path),
                          "%s/desk-crash.log", config->manager_log_dir);
    if (length <= 0 || (size_t)length >= sizeof(g_desk_crash_log_path)) {
        g_desk_crash_log_path[0] = '\0';
        return;
    }
    (void)cubicle_mkdir_p(config->manager_log_dir);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_crash_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    (void)sigaction(SIGABRT, &action, NULL);
    (void)sigaction(SIGBUS, &action, NULL);
    (void)sigaction(SIGFPE, &action, NULL);
    (void)sigaction(SIGILL, &action, NULL);
    (void)sigaction(SIGSEGV, &action, NULL);
}

static void desk_debug_timestamp(char *buffer, size_t size)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        snprintf(buffer, size, "unknown-time");
        return;
    }
    struct tm tm_value;
    localtime_r(&ts.tv_sec, &tm_value);
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &tm_value);
}

static void desk_debug_log(const char *format, ...)
{
    if (!g_desk_debug_terminal || g_desk_debug_log_path[0] == '\0') {
        return;
    }

    FILE *file = fopen(g_desk_debug_log_path, "a");
    if (file == NULL) {
        return;
    }

    char timestamp[64];
    desk_debug_timestamp(timestamp, sizeof(timestamp));
    fprintf(file, "%s pid=%ld ", timestamp, (long)getpid());

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fclose(file);
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

static int pane_create_leaf(desk_pane_layout_t *panes, int pane_id,
                            const char *label)
{
    int node = pane_alloc_node(panes);
    if (node >= 0) {
        panes->nodes[node].pane_id = pane_id;
        snprintf(panes->nodes[node].label, sizeof(panes->nodes[node].label),
                 "%s", label);
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
        panes->nodes[node].split_size = 0;
    }
    return node;
}

static void pane_layout_reset(desk_pane_layout_t *panes)
{
    memset(panes, 0, sizeof(*panes));
    int one = pane_create_leaf(panes, 1, "1");
    int two = pane_create_leaf(panes, 2, "2");
    int three = pane_create_leaf(panes, 3, "3");
    int four = pane_create_leaf(panes, 4, "4");
    int top = pane_create_split(panes, DESK_SPLIT_HORIZONTAL, one, two);
    int bottom = pane_create_split(panes, DESK_SPLIT_HORIZONTAL, three, four);
    panes->root = pane_create_split(panes, DESK_SPLIT_VERTICAL, top, bottom);
    panes->active_pane_id = 1;
    panes->next_pane_id = 5;
    panes->zoom = DESK_ZOOM_NONE;
    panes->resize_mode = false;
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

static int pane_find_leaf_node(const desk_pane_layout_t *panes, int node,
                               int pane_id);

static int pane_layout_zoom_target(const desk_pane_layout_t *panes)
{
    if ((panes->zoom == DESK_ZOOM_HORIZONTAL ||
         panes->zoom == DESK_ZOOM_VERTICAL) &&
        panes->zoom_pane_id > 0 &&
        pane_find_leaf_node(panes, panes->root, panes->zoom_pane_id) >= 0) {
        return panes->zoom_pane_id;
    }
    return panes->active_pane_id;
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
}

static void pane_layout_previous(desk_pane_layout_t *panes)
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
        if (fallback == 0 || pane_id > fallback) {
            fallback = pane_id;
        }
        if (pane_id < panes->active_pane_id &&
            (best == 0 || pane_id > best)) {
            best = pane_id;
        }
    }
    panes->active_pane_id = best != 0 ? best : fallback;
}

static int pane_layout_next_id(const desk_pane_layout_t *panes);

static size_t pane_layout_leaf_count(const desk_pane_layout_t *panes)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(panes->nodes) / sizeof(panes->nodes[0]);
         ++i) {
        if (panes->nodes[i].used &&
            panes->nodes[i].split == DESK_SPLIT_NONE) {
            count++;
        }
    }
    return count;
}

static int pane_layout_split(desk_pane_layout_t *panes, desk_split_t split)
{
    int leaf = pane_find_leaf_node(panes, panes->root, panes->active_pane_id);
    if (leaf < 0) {
        return -1;
    }
    char first_label[64];
    char second_label[64];
    int first_length = snprintf(first_label, sizeof(first_label), "%s.1",
                                panes->nodes[leaf].label);
    int second_length = snprintf(second_label, sizeof(second_label), "%s.2",
                                 panes->nodes[leaf].label);
    if (first_length < 0 || second_length < 0 ||
        first_length >= (int)sizeof(first_label) ||
        second_length >= (int)sizeof(second_label)) {
        return -1;
    }
    int current_pane_id = panes->nodes[leaf].pane_id;
    int new_pane_id = panes->next_pane_id++;
    int first_leaf = pane_create_leaf(panes, current_pane_id, first_label);
    int new_leaf = pane_create_leaf(panes, new_pane_id, second_label);
    if (first_leaf < 0 || new_leaf < 0) {
        if (first_leaf >= 0) panes->nodes[first_leaf].used = false;
        if (new_leaf >= 0) panes->nodes[new_leaf].used = false;
        panes->next_pane_id--;
        return -1;
    }
    panes->nodes[leaf].split = split;
    panes->nodes[leaf].first = first_leaf;
    panes->nodes[leaf].second = new_leaf;
    panes->nodes[leaf].split_size = 0;
    panes->nodes[leaf].pane_id = 0;
    panes->active_pane_id = panes->nodes[new_leaf].pane_id;
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
    return 0;
}

static void pane_layout_compact_after_delete(desk_pane_layout_t *panes,
                                             int removed_pane_id)
{
    for (size_t i = 0; i < sizeof(panes->nodes) / sizeof(panes->nodes[0]);
         ++i) {
        if (!panes->nodes[i].used ||
            panes->nodes[i].split != DESK_SPLIT_NONE) {
            continue;
        }
        if (panes->nodes[i].pane_id > removed_pane_id) {
            panes->nodes[i].pane_id--;
        }
    }
    if (panes->active_pane_id > removed_pane_id) {
        panes->active_pane_id--;
    }
    if (panes->zoom_pane_id == removed_pane_id) {
        panes->zoom_pane_id = panes->active_pane_id;
    } else if (panes->zoom_pane_id > removed_pane_id) {
        panes->zoom_pane_id--;
    }
    panes->next_pane_id = pane_layout_next_id(panes);
}

static void pane_layout_status(desk_pane_layout_t *panes,
                               const char *message)
{
    snprintf(panes->status, sizeof(panes->status), "%s", message);
}

static void pane_layout_clear_status(desk_pane_layout_t *panes)
{
    panes->status[0] = '\0';
}

static int split_default_size(desk_rect_t rect, desk_split_t split)
{
    return split == DESK_SPLIT_HORIZONTAL ? rect.cols / 2 : rect.rows / 2;
}

static int split_clamp_size(desk_rect_t rect, desk_split_t split, int size)
{
    int total = split == DESK_SPLIT_HORIZONTAL ? rect.cols : rect.rows;
    int minimum = split == DESK_SPLIT_HORIZONTAL ? DESK_MIN_PANE_COLS
                                                 : DESK_MIN_PANE_ROWS;
    int maximum = total - minimum - 1;
    if (maximum < minimum) {
        minimum = total > 1 ? 1 : 0;
        maximum = total > 1 ? total - 2 : 0;
    }
    if (size < minimum) {
        return minimum;
    }
    if (size > maximum) {
        return maximum;
    }
    return size;
}

static int split_effective_size(const desk_pane_node_t *entry,
                                desk_rect_t rect)
{
    int size = entry->split_size > 0 ? entry->split_size
                                     : split_default_size(rect, entry->split);
    return split_clamp_size(rect, entry->split, size);
}

static void split_rect(desk_rect_t rect, const desk_pane_node_t *entry,
                       desk_rect_t *first, desk_rect_t *second,
                       desk_rect_t *divider)
{
    *first = rect;
    *second = rect;
    memset(divider, 0, sizeof(*divider));
    if (entry->split == DESK_SPLIT_HORIZONTAL) {
        int left = split_effective_size(entry, rect);
        first->cols = left;
        second->col = rect.col + left + 1;
        second->cols = rect.cols - left - 1;
        divider->row = rect.row;
        divider->col = rect.col + left;
        divider->rows = rect.rows;
        divider->cols = 1;
    } else {
        int top = split_effective_size(entry, rect);
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

    int zoom_target = pane_layout_zoom_target(panes);
    bool first_has_active =
        pane_subtree_contains(panes, entry->first, zoom_target);
    bool second_has_active =
        pane_subtree_contains(panes, entry->second, zoom_target);
    bool zooms_this_split =
        panes->zoom == DESK_ZOOM_FULL ||
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
    split_rect(rect, entry, &first, &second, &divider);
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

static int rect_center_row(desk_rect_t rect)
{
    return rect.row * 2 + rect.rows;
}

static int rect_center_col(desk_rect_t rect)
{
    return rect.col * 2 + rect.cols;
}

static int rect_overlap_1d(int start_a, int length_a, int start_b, int length_b)
{
    int end_a = start_a + length_a;
    int end_b = start_b + length_b;
    int start = start_a > start_b ? start_a : start_b;
    int end = end_a < end_b ? end_a : end_b;
    return end > start ? end - start : 0;
}

static bool pane_direction_candidate_score(desk_rect_t active,
                                           desk_rect_t candidate,
                                           desk_pane_direction_t direction,
                                           bool require_overlap,
                                           int *gap_out,
                                           int *perpendicular_out)
{
    int gap = 0;
    int overlap = 0;
    int perpendicular = 0;
    if (direction == DESK_PANE_LEFT) {
        if (candidate.col >= active.col) {
            return false;
        }
        gap = active.col - (candidate.col + candidate.cols);
        if (gap < 0) {
            gap = 0;
        }
        overlap = rect_overlap_1d(active.row, active.rows,
                                  candidate.row, candidate.rows);
        perpendicular = abs(rect_center_row(active) -
                            rect_center_row(candidate));
    } else if (direction == DESK_PANE_RIGHT) {
        if (candidate.col <= active.col) {
            return false;
        }
        gap = candidate.col - (active.col + active.cols);
        if (gap < 0) {
            gap = 0;
        }
        overlap = rect_overlap_1d(active.row, active.rows,
                                  candidate.row, candidate.rows);
        perpendicular = abs(rect_center_row(active) -
                            rect_center_row(candidate));
    } else if (direction == DESK_PANE_ABOVE) {
        if (candidate.row >= active.row) {
            return false;
        }
        gap = active.row - (candidate.row + candidate.rows);
        if (gap < 0) {
            gap = 0;
        }
        overlap = rect_overlap_1d(active.col, active.cols,
                                  candidate.col, candidate.cols);
        perpendicular = abs(rect_center_col(active) -
                            rect_center_col(candidate));
    } else {
        if (candidate.row <= active.row) {
            return false;
        }
        gap = candidate.row - (active.row + active.rows);
        if (gap < 0) {
            gap = 0;
        }
        overlap = rect_overlap_1d(active.col, active.cols,
                                  candidate.col, candidate.cols);
        perpendicular = abs(rect_center_col(active) -
                            rect_center_col(candidate));
    }

    if (require_overlap && overlap <= 0) {
        return false;
    }
    *gap_out = gap;
    *perpendicular_out = perpendicular;
    return true;
}

static bool pane_layout_select_direction(desk_pane_layout_t *panes,
                                         const desk_terminal_t *terminal,
                                         size_t pane_count,
                                         desk_pane_direction_t direction)
{
    int active_id = panes->active_pane_id;
    if (active_id <= 0 || (size_t)active_id > pane_count) {
        return false;
    }

    desk_rect_t active;
    if (!pane_layout_rect_for_pane(panes, terminal, active_id, &active)) {
        return false;
    }

    int best = 0;
    int best_gap = 0;
    int best_perpendicular = 0;
    for (int pass = 0; pass < 2 && best == 0; ++pass) {
        bool require_overlap = pass == 0;
        for (size_t i = 0; i < pane_count; ++i) {
            int candidate_id = (int)i + 1;
            if (candidate_id == active_id) {
                continue;
            }
            desk_rect_t candidate;
            if (!pane_layout_rect_for_pane(panes, terminal, candidate_id,
                                           &candidate)) {
                continue;
            }
            int gap = 0;
            int perpendicular = 0;
            if (!pane_direction_candidate_score(active, candidate, direction,
                                                require_overlap, &gap,
                                                &perpendicular)) {
                continue;
            }
            if (best == 0 || gap < best_gap ||
                (gap == best_gap && perpendicular < best_perpendicular) ||
                (gap == best_gap && perpendicular == best_perpendicular &&
                 candidate_id < best)) {
                best = candidate_id;
                best_gap = gap;
                best_perpendicular = perpendicular;
            }
        }
    }

    if (best == 0) {
        return false;
    }
    panes->active_pane_id = best;
    return true;
}

static bool pane_content_rect_for_pane(const desk_pane_layout_t *panes,
                                       const desk_terminal_t *terminal,
                                       int pane_id,
                                       desk_rect_t *out)
{
    desk_rect_t rect;
    if (!pane_layout_rect_for_pane(panes, terminal, pane_id, &rect)) {
        return false;
    }
    if (rect.rows <= DESK_PANE_TITLE_ROWS) {
        return false;
    }
    rect.row += DESK_PANE_TITLE_ROWS;
    rect.rows -= DESK_PANE_TITLE_ROWS;
    *out = rect;
    return true;
}

static bool pane_layout_rect_for_tree_node(const desk_pane_layout_t *panes,
                                           int node,
                                           int target,
                                           desk_rect_t rect,
                                           desk_rect_t *out)
{
    if (node < 0 || !panes->nodes[node].used || rect.rows <= 0 ||
        rect.cols <= 0) {
        return false;
    }
    if (node == target) {
        *out = rect;
        return true;
    }

    const desk_pane_node_t *entry = &panes->nodes[node];
    if (entry->split == DESK_SPLIT_NONE) {
        return false;
    }

    int zoom_target = pane_layout_zoom_target(panes);
    bool first_has_active =
        pane_subtree_contains(panes, entry->first, zoom_target);
    bool second_has_active =
        pane_subtree_contains(panes, entry->second, zoom_target);
    bool zooms_this_split =
        panes->zoom == DESK_ZOOM_FULL ||
        (panes->zoom == DESK_ZOOM_HORIZONTAL &&
         entry->split == DESK_SPLIT_HORIZONTAL) ||
        (panes->zoom == DESK_ZOOM_VERTICAL &&
         entry->split == DESK_SPLIT_VERTICAL);
    if (zooms_this_split && (first_has_active || second_has_active)) {
        return pane_layout_rect_for_tree_node(
            panes, first_has_active ? entry->first : entry->second, target,
            rect, out);
    }

    desk_rect_t first;
    desk_rect_t second;
    desk_rect_t divider;
    split_rect(rect, entry, &first, &second, &divider);
    return pane_layout_rect_for_tree_node(panes, entry->first, target, first,
                                          out) ||
           pane_layout_rect_for_tree_node(panes, entry->second, target, second,
                                          out);
}

static int pane_resize_delta_for_side(desk_resize_side_t side, int delta)
{
    (void)side;
    return delta;
}

static int pane_layout_resize_side(desk_pane_layout_t *panes,
                                   const desk_terminal_t *terminal,
                                   desk_resize_side_t side,
                                   int delta)
{
    int child = pane_find_leaf_node(panes, panes->root,
                                    panes->active_pane_id);
    while (child >= 0) {
        int parent = pane_find_parent(panes, panes->root, child);
        if (parent < 0) {
            return -1;
        }

        desk_pane_node_t *entry = &panes->nodes[parent];
        bool child_is_first = entry->first == child;
        bool owns_side =
            (side == DESK_RESIZE_LEFT &&
             entry->split == DESK_SPLIT_HORIZONTAL && !child_is_first) ||
            (side == DESK_RESIZE_RIGHT &&
             entry->split == DESK_SPLIT_HORIZONTAL && child_is_first) ||
            (side == DESK_RESIZE_TOP &&
             entry->split == DESK_SPLIT_VERTICAL && !child_is_first) ||
            (side == DESK_RESIZE_BOTTOM &&
             entry->split == DESK_SPLIT_VERTICAL && child_is_first);
        if (owns_side) {
            desk_rect_t rect;
            desk_rect_t root = {0, 0, terminal->rows, terminal->cols};
            if (!pane_layout_rect_for_tree_node(panes, panes->root, parent,
                                                root, &rect)) {
                return -1;
            }
            int size = split_effective_size(entry, rect);
            size += pane_resize_delta_for_side(side, delta);
            entry->split_size = split_clamp_size(rect, entry->split, size);
            return 0;
        }
        child = parent;
    }
    return -1;
}

static bool pane_split_divider_rect(const desk_pane_layout_t *panes,
                                    const desk_terminal_t *terminal,
                                    int split_node,
                                    desk_rect_t *divider)
{
    desk_rect_t rect;
    desk_rect_t first;
    desk_rect_t second;
    desk_rect_t root = {0, 0, terminal->rows, terminal->cols};
    if (!pane_layout_rect_for_tree_node(panes, panes->root, split_node, root,
                                        &rect)) {
        return false;
    }
    split_rect(rect, &panes->nodes[split_node], &first, &second, divider);
    return true;
}

static bool pane_dividers_align(const desk_rect_t *first,
                                const desk_rect_t *second,
                                desk_split_t split)
{
    if (split == DESK_SPLIT_HORIZONTAL) {
        return first->col == second->col && first->rows == second->rows &&
               first->row == second->row;
    }
    return first->row == second->row && first->cols == second->cols &&
           first->col == second->col;
}

static int pane_layout_transpose(desk_pane_layout_t *panes,
                                 const desk_terminal_t *terminal)
{
    int child = pane_find_leaf_node(panes, panes->root,
                                    panes->active_pane_id);
    while (child >= 0) {
        int active_split = pane_find_parent(panes, panes->root, child);
        if (active_split < 0) {
            pane_layout_status(panes, "transpose: no eligible parent split");
            return -1;
        }
        int local_parent = pane_find_parent(panes, panes->root, active_split);
        int grandparent = local_parent >= 0
                              ? pane_find_parent(panes, panes->root,
                                                 local_parent)
                              : -1;
        if (local_parent < 0 || grandparent < 0) {
            child = active_split;
            continue;
        }

        desk_pane_node_t *active_node = &panes->nodes[active_split];
        desk_pane_node_t *local_node = &panes->nodes[local_parent];
        desk_pane_node_t *grand_node = &panes->nodes[grandparent];
        if (active_node->split == DESK_SPLIT_NONE ||
            local_node->split == DESK_SPLIT_NONE ||
            grand_node->split == DESK_SPLIT_NONE ||
            local_node->split != grand_node->split ||
            active_node->split == grand_node->split) {
            child = active_split;
            continue;
        }

        bool local_is_first = grand_node->first == local_parent;
        bool active_is_first = local_node->first == active_split;
        int sibling_split = local_is_first ? grand_node->second
                                           : grand_node->first;
        int outside = active_is_first ? local_node->second
                                      : local_node->first;
        if ((local_is_first && active_is_first) ||
            (!local_is_first && !active_is_first)) {
            child = active_split;
            continue;
        }
        if (sibling_split < 0 || !panes->nodes[sibling_split].used ||
            panes->nodes[sibling_split].split != active_node->split) {
            pane_layout_status(panes, "transpose: adjacent split mismatch");
            return -1;
        }

        desk_rect_t active_divider;
        desk_rect_t sibling_divider;
        if (!pane_split_divider_rect(panes, terminal, active_split,
                                     &active_divider) ||
            !pane_split_divider_rect(panes, terminal, sibling_split,
                                     &sibling_divider)) {
            pane_layout_status(panes, "transpose: cannot resolve dividers");
            return -1;
        }
        if (!pane_dividers_align(&active_divider, &sibling_divider,
                                 active_node->split)) {
            pane_layout_status(panes, "transpose: dividers are not aligned");
            return -1;
        }

        int active_first = active_node->first;
        int active_second = active_node->second;
        int sibling_first = panes->nodes[sibling_split].first;
        int sibling_second = panes->nodes[sibling_split].second;
        desk_split_t outer_split = active_node->split;
        desk_split_t pair_split = grand_node->split;
        int outer_size = active_node->split_size;
        int pair_size = grand_node->split_size;

        grand_node->split = pair_split;
        grand_node->split_size = local_node->split_size;
        if (local_is_first) {
            grand_node->first = outside;
            grand_node->second = active_split;
        } else {
            grand_node->first = active_split;
            grand_node->second = outside;
        }

        active_node->split = outer_split;
        active_node->split_size = outer_size;
        active_node->first = local_parent;
        active_node->second = sibling_split;

        local_node->split = pair_split;
        local_node->split_size = pair_size;
        panes->nodes[sibling_split].split = pair_split;
        panes->nodes[sibling_split].split_size = pair_size;
        if (local_is_first) {
            local_node->first = active_first;
            local_node->second = sibling_first;
            panes->nodes[sibling_split].first = active_second;
            panes->nodes[sibling_split].second = sibling_second;
        } else {
            local_node->first = sibling_first;
            local_node->second = active_first;
            panes->nodes[sibling_split].first = sibling_second;
            panes->nodes[sibling_split].second = active_second;
        }

        pane_layout_clear_status(panes);
        return 0;
    }

    pane_layout_status(panes, "transpose: no active pane");
    return -1;
}

static int pane_layout_next_id(const desk_pane_layout_t *panes)
{
    int next_id = 1;
    for (size_t i = 0; i < sizeof(panes->nodes) / sizeof(panes->nodes[0]);
         ++i) {
        if (panes->nodes[i].used &&
            panes->nodes[i].split == DESK_SPLIT_NONE &&
            panes->nodes[i].pane_id >= next_id) {
            next_id = panes->nodes[i].pane_id + 1;
        }
    }
    return next_id;
}

static bool layout_name_is_safe(const char *name)
{
    if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        return false;
    }
    for (const char *cursor = name; *cursor != '\0'; ++cursor) {
        if (!isalnum((unsigned char)*cursor) && *cursor != '-' &&
            *cursor != '_' && *cursor != '.') {
            return false;
        }
    }
    return true;
}

static int layout_path_from_name(const char *name, char path[PATH_MAX])
{
    if (!layout_name_is_safe(name)) {
        errno = EINVAL;
        return -1;
    }
    const char *suffix = ".layout";
    size_t name_length = strlen(name);
    size_t suffix_length = strlen(suffix);
    bool has_suffix =
        name_length >= suffix_length &&
        strcmp(name + name_length - suffix_length, suffix) == 0;
    int length = snprintf(path, PATH_MAX, "%s%s", name,
                          has_suffix ? "" : suffix);
    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static bool layout_has_suffix(const char *path)
{
    const char *suffix = ".layout";
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);
    return path_length >= suffix_length &&
           strcmp(path + path_length - suffix_length, suffix) == 0;
}

static int pane_layout_write(FILE *file, const desk_pane_layout_t *panes,
                             bool include_header)
{
    if (include_header) {
        fprintf(file, "desk-layout-v1\n");
    }
    fprintf(file, "root %d\n", panes->root);
    fprintf(file, "active %d\n", panes->active_pane_id);
    fprintf(file, "next %d\n", panes->next_pane_id);
    fprintf(file, "zoom %d\n", (int)panes->zoom);
    fprintf(file, "zoom_pane %d\n", panes->zoom_pane_id);
    for (size_t i = 0; i < sizeof(panes->nodes) / sizeof(panes->nodes[0]);
         ++i) {
        if (!panes->nodes[i].used) {
            continue;
        }
        const desk_pane_node_t *node = &panes->nodes[i];
        fprintf(file, "node %zu %d %d %d %d %d %s\n", i, node->pane_id,
                (int)node->split, node->first, node->second,
                node->split_size, node->label);
    }
    return ferror(file) ? -1 : 0;
}

static int pane_layout_save(const desk_pane_layout_t *panes,
                            const char *path)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }
    if (pane_layout_write(file, panes, true) < 0) {
        fclose(file);
        return -1;
    }
    return fclose(file);
}

static void pane_layout_init_loaded(desk_pane_layout_t *loaded)
{
    memset(loaded, 0, sizeof(*loaded));
    loaded->root = -1;
    loaded->active_pane_id = 1;
    loaded->next_pane_id = 1;
}

static int pane_layout_parse_line(desk_pane_layout_t *loaded,
                                  const char *line)
{
    int value = 0;
    if (sscanf(line, "root %d", &value) == 1) {
        loaded->root = value;
        return 0;
    }
    if (sscanf(line, "active %d", &value) == 1) {
        loaded->active_pane_id = value;
        return 0;
    }
    if (sscanf(line, "next %d", &value) == 1) {
        loaded->next_pane_id = value;
        return 0;
    }
    if (sscanf(line, "zoom %d", &value) == 1) {
        loaded->zoom = (desk_zoom_t)value;
        return 0;
    }
    if (sscanf(line, "zoom_pane %d", &value) == 1) {
        loaded->zoom_pane_id = value;
        return 0;
    }

    int index = -1;
    int pane_id = 0;
    int split = 0;
    int first = 0;
    int second = 0;
    int split_size = 0;
    char label[64] = "";
    int matched = sscanf(line, "node %d %d %d %d %d %d %63[^\n]", &index,
                         &pane_id, &split, &first, &second, &split_size,
                         label);
    if (matched == 6 || matched == 7) {
        if (index < 0 ||
            index >= (int)(sizeof(loaded->nodes) / sizeof(loaded->nodes[0])) ||
            split < DESK_SPLIT_NONE || split > DESK_SPLIT_VERTICAL ||
            (split == DESK_SPLIT_NONE && matched != 7)) {
            errno = EINVAL;
            return -1;
        }
        loaded->nodes[index].used = true;
        loaded->nodes[index].pane_id = pane_id;
        snprintf(loaded->nodes[index].label,
                 sizeof(loaded->nodes[index].label), "%s", label);
        loaded->nodes[index].split = (desk_split_t)split;
        loaded->nodes[index].first = first;
        loaded->nodes[index].second = second;
        loaded->nodes[index].split_size = split_size;
        return 0;
    }

    errno = EINVAL;
    return -1;
}

static int pane_layout_finish_loaded(desk_pane_layout_t *panes,
                                     desk_pane_layout_t *loaded)
{
    if (loaded->root < 0 ||
        loaded->root >= (int)(sizeof(loaded->nodes) / sizeof(loaded->nodes[0])) ||
        !loaded->nodes[loaded->root].used ||
        pane_find_leaf_node(loaded, loaded->root, loaded->active_pane_id) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (loaded->next_pane_id <= 0) {
        loaded->next_pane_id = pane_layout_next_id(loaded);
    }
    if (loaded->zoom == DESK_ZOOM_NONE ||
        pane_find_leaf_node(loaded, loaded->root, loaded->zoom_pane_id) < 0) {
        loaded->zoom_pane_id = loaded->zoom == DESK_ZOOM_NONE
                                   ? 0
                                   : loaded->active_pane_id;
    }
    loaded->resize_mode = false;
    *panes = *loaded;
    return 0;
}

static int pane_layout_load_file(desk_pane_layout_t *panes, const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    desk_pane_layout_t loaded;
    pane_layout_init_loaded(&loaded);
    char line[256];
    if (fgets(line, sizeof(line), file) == NULL ||
        strcmp(line, "desk-layout-v1\n") != 0) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (pane_layout_parse_line(&loaded, line) < 0) {
            fclose(file);
            return -1;
        }
    }
    fclose(file);
    return pane_layout_finish_loaded(panes, &loaded);
}

static int pane_layout_build_grid_range(desk_pane_layout_t *panes,
                                        int row_start,
                                        int rows,
                                        int col_start,
                                        int cols,
                                        int total_cols)
{
    if (rows == 1 && cols == 1) {
        int pane_id = row_start * total_cols + col_start + 1;
        char label[32];
        snprintf(label, sizeof(label), "%d", pane_id);
        return pane_create_leaf(panes, pane_id, label);
    }
    if (cols > 1) {
        int left_cols = cols / 2;
        int left = pane_layout_build_grid_range(panes, row_start, rows,
                                                col_start, left_cols,
                                                total_cols);
        int right = pane_layout_build_grid_range(
            panes, row_start, rows, col_start + left_cols, cols - left_cols,
            total_cols);
        return left < 0 || right < 0
                   ? -1
                   : pane_create_split(panes, DESK_SPLIT_HORIZONTAL, left,
                                       right);
    }

    int top_rows = rows / 2;
    int top = pane_layout_build_grid_range(panes, row_start, top_rows,
                                           col_start, cols, total_cols);
    int bottom = pane_layout_build_grid_range(
        panes, row_start + top_rows, rows - top_rows, col_start, cols,
        total_cols);
    return top < 0 || bottom < 0
               ? -1
               : pane_create_split(panes, DESK_SPLIT_VERTICAL, top, bottom);
}

static int pane_layout_grid(desk_pane_layout_t *panes, int rows, int cols)
{
    if (rows <= 0 || cols <= 0 || rows * cols > 16) {
        errno = EINVAL;
        return -1;
    }
    memset(panes, 0, sizeof(*panes));
    panes->root = pane_layout_build_grid_range(panes, 0, rows, 0, cols, cols);
    if (panes->root < 0) {
        errno = EINVAL;
        return -1;
    }
    panes->active_pane_id = 1;
    panes->next_pane_id = rows * cols + 1;
    panes->zoom = DESK_ZOOM_NONE;
    panes->resize_mode = false;
    return 0;
}

static int pane_layout_three_panes(desk_pane_layout_t *panes)
{
    memset(panes, 0, sizeof(*panes));
    int top_left = pane_create_leaf(panes, 1, "1");
    int bottom_left = pane_create_leaf(panes, 2, "2");
    int right = pane_create_leaf(panes, 3, "3");
    if (top_left < 0 || bottom_left < 0 || right < 0) {
        errno = EINVAL;
        return -1;
    }
    int left = pane_create_split(panes, DESK_SPLIT_VERTICAL, top_left,
                                 bottom_left);
    if (left < 0) {
        errno = EINVAL;
        return -1;
    }
    panes->root = pane_create_split(panes, DESK_SPLIT_HORIZONTAL, left, right);
    if (panes->root < 0) {
        errno = EINVAL;
        return -1;
    }
    panes->active_pane_id = 1;
    panes->next_pane_id = 4;
    panes->zoom = DESK_ZOOM_NONE;
    panes->resize_mode = false;
    return 0;
}

static void desk_auto_grid(size_t pane_count, int *rows, int *cols)
{
    if (pane_count <= 1) {
        *rows = 1;
        *cols = 1;
        return;
    }
    if (pane_count == 2) {
        *rows = 1;
        *cols = 2;
        return;
    }
    if (pane_count <= 4) {
        *rows = 2;
        *cols = 2;
        return;
    }

    int computed_cols = 1;
    while ((size_t)computed_cols * (size_t)computed_cols < pane_count) {
        computed_cols++;
    }
    *cols = computed_cols;
    *rows = (int)((pane_count + (size_t)computed_cols - 1) /
                  (size_t)computed_cols);
}

static int pane_layout_auto(desk_pane_layout_t *panes, size_t pane_count)
{
    if (pane_count == 3) {
        return pane_layout_three_panes(panes);
    }
    int rows = 0;
    int cols = 0;
    desk_auto_grid(pane_count, &rows, &cols);
    return pane_layout_grid(panes, rows, cols);
}

static bool parse_grid_layout(const char *text, int *rows, int *cols)
{
    char *end = NULL;
    long parsed_rows = strtol(text, &end, 10);
    if (end == text || (*end != 'x' && *end != 'X')) {
        return false;
    }
    char *col_start = end + 1;
    long parsed_cols = strtol(col_start, &end, 10);
    if (end == col_start || *end != '\0' || parsed_rows <= 0 ||
        parsed_cols <= 0 || parsed_rows > 16 || parsed_cols > 16) {
        return false;
    }
    *rows = (int)parsed_rows;
    *cols = (int)parsed_cols;
    return true;
}

static int DESK_UNUSED pane_layout_load_startup(desk_pane_layout_t *panes,
                                                const char *layout_spec)
{
    pane_layout_reset(panes);
    if (layout_spec == NULL) {
        if (pane_layout_load_file(panes, "default.layout") == 0) {
            return 0;
        }
        return errno == ENOENT ? 0 : -1;
    }

    int rows = 0;
    int cols = 0;
    if (parse_grid_layout(layout_spec, &rows, &cols)) {
        return pane_layout_grid(panes, rows, cols);
    }
    if (layout_has_suffix(layout_spec) || strchr(layout_spec, '/') != NULL) {
        return pane_layout_load_file(panes, layout_spec);
    }
    char path[PATH_MAX];
    if (layout_path_from_name(layout_spec, path) < 0) {
        return -1;
    }
    return pane_layout_load_file(panes, path);
}

static int desk_layout_path_for_workspace(const char *workspace_id,
                                          char path[PATH_MAX])
{
    char state_dir[PATH_MAX];
    if (cubeui_state_dir(state_dir) < 0) {
        return -1;
    }
    char layout_dir[PATH_MAX];
    int length = snprintf(layout_dir, sizeof(layout_dir), "%s/desk-layouts",
                          state_dir);
    if (length < 0 || (size_t)length >= sizeof(layout_dir) ||
        cubicle_mkdir_p(layout_dir) < 0) {
        return -1;
    }
    length = snprintf(path, PATH_MAX, "%s/%s.layout", layout_dir,
                      workspace_id);
    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static int desk_named_layout_dir(char path[PATH_MAX])
{
    char state_dir[PATH_MAX];
    if (cubeui_state_dir(state_dir) < 0) {
        return -1;
    }
    int length = snprintf(path, PATH_MAX, "%s/desk-layouts/named",
                          state_dir);
    if (length < 0 || length >= PATH_MAX) {
        return -1;
    }
    return cubicle_mkdir_p(path);
}

static int desk_named_layout_path(const char *name, char path[PATH_MAX])
{
    char dir[PATH_MAX];
    char file_name[PATH_MAX];
    if (desk_named_layout_dir(dir) < 0 || layout_path_from_name(name, file_name) < 0) {
        return -1;
    }
    int length = snprintf(path, PATH_MAX, "%s/%s", dir, file_name);
    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static void desk_layout_collect_leaf_panes(const desk_pane_layout_t *layout,
                                           int node_index,
                                           bool pane_ids[32],
                                           int *max_pane_id)
{
    if (node_index < 0 ||
        node_index >= (int)(sizeof(layout->nodes) / sizeof(layout->nodes[0])) ||
        !layout->nodes[node_index].used) {
        return;
    }
    const desk_pane_node_t *node = &layout->nodes[node_index];
    if (node->split == DESK_SPLIT_NONE) {
        if (node->pane_id > 0 && node->pane_id <= 32) {
            pane_ids[node->pane_id - 1] = true;
            if (node->pane_id > *max_pane_id) {
                *max_pane_id = node->pane_id;
            }
        }
        return;
    }
    desk_layout_collect_leaf_panes(layout, node->first, pane_ids, max_pane_id);
    desk_layout_collect_leaf_panes(layout, node->second, pane_ids, max_pane_id);
}

static int desk_save_named_layout(desk_session_t *session, const char *name)
{
    char path[PATH_MAX];
    if (desk_named_layout_path(name, path) < 0) {
        return -1;
    }
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }
    fprintf(file, "desk-named-layout-v1\n");
    fprintf(file, "workspace %s\n", session->workspace.id);
    for (size_t i = 0; i < session->pane_count; ++i) {
        if (session->panes[i].process.id[0] != '\0') {
            fprintf(file, "pane %zu %s\n", i + 1, session->panes[i].process.id);
        }
    }
    desk_zoom_t saved_zoom = session->layout.zoom;
    int saved_zoom_pane_id = session->layout.zoom_pane_id;
    session->layout.zoom = DESK_ZOOM_NONE;
    session->layout.zoom_pane_id = 0;
    if (pane_layout_write(file, &session->layout, false) < 0) {
        session->layout.zoom = saved_zoom;
        session->layout.zoom_pane_id = saved_zoom_pane_id;
        fclose(file);
        return -1;
    }
    session->layout.zoom = saved_zoom;
    session->layout.zoom_pane_id = saved_zoom_pane_id;
    return fclose(file);
}

static int desk_load_named_layout_file(const char *path,
                                       desk_pane_layout_t *layout,
                                       char process_ids[32][CUBICLE_ID_STRING_LENGTH])
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    desk_pane_layout_t loaded;
    pane_layout_init_loaded(&loaded);
    memset(process_ids, 0, 32 * CUBICLE_ID_STRING_LENGTH);

    char line[256];
    if (fgets(line, sizeof(line), file) == NULL ||
        strcmp(line, "desk-named-layout-v1\n") != 0) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    bool workspace_seen = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char value[CUBICLE_ID_STRING_LENGTH] = "";
        if (sscanf(line, "workspace %32s", value) == 1) {
            workspace_seen = true;
            continue;
        }
        int pane_id = 0;
        if (sscanf(line, "pane %d %32s", &pane_id, value) == 2) {
            if (pane_id <= 0 || pane_id > 32) {
                fclose(file);
                errno = EINVAL;
                return -1;
            }
            snprintf(process_ids[pane_id - 1], CUBICLE_ID_STRING_LENGTH,
                     "%s", value);
            continue;
        }
        if (pane_layout_parse_line(&loaded, line) < 0) {
            fclose(file);
            return -1;
        }
    }
    fclose(file);
    if (!workspace_seen) {
        errno = EINVAL;
        return -1;
    }
    return pane_layout_finish_loaded(layout, &loaded);
}

static cubicle_error_code_t connect_client(cubicle_client_t **client_out,
                                           cubicle_config_t *config_out,
                                           const char *manager_override,
                                           char *error,
                                           size_t error_size)
{
    cubicle_config_t config;
    char config_error[512];
    if (cubicle_config_load_client(&config, config_error,
                                   sizeof(config_error)) < 0) {
        snprintf(error, error_size, "configuration error: %s", config_error);
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    if (config.desk_debug_library) {
        char log_path[CUBICLE_PATH_MAX];
        int length = snprintf(log_path, sizeof(log_path),
                              "%s/client-library.log",
                              config.manager_log_dir);
        if (length > 0 && (size_t)length < sizeof(log_path)) {
            (void)cubicle_mkdir_p(config.manager_log_dir);
            (void)setenv("CUBICLE_LIBRARY_DEBUG", "library", 1);
            (void)setenv("CUBICLE_LIBRARY_DEBUG_PROGRAM", "desk", 1);
            (void)setenv("CUBICLE_LIBRARY_DEBUG_LOG", log_path, 1);
        }
    }
    g_desk_debug_terminal = config.desk_debug_terminal;
    g_desk_debug_log_path[0] = '\0';
    if (config.desk_debug_terminal) {
        int length = snprintf(g_desk_debug_log_path,
                              sizeof(g_desk_debug_log_path),
                              "%s/desk-terminal.log",
                              config.manager_log_dir);
        if (length > 0 && (size_t)length < sizeof(g_desk_debug_log_path)) {
            (void)cubicle_mkdir_p(config.manager_log_dir);
            desk_debug_log("event=config terminal_debug=enabled log=%s",
                           g_desk_debug_log_path);
        } else {
            g_desk_debug_log_path[0] = '\0';
        }
    }

    char configured_endpoint[CUBICLE_ENDPOINT_URI_MAX];
    const char *manager_uri =
        manager_override != NULL && manager_override[0] != '\0'
            ? manager_override
            : cubeui_resolve_manager_endpoint(NULL, &config,
                                              configured_endpoint,
                                              sizeof(configured_endpoint));
    const char *manager_environment = getenv("CUBICLE_MANAGER_SOCKET");
    int allow_automanager =
        manager_override == NULL &&
        config.desk_automanager &&
        (manager_environment == NULL || manager_environment[0] == '\0');
    if (cubeui_autostart_manager(manager_uri, &config, allow_automanager,
                                 error, error_size) < 0) {
        return CUBICLE_ERR_MANAGER_UNAVAILABLE;
    }

    cubicle_error_code_t code = cubicle_client_connect_uri(manager_uri, NULL,
                                                           client_out);
    if (code != CUBICLE_OK) {
        snprintf(error, error_size, "failed to connect to manager");
    } else if (config_out != NULL) {
        *config_out = config;
    }
    return code;
}

static cubicle_error_code_t DESK_UNUSED resolve_attachment_target(
    const char *process_name,
    desk_attachment_t *target,
    char *error,
    size_t error_size)
{
    memset(target, 0, sizeof(*target));
    if (cubeui_read_selected_workspace(target->workspace,
                                       sizeof(target->workspace)) < 0) {
        snprintf(error, error_size, "no workspace selected");
        return CUBICLE_ERR_NOT_FOUND;
    }

    cubicle_error_code_t code = connect_client(&target->manager, NULL, NULL,
                                               error, error_size);
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

static void DESK_UNUSED desk_attachment_cleanup(desk_attachment_t *target)
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

static bool pane_glyph_is_horizontal(const char *glyph)
{
    return strcmp(glyph, DESK_ACTIVE_HORIZONTAL) == 0 ||
           strcmp(glyph, DESK_INACTIVE_HORIZONTAL) == 0 ||
           strcmp(glyph, DESK_ACTIVE_MIDDLE_JUNCTION) == 0 ||
           strcmp(glyph, DESK_INACTIVE_MIDDLE_JUNCTION) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_DOWN) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_UP) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_RIGHT) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_LEFT) == 0 ||
           strcmp(glyph, DESK_INACTIVE_T_DOWN) == 0 ||
           strcmp(glyph, DESK_INACTIVE_T_UP) == 0 ||
           strcmp(glyph, DESK_INACTIVE_T_RIGHT) == 0 ||
           strcmp(glyph, DESK_INACTIVE_T_LEFT) == 0;
}

static bool pane_glyph_is_vertical(const char *glyph)
{
    return strcmp(glyph, DESK_ACTIVE_VERTICAL) == 0 ||
           strcmp(glyph, DESK_INACTIVE_VERTICAL) == 0 ||
           strcmp(glyph, DESK_ACTIVE_MIDDLE_JUNCTION) == 0 ||
           strcmp(glyph, DESK_INACTIVE_MIDDLE_JUNCTION) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_DOWN) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_UP) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_RIGHT) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_LEFT) == 0 ||
           strcmp(glyph, DESK_INACTIVE_T_DOWN) == 0 ||
           strcmp(glyph, DESK_INACTIVE_T_UP) == 0 ||
           strcmp(glyph, DESK_INACTIVE_T_RIGHT) == 0 ||
           strcmp(glyph, DESK_INACTIVE_T_LEFT) == 0;
}

static bool pane_glyph_is_active(const char *glyph)
{
    return strcmp(glyph, DESK_ACTIVE_HORIZONTAL) == 0 ||
           strcmp(glyph, DESK_ACTIVE_VERTICAL) == 0 ||
           strcmp(glyph, DESK_ACTIVE_MIDDLE_JUNCTION) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_DOWN) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_UP) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_RIGHT) == 0 ||
           strcmp(glyph, DESK_ACTIVE_T_LEFT) == 0;
}

static const char *pane_canvas_glyph_at(const char **canvas,
                                        const desk_terminal_t *terminal,
                                        int row, int col)
{
    if (row < 0 || row >= terminal->rows || col < 0 || col >= terminal->cols) {
        return " ";
    }
    return canvas[(size_t)row * (size_t)terminal->cols + (size_t)col];
}

static void pane_canvas_connect_dividers(const char **canvas,
                                         const desk_terminal_t *terminal)
{
    for (int row = 0; row < terminal->rows; ++row) {
        for (int col = 0; col < terminal->cols; ++col) {
            const char *current = pane_canvas_glyph_at(canvas, terminal, row,
                                                       col);
            bool has_horizontal = pane_glyph_is_horizontal(current);
            bool has_vertical = pane_glyph_is_vertical(current);
            bool active = pane_glyph_is_active(current);

            const char *left = pane_canvas_glyph_at(canvas, terminal, row,
                                                    col - 1);
            const char *right = pane_canvas_glyph_at(canvas, terminal, row,
                                                     col + 1);
            const char *above = pane_canvas_glyph_at(canvas, terminal, row - 1,
                                                     col);
            const char *below = pane_canvas_glyph_at(canvas, terminal, row + 1,
                                                     col);
            bool connects_left = pane_glyph_is_horizontal(left);
            bool connects_right = pane_glyph_is_horizontal(right);
            bool connects_above = pane_glyph_is_vertical(above);
            bool connects_below = pane_glyph_is_vertical(below);

            if (connects_left || connects_right) {
                has_horizontal = true;
                active = active || pane_glyph_is_active(left) ||
                         pane_glyph_is_active(right);
            }
            if (connects_above || connects_below) {
                has_vertical = true;
                active = active || pane_glyph_is_active(above) ||
                         pane_glyph_is_active(below);
            }
            if (has_horizontal && has_vertical) {
                bool connects_current_horizontal =
                    pane_glyph_is_horizontal(current);
                bool connects_current_vertical = pane_glyph_is_vertical(current);
                bool left_line = connects_left || connects_current_horizontal;
                bool right_line = connects_right || connects_current_horizontal;
                bool above_line = connects_above || connects_current_vertical;
                bool below_line = connects_below || connects_current_vertical;
                const char *connector = active ? DESK_ACTIVE_MIDDLE_JUNCTION
                                               : DESK_INACTIVE_MIDDLE_JUNCTION;
                if (!above_line) {
                    connector = active ? DESK_ACTIVE_T_DOWN
                                       : DESK_INACTIVE_T_DOWN;
                } else if (!below_line) {
                    connector = active ? DESK_ACTIVE_T_UP
                                       : DESK_INACTIVE_T_UP;
                } else if (!left_line) {
                    connector = active ? DESK_ACTIVE_T_RIGHT
                                       : DESK_INACTIVE_T_RIGHT;
                } else if (!right_line) {
                    connector = active ? DESK_ACTIVE_T_LEFT
                                       : DESK_INACTIVE_T_LEFT;
                }
                canvas[(size_t)row * (size_t)terminal->cols + (size_t)col] =
                    connector;
            }
        }
    }
}

static void pane_canvas_write_label(const char **canvas,
                                    const desk_terminal_t *terminal,
                                    desk_rect_t rect, const char *pane_label,
                                    bool active,
                                    bool resize_mode)
{
    if (rect.rows <= 0 || rect.cols <= 0) {
        return;
    }
    char label[32];
    int length = 0;
    if (active && resize_mode) {
        length = snprintf(label, sizeof(label), "<%s>", pane_label);
    } else if (active) {
        length = snprintf(label, sizeof(label), "[%s]", pane_label);
    } else {
        length = snprintf(label, sizeof(label), "%s", pane_label);
    }
    if (length < 0) {
        return;
    }
    int limit = length < rect.cols ? length : rect.cols;
    for (int i = 0; i < limit; ++i) {
        pane_canvas_put(canvas, terminal, rect.row, rect.col + i,
                        ascii_glyph(label[i]));
    }
}

static void pane_canvas_write_status(const char **canvas,
                                     const desk_terminal_t *terminal,
                                     const char *status)
{
    if (status[0] == '\0' || terminal->rows <= 0) {
        return;
    }
    int row = terminal->rows - 1;
    int col = 0;
    for (const char *cursor = status;
         *cursor != '\0' && col < terminal->cols; ++cursor, ++col) {
        pane_canvas_put(canvas, terminal, row, col, ascii_glyph(*cursor));
    }
}

static void pane_render_node(const desk_pane_layout_t *panes,
                             const desk_terminal_t *terminal,
                             const char **canvas, int node, desk_rect_t rect,
                             const desk_rect_t *active_rect)
{
    if (node < 0 || !panes->nodes[node].used || rect.rows <= 0 ||
        rect.cols <= 0) {
        return;
    }
    const desk_pane_node_t *entry = &panes->nodes[node];
    if (entry->split == DESK_SPLIT_NONE) {
        pane_canvas_write_label(canvas, terminal, rect, entry->label,
                                entry->pane_id == panes->active_pane_id,
                                panes->resize_mode);
        return;
    }

    int zoom_target = pane_layout_zoom_target(panes);
    bool first_has_active =
        pane_subtree_contains(panes, entry->first, zoom_target);
    bool second_has_active =
        pane_subtree_contains(panes, entry->second, zoom_target);
    bool zooms_this_split =
        panes->zoom == DESK_ZOOM_FULL ||
        (panes->zoom == DESK_ZOOM_HORIZONTAL &&
         entry->split == DESK_SPLIT_HORIZONTAL) ||
        (panes->zoom == DESK_ZOOM_VERTICAL &&
         entry->split == DESK_SPLIT_VERTICAL);
    if (zooms_this_split && (first_has_active || second_has_active)) {
        pane_render_node(panes, terminal, canvas,
                         first_has_active ? entry->first : entry->second,
                         rect, active_rect);
        return;
    }

    desk_rect_t first;
    desk_rect_t second;
    desk_rect_t divider;
    split_rect(rect, entry, &first, &second, &divider);
    pane_render_node(panes, terminal, canvas, entry->first, first,
                     active_rect);
    pane_render_node(panes, terminal, canvas, entry->second, second,
                     active_rect);

    for (int row = 0; row < divider.rows; ++row) {
        for (int col = 0; col < divider.cols; ++col) {
            int target_row = divider.row + row;
            int target_col = divider.col + col;
            bool active_divider = false;
            if (entry->split == DESK_SPLIT_HORIZONTAL) {
                active_divider =
                    (active_rect->col + active_rect->cols == target_col ||
                     active_rect->col == target_col + 1) &&
                    target_row >= active_rect->row &&
                    target_row < active_rect->row + active_rect->rows;
            } else {
                active_divider =
                    (active_rect->row + active_rect->rows == target_row ||
                     active_rect->row == target_row + 1) &&
                    target_col >= active_rect->col &&
                    target_col < active_rect->col + active_rect->cols;
            }
            const char *line = entry->split == DESK_SPLIT_HORIZONTAL
                                   ? (active_divider ? DESK_ACTIVE_VERTICAL
                                                     : DESK_INACTIVE_VERTICAL)
                                   : (active_divider
                                          ? DESK_ACTIVE_HORIZONTAL
                                          : DESK_INACTIVE_HORIZONTAL);
            const char *junction = active_divider
                                       ? DESK_ACTIVE_MIDDLE_JUNCTION
                                       : DESK_INACTIVE_MIDDLE_JUNCTION;
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
    desk_rect_t active_rect = root;
    (void)pane_layout_rect_for_pane(panes, terminal, panes->active_pane_id,
                                    &active_rect);
    pane_render_node(panes, terminal, canvas, panes->root, root,
                     &active_rect);
    pane_canvas_connect_dividers(canvas, terminal);
    pane_canvas_write_status(canvas, terminal, panes->status);
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
    (void)cubeui_write_all(STDOUT_FILENO, frame, used);
}

static int DESK_UNUSED desk_dump_layout(const desk_terminal_t *terminal,
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

static int DESK_UNUSED desk_prompt_layout_name(const desk_terminal_t *terminal,
                                               char *name,
                                               size_t name_size)
{
    char prompt[256];
    size_t used = 0;
    char cursor[32];
    int cursor_length = snprintf(cursor, sizeof(cursor), "\x1b[%d;1H",
                                 terminal->rows);
    if (cursor_length > 0 && (size_t)cursor_length < sizeof(cursor)) {
        append_text(prompt, sizeof(prompt), &used, cursor);
    }
    append_text(prompt, sizeof(prompt), &used, "\x1b[2KLayout name: ");
    (void)cubeui_write_all(STDOUT_FILENO, prompt, used);

    size_t length = 0;
    name[0] = '\0';
    while (!g_stop_requested) {
        unsigned char ch;
        ssize_t rc = read(STDIN_FILENO, &ch, 1);
        if (rc < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            name[length] = '\0';
            return length > 0 ? 0 : -1;
        }
        if (ch == 27 || ch == 3) {
            return -1;
        }
        if (ch == 127 || ch == '\b') {
            if (length > 0) {
                length--;
                name[length] = '\0';
                (void)cubeui_write_all(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        if ((isalnum(ch) || ch == '-' || ch == '_' || ch == '.') &&
            length + 1 < name_size) {
            name[length++] = (char)ch;
            name[length] = '\0';
            char out[2] = {(char)ch, '\0'};
            (void)cubeui_write_all(STDOUT_FILENO, out, 1);
        }
    }
    return -1;
}

static void DESK_UNUSED desk_render_cube_one(const desk_terminal_t *terminal,
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
        int terminal_row = rect.row + row + 2;

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

    (void)cubeui_write_all(STDOUT_FILENO, frame, used);
}

static void grid_clear(desk_grid_t *grid)
{
    if (grid->cells != NULL) {
        size_t count = (size_t)grid->rows * (size_t)grid->cols;
        for (size_t i = 0; i < count; ++i) {
            snprintf(grid->cells[i].text, sizeof(grid->cells[i].text), " ");
            grid->cells[i].sgr[0] = '\0';
        }
    }
    if (grid->dirty_rows != NULL) {
        for (int row = 0; row < grid->rows; ++row) {
            grid->dirty_rows[row] = true;
        }
    }
    grid->cursor_row = 0;
    grid->cursor_col = 0;
    grid->cursor_visible = true;
    grid->scroll_top = 0;
    grid->scroll_bottom = grid->rows > 0 ? grid->rows - 1 : 0;
    grid->current_sgr[0] = '\0';
    grid->escape_active = false;
    grid->csi_active = false;
    grid->csi_length = 0;
    grid->utf8_length = 0;
    grid->utf8_expected = 0;
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
    bool *dirty_rows = malloc((size_t)rows * sizeof(*dirty_rows));
    if (cells == NULL || dirty_rows == NULL) {
        free(cells);
        free(dirty_rows);
        return -1;
    }
    free(grid->cells);
    free(grid->dirty_rows);
    memset(grid, 0, sizeof(*grid));
    grid->cells = cells;
    grid->dirty_rows = dirty_rows;
    grid->rows = rows;
    grid->cols = cols;
    grid_clear(grid);
    return 0;
}

static void grid_cleanup(desk_grid_t *grid)
{
    free(grid->cells);
    free(grid->dirty_rows);
    memset(grid, 0, sizeof(*grid));
}

static void desk_scrollback_clear(desk_pane_t *pane)
{
    if (pane == NULL) {
        return;
    }
    for (size_t i = 0; i < pane->scrollback_count; ++i) {
        size_t index = (pane->scrollback_head + i) %
                       pane->scrollback_capacity;
        free(pane->scrollback[index].cells);
        pane->scrollback[index].cells = NULL;
        pane->scrollback[index].cols = 0;
    }
    free(pane->scrollback);
    pane->scrollback = NULL;
    pane->scrollback_head = 0;
    pane->scrollback_count = 0;
    pane->scrollback_capacity = 0;
    pane->scrollback_offset = 0;
}

static int desk_scrollback_set_limit(desk_pane_t *pane, size_t limit)
{
    if (pane == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (pane->scrollback_capacity == limit) {
        pane->scrollback_limit = limit;
        return 0;
    }
    desk_scrollback_clear(pane);
    pane->scrollback_limit = limit;
    if (limit == 0) {
        return 0;
    }
    pane->scrollback = calloc(limit, sizeof(*pane->scrollback));
    if (pane->scrollback == NULL) {
        pane->scrollback_limit = 0;
        return -1;
    }
    pane->scrollback_capacity = limit;
    return 0;
}

static int desk_scrollback_append(desk_pane_t *pane,
                                  const cubicle_terminal_scrollback_line_t *line)
{
    if (pane == NULL || line == NULL || line->cells == NULL ||
        line->cols == 0 || pane->scrollback_capacity == 0) {
        return 0;
    }
    desk_cell_t *cells = calloc(line->cols, sizeof(*cells));
    if (cells == NULL) {
        return -1;
    }
    for (unsigned int col = 0; col < line->cols; ++col) {
        snprintf(cells[col].text, sizeof(cells[col].text), "%s",
                 line->cells[col].text[0] == '\0' ? " "
                                                  : line->cells[col].text);
        snprintf(cells[col].sgr, sizeof(cells[col].sgr), "%s",
                 line->cells[col].sgr);
    }

    size_t slot = 0;
    if (pane->scrollback_count == pane->scrollback_capacity) {
        slot = pane->scrollback_head;
        free(pane->scrollback[slot].cells);
        pane->scrollback_head =
            (pane->scrollback_head + 1) % pane->scrollback_capacity;
        if (pane->scrollback_offset > pane->scrollback_count) {
            pane->scrollback_offset = pane->scrollback_count;
        }
    } else {
        slot = (pane->scrollback_head + pane->scrollback_count) %
               pane->scrollback_capacity;
        pane->scrollback_count++;
    }
    pane->scrollback[slot].cols = (int)line->cols;
    pane->scrollback[slot].cells = cells;
    return 0;
}

static int desk_drain_model_scrollback(desk_pane_t *pane)
{
    if (pane == NULL || pane->terminal_model == NULL ||
        pane->scrollback_limit == 0) {
        return 0;
    }
    cubicle_terminal_scrollback_line_t *lines = NULL;
    size_t line_count = 0;
    if (cubicle_terminal_model_take_scrollback(pane->terminal_model, &lines,
                                               &line_count) < 0) {
        return -1;
    }
    for (size_t i = 0; i < line_count; ++i) {
        if (desk_scrollback_append(pane, &lines[i]) < 0) {
            cubicle_terminal_scrollback_cleanup(lines, line_count);
            return -1;
        }
    }
    cubicle_terminal_scrollback_cleanup(lines, line_count);
    return 0;
}

static bool desk_pane_return_to_live(desk_pane_t *pane)
{
    if (pane == NULL || pane->scrollback_offset == 0) {
        return false;
    }
    pane->scrollback_offset = 0;
    return true;
}

static bool desk_scroll_active_pane(desk_session_t *session,
                                    const desk_terminal_t *terminal,
                                    const char *command)
{
    desk_pane_t *pane = desk_pane_for_id(session,
                                         session->layout.active_pane_id);
    if (pane == NULL) {
        return false;
    }
    size_t max_offset = pane->scrollback_count;
    size_t page = pane->grid.rows > 1 ? (size_t)(pane->grid.rows - 1) : 1;
    size_t before = pane->scrollback_offset;
    if (strcmp(command, "scroll.page_up") == 0) {
        pane->scrollback_offset =
            pane->scrollback_offset + page > max_offset
                ? max_offset
                : pane->scrollback_offset + page;
    } else if (strcmp(command, "scroll.page_down") == 0) {
        pane->scrollback_offset =
            pane->scrollback_offset > page ? pane->scrollback_offset - page
                                           : 0;
    } else if (strcmp(command, "scroll.line_up") == 0) {
        if (pane->scrollback_offset < max_offset) {
            pane->scrollback_offset++;
        }
    } else if (strcmp(command, "scroll.line_down") == 0) {
        if (pane->scrollback_offset > 0) {
            pane->scrollback_offset--;
        }
    } else if (strcmp(command, "scroll.top") == 0) {
        pane->scrollback_offset = max_offset;
    } else if (strcmp(command, "scroll.bottom") == 0) {
        pane->scrollback_offset = 0;
    } else {
        return false;
    }
    (void)terminal;
    return pane->scrollback_offset != before;
}

static void grid_apply_snapshot(desk_grid_t *grid,
                                const cubicle_terminal_snapshot_t *snapshot)
{
    if (grid->cells == NULL || snapshot->cells == NULL) {
        return;
    }

    for (int row = 0; row < grid->rows; ++row) {
        for (int col = 0; col < grid->cols; ++col) {
            const char *source_text = " ";
            const char *source_sgr = "";
            if (row < (int)snapshot->rows && col < (int)snapshot->cols) {
                const cubicle_terminal_cell_t *source =
                    &snapshot->cells[(size_t)row * (size_t)snapshot->cols +
                                     (size_t)col];
                source_text = source->text[0] == '\0' ? " " : source->text;
                source_sgr = source->sgr;
            }
            desk_cell_t *target =
                &grid->cells[(size_t)row * (size_t)grid->cols + (size_t)col];
            if (strcmp(target->text, source_text) != 0 ||
                strcmp(target->sgr, source_sgr) != 0) {
                snprintf(target->text, sizeof(target->text), "%s",
                         source_text);
                snprintf(target->sgr, sizeof(target->sgr), "%s", source_sgr);
                if (grid->dirty_rows != NULL) {
                    grid->dirty_rows[row] = true;
                }
            }
        }
    }
    if (snapshot->cursor_row < (unsigned int)grid->rows) {
        grid->cursor_row = (int)snapshot->cursor_row;
    }
    if (snapshot->cursor_col < (unsigned int)grid->cols) {
        grid->cursor_col = (int)snapshot->cursor_col;
    }
    grid->cursor_visible = snapshot->cursor_visible;
}

static int refresh_pane_from_model(desk_pane_t *pane)
{
    if (pane->terminal_model == NULL) {
        return 0;
    }
    bool dirty_rows[1000];
    if (cubicle_terminal_model_get_dirty_rows(
            pane->terminal_model, dirty_rows,
            sizeof(dirty_rows) / sizeof(dirty_rows[0])) < 0) {
        return -1;
    }
    for (int row = 0; row < pane->grid.rows; ++row) {
        if (dirty_rows[row] && pane->grid.dirty_rows != NULL) {
            pane->grid.dirty_rows[row] = true;
        }
    }

    cubicle_terminal_snapshot_t snapshot;
    if (cubicle_terminal_model_snapshot(pane->terminal_model, 0,
                                        &snapshot) < 0) {
        return -1;
    }
    grid_apply_snapshot(&pane->grid, &snapshot);
    cubicle_terminal_snapshot_cleanup(&snapshot);
    cubicle_terminal_model_clear_dirty_rows(pane->terminal_model);
    return 0;
}

static int reload_pane_snapshot(desk_pane_t *pane)
{
    uint64_t before_offset =
        cubicle_attachment_read_offset(pane->attachment, CUBICLE_STREAM_TTY);
    desk_debug_log("event=snapshot_reload_start process=%s id=%s",
                   pane->process.friendly_name, pane->process.id);
    cubicle_terminal_snapshot_t snapshot;
    cubicle_error_code_t code = cubicle_attachment_snapshot(pane->attachment,
                                                            &snapshot);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last =
            cubicle_attachment_last_error(pane->attachment);
        desk_debug_log("event=snapshot_reload_failed process=%s code=%d message=\"%s\"",
                       pane->process.friendly_name, (int)code,
                       last != NULL ? last->message : "");
        return -1;
    }

    if (pane->terminal_model == NULL) {
        if (cubicle_terminal_model_create(snapshot.rows, snapshot.cols,
                                          &pane->terminal_model) < 0) {
            desk_debug_log("event=snapshot_model_create_failed process=%s rows=%u cols=%u errno=%d",
                           pane->process.friendly_name, snapshot.rows,
                           snapshot.cols, errno);
            cubicle_terminal_snapshot_cleanup(&snapshot);
            return -1;
        }
    }
    if (cubicle_terminal_model_load_snapshot(pane->terminal_model,
                                             &snapshot) < 0 ||
        cubicle_terminal_model_set_scrollback_capture_limit(
            pane->terminal_model, pane->scrollback_limit) < 0 ||
        grid_resize(&pane->grid, (int)snapshot.rows,
                    (int)snapshot.cols) < 0) {
        desk_debug_log("event=snapshot_apply_failed process=%s rows=%u cols=%u offset=%llu errno=%d",
                       pane->process.friendly_name, snapshot.rows,
                       snapshot.cols,
                       (unsigned long long)snapshot.offset, errno);
        cubicle_terminal_snapshot_cleanup(&snapshot);
        return -1;
    }
    grid_apply_snapshot(&pane->grid, &snapshot);
    cubicle_terminal_scrollback_line_t *discarded = NULL;
    size_t discarded_count = 0;
    if (cubicle_terminal_model_take_scrollback(pane->terminal_model,
                                               &discarded,
                                               &discarded_count) == 0) {
        cubicle_terminal_scrollback_cleanup(discarded, discarded_count);
    }
    cubicle_terminal_model_clear_dirty_rows(pane->terminal_model);
    uint64_t after_offset =
        cubicle_attachment_read_offset(pane->attachment, CUBICLE_STREAM_TTY);
    desk_debug_log("event=snapshot_reload_ok process=%s rows=%u cols=%u offset=%llu read_offset_before=%llu read_offset_after=%llu cursor=%u,%u visible=%s",
                   pane->process.friendly_name, snapshot.rows, snapshot.cols,
                   (unsigned long long)snapshot.offset,
                   (unsigned long long)before_offset,
                   (unsigned long long)after_offset, snapshot.cursor_row,
                   snapshot.cursor_col,
                   snapshot.cursor_visible ? "true" : "false");
    cubicle_terminal_snapshot_cleanup(&snapshot);
    return 0;
}


static long long desk_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0;
    }
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static void desk_cursor_reset_blink(desk_session_t *session)
{
    session->cursor_blink_visible = true;
    session->cursor_next_blink_ms =
        desk_monotonic_ms() + DESK_CURSOR_BLINK_MS;
}

static desk_pane_t *desk_pane_for_id(desk_session_t *session, int pane_id)
{
    if (pane_id <= 0 || (size_t)pane_id > session->pane_count) {
        return NULL;
    }
    return &session->panes[(size_t)pane_id - 1];
}

static bool desk_cursor_target(const desk_terminal_t *terminal,
                               desk_session_t *session,
                               int *pane_id,
                               int *row,
                               int *col)
{
    int active = session->layout.active_pane_id;
    desk_pane_t *pane = desk_pane_for_id(session, active);
    desk_rect_t rect;
    if (pane == NULL || pane->scrollback_offset > 0 ||
        !pane->grid.cursor_visible ||
        !pane_content_rect_for_pane(&session->layout, terminal, active,
                                    &rect)) {
        return false;
    }
    if (pane->grid.cursor_row < 0 || pane->grid.cursor_col < 0 ||
        pane->grid.cursor_row >= pane->grid.rows ||
        pane->grid.cursor_col >= pane->grid.cols ||
        pane->grid.cursor_row >= rect.rows ||
        pane->grid.cursor_col >= rect.cols) {
        return false;
    }
    *pane_id = active;
    *row = pane->grid.cursor_row;
    *col = pane->grid.cursor_col;
    return true;
}

static void desk_render_grid_cell(const desk_terminal_t *terminal,
                                  const desk_pane_layout_t *panes,
                                  int pane_id,
                                  const desk_grid_t *grid,
                                  int row,
                                  int col,
                                  bool reverse)
{
    desk_rect_t rect;
    if (!pane_content_rect_for_pane(panes, terminal, pane_id, &rect) ||
        row < 0 || col < 0 || row >= grid->rows || col >= grid->cols ||
        row >= rect.rows || col >= rect.cols) {
        return;
    }

    const desk_cell_t *cell =
        &grid->cells[(size_t)row * (size_t)grid->cols + (size_t)col];
    const char *text = cell->text[0] == '\0' ? " " : cell->text;
    const char *sgr = cell->sgr[0] == '\0' ? "\x1b[0m" : cell->sgr;
    char frame[256];
    int length = snprintf(frame, sizeof(frame), "\x1b[%d;%dH%s%s%s\x1b[0m",
                          rect.row + row + 1, rect.col + col + 1, sgr,
                          reverse ? "\x1b[7m" : "", text);
    if (length > 0 && (size_t)length < sizeof(frame)) {
        (void)cubeui_write_all(STDOUT_FILENO, frame, (size_t)length);
    }
}

static void desk_cursor_erase(const desk_terminal_t *terminal,
                              desk_session_t *session)
{
    if (!session->cursor_drawn) {
        return;
    }
    desk_pane_t *pane = desk_pane_for_id(session, session->cursor_pane_id);
    if (pane != NULL) {
        desk_render_grid_cell(terminal, &session->layout,
                              session->cursor_pane_id, &pane->grid,
                              session->cursor_row, session->cursor_col, false);
    }
    session->cursor_drawn = false;
}

static void desk_cursor_render(const desk_terminal_t *terminal,
                               desk_session_t *session)
{
    int pane_id = 0;
    int row = 0;
    int col = 0;
    bool has_target =
        desk_cursor_target(terminal, session, &pane_id, &row, &col);
    bool should_draw = has_target && session->cursor_blink_visible;

    if (session->cursor_drawn &&
        (!should_draw || session->cursor_pane_id != pane_id ||
         session->cursor_row != row || session->cursor_col != col)) {
        desk_cursor_erase(terminal, session);
    }
    if (!should_draw || session->cursor_drawn) {
        return;
    }

    desk_pane_t *pane = desk_pane_for_id(session, pane_id);
    if (pane == NULL) {
        return;
    }
    desk_render_grid_cell(terminal, &session->layout, pane_id, &pane->grid,
                          row, col, true);
    session->cursor_drawn = true;
    session->cursor_pane_id = pane_id;
    session->cursor_row = row;
    session->cursor_col = col;
}

static void desk_cursor_tick(const desk_terminal_t *terminal,
                             desk_session_t *session)
{
    long long now = desk_monotonic_ms();
    if (session->cursor_next_blink_ms == 0) {
        desk_cursor_reset_blink(session);
    }
    if (now < session->cursor_next_blink_ms) {
        desk_cursor_render(terminal, session);
        return;
    }
    session->cursor_blink_visible = !session->cursor_blink_visible;
    session->cursor_next_blink_ms = now + DESK_CURSOR_BLINK_MS;
    desk_cursor_render(terminal, session);
}

static void desk_render_pane_title(const desk_terminal_t *terminal,
                                   const desk_pane_layout_t *panes,
                                   int pane_id,
                                   const char *title,
                                   bool mouse_titles)
{
    desk_rect_t rect;
    if (!pane_layout_rect_for_pane(panes, terminal, pane_id, &rect) ||
        rect.rows <= 0 || rect.cols <= 0) {
        return;
    }

    const char *label = title != NULL && title[0] != '\0' ? title : "untitled";
    bool active = pane_id == panes->active_pane_id;
    char frame[2048];
    size_t used = 0;
    char cursor[32];
    int cursor_length = snprintf(cursor, sizeof(cursor), "\x1b[%d;%dH",
                                 rect.row + 1, rect.col + 1);
    if (cursor_length <= 0 || (size_t)cursor_length >= sizeof(cursor)) {
        return;
    }

    append_text(frame, sizeof(frame), &used, cursor);
    append_text(frame, sizeof(frame), &used, "\x1b[0m");
    for (int col = 0; col < rect.cols; ++col) {
        append_text(frame, sizeof(frame), &used, " ");
    }

    append_text(frame, sizeof(frame), &used, cursor);
    append_text(frame, sizeof(frame), &used,
                active ? "\x1b[1;7m" : "\x1b[2m");
    if (rect.cols > 2) {
        append_text(frame, sizeof(frame), &used, " ");
    }
    int remaining = rect.cols > 2 ? rect.cols - 2 : rect.cols;
    if (!active && mouse_titles && remaining > 0) {
        append_text(frame, sizeof(frame), &used, "[");
        --remaining;
    }
    for (const char *cursor_label = label;
         *cursor_label != '\0' && remaining > 0;
         ++cursor_label, --remaining) {
        unsigned char byte = (unsigned char)*cursor_label;
        char out[2] = {
            (char)(byte >= 0x20 && byte != 0x7f ? byte : '?'),
            '\0',
        };
        append_text(frame, sizeof(frame), &used, out);
    }
    if (!active && mouse_titles && remaining > 0) {
        append_text(frame, sizeof(frame), &used, "]");
        --remaining;
    }
    if (rect.cols > 2) {
        append_text(frame, sizeof(frame), &used, " ");
    }
    append_text(frame, sizeof(frame), &used, "\x1b[0m");
    (void)cubeui_write_all(STDOUT_FILENO, frame, used);
}

static void desk_render_cube_grid_rows(const desk_terminal_t *terminal,
                                       const desk_pane_layout_t *panes,
                                       int pane_id,
                                       desk_pane_t *pane,
                                       bool mouse_titles,
                                       bool dirty_only)
{
    char frame[65536];
    size_t used = 0;
    desk_rect_t rect;
    desk_grid_t *grid = &pane->grid;
    bool scrollback_view = pane->scrollback_offset > 0;

    if (!pane_content_rect_for_pane(panes, terminal, pane_id, &rect)) {
        return;
    }

    if (scrollback_view) {
        dirty_only = false;
    }

    size_t total_rows = pane->scrollback_count +
                        (grid->rows > 0 ? (size_t)grid->rows : 0);
    size_t start_row = 0;
    if (scrollback_view && total_rows > (size_t)grid->rows) {
        size_t live_start = total_rows - (size_t)grid->rows;
        start_row = pane->scrollback_offset > live_start
                        ? 0
                        : live_start - pane->scrollback_offset;
    }

    for (int row = 0; row < grid->rows; ++row) {
        if (dirty_only && grid->dirty_rows != NULL &&
            !grid->dirty_rows[row]) {
            continue;
        }
        char cursor[32];
        char active_sgr[96] = "";
        int terminal_row = rect.row + row + 1;
        int cursor_length = snprintf(cursor, sizeof(cursor), "\x1b[%d;%dH",
                                     terminal_row, rect.col + 1);
        if (cursor_length > 0 && (size_t)cursor_length < sizeof(cursor)) {
            append_text(frame, sizeof(frame), &used, cursor);
        }
        for (int col = 0; col < rect.cols; ++col) {
            const char *text = " ";
            const char *sgr = "";
            if (scrollback_view) {
                size_t virtual_row = start_row + (size_t)row;
                if (virtual_row < pane->scrollback_count) {
                    size_t index = (pane->scrollback_head + virtual_row) %
                                   pane->scrollback_capacity;
                    const desk_scrollback_line_t *line =
                        &pane->scrollback[index];
                    if (col < line->cols) {
                        const desk_cell_t *cell = &line->cells[col];
                        text = cell->text;
                        sgr = cell->sgr;
                    }
                } else {
                    size_t grid_row = virtual_row - pane->scrollback_count;
                    if (grid_row < (size_t)grid->rows && col < grid->cols) {
                        const desk_cell_t *cell =
                            &grid->cells[grid_row * (size_t)grid->cols +
                                         (size_t)col];
                        text = cell->text;
                        sgr = cell->sgr;
                    }
                }
            } else if (row < grid->rows && col < grid->cols) {
                const desk_cell_t *cell =
                    &grid->cells[(size_t)row * (size_t)grid->cols +
                                 (size_t)col];
                text = cell->text;
                sgr = cell->sgr;
            }
            if (strcmp(active_sgr, sgr) != 0) {
                append_text(frame, sizeof(frame), &used,
                            sgr[0] == '\0' ? "\x1b[0m" : sgr);
                snprintf(active_sgr, sizeof(active_sgr), "%s", sgr);
            }
            append_text(frame, sizeof(frame), &used,
                        text[0] == '\0' ? " " : text);
        }
        if (active_sgr[0] != '\0') {
            append_text(frame, sizeof(frame), &used, "\x1b[0m");
        }
        if (grid->dirty_rows != NULL) {
            grid->dirty_rows[row] = false;
        }
    }

    append_text(frame, sizeof(frame), &used, "\x1b[0m");
    (void)cubeui_write_all(STDOUT_FILENO, frame, used);
    char title[160];
    snprintf(title, sizeof(title), "%.96s%s%zu/%zu",
             pane->process.friendly_name,
             scrollback_view ? " scroll " : "",
             scrollback_view ? pane->scrollback_offset : 0,
             scrollback_view ? pane->scrollback_count : 0);
    desk_render_pane_title(terminal, panes, pane_id,
                           scrollback_view ? title
                                           : pane->process.friendly_name,
                           mouse_titles);
}

static void desk_render_cube_grid(const desk_terminal_t *terminal,
                                  const desk_pane_layout_t *panes,
                                  int pane_id,
                                  desk_pane_t *pane,
                                  bool mouse_titles)
{
    desk_render_cube_grid_rows(terminal, panes, pane_id, pane, mouse_titles,
                               false);
}

static void desk_render_dirty_cube_grid(const desk_terminal_t *terminal,
                                        const desk_pane_layout_t *panes,
                                        int pane_id,
                                        desk_pane_t *pane,
                                        bool mouse_titles)
{
    desk_render_cube_grid_rows(terminal, panes, pane_id, pane, mouse_titles,
                               true);
}

static int pane_content_size(const desk_terminal_t *terminal,
                             const desk_pane_layout_t *panes,
                             int pane_id,
                             unsigned int *rows,
                             unsigned int *cols)
{
    desk_rect_t rect;
    if (!pane_layout_rect_for_pane(panes, terminal, pane_id, &rect) ||
        rect.rows <= 1 || rect.cols <= 0) {
        return -1;
    }
    *rows = (unsigned int)(rect.rows - 1);
    *cols = (unsigned int)rect.cols;
    return 0;
}

static bool parse_resize_arrow(const unsigned char *input,
                               size_t length,
                               size_t offset,
                               desk_resize_side_t *side,
                               int *delta,
                               size_t *consumed)
{
    if (offset + 2 >= length || input[offset] != 0x1b ||
        input[offset + 1] != '[') {
        return false;
    }

    size_t final = offset + 2;
    while (final < length &&
           !(input[final] == 'A' || input[final] == 'B' ||
             input[final] == 'C' || input[final] == 'D')) {
        final++;
    }
    if (final >= length) {
        return false;
    }

    bool shifted = false;
    for (size_t i = offset + 2; i + 1 < final; ++i) {
        if (input[i] == ';' && input[i + 1] == '2') {
            shifted = true;
            break;
        }
    }

    switch (input[final]) {
    case 'A':
        *side = shifted ? DESK_RESIZE_BOTTOM : DESK_RESIZE_TOP;
        *delta = -1;
        break;
    case 'B':
        *side = shifted ? DESK_RESIZE_BOTTOM : DESK_RESIZE_TOP;
        *delta = 1;
        break;
    case 'C':
        *side = shifted ? DESK_RESIZE_RIGHT : DESK_RESIZE_LEFT;
        *delta = 1;
        break;
    case 'D':
        *side = shifted ? DESK_RESIZE_RIGHT : DESK_RESIZE_LEFT;
        *delta = -1;
        break;
    default:
        return false;
    }
    *consumed = final - offset + 1;
    return true;
}

static void desk_apply_pane_labels(desk_session_t *session)
{
    for (size_t i = 0; i < session->pane_count; ++i) {
        int node = pane_find_leaf_node(&session->layout, session->layout.root,
                                       (int)i + 1);
        if (node < 0) {
            continue;
        }
        const char *name = session->panes[i].process.friendly_name[0] != '\0'
                               ? session->panes[i].process.friendly_name
                               : session->panes[i].process.id;
        snprintf(session->layout.nodes[node].label,
                 sizeof(session->layout.nodes[node].label), "%.*s",
                 (int)sizeof(session->layout.nodes[node].label) - 1, name);
    }
}

static int desk_save_layout(desk_session_t *session)
{
    if (session->layout_path[0] == '\0') {
        return 0;
    }
    desk_zoom_t saved_zoom = session->layout.zoom;
    int saved_zoom_pane_id = session->layout.zoom_pane_id;
    session->layout.zoom = DESK_ZOOM_NONE;
    session->layout.zoom_pane_id = 0;
    int result = pane_layout_save(&session->layout, session->layout_path);
    session->layout.zoom = saved_zoom;
    session->layout.zoom_pane_id = saved_zoom_pane_id;
    return result;
}

static bool process_is_attachable(const cubicle_process_info_t *process)
{
    return process->state == CUBICLE_PROCESS_RUNNING &&
           (process->mode == CUBICLE_PROCESS_TTY ||
            process->mode == CUBICLE_PROCESS_TTY_CAPTURED_STDERR);
}

static bool process_belongs_to_workspace(const cubicle_process_info_t *process,
                                         const char *workspace_id)
{
    return workspace_id != NULL && workspace_id[0] != '\0' &&
           strcmp(process->workspace_id, workspace_id) == 0;
}

static int resolve_workspace(desk_session_t *session,
                             const char *workspace_arg,
                             char *error,
                             size_t error_size)
{
    char workspace_name[CUBICLE_NAME_MAX];
    bool from_selected = false;
    if (workspace_arg != NULL && workspace_arg[0] != '\0') {
        snprintf(workspace_name, sizeof(workspace_name), "%s", workspace_arg);
    } else {
        if (cubeui_read_selected_workspace(workspace_name,
                                           sizeof(workspace_name)) < 0) {
            snprintf(error, error_size, "no workspace selected");
            return 1;
        }
        from_selected = true;
    }

    cubicle_error_code_t code = cubicle_workspace_get(
        session->manager, workspace_name, &session->workspace);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        if (code == CUBICLE_ERR_NOT_FOUND && from_selected) {
            cubeui_clear_selected_workspace_if_matches(workspace_name);
            snprintf(error, error_size,
                     "selected workspace '%s' was not found by the manager\n"
                     "hint: cleared the stale selection; run `cube workspace list` and then `cube workspace NAME` to select an existing workspace",
                     workspace_name);
            return 1;
        }
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "workspace lookup failed");
        return 2;
    }
    return 0;
}

static int load_workspace_processes(desk_session_t *session,
                                    char *error,
                                    size_t error_size)
{
    cubicle_process_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.workspace_id = session->workspace.id;

    cubicle_process_info_t *processes = NULL;
    size_t process_count = 0;
    cubicle_page_info_t page;
    memset(&page, 0, sizeof(page));
    cubicle_error_code_t code = cubicle_process_list(
        session->manager, &filter, &processes, &process_count, &page);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "process list failed");
        return 2;
    }

    for (size_t i = 0; i < process_count; ++i) {
        if (!process_is_attachable(&processes[i])) {
            continue;
        }
        if (session->pane_count >= sizeof(session->panes) /
                                     sizeof(session->panes[0])) {
            break;
        }
        session->panes[session->pane_count].process = processes[i];
        session->pane_count++;
    }
    cubicle_process_list_free(processes);

    if (session->pane_count == 0) {
        snprintf(error, error_size,
                 "workspace '%s' has no running TTY cubes",
                 session->workspace.name);
        return 1;
    }
    return 0;
}

static int load_or_create_layout(desk_session_t *session,
                                 char *error,
                                 size_t error_size)
{
    if (desk_layout_path_for_workspace(session->workspace.id,
                                       session->layout_path) < 0) {
        snprintf(error, error_size, "failed to resolve desk layout path: %s",
                 strerror(errno));
        return 2;
    }

    int load_result = pane_layout_load_file(&session->layout,
                                            session->layout_path);
    if (load_result == 0) {
        if (pane_layout_leaf_count(&session->layout) == session->pane_count) {
            session->layout.zoom = DESK_ZOOM_NONE;
            session->layout.zoom_pane_id = 0;
            session->layout.resize_mode = false;
            desk_apply_pane_labels(session);
            return 0;
        }
        errno = EINVAL;
    }
    if (errno != ENOENT && errno != EINVAL) {
        snprintf(error, error_size, "failed to load desk layout: %s",
                 strerror(errno));
        return 2;
    }
    if (pane_layout_auto(&session->layout, session->pane_count) < 0) {
        snprintf(error, error_size, "failed to create desk layout");
        return 2;
    }
    desk_apply_pane_labels(session);
    (void)desk_save_layout(session);
    return 0;
}

static int resize_pane_attachment(desk_session_t *session,
                                  const desk_terminal_t *terminal,
                                  size_t pane_index,
                                  bool *changed)
{
    desk_pane_t *pane = &session->panes[pane_index];
    unsigned int rows = 0;
    unsigned int cols = 0;
    desk_rect_t rect;
    if (!pane_layout_rect_for_pane(&session->layout, terminal,
                                   (int)pane_index + 1, &rect)) {
        return 0;
    }
    if (rect.rows <= DESK_PANE_TITLE_ROWS || rect.cols <= 0) {
        return -1;
    }
    rows = (unsigned int)(rect.rows - DESK_PANE_TITLE_ROWS);
    cols = (unsigned int)rect.cols;
    bool size_changed = pane->rows != rows || pane->cols != cols;
    if (pane->attachment != NULL) {
        bool sent = false;
        cubicle_error_code_t code = cubicle_attachment_resize_tracked(
            pane->attachment, &pane->resize, rows, cols, size_changed, &sent);
        if (code != CUBICLE_OK) {
            return -1;
        }
        if ((sent || size_changed) && reload_pane_snapshot(pane) < 0) {
            return -1;
        }
        size_changed = size_changed || sent;
    } else {
        if (grid_resize(&pane->grid, (int)rows, (int)cols) < 0 ||
            (pane->terminal_model != NULL &&
             cubicle_terminal_model_resize(pane->terminal_model, rows, cols) < 0)) {
            return -1;
        }
    }
    pane->rows = rows;
    pane->cols = cols;
    if (changed != NULL && size_changed) {
        *changed = true;
    }
    return 0;
}

static int resize_all_panes(desk_session_t *session,
                            const desk_terminal_t *terminal,
                            bool *changed)
{
    for (size_t i = 0; i < session->pane_count; ++i) {
        if (resize_pane_attachment(session, terminal, i, changed) < 0) {
            return -1;
        }
    }
    return 0;
}

static int refresh_pane_sizes(desk_session_t *session,
                              desk_terminal_t *terminal,
                              bool query_terminal,
                              bool *changed)
{
    if (query_terminal && cubeui_terminal_query_size(terminal) < 0) {
        return -1;
    }
    return resize_all_panes(session, terminal, changed);
}

static int flush_pending_terminal_resize(desk_session_t *session,
                                         desk_terminal_t *terminal,
                                         bool *layout_changed)
{
    if (!session->terminal_size_dirty) {
        return 0;
    }

    bool sizes_changed = false;
    if (refresh_pane_sizes(session, terminal, true, &sizes_changed) < 0) {
        return -1;
    }
    session->terminal_size_dirty = false;
    if (sizes_changed && layout_changed != NULL) {
        *layout_changed = true;
    }
    return 0;
}

static cubicle_error_code_t attach_pane(desk_session_t *session,
                                        size_t pane_index,
                                        char *error,
                                        size_t error_size)
{
    desk_pane_t *pane = &session->panes[pane_index];
    if (desk_scrollback_set_limit(pane, session->scrollback_limit) < 0) {
        snprintf(error, error_size, "failed to initialize pane scrollback");
        return CUBICLE_ERR_INTERNAL;
    }
    desk_debug_log("event=attach_start pane=%zu process=%s id=%s rows=%u cols=%u",
                   pane_index + 1, pane->process.friendly_name,
                   pane->process.id, pane->rows, pane->cols);
    cubicle_attachment_request_t request;
    memset(&request, 0, sizeof(request));
    request.process_id = pane->process.id;
    request.channels = CUBICLE_CHANNEL_TTY | CUBICLE_CHANNEL_STDOUT |
                       CUBICLE_CHANNEL_STDIN;
    request.mode = CUBICLE_ATTACHMENT_INTERACTIVE;
    request.rows = pane->rows;
    request.cols = pane->cols;

    cubicle_attachment_grant_t grant;
    cubicle_error_code_t code = cubicle_attachment_request(
        session->manager, &request, &grant);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        desk_debug_log("event=attach_request_failed pane=%zu process=%s code=%d message=\"%s\"",
                       pane_index + 1, pane->process.friendly_name,
                       (int)code, last != NULL ? last->message : "");
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "attachment request failed");
        return code;
    }

    cubicle_attachment_options_t options;
    memset(&options, 0, sizeof(options));
    code = session->manager_relay_attachments
               ? cubicle_attachment_connect_relay(session->manager, &grant,
                                                  &options, &pane->attachment)
               : cubicle_attachment_connect(&grant, &options,
                                            &pane->attachment);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last =
            pane->attachment != NULL
                ? cubicle_attachment_last_error(pane->attachment)
                : cubicle_client_last_error(session->manager);
        desk_debug_log("event=attach_connect_failed pane=%zu process=%s code=%d",
                       pane_index + 1, pane->process.friendly_name,
                       (int)code);
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "controller attachment failed");
        return code;
    }
    desk_debug_log("event=attach_connected pane=%zu process=%s endpoint=%s",
                   pane_index + 1, pane->process.friendly_name,
                   grant.endpoint.uri);
    code = cubicle_attachment_resize_tracked(pane->attachment, &pane->resize,
                                             pane->rows, pane->cols, true,
                                             NULL);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last =
            cubicle_attachment_last_error(pane->attachment);
        desk_debug_log("event=attach_resize_failed pane=%zu process=%s code=%d message=\"%s\"",
                       pane_index + 1, pane->process.friendly_name,
                       (int)code, last != NULL ? last->message : "");
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "initial controller resize failed");
        cubicle_attachment_disconnect(pane->attachment);
        pane->attachment = NULL;
        return code;
    }

    if (reload_pane_snapshot(pane) < 0) {
        snprintf(error, error_size, "terminal model initialization failed");
        cubicle_attachment_disconnect(pane->attachment);
        pane->attachment = NULL;
        return CUBICLE_ERR_INTERNAL;
    }
    code = cubicle_attachment_stream_start(pane->attachment,
                                           CUBICLE_STREAM_TTY);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last =
            cubicle_attachment_last_error(pane->attachment);
        desk_debug_log("event=attach_stream_start_failed pane=%zu process=%s code=%d message=\"%s\" action=polling_fallback",
                       pane_index + 1, pane->process.friendly_name,
                       (int)code, last != NULL ? last->message : "");
    } else {
        desk_debug_log("event=attach_stream_start_ok pane=%zu process=%s fd=%d",
                       pane_index + 1, pane->process.friendly_name,
                       cubicle_attachment_stream_fd(pane->attachment,
                                                   CUBICLE_STREAM_TTY));
    }
    desk_debug_log("event=attach_ok pane=%zu process=%s",
                   pane_index + 1, pane->process.friendly_name);
    return CUBICLE_OK;
}

static int attach_all_panes(desk_session_t *session,
                            char *error,
                            size_t error_size)
{
    for (size_t i = 0; i < session->pane_count; ++i) {
        cubicle_error_code_t code = attach_pane(session, i, error, error_size);
        if (code != CUBICLE_OK) {
            return code == CUBICLE_ERR_NOT_FOUND ? 1 : 2;
        }
    }
    return 0;
}

static void desk_cleanup_pane(desk_pane_t *pane)
{
    cubicle_attachment_disconnect(pane->attachment);
    pane->attachment = NULL;
    cubicle_terminal_model_destroy(pane->terminal_model);
    pane->terminal_model = NULL;
    grid_cleanup(&pane->grid);
    desk_scrollback_clear(pane);
    memset(pane, 0, sizeof(*pane));
}

static bool desk_process_has_ended(cubicle_process_state_t state)
{
    return state == CUBICLE_PROCESS_COMPLETED ||
           state == CUBICLE_PROCESS_FAILED ||
           state == CUBICLE_PROCESS_LOST ||
           state == CUBICLE_PROCESS_REMOVED;
}

static void desk_show_notice(desk_session_t *session, const char *message)
{
    snprintf(session->notice, sizeof(session->notice), "%s",
             message == NULL ? "" : message);
    session->notice_until_ms = desk_monotonic_ms() + DESK_NOTICE_MS;
}

static void desk_show_process_ended_notice(desk_session_t *session,
                                           const char *process_name)
{
    char message[256];
    snprintf(message, sizeof(message), "%s just ended",
             process_name != NULL && process_name[0] != '\0'
                 ? process_name
                 : "process");
    desk_show_notice(session, message);
}

static int desk_remove_pane_at(desk_session_t *session,
                               const desk_terminal_t *terminal,
                               size_t pane_index,
                               char *error,
                               size_t error_size)
{
    if (pane_index >= session->pane_count) {
        snprintf(error, error_size, "no such pane");
        return -1;
    }
    if (session->pane_count <= 1) {
        snprintf(error, error_size, "cannot delete the only pane");
        return -1;
    }

    int removed_pane_id = (int)pane_index + 1;
    int previous_active = session->layout.active_pane_id;
    session->layout.active_pane_id = removed_pane_id;
    if (pane_layout_delete_active(&session->layout) < 0) {
        session->layout.active_pane_id = previous_active;
        snprintf(error, error_size, "failed to delete pane");
        return -1;
    }
    pane_layout_compact_after_delete(&session->layout, removed_pane_id);
    desk_cleanup_pane(&session->panes[pane_index]);
    for (size_t i = pane_index + 1; i < session->pane_count; ++i) {
        session->panes[i - 1] = session->panes[i];
    }
    memset(&session->panes[session->pane_count - 1], 0,
           sizeof(session->panes[session->pane_count - 1]));
    session->pane_count--;
    session->zoomed = false;
    desk_apply_pane_labels(session);
    bool resized = false;
    if (resize_all_panes(session, terminal, &resized) < 0) {
        snprintf(error, error_size, "failed to resize panes");
        return -1;
    }
    (void)desk_save_layout(session);
    return 0;
}

static void desk_detach_pane_after_read_failure(desk_session_t *session,
                                                size_t pane_index)
{
    desk_pane_t *pane = &session->panes[pane_index];
    cubicle_attachment_disconnect(pane->attachment);
    pane->attachment = NULL;

    cubicle_process_info_t updated;
    cubicle_error_code_t code = cubicle_process_get(
        session->manager, pane->process.id,
        pane->process.workspace_id[0] == '\0' ? NULL
                                              : pane->process.workspace_id,
        &updated);
    if (code == CUBICLE_OK) {
        pane->process = updated;
        desk_apply_pane_labels(session);
    }
}

static int desk_refresh_ended_panes(desk_session_t *session,
                                    const desk_terminal_t *terminal,
                                    bool *layout_changed)
{
    long long now = desk_monotonic_ms();
    if (now > 0 && session->next_process_refresh_ms > 0 &&
        now < session->next_process_refresh_ms) {
        return 0;
    }
    session->next_process_refresh_ms =
        now > 0 ? now + DESK_PROCESS_REFRESH_MS : 0;

    for (size_t i = 0; i < session->pane_count;) {
        desk_pane_t *pane = &session->panes[i];
        if (pane->process.id[0] == '\0') {
            ++i;
            continue;
        }

        cubicle_process_info_t updated;
        cubicle_error_code_t code = cubicle_process_get(
            session->manager, pane->process.id,
            pane->process.workspace_id[0] == '\0' ? NULL
                                                  : pane->process.workspace_id,
            &updated);
        if (code != CUBICLE_OK) {
            ++i;
            continue;
        }
        if (!desk_process_has_ended(updated.state)) {
            pane->process = updated;
            if (pane->attachment == NULL) {
                char error[256];
                desk_debug_log("event=pane_reattach_start pane=%zu process=%s",
                               i + 1, pane->process.friendly_name);
                cubicle_error_code_t attach_code =
                    attach_pane(session, i, error, sizeof(error));
                if (attach_code == CUBICLE_OK) {
                    desk_debug_log("event=pane_reattach_ok pane=%zu process=%s",
                                   i + 1, pane->process.friendly_name);
                    if (layout_changed != NULL) {
                        *layout_changed = true;
                    }
                } else {
                    desk_debug_log("event=pane_reattach_failed pane=%zu process=%s code=%d message=\"%s\"",
                                   i + 1, pane->process.friendly_name,
                                   (int)attach_code, error);
                }
            }
            ++i;
            continue;
        }
        if (pane->ended_notice_shown) {
            pane->process = updated;
            ++i;
            continue;
        }

        char name[CUBICLE_NAME_MAX];
        snprintf(name, sizeof(name), "%s",
                 pane->process.friendly_name[0] != '\0'
                     ? pane->process.friendly_name
                     : pane->process.id);
        desk_debug_log("event=pane_process_ended pane=%zu process=%s state=%s",
                       i + 1, name, cubicle_process_state_name(updated.state));

        if (session->pane_count > 1) {
            char error[256];
            if (desk_remove_pane_at(session, terminal, i, error,
                                    sizeof(error)) < 0) {
                desk_debug_log("event=pane_remove_failed pane=%zu process=%s message=\"%s\"",
                               i + 1, name, error);
                pane->process = updated;
                pane->ended_notice_shown = true;
                ++i;
            }
            desk_show_process_ended_notice(session, name);
            if (layout_changed != NULL) {
                *layout_changed = true;
            }
            continue;
        }

        cubicle_attachment_disconnect(pane->attachment);
        pane->attachment = NULL;
        pane->process = updated;
        pane->ended_notice_shown = true;
        desk_show_process_ended_notice(session, name);
        if (layout_changed != NULL) {
            *layout_changed = true;
        }
        ++i;
    }
    return 0;
}

static void desk_disconnect_all_panes(desk_session_t *session)
{
    for (size_t i = 0; i < session->pane_count; ++i) {
        desk_cleanup_pane(&session->panes[i]);
    }
    memset(session->panes, 0, sizeof(session->panes));
    session->pane_count = 0;
}

static int desk_switch_workspace(desk_session_t *session,
                                 const desk_terminal_t *terminal,
                                 const cubicle_workspace_info_t *workspace,
                                 char *error,
                                 size_t error_size)
{
    cubicle_process_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.workspace_id = workspace->id;
    cubicle_process_info_t *processes = NULL;
    size_t process_count = 0;
    cubicle_page_info_t page;
    memset(&page, 0, sizeof(page));
    cubicle_error_code_t code = cubicle_process_list(
        session->manager, &filter, &processes, &process_count, &page);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "process list failed");
        return 2;
    }

    cubicle_process_info_t attachable[32];
    size_t attachable_count = 0;
    for (size_t i = 0; i < process_count; ++i) {
        if (!process_belongs_to_workspace(&processes[i], workspace->id) ||
            !process_is_attachable(&processes[i])) {
            continue;
        }
        if (attachable_count >= sizeof(attachable) / sizeof(attachable[0])) {
            break;
        }
        attachable[attachable_count++] = processes[i];
    }
    cubicle_process_list_free(processes);
    if (attachable_count == 0) {
        snprintf(error, error_size,
                 "workspace '%s' has no running TTY cubes", workspace->name);
        return 1;
    }

    desk_disconnect_all_panes(session);
    session->workspace = *workspace;
    snprintf(session->last_opened_workspace_id,
             sizeof(session->last_opened_workspace_id), "%s", workspace->id);
    for (size_t i = 0; i < attachable_count; ++i) {
        session->panes[i].process = attachable[i];
    }
    session->pane_count = attachable_count;
    session->zoomed = false;
    session->cursor_drawn = false;
    session->terminal_size_dirty = true;
    int result = load_or_create_layout(session, error, error_size);
    if (result == 0) {
        result = desk_load_workspace_macros(session, error, error_size);
    }
    bool changed = false;
    if (result == 0 &&
        resize_all_panes(session, terminal, &changed) < 0) {
        snprintf(error, error_size, "terminal too small for desk");
        result = 2;
    }
    if (result == 0) {
        result = attach_all_panes(session, error, error_size);
    }
    return result;
}

static bool desk_process_is_open(const desk_session_t *session,
                                 const char *process_id)
{
    for (size_t i = 0; i < session->pane_count; ++i) {
        if (session->panes[i].attachment != NULL &&
            strcmp(session->panes[i].process.id, process_id) == 0) {
            return true;
        }
    }
    return false;
}

static void desk_menu_select_first_enabled(desk_open_menu_t *menu)
{
    menu->selected = 0;
    menu->scroll_offset = 0;
    for (size_t i = 0; i < menu->item_count; ++i) {
        if (!menu->items[i].disabled) {
            menu->selected = i;
            return;
        }
    }
}

static bool desk_menu_select_workspace(desk_open_menu_t *menu,
                                       const char *workspace_id)
{
    if (workspace_id == NULL || workspace_id[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < menu->item_count; ++i) {
        if (menu->items[i].kind == DESK_MENU_ITEM_WORKSPACE &&
            strcmp(menu->items[i].workspace.id, workspace_id) == 0 &&
            !menu->items[i].disabled) {
            menu->selected = i;
            menu->scroll_offset = i;
            return true;
        }
    }
    return false;
}

static bool desk_layout_name_matches_filter(const char *name,
                                            const char *filter)
{
    if (filter == NULL || filter[0] == '\0') {
        return true;
    }
    size_t filter_length = strlen(filter);
    for (const char *cursor = name; *cursor != '\0'; ++cursor) {
        size_t i = 0;
        while (i < filter_length && cursor[i] != '\0' &&
               tolower((unsigned char)cursor[i]) ==
                   tolower((unsigned char)filter[i])) {
            ++i;
        }
        if (i == filter_length) {
            return true;
        }
    }
    return false;
}

static int desk_menu_add_layout(desk_session_t *session, const char *name)
{
    if (session->open_menu.item_count >= DESK_MENU_MAX_ITEMS) {
        return 0;
    }
    desk_menu_item_t *item =
        &session->open_menu.items[session->open_menu.item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = DESK_MENU_ITEM_LAYOUT;
    snprintf(item->layout_name, sizeof(item->layout_name), "%s", name);
    return 0;
}

static int desk_load_layout_picker_items(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    char filter[sizeof(menu->command)];
    snprintf(filter, sizeof(filter), "%s", menu->command);
    menu->item_count = 0;
    menu->selected = 0;
    menu->scroll_offset = 0;
    menu->status[0] = '\0';

    char dir[PATH_MAX];
    if (desk_named_layout_dir(dir) < 0) {
        snprintf(menu->status, sizeof(menu->status),
                 "failed to open layout directory");
        return -1;
    }
    DIR *handle = opendir(dir);
    if (handle == NULL) {
        snprintf(menu->status, sizeof(menu->status), "no saved layouts");
        return 0;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(handle)) != NULL) {
        if (entry->d_name[0] == '.' || !layout_has_suffix(entry->d_name)) {
            continue;
        }
        char name[CUBICLE_NAME_MAX];
        snprintf(name, sizeof(name), "%s", entry->d_name);
        size_t length = strlen(name);
        if (length > strlen(".layout")) {
            name[length - strlen(".layout")] = '\0';
        }
        if (desk_layout_name_matches_filter(name, filter)) {
            (void)desk_menu_add_layout(session, name);
        }
    }
    closedir(handle);
    if (menu->item_count == 0) {
        snprintf(menu->status, sizeof(menu->status),
                 filter[0] == '\0' ? "no saved layouts" : "no matching layouts");
    }
    desk_menu_select_first_enabled(menu);
    return 0;
}

static int desk_menu_add_process(desk_session_t *session,
                                 const cubicle_process_info_t *process)
{
    if (session->open_menu.item_count >= DESK_MENU_MAX_ITEMS) {
        return 0;
    }
    desk_menu_item_t *item =
        &session->open_menu.items[session->open_menu.item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = DESK_MENU_ITEM_PROCESS;
    item->process = *process;
    item->disabled = desk_process_is_open(session, process->id);
    return 0;
}

static int desk_menu_add_workspace(desk_session_t *session,
                                   const cubicle_workspace_info_t *workspace)
{
    if (session->open_menu.item_count >= DESK_MENU_MAX_ITEMS) {
        return 0;
    }
    desk_menu_item_t *item =
        &session->open_menu.items[session->open_menu.item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = DESK_MENU_ITEM_WORKSPACE;
    item->workspace = *workspace;
    return 0;
}

static int desk_menu_add_new_process(desk_session_t *session)
{
    if (session->open_menu.item_count >= DESK_MENU_MAX_ITEMS) {
        return 0;
    }
    desk_menu_item_t *item =
        &session->open_menu.items[session->open_menu.item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = DESK_MENU_ITEM_NEW;
    return 0;
}

static int desk_menu_add_macro(desk_session_t *session, size_t macro_index)
{
    if (session->open_menu.item_count >= DESK_MENU_MAX_ITEMS ||
        macro_index >= session->macro_count) {
        return 0;
    }
    desk_menu_item_t *item =
        &session->open_menu.items[session->open_menu.item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = DESK_MENU_ITEM_MACRO;
    item->macro_index = macro_index;
    return 0;
}

static int desk_menu_add_new_macro(desk_session_t *session)
{
    if (session->open_menu.item_count >= DESK_MENU_MAX_ITEMS) {
        return 0;
    }
    desk_menu_item_t *item =
        &session->open_menu.items[session->open_menu.item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = DESK_MENU_ITEM_MACRO_NEW;
    return 0;
}

static void desk_begin_macro_list(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    memset(menu, 0, sizeof(*menu));
    menu->level = DESK_MENU_MACROS;
    menu->workspace = session->workspace;
    for (size_t i = 0; i < session->macro_count; ++i) {
        (void)desk_menu_add_macro(session, i);
    }
    (void)desk_menu_add_new_macro(session);
    desk_menu_select_first_enabled(menu);
}

static void desk_begin_macro_name_prompt(desk_session_t *session,
                                         size_t macro_index)
{
    desk_open_menu_t *menu = &session->open_menu;
    memset(menu, 0, sizeof(*menu));
    menu->level = DESK_MENU_MACRO_NAME;
    menu->workspace = session->workspace;
    menu->edit_macro_index = macro_index;
    if (macro_index < session->macro_count) {
        menu->edit_macro_ordinal = session->macros[macro_index].ordinal;
        snprintf(menu->command, sizeof(menu->command), "%s",
                 session->macros[macro_index].name);
    } else {
        menu->edit_macro_ordinal = session->macro_count + 1;
    }
    menu->command_length = strlen(menu->command);
    snprintf(menu->status, sizeof(menu->status), "Macro name");
}

static void desk_begin_macro_text_prompt(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    char name[CUBICLE_WORKSPACE_MACRO_NAME_MAX];
    size_t name_length = strlen(menu->command);
    if (name_length >= sizeof(name)) {
        name_length = sizeof(name) - 1;
    }
    memcpy(name, menu->command, name_length);
    name[name_length] = '\0';
    uint64_t ordinal = menu->edit_macro_ordinal;
    size_t macro_index = menu->edit_macro_index;
    memset(menu->command, 0, sizeof(menu->command));
    menu->command_length = 0;
    snprintf(menu->macro_name, sizeof(menu->macro_name), "%s", name);
    menu->edit_macro_ordinal = ordinal;
    menu->edit_macro_index = macro_index;
    menu->level = DESK_MENU_MACRO_TEXT;
    if (macro_index < session->macro_count) {
        snprintf(menu->command, sizeof(menu->command), "%s",
                 session->macros[macro_index].text);
        menu->command_length = strlen(menu->command);
    }
    snprintf(menu->status, sizeof(menu->status), "Macro text");
}

static void desk_begin_macro_target_prompt(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    const char *text = menu->command;
    while (*text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }
    if (text[0] == '\0') {
        snprintf(menu->status, sizeof(menu->status),
                 "macro text is required");
        return;
    }
    snprintf(menu->macro_text, sizeof(menu->macro_text), "%s", text);
    memset(menu->command, 0, sizeof(menu->command));
    menu->command_length = 0;
    menu->level = DESK_MENU_MACRO_TARGET;
    menu->item_count = 0;
    menu->selected = 0;
    menu->scroll_offset = 0;

    if (menu->item_count < DESK_MENU_MAX_ITEMS) {
        desk_menu_item_t *item = &menu->items[menu->item_count++];
        memset(item, 0, sizeof(*item));
        item->kind = DESK_MENU_ITEM_MACRO_TARGET;
        item->target_pane = 0;
        snprintf(item->description, sizeof(item->description), "Active Pane");
    }
    uint64_t selected_target =
        menu->edit_macro_index < session->macro_count
            ? session->macros[menu->edit_macro_index].target_pane
            : 0;
    for (size_t i = 0; i < session->pane_count &&
                       menu->item_count < DESK_MENU_MAX_ITEMS;
         ++i) {
        desk_menu_item_t *item = &menu->items[menu->item_count++];
        memset(item, 0, sizeof(*item));
        item->kind = DESK_MENU_ITEM_MACRO_TARGET;
        item->target_pane = (uint64_t)(i + 1);
        const char *name = session->panes[i].process.friendly_name[0] != '\0'
                               ? session->panes[i].process.friendly_name
                               : session->panes[i].process.id;
        snprintf(item->description, sizeof(item->description), "%llu. %.80s",
                 (unsigned long long)item->target_pane, name);
        if (selected_target == item->target_pane) {
            menu->selected = menu->item_count - 1;
        }
    }
    snprintf(menu->status, sizeof(menu->status), "Select macro target");
}

static int desk_apply_macro_target_selection(desk_session_t *session,
                                             uint64_t target_pane)
{
    desk_open_menu_t *menu = &session->open_menu;
    if (menu->macro_name[0] == '\0' || menu->macro_text[0] == '\0') {
        snprintf(menu->status, sizeof(menu->status),
                 "macro name and text are required");
        return 0;
    }

    cubicle_workspace_macro_info_t macro;
    memset(&macro, 0, sizeof(macro));
    snprintf(macro.workspace_id, sizeof(macro.workspace_id), "%s",
             session->workspace.id);
    macro.ordinal = menu->edit_macro_ordinal;
    snprintf(macro.name, sizeof(macro.name), "%s", menu->macro_name);
    snprintf(macro.text, sizeof(macro.text), "%s", menu->macro_text);
    macro.target_pane = target_pane;
    if (menu->edit_macro_index < session->macro_count) {
        snprintf(macro.key_name, sizeof(macro.key_name), "%s",
                 session->macros[menu->edit_macro_index].key_name);
    }
    char error[256];
    if (desk_save_macro(session, &macro, error, sizeof(error)) < 0) {
        snprintf(menu->status, sizeof(menu->status), "%s", error);
        return 0;
    }
    desk_begin_macro_list(session);
    snprintf(session->open_menu.status, sizeof(session->open_menu.status),
             "saved macro %s", macro.name);
    return 0;
}

static int desk_load_process_menu(desk_session_t *session,
                                  const cubicle_workspace_info_t *workspace)
{
    desk_open_menu_t *menu = &session->open_menu;
    memset(menu, 0, sizeof(*menu));
    menu->level = DESK_MENU_WORKSPACE;
    menu->workspace = *workspace;
    snprintf(session->last_opened_workspace_id,
             sizeof(session->last_opened_workspace_id), "%s", workspace->id);

    cubicle_process_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.workspace_id = workspace->id;
    cubicle_process_info_t *processes = NULL;
    size_t process_count = 0;
    cubicle_page_info_t page;
    memset(&page, 0, sizeof(page));
    cubicle_error_code_t code = cubicle_process_list(
        session->manager, &filter, &processes, &process_count, &page);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        snprintf(menu->status, sizeof(menu->status), "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "process list failed");
        return -1;
    }

    for (size_t i = 0; i < process_count; ++i) {
        if (process_belongs_to_workspace(&processes[i], workspace->id) &&
            process_is_attachable(&processes[i])) {
            (void)desk_menu_add_process(session, &processes[i]);
        }
    }
    cubicle_process_list_free(processes);
    if (menu->item_count == 0) {
        snprintf(menu->status, sizeof(menu->status),
                 "workspace has no running cubes");
    }
    (void)desk_menu_add_new_process(session);
    desk_menu_select_first_enabled(menu);
    return 0;
}

static int desk_open_root_menu(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    memset(menu, 0, sizeof(*menu));
    menu->level = DESK_MENU_ROOT;
    menu->workspace = session->workspace;

    cubicle_process_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.workspace_id = session->workspace.id;
    cubicle_process_info_t *processes = NULL;
    size_t process_count = 0;
    cubicle_page_info_t page;
    memset(&page, 0, sizeof(page));
    cubicle_error_code_t code = cubicle_process_list(
        session->manager, &filter, &processes, &process_count, &page);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        snprintf(menu->status, sizeof(menu->status), "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "process list failed");
        return -1;
    }

    for (size_t i = 0; i < process_count; ++i) {
        if (process_belongs_to_workspace(&processes[i], session->workspace.id) &&
            process_is_attachable(&processes[i])) {
            (void)desk_menu_add_process(session, &processes[i]);
        }
    }
    cubicle_process_list_free(processes);

    cubicle_workspace_info_t *workspaces = NULL;
    size_t workspace_count = 0;
    memset(&page, 0, sizeof(page));
    code = cubicle_workspace_list(session->manager, NULL, &workspaces,
                                  &workspace_count, &page);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        snprintf(menu->status, sizeof(menu->status), "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "workspace list failed");
        return -1;
    }
    for (size_t i = 0; i < workspace_count; ++i) {
        if (strcmp(workspaces[i].id, session->workspace.id) != 0) {
            (void)desk_menu_add_workspace(session, &workspaces[i]);
        }
    }
    cubicle_workspace_list_free(workspaces);
    (void)desk_menu_add_new_process(session);
    if (menu->item_count == 0) {
        snprintf(menu->status, sizeof(menu->status),
                 "no cubes or other workspaces available");
    }
    if (!desk_menu_select_workspace(menu, session->last_opened_workspace_id)) {
        desk_menu_select_first_enabled(menu);
    }
    return 0;
}

static void desk_close_open_menu(desk_session_t *session)
{
    memset(&session->open_menu, 0, sizeof(session->open_menu));
    session->pending_split = DESK_PENDING_SPLIT_NONE;
}

static void desk_begin_save_layout_prompt(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    memset(menu, 0, sizeof(*menu));
    menu->level = DESK_MENU_SAVE_LAYOUT;
    menu->workspace = session->workspace;
}

static void desk_begin_layout_picker(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    memset(menu, 0, sizeof(*menu));
    menu->level = DESK_MENU_LAYOUT_PICKER;
    menu->workspace = session->workspace;
    (void)desk_load_layout_picker_items(session);
}

static const char *desk_command_description(const char *command)
{
    if (strcmp(command, "pane.next") == 0) {
        return "Select next pane";
    }
    if (strcmp(command, "pane.previous") == 0) {
        return "Select previous pane";
    }
    if (strcmp(command, "pane.left") == 0) {
        return "Select pane to the left";
    }
    if (strcmp(command, "pane.right") == 0) {
        return "Select pane to the right";
    }
    if (strcmp(command, "pane.above") == 0) {
        return "Select pane above";
    }
    if (strcmp(command, "pane.below") == 0) {
        return "Select pane below";
    }
    if (strcmp(command, "layout.zoom") == 0) {
        return "Toggle active pane zoom";
    }
    if (strcmp(command, "layout.resize.toggle") == 0) {
        return "Toggle layout mode";
    }
    if (strcmp(command, "layout.transpose") == 0) {
        return "Transpose panes on axis";
    }
    if (strcmp(command, "layout.split.horizontal") == 0) {
        return "Split pane horizontally";
    }
    if (strcmp(command, "layout.split.vertical") == 0) {
        return "Split pane vertically";
    }
    if (strcmp(command, "layout.delete") == 0) {
        return "Delete active pane";
    }
    if (strcmp(command, "layout.reset") == 0) {
        return "Reset automatic layout";
    }
    if (strcmp(command, "layout.zoom.horizontal") == 0) {
        return "Zoom active split horizontally";
    }
    if (strcmp(command, "layout.zoom.vertical") == 0) {
        return "Zoom active split vertically";
    }
    if (strcmp(command, "layout.save") == 0) {
        return "Save current layout";
    }
    if (strcmp(command, "layout.load") == 0) {
        return "Load saved layout";
    }
    if (strcmp(command, "bindings.show") == 0) {
        return "Show key bindings";
    }
    if (strcmp(command, "scroll.page_up") == 0) {
        return "Scroll active pane up one page";
    }
    if (strcmp(command, "scroll.page_down") == 0) {
        return "Scroll active pane down one page";
    }
    if (strcmp(command, "scroll.line_up") == 0) {
        return "Scroll active pane up one line";
    }
    if (strcmp(command, "scroll.line_down") == 0) {
        return "Scroll active pane down one line";
    }
    if (strcmp(command, "scroll.top") == 0) {
        return "Scroll active pane to top";
    }
    if (strcmp(command, "scroll.bottom") == 0) {
        return "Return active pane to live output";
    }
    if (strcmp(command, "menu.open") == 0) {
        return "Open cube menu";
    }
    if (strcmp(command, "mouse.toggle") == 0) {
        return "Toggle mouse title selection";
    }
    if (strcmp(command, "quit") == 0) {
        return "Quit Desk";
    }
    return "Unknown command";
}

static void desk_format_binding_key(const cubicle_desk_key_binding_t *binding,
                                    char *buffer,
                                    size_t size)
{
    snprintf(buffer, size, "%s", binding->key_name);
}

static void desk_append_binding_text(char *destination,
                                     size_t destination_size,
                                     const char *binding)
{
    if (destination[0] != '\0') {
        strncat(destination, ", ",
                destination_size - strlen(destination) - 1);
    }
    strncat(destination, binding,
            destination_size - strlen(destination) - 1);
}

static void desk_add_bindings_item(desk_session_t *session,
                                   const char *command)
{
    if (session->open_menu.item_count >= DESK_MENU_MAX_ITEMS) {
        return;
    }
    desk_menu_item_t *item =
        &session->open_menu.items[session->open_menu.item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = DESK_MENU_ITEM_BINDING;
    snprintf(item->command_name, sizeof(item->command_name), "%s", command);
    snprintf(item->description, sizeof(item->description), "%s",
             desk_command_description(command));
    for (size_t i = 0; i < session->key_binding_count; ++i) {
        const cubicle_desk_key_binding_t *binding = &session->key_bindings[i];
        if (binding->unbind || strcmp(binding->command, command) != 0) {
            continue;
        }
        char key[48];
        desk_format_binding_key(binding, key, sizeof(key));
        desk_append_binding_text(item->key_text, sizeof(item->key_text), key);
    }
    if (item->key_text[0] == '\0') {
        snprintf(item->key_text, sizeof(item->key_text), "unbound");
    }
}

static void desk_add_macro_bindings_item(desk_session_t *session,
                                         const cubicle_workspace_macro_info_t *macro)
{
    if (session->open_menu.item_count >= DESK_MENU_MAX_ITEMS) {
        return;
    }
    desk_menu_item_t *item =
        &session->open_menu.items[session->open_menu.item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = DESK_MENU_ITEM_BINDING;
    desk_macro_command_name(macro->ordinal, item->command_name,
                            sizeof(item->command_name));
    snprintf(item->description, sizeof(item->description), "%llu. %s",
             (unsigned long long)macro->ordinal, macro->name);
    if (macro->ordinal > 0 && macro->ordinal <= 10) {
        char default_key[32];
        snprintf(default_key, sizeof(default_key), "Prefix-%llu",
                 (unsigned long long)(macro->ordinal == 10 ? 0
                                                           : macro->ordinal));
        desk_append_binding_text(item->key_text, sizeof(item->key_text),
                                 default_key);
    }
    if (macro->key_name[0] != '\0') {
        desk_append_binding_text(item->key_text, sizeof(item->key_text),
                                 macro->key_name);
    }
    if (item->key_text[0] == '\0') {
        snprintf(item->key_text, sizeof(item->key_text), "unbound");
    }
}

static void desk_begin_bindings_overlay(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    memset(menu, 0, sizeof(*menu));
    menu->level = DESK_MENU_BINDINGS;
    menu->workspace = session->workspace;
    desk_add_bindings_item(session, "pane.next");
    desk_add_bindings_item(session, "pane.previous");
    desk_add_bindings_item(session, "pane.left");
    desk_add_bindings_item(session, "pane.right");
    desk_add_bindings_item(session, "pane.above");
    desk_add_bindings_item(session, "pane.below");
    desk_add_bindings_item(session, "layout.zoom");
    desk_add_bindings_item(session, "layout.resize.toggle");
    desk_add_bindings_item(session, "layout.transpose");
    desk_add_bindings_item(session, "layout.split.horizontal");
    desk_add_bindings_item(session, "layout.split.vertical");
    desk_add_bindings_item(session, "layout.delete");
    desk_add_bindings_item(session, "layout.reset");
    desk_add_bindings_item(session, "layout.zoom.horizontal");
    desk_add_bindings_item(session, "layout.zoom.vertical");
    desk_add_bindings_item(session, "layout.save");
    desk_add_bindings_item(session, "layout.load");
    desk_add_bindings_item(session, "bindings.show");
    desk_add_bindings_item(session, "scroll.page_up");
    desk_add_bindings_item(session, "scroll.page_down");
    desk_add_bindings_item(session, "scroll.line_up");
    desk_add_bindings_item(session, "scroll.line_down");
    desk_add_bindings_item(session, "scroll.top");
    desk_add_bindings_item(session, "scroll.bottom");
    desk_add_bindings_item(session, "menu.open");
    desk_add_bindings_item(session, "mouse.toggle");
    desk_add_bindings_item(session, "quit");
    for (size_t i = 0; i < session->macro_count; ++i) {
        desk_add_macro_bindings_item(session, &session->macros[i]);
    }
}

static void desk_select_binding_command(desk_session_t *session,
                                        const char *command)
{
    desk_open_menu_t *menu = &session->open_menu;
    for (size_t i = 0; i < menu->item_count; ++i) {
        if (strcmp(menu->items[i].command_name, command) == 0) {
            menu->selected = i;
            menu->scroll_offset = 0;
            return;
        }
    }
}

static void desk_begin_binding_edit_prompt(desk_session_t *session,
                                           const char *command)
{
    desk_open_menu_t *menu = &session->open_menu;
    char first_key[sizeof(menu->command)] = {0};
    cubicle_workspace_macro_info_t *macro =
        desk_find_macro_by_command(session, command);
    if (macro != NULL) {
        snprintf(first_key, sizeof(first_key), "%s", macro->key_name);
    } else {
        for (size_t i = 0; i < session->key_binding_count; ++i) {
            const cubicle_desk_key_binding_t *binding = &session->key_bindings[i];
            if (!binding->unbind && strcmp(binding->command, command) == 0) {
                desk_format_binding_key(binding, first_key, sizeof(first_key));
                break;
            }
        }
    }

    memset(menu, 0, sizeof(*menu));
    menu->level = DESK_MENU_BINDING_EDIT;
    menu->workspace = session->workspace;
    snprintf(menu->edit_command, sizeof(menu->edit_command), "%s", command);
    snprintf(menu->command, sizeof(menu->command), "%s", first_key);
    menu->command_length = strlen(menu->command);
    snprintf(menu->status, sizeof(menu->status),
             "Editing [%s]. Use none to unbind.", command);
}

static int desk_parse_binding_key_text(const char *text,
                                       cubicle_desk_key_binding_t *binding,
                                       char *error,
                                       size_t error_size)
{
    memset(binding, 0, sizeof(*binding));
    return cubicle_config_parse_desk_key_name(
        text, &binding->uses_prefix, &binding->key, binding->sequence,
        &binding->sequence_length, binding->key_name,
        sizeof(binding->key_name), error, error_size);
}

static void desk_remove_bindings_for_command(desk_session_t *session,
                                             const char *command)
{
    for (size_t i = 0; i < session->key_binding_count;) {
        if (strcmp(session->key_bindings[i].command, command) == 0) {
            memmove(&session->key_bindings[i], &session->key_bindings[i + 1],
                    (session->key_binding_count - i - 1) *
                        sizeof(session->key_bindings[0]));
            session->key_binding_count--;
            continue;
        }
        i++;
    }
}

static const char *desk_find_binding_conflict(const desk_session_t *session,
                                             const char *command,
                                             const cubicle_desk_key_binding_t *candidate)
{
    for (size_t i = 0; i < session->key_binding_count; ++i) {
        const cubicle_desk_key_binding_t *binding = &session->key_bindings[i];
        if (binding->unbind ||
            strcmp(binding->command, command) == 0 ||
            binding->uses_prefix != candidate->uses_prefix ||
            binding->sequence_length != candidate->sequence_length ||
            memcmp(binding->sequence, candidate->sequence,
                   candidate->sequence_length) != 0) {
            continue;
        }
        return binding->command;
    }
    for (size_t i = 0; i < session->macro_count; ++i) {
        const cubicle_workspace_macro_info_t *macro = &session->macros[i];
        if (macro->key_name[0] == '\0') {
            continue;
        }
        char macro_command[CUBICLE_DESK_COMMAND_NAME_MAX];
        desk_macro_command_name(macro->ordinal, macro_command,
                                sizeof(macro_command));
        if (strcmp(macro_command, command) == 0) {
            continue;
        }
        cubicle_desk_key_binding_t macro_binding;
        char parse_error[128];
        if (desk_parse_binding_key_text(macro->key_name, &macro_binding,
                                        parse_error,
                                        sizeof(parse_error)) == 0 &&
            macro_binding.uses_prefix == candidate->uses_prefix &&
            macro_binding.sequence_length == candidate->sequence_length &&
            memcmp(macro_binding.sequence, candidate->sequence,
                   candidate->sequence_length) == 0) {
            static char conflict[CUBICLE_DESK_COMMAND_NAME_MAX];
            snprintf(conflict, sizeof(conflict), "%s", macro_command);
            return conflict;
        }
    }
    return NULL;
}

static int desk_apply_binding_edit(desk_session_t *session)
{
    desk_open_menu_t *menu = &session->open_menu;
    char command[CUBICLE_DESK_COMMAND_NAME_MAX];
    snprintf(command, sizeof(command), "%s", menu->edit_command);
    cubicle_workspace_macro_info_t *macro =
        desk_find_macro_by_command(session, command);

    if (desk_string_equals_case(menu->command, "none")) {
        if (macro != NULL) {
            macro->key_name[0] = '\0';
            char error[256];
            int persisted = desk_save_macro(session, macro, error,
                                            sizeof(error));
            desk_begin_bindings_overlay(session);
            desk_select_binding_command(session, command);
            snprintf(session->open_menu.status,
                     sizeof(session->open_menu.status),
                     persisted == 0 ? "[%s] custom key removed"
                                    : "[%s] custom key remove failed",
                     command);
            return 0;
        }
        desk_remove_bindings_for_command(session, command);
        char persist_error[256] = "";
        int persisted = cubicle_config_write_user_desk_key_bindings(
            session->key_bindings, session->key_binding_count,
            session->startup_key_bindings, session->startup_key_binding_count,
            persist_error, sizeof(persist_error));
        desk_begin_bindings_overlay(session);
        desk_select_binding_command(session, command);
        if (persisted == 0) {
            snprintf(session->open_menu.status,
                     sizeof(session->open_menu.status), "[%s] is unbound",
                     command);
        } else {
            snprintf(session->open_menu.status,
                     sizeof(session->open_menu.status),
                     "[%s] is unbound; config write failed", command);
        }
        return 0;
    }

    cubicle_desk_key_binding_t parsed;
    char parse_error[128] = "";
    if (desk_parse_binding_key_text(menu->command, &parsed, parse_error,
                                    sizeof(parse_error)) < 0) {
        snprintf(menu->status, sizeof(menu->status),
                 "%s", parse_error[0] != '\0'
                          ? parse_error
                          : "Invalid key. Try Prefix-?, C-G, or C-S-Right.");
        return 0;
    }

    const char *conflict =
        desk_find_binding_conflict(session, command, &parsed);
    if (conflict != NULL) {
        snprintf(menu->status, sizeof(menu->status),
                 "%s is already bound to [%s]", parsed.key_name, conflict);
        return 0;
    }

    desk_remove_bindings_for_command(session, command);
    if (macro != NULL) {
        snprintf(macro->key_name, sizeof(macro->key_name), "%s",
                 parsed.key_name);
        char error[256];
        int persisted = desk_save_macro(session, macro, error, sizeof(error));
        desk_begin_bindings_overlay(session);
        desk_select_binding_command(session, command);
        snprintf(session->open_menu.status, sizeof(session->open_menu.status),
                 persisted == 0 ? "[%s] now uses %s"
                                : "[%s] key save failed",
                 command, parsed.key_name);
        return 0;
    }
    if (session->key_binding_count >= CUBICLE_DESK_KEY_BINDING_MAX) {
        snprintf(menu->status, sizeof(menu->status),
                 "No room for another key binding");
        return 0;
    }

    cubicle_desk_key_binding_t *binding =
        &session->key_bindings[session->key_binding_count++];
    memset(binding, 0, sizeof(*binding));
    *binding = parsed;
    snprintf(binding->command, sizeof(binding->command), "%s", command);

    char persist_error[256] = "";
    int persisted = cubicle_config_write_user_desk_key_bindings(
        session->key_bindings, session->key_binding_count,
        session->startup_key_bindings, session->startup_key_binding_count,
        persist_error, sizeof(persist_error));
    desk_begin_bindings_overlay(session);
    desk_select_binding_command(session, command);
    if (persisted == 0) {
        snprintf(session->open_menu.status, sizeof(session->open_menu.status),
                 "[%s] now uses %s", command, binding->key_name);
    } else {
        snprintf(session->open_menu.status, sizeof(session->open_menu.status),
                 "[%s] now uses %s; config write failed", command,
                 binding->key_name);
    }
    return 0;
}

static void desk_menu_enable_mouse(void)
{
    (void)cubeui_write_all(STDOUT_FILENO, "\x1b[?1000h\x1b[?1006h", 16);
}

static void desk_menu_disable_mouse(void)
{
    (void)cubeui_write_all(STDOUT_FILENO, "\x1b[?1006l\x1b[?1000l", 16);
}

static void desk_suspend_mouse_for_selection(desk_session_t *session)
{
    desk_menu_disable_mouse();
    session->mouse_suspended_until_ms = desk_monotonic_ms() + 1500;
    desk_debug_log("event=mouse_selection_suspend duration_ms=1500");
}

static void desk_resume_mouse_if_ready(desk_session_t *session)
{
    if (!session->mouse_titles || session->mouse_suspended_until_ms == 0) {
        return;
    }
    long long now = desk_monotonic_ms();
    if (now == 0 || now < session->mouse_suspended_until_ms) {
        return;
    }
    session->mouse_suspended_until_ms = 0;
    desk_menu_enable_mouse();
    desk_debug_log("event=mouse_selection_resume");
}

static bool sgr_mouse_is_left_button(int button)
{
    return (button & 3) == 0;
}

static bool sgr_mouse_has_modifier(int button)
{
    return (button & (4 | 8 | 16)) != 0;
}

static bool desk_title_hit_test(const desk_terminal_t *terminal,
                                const desk_session_t *session,
                                int row,
                                int col,
                                int *pane_id)
{
    for (size_t i = 0; i < session->pane_count; ++i) {
        int candidate = (int)i + 1;
        if (candidate == session->layout.active_pane_id) {
            continue;
        }

        desk_rect_t rect;
        if (!pane_layout_rect_for_pane(&session->layout, terminal, candidate,
                                       &rect) ||
            rect.rows <= 0 || rect.cols <= 0) {
            continue;
        }
        if (row != rect.row + 1) {
            continue;
        }

        const char *label =
            session->panes[i].process.friendly_name[0] != '\0'
                ? session->panes[i].process.friendly_name
                : session->panes[i].process.id;
        int title_start = rect.col + 1;
        int title_cols = rect.cols;
        if (rect.cols > 2) {
            title_cols = rect.cols - 2;
        }
        int label_cols = 0;
        for (const char *cursor = label;
             *cursor != '\0' && label_cols < title_cols;
             ++cursor) {
            ++label_cols;
        }
        if (session->mouse_titles && label_cols + 2 <= title_cols) {
            label_cols += 2;
        }
        int hit_start = title_start;
        int hit_end = title_start + label_cols - 1;
        if (rect.cols > 2) {
            hit_end += 2;
        }
        if (label_cols > 0 && col >= hit_start && col <= hit_end) {
            *pane_id = candidate;
            return true;
        }
    }
    return false;
}

static void desk_menu_keep_selection_visible(desk_open_menu_t *menu,
                                             int visible_items)
{
    if (visible_items <= 0) {
        menu->scroll_offset = 0;
        return;
    }
    if (menu->selected < menu->scroll_offset) {
        menu->scroll_offset = menu->selected;
    }
    size_t visible = (size_t)visible_items;
    if (menu->selected >= menu->scroll_offset + visible) {
        menu->scroll_offset = menu->selected - visible + 1;
    }
}

static void desk_menu_move_selection(desk_open_menu_t *menu, int delta,
                                     int visible_items)
{
    if (menu->item_count == 0) {
        return;
    }
    size_t current = menu->selected < menu->item_count ? menu->selected : 0;
    for (size_t tries = 0; tries < menu->item_count; ++tries) {
        if (delta > 0) {
            current = (current + 1) % menu->item_count;
        } else {
            current = current == 0 ? menu->item_count - 1 : current - 1;
        }
        if (!menu->items[current].disabled) {
            menu->selected = current;
            desk_menu_keep_selection_visible(menu, visible_items);
            return;
        }
    }
}

static int desk_replace_active_pane_process(desk_session_t *session,
                                            const desk_terminal_t *terminal,
                                            const cubicle_process_info_t *process,
                                            char *error,
                                            size_t error_size)
{
    int active = session->layout.active_pane_id;
    if (active <= 0 ||
        (size_t)active > sizeof(session->panes) / sizeof(session->panes[0]) ||
        (size_t)active > session->pane_count + 1) {
        snprintf(error, error_size, "no active pane");
        return -1;
    }
    size_t pane_index = (size_t)active - 1;
    if (pane_index == session->pane_count) {
        memset(&session->panes[pane_index], 0,
               sizeof(session->panes[pane_index]));
        session->pane_count++;
    }
    desk_pane_t *pane = &session->panes[pane_index];
    cubicle_attachment_disconnect(pane->attachment);
    pane->attachment = NULL;
    cubicle_terminal_model_destroy(pane->terminal_model);
    pane->terminal_model = NULL;
    grid_cleanup(&pane->grid);
    desk_scrollback_clear(pane);
    memset(&pane->grid, 0, sizeof(pane->grid));
    memset(&pane->resize, 0, sizeof(pane->resize));
    pane->rows = 0;
    pane->cols = 0;
    pane->process = *process;
    bool changed = false;
    if (resize_pane_attachment(session, terminal, pane_index, &changed) < 0) {
        snprintf(error, error_size, "failed to resize pane");
        return -1;
    }
    cubicle_error_code_t code = attach_pane(session, pane_index, error,
                                            error_size);
    if (code != CUBICLE_OK) {
        return -1;
    }
    desk_apply_pane_labels(session);
    return 0;
}

static int desk_commit_pending_split_process(desk_session_t *session,
                                             const desk_terminal_t *terminal,
                                             const cubicle_process_info_t *process,
                                             char *error,
                                             size_t error_size)
{
    if (session->pending_split == DESK_PENDING_SPLIT_NONE) {
        return desk_replace_active_pane_process(session, terminal, process,
                                                error, error_size);
    }
    if (session->pane_count >= sizeof(session->panes) / sizeof(session->panes[0])) {
        snprintf(error, error_size, "maximum pane count reached");
        session->pending_split = DESK_PENDING_SPLIT_NONE;
        return -1;
    }

    desk_pane_layout_t saved_layout = session->layout;
    size_t saved_pane_count = session->pane_count;
    desk_split_t split = session->pending_split ==
                                 DESK_PENDING_SPLIT_HORIZONTAL
                             ? DESK_SPLIT_HORIZONTAL
                             : DESK_SPLIT_VERTICAL;
    session->pending_split = DESK_PENDING_SPLIT_NONE;
    if (pane_layout_split(&session->layout, split) < 0) {
        snprintf(error, error_size, "failed to split pane");
        return -1;
    }
    session->zoomed = false;
    bool resized = false;
    if (resize_all_panes(session, terminal, &resized) < 0 ||
        desk_replace_active_pane_process(session, terminal, process, error,
                                         error_size) < 0) {
        if (session->pane_count > saved_pane_count) {
            desk_cleanup_pane(&session->panes[saved_pane_count]);
            session->pane_count = saved_pane_count;
        }
        session->layout = saved_layout;
        bool rollback_resized = false;
        (void)resize_all_panes(session, terminal, &rollback_resized);
        return -1;
    }
    desk_apply_pane_labels(session);
    (void)desk_save_layout(session);
    return 0;
}

static int desk_begin_split_process_menu(desk_session_t *session,
                                         desk_pending_split_t split,
                                         char *error,
                                         size_t error_size)
{
    if (session->pane_count >= sizeof(session->panes) / sizeof(session->panes[0])) {
        snprintf(error, error_size, "maximum pane count reached");
        return -1;
    }
    session->pending_split = split;
    if (desk_load_process_menu(session, &session->workspace) < 0) {
        session->pending_split = DESK_PENDING_SPLIT_NONE;
        snprintf(error, error_size, "%s", session->open_menu.status);
        return -1;
    }
    snprintf(session->open_menu.status, sizeof(session->open_menu.status),
             "select a cube for the new pane");
    return 0;
}

static int desk_delete_active_pane(desk_session_t *session,
                                   const desk_terminal_t *terminal,
                                   char *error,
                                   size_t error_size)
{
    if (session->pane_count <= 1) {
        snprintf(error, error_size, "cannot delete the only pane");
        return -1;
    }
    int removed_pane_id = session->layout.active_pane_id;
    if (removed_pane_id <= 0 || (size_t)removed_pane_id > session->pane_count) {
        snprintf(error, error_size, "no active pane");
        return -1;
    }
    return desk_remove_pane_at(session, terminal, (size_t)removed_pane_id - 1,
                               error, error_size);
}

static int desk_apply_named_layout(desk_session_t *session,
                                   const desk_terminal_t *terminal,
                                   const char *layout_name,
                                   char *error,
                                   size_t error_size)
{
    char path[PATH_MAX];
    if (desk_named_layout_path(layout_name, path) < 0) {
        snprintf(error, error_size, "invalid layout name");
        return -1;
    }

    desk_pane_layout_t loaded_layout;
    char process_ids[32][CUBICLE_ID_STRING_LENGTH];
    if (desk_load_named_layout_file(path, &loaded_layout, process_ids) < 0) {
        snprintf(error, error_size, "failed to load layout");
        return -1;
    }

    bool leaf_panes[32] = {false};
    int max_pane_id = 0;
    desk_layout_collect_leaf_panes(&loaded_layout, loaded_layout.root,
                                   leaf_panes, &max_pane_id);
    if (max_pane_id <= 0) {
        snprintf(error, error_size, "layout has no panes");
        return -1;
    }
    for (int pane_id = 1; pane_id <= max_pane_id; ++pane_id) {
        if (!leaf_panes[pane_id - 1]) {
            snprintf(error, error_size, "layout has non-contiguous panes");
            return -1;
        }
    }

    cubicle_process_info_t processes[32];
    memset(processes, 0, sizeof(processes));
    for (int pane_id = 1; pane_id <= max_pane_id; ++pane_id) {
        if (!leaf_panes[pane_id - 1]) {
            continue;
        }
        if (process_ids[pane_id - 1][0] == '\0') {
            snprintf(error, error_size, "layout is missing a cube for pane %d",
                     pane_id);
            return -1;
        }
        cubicle_error_code_t code = cubicle_process_get(
            session->manager, process_ids[pane_id - 1], session->workspace.id,
            &processes[pane_id - 1]);
        if (code != CUBICLE_OK || !process_is_attachable(&processes[pane_id - 1])) {
            snprintf(error, error_size, "cube for pane %d is not attachable",
                     pane_id);
            return -1;
        }
    }

    desk_disconnect_all_panes(session);
    session->layout = loaded_layout;
    session->layout.zoom = DESK_ZOOM_NONE;
    session->layout.zoom_pane_id = 0;
    session->layout.resize_mode = false;
    session->zoomed = false;
    session->pane_count = (size_t)max_pane_id;
    for (int pane_id = 1; pane_id <= max_pane_id; ++pane_id) {
        if (leaf_panes[pane_id - 1]) {
            session->panes[pane_id - 1].process = processes[pane_id - 1];
        }
    }

    bool changed = false;
    if (resize_all_panes(session, terminal, &changed) < 0 ||
        attach_all_panes(session, error, error_size) != 0) {
        return -1;
    }
    desk_apply_pane_labels(session);
    (void)desk_save_layout(session);
    return 0;
}

static void render_all_panes(const desk_terminal_t *terminal,
                             desk_session_t *session)
{
    desk_cursor_erase(terminal, session);
    desk_render_layout(terminal, &session->layout);
    for (size_t i = 0; i < session->pane_count; ++i) {
        desk_render_cube_grid(terminal, &session->layout, (int)i + 1,
                              &session->panes[i],
                              session->mouse_titles);
    }
    desk_cursor_reset_blink(session);
    desk_cursor_render(terminal, session);
    desk_render_notice(terminal, session);
}

static int desk_apply_layout_change(desk_session_t *session,
                                    const desk_terminal_t *terminal)
{
    bool sizes_changed = false;
    if (resize_all_panes(session, terminal, &sizes_changed) < 0) {
        return -1;
    }
    (void)desk_save_layout(session);
    render_all_panes(terminal, session);
    return 0;
}

static void desk_render_notice(const desk_terminal_t *terminal,
                               const desk_session_t *session)
{
    if (session->notice[0] == '\0' || terminal->rows <= 0 ||
        terminal->cols <= 0) {
        return;
    }

    char text[sizeof(session->notice)];
    size_t text_length = 0;
    for (const unsigned char *cursor = (const unsigned char *)session->notice;
         *cursor != '\0' && text_length + 1 < sizeof(text); ++cursor) {
        text[text_length++] = (*cursor >= 0x20 && *cursor != 0x7f)
                                  ? (char)*cursor
                                  : '?';
    }
    text[text_length] = '\0';
    if (text_length == 0) {
        return;
    }

    int box_cols = (int)text_length + 4;
    if (box_cols > terminal->cols) {
        box_cols = terminal->cols;
    }
    if (box_cols < 4) {
        return;
    }
    int row = terminal->rows / 2;
    int col = (terminal->cols - box_cols) / 2 + 1;
    int inner_cols = box_cols - 4;

    char line[1024];
    int length = snprintf(line, sizeof(line),
                          "\x1b[%d;%dH\x1b[2;7m %.*s \x1b[0m",
                          row, col, inner_cols, text);
    if (length > 0 && (size_t)length < sizeof(line)) {
        (void)cubeui_write_all(STDOUT_FILENO, line, (size_t)length);
    }
}

static bool desk_menu_geometry(const desk_terminal_t *terminal,
                               const desk_session_t *session,
                               desk_menu_geometry_t *geometry)
{
    const desk_open_menu_t *menu = &session->open_menu;
    desk_rect_t pane_rect;
    if (menu->level == DESK_MENU_SAVE_LAYOUT ||
        menu->level == DESK_MENU_LAYOUT_PICKER ||
        menu->level == DESK_MENU_BINDINGS ||
        menu->level == DESK_MENU_BINDING_EDIT ||
        menu->level == DESK_MENU_MACROS ||
        menu->level == DESK_MENU_MACRO_NAME ||
        menu->level == DESK_MENU_MACRO_TEXT ||
        menu->level == DESK_MENU_MACRO_TARGET) {
        pane_rect.row = 0;
        pane_rect.col = 0;
        pane_rect.rows = terminal->rows;
        pane_rect.cols = terminal->cols;
    } else {
        if (!pane_layout_rect_for_pane(&session->layout, terminal,
                                       session->layout.active_pane_id,
                                       &pane_rect)) {
            return false;
        }
    }

    int box_cols = pane_rect.cols > 74 ? 74 : pane_rect.cols;
    if (box_cols < 32) {
        box_cols = pane_rect.cols;
    }
    int wanted_rows = 7 + (int)menu->item_count;
    if (menu->status[0] != '\0') {
        wanted_rows += 2;
    }
    if (menu->level == DESK_MENU_LAYOUT_PICKER) {
        wanted_rows += 1;
    }
    int box_rows = wanted_rows;
    if (box_rows > pane_rect.rows) {
        box_rows = pane_rect.rows;
    }
    if (box_rows < 6) {
        box_rows = pane_rect.rows < 6 ? pane_rect.rows : 6;
    }
    if (box_rows <= 0 || box_cols <= 0) {
        return false;
    }
    geometry->row = pane_rect.row + (pane_rect.rows - box_rows) / 2;
    geometry->col = pane_rect.col + (pane_rect.cols - box_cols) / 2;
    geometry->rows = box_rows;
    geometry->cols = box_cols;
    geometry->item_row =
        geometry->row + (menu->level == DESK_MENU_LAYOUT_PICKER ? 7 : 6);
    int header_rows = menu->level == DESK_MENU_LAYOUT_PICKER ? 7 : 6;
    geometry->visible_items = box_rows > header_rows ? box_rows - header_rows : 0;
    return true;
}

static void desk_render_open_menu(const desk_terminal_t *terminal,
                                  const desk_session_t *session)
{
    const desk_open_menu_t *menu = &session->open_menu;
    if (menu->level == DESK_MENU_CLOSED) {
        return;
    }

    desk_menu_geometry_t geometry;
    if (!desk_menu_geometry(terminal, session, &geometry)) {
        return;
    }
    int box_row = geometry.row;
    int box_col = geometry.col;
    int box_rows = geometry.rows;
    int box_cols = geometry.cols;
    int inner_cols = box_cols > 2 ? box_cols - 2 : box_cols;
    int visible_items = geometry.visible_items;

    char frame[262144];
    size_t used = 0;

    char line[512];
    const char *heading = "Open cube";
    if (menu->level == DESK_MENU_WORKSPACE) {
        heading = "Open cube from workspace";
    } else if (menu->level == DESK_MENU_NEW_COMMAND) {
        heading = "New cube";
    } else if (menu->level == DESK_MENU_SAVE_LAYOUT) {
        heading = "Save layout";
    } else if (menu->level == DESK_MENU_LAYOUT_PICKER) {
        heading = "Load layout";
    } else if (menu->level == DESK_MENU_BINDINGS) {
        heading = "Key bindings";
    } else if (menu->level == DESK_MENU_BINDING_EDIT) {
        heading = "Edit binding";
    } else if (menu->level == DESK_MENU_MACROS) {
        heading = "Macros";
    } else if (menu->level == DESK_MENU_MACRO_NAME) {
        heading = "Macro name";
    } else if (menu->level == DESK_MENU_MACRO_TEXT) {
        heading = "Macro text";
    } else if (menu->level == DESK_MENU_MACRO_TARGET) {
        heading = "Macro target";
    }

    for (int row = 0; row < box_rows; ++row) {
        int terminal_row = box_row + row + 1;
        int terminal_col = box_col + 1;
        int length = snprintf(line, sizeof(line), "\x1b[%d;%dH",
                              terminal_row, terminal_col);
        if (length > 0 && (size_t)length < sizeof(line)) {
            append_text(frame, sizeof(frame), &used, line);
        }
        append_text(frame, sizeof(frame), &used, "\x1b[48;5;236m");
        for (int col = 0; col < box_cols; ++col) {
            append_text(frame, sizeof(frame), &used, " ");
        }
    }

    int length = snprintf(line, sizeof(line),
                          "\x1b[%d;%dH\x1b[48;5;236m\x1b[1m%.*s\x1b[0m",
                          box_row + 2, box_col + 3, inner_cols, heading);
    if (length > 0 && (size_t)length < sizeof(line)) {
        append_text(frame, sizeof(frame), &used, line);
    }
    int workspace_name_cols = inner_cols > 11 ? inner_cols - 11 : 0;
    length = snprintf(line, sizeof(line),
                      "\x1b[%d;%dH\x1b[48;5;236mWorkspace: %.*s\x1b[0m",
                      box_row + 3, box_col + 3, workspace_name_cols,
                      menu->workspace.name);
    if (length > 0 && (size_t)length < sizeof(line)) {
        append_text(frame, sizeof(frame), &used, line);
    }
    length = snprintf(line, sizeof(line),
                      "\x1b[%d;%dH\x1b[48;5;236m\x1b[2m%.*s\x1b[0m",
                      box_row + 4, box_col + 3, inner_cols,
                      menu->level == DESK_MENU_BINDINGS
                          ? "e edits. j/k or arrows scroll. q closes."
                          : menu->level == DESK_MENU_BINDING_EDIT
                          ? "Enter saves. Esc cancels. none unbinds."
                          : menu->level == DESK_MENU_MACROS
                          ? "Enter edits. u/n reorders. d deletes. q closes."
                          : menu->level == DESK_MENU_MACRO_NAME ||
                                    menu->level == DESK_MENU_MACRO_TEXT
                                ? "Enter continues. Esc cancels."
                          : menu->level == DESK_MENU_MACRO_TARGET
                          ? "Enter selects target. j/k or arrows scroll."
                          : menu->level == DESK_MENU_LAYOUT_PICKER
                          ? "Type to filter. Enter loads. q closes."
                          : menu->level == DESK_MENU_ROOT
                          ? "Enter selects. m macros. q closes. Esc goes back."
                          : "Enter selects. q closes. Esc goes back.");
    if (length > 0 && (size_t)length < sizeof(line)) {
        append_text(frame, sizeof(frame), &used, line);
    }

    int row = box_row + 6;
    if (menu->level == DESK_MENU_NEW_COMMAND ||
        menu->level == DESK_MENU_SAVE_LAYOUT ||
        menu->level == DESK_MENU_BINDING_EDIT ||
        menu->level == DESK_MENU_MACRO_NAME ||
        menu->level == DESK_MENU_MACRO_TEXT) {
        int prompt_cols = inner_cols > 9 ? inner_cols - 9 : 0;
        const char *command = menu->command;
        size_t command_length = strlen(command);
        if ((int)command_length > prompt_cols) {
            command += command_length - (size_t)prompt_cols;
        }
        const char *prompt =
            menu->level == DESK_MENU_SAVE_LAYOUT
                ? "Name:"
                : menu->level == DESK_MENU_BINDING_EDIT
                      ? "Key:"
                      : menu->level == DESK_MENU_MACRO_NAME
                            ? "Name:"
                            : menu->level == DESK_MENU_MACRO_TEXT ? "Text:"
                                                                  : "Command:";
        length = snprintf(line, sizeof(line),
                          "\x1b[%d;%dH\x1b[48;5;236m%s %.*s\x1b[0m",
                          row, box_col + 3, prompt, prompt_cols, command);
        if (length > 0 && (size_t)length < sizeof(line)) {
            append_text(frame, sizeof(frame), &used, line);
        }
        goto render_menu_status;
    }
    if (menu->level == DESK_MENU_LAYOUT_PICKER) {
        int prompt_cols = inner_cols > 8 ? inner_cols - 8 : 0;
        length = snprintf(line, sizeof(line),
                          "\x1b[%d;%dH\x1b[48;5;236mSearch: %.*s\x1b[0m",
                          row, box_col + 3, prompt_cols, menu->command);
        if (length > 0 && (size_t)length < sizeof(line)) {
            append_text(frame, sizeof(frame), &used, line);
        }
        row++;
    }

    if (menu->item_count == 0) {
        length = snprintf(line, sizeof(line),
                          "\x1b[%d;%dH\x1b[48;5;236m%.*s\x1b[0m",
                          row, box_col + 3, inner_cols,
                          menu->status[0] != '\0' ? menu->status
                                                   : "No entries");
        if (length > 0 && (size_t)length < sizeof(line)) {
            append_text(frame, sizeof(frame), &used, line);
        }
    }
    for (size_t visible_index = 0;
         visible_index < menu->item_count && (int)visible_index < visible_items;
         ++visible_index) {
        size_t i = menu->scroll_offset + visible_index;
        if (i >= menu->item_count) {
            break;
        }
        const desk_menu_item_t *item = &menu->items[i];
        const char *marker = i == menu->selected ? ">" : " ";
        const char *style = i == menu->selected
                                ? "\x1b[1;7;48;5;236m"
                                : (item->disabled ? "\x1b[2;48;5;236m"
                                                  : "\x1b[48;5;236m");
        if (item->kind == DESK_MENU_ITEM_NEW) {
            int name_cols = inner_cols > 2 ? inner_cols - 2 : 0;
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 3, style, inner_cols,
                              marker);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 5, style, name_cols, "New");
        } else if (item->kind == DESK_MENU_ITEM_MACRO_NEW) {
            int name_cols = inner_cols > 2 ? inner_cols - 2 : 0;
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 3, style, inner_cols,
                              marker);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 5, style, name_cols,
                              "New macro");
        } else if (item->kind == DESK_MENU_ITEM_MACRO) {
            int name_cols = inner_cols > 8 ? inner_cols - 8 : 0;
            const cubicle_workspace_macro_info_t *macro =
                item->macro_index < session->macro_count
                    ? &session->macros[item->macro_index]
                    : NULL;
            char macro_text[160];
            if (macro != NULL) {
                snprintf(macro_text, sizeof(macro_text), "%llu. %s",
                         (unsigned long long)macro->ordinal, macro->name);
            } else {
                snprintf(macro_text, sizeof(macro_text), "missing macro");
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 3, style, inner_cols,
                              marker);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 5, style, name_cols,
                              macro_text);
        } else if (item->kind == DESK_MENU_ITEM_MACRO_TARGET) {
            int name_cols = inner_cols > 2 ? inner_cols - 2 : 0;
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 3, style, inner_cols,
                              marker);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 5, style, name_cols,
                              item->description);
        } else if (item->kind == DESK_MENU_ITEM_PROCESS) {
            const char *name = item->process.friendly_name[0] != '\0'
                                   ? item->process.friendly_name
                                   : item->process.id;
            int name_cols = inner_cols > 2 ? inner_cols - 2 : 0;
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 3, style, inner_cols,
                              marker);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 5, style, name_cols, name);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
        } else if (item->kind == DESK_MENU_ITEM_LAYOUT) {
            int name_cols = inner_cols > 2 ? inner_cols - 2 : 0;
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 3, style, inner_cols,
                              marker);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 5, style, name_cols,
                              item->layout_name);
        } else if (item->kind == DESK_MENU_ITEM_BINDING) {
            int command_cols = inner_cols / 3;
            if (command_cols < 18) {
                command_cols = 18;
            }
            if (command_cols > inner_cols - 18) {
                command_cols = inner_cols > 18 ? inner_cols - 18 : inner_cols;
            }
            int remaining_cols = inner_cols - command_cols - 3;
            int description_cols = remaining_cols / 2;
            int key_cols = remaining_cols - description_cols;
            if (description_cols < 0) {
                description_cols = 0;
            }
            if (key_cols < 0) {
                key_cols = 0;
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 3, style, inner_cols,
                              marker);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            char command_text[96];
            snprintf(command_text, sizeof(command_text), "[%s]",
                     item->command_name);
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 5, style, command_cols,
                              command_text);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 6 + command_cols, style,
                              description_cols, item->description);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 7 + command_cols +
                                       description_cols,
                              style, key_cols, item->key_text);
        } else {
            int name_cols = inner_cols > 13 ? inner_cols - 13 : 0;
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%s%.*s\x1b[0m",
                              row, box_col + 3, style, inner_cols, marker);
            if (length > 0 && (size_t)length < sizeof(line)) {
                append_text(frame, sizeof(frame), &used, line);
            }
            length = snprintf(line, sizeof(line),
                              "\x1b[%d;%dH%sworkspace: %.*s  >\x1b[0m",
                              row, box_col + 5, style, name_cols,
                              item->workspace.name);
        }
        if (length > 0 && (size_t)length < sizeof(line)) {
            append_text(frame, sizeof(frame), &used, line);
        }
        ++row;
    }

render_menu_status:
    if (menu->status[0] != '\0' && row + 1 < box_row + box_rows) {
        length = snprintf(line, sizeof(line),
                          "\x1b[%d;%dH\x1b[2;48;5;236m%.*s\x1b[0m",
                          row + 1, box_col + 3, inner_cols, menu->status);
        if (length > 0 && (size_t)length < sizeof(line)) {
            append_text(frame, sizeof(frame), &used, line);
        }
    }
    append_text(frame, sizeof(frame), &used, "\x1b[0m");
    (void)cubeui_write_all(STDOUT_FILENO, frame, used);
}

static int read_and_render_pane_output(const desk_terminal_t *terminal,
                                       desk_session_t *session,
                                       size_t pane_index,
                                       bool *output_seen)
{
    desk_pane_t *pane = &session->panes[pane_index];
    bool pane_changed = false;

    if (pane->attachment == NULL) {
        return 0;
    }

    for (int burst = 0; burst < DESK_OUTPUT_READ_BURST; ++burst) {
        unsigned char output[4096];
        ssize_t nread = cubicle_attachment_read(pane->attachment, output,
                                                sizeof(output));
        if (nread < 0) {
            const cubicle_error_t *last =
                cubicle_attachment_last_error(pane->attachment);
            desk_debug_log("event=read_failed pane=%zu process=%s errno=%d message=\"%s\"",
                           pane_index + 1, pane->process.friendly_name, errno,
                           last != NULL ? last->message : "");
            desk_detach_pane_after_read_failure(session, pane_index);
            desk_debug_log("event=pane_detached pane=%zu process=%s reason=read_failed",
                           pane_index + 1, pane->process.friendly_name);
            return 0;
        }
        if (nread == 0) {
            break;
        }
        uint64_t read_offset =
            cubicle_attachment_read_offset(pane->attachment,
                                           CUBICLE_STREAM_TTY);
        desk_debug_log("event=read_ok pane=%zu process=%s bytes=%zd burst=%d read_offset=%llu",
                       pane_index + 1, pane->process.friendly_name, nread,
                       burst, (unsigned long long)read_offset);
        if (pane->terminal_model != NULL &&
            cubicle_terminal_model_feed(pane->terminal_model, output,
                                        (size_t)nread) < 0) {
            int saved_errno = errno;
            desk_debug_log("event=model_feed_failed pane=%zu process=%s bytes=%zd errno=%d action=resync",
                           pane_index + 1, pane->process.friendly_name, nread,
                           saved_errno);
            if (reload_pane_snapshot(pane) < 0) {
                desk_debug_log("event=model_feed_resync_failed pane=%zu process=%s errno=%d",
                               pane_index + 1,
                               pane->process.friendly_name, errno);
                return -1;
            }
            desk_debug_log("event=model_feed_resync_ok pane=%zu process=%s",
                           pane_index + 1, pane->process.friendly_name);
            pane_changed = true;
            if (output_seen != NULL) {
                *output_seen = true;
            }
            break;
        }
        if (desk_drain_model_scrollback(pane) < 0) {
            desk_debug_log("event=scrollback_drain_failed pane=%zu process=%s errno=%d",
                           pane_index + 1, pane->process.friendly_name, errno);
            return -1;
        }
        pane_changed = true;
        if (output_seen != NULL) {
            *output_seen = true;
        }
    }

    if (pane_changed && refresh_pane_from_model(pane) < 0) {
        desk_debug_log("event=refresh_from_model_failed pane=%zu process=%s errno=%d",
                       pane_index + 1, pane->process.friendly_name, errno);
        return -1;
    }

    if (pane_changed) {
        desk_cursor_erase(terminal, session);
        desk_render_dirty_cube_grid(terminal, &session->layout,
                                    (int)pane_index + 1, pane,
                                    session->mouse_titles);
        desk_cursor_reset_blink(session);
        desk_cursor_render(terminal, session);
        desk_render_notice(terminal, session);
    }
    return 0;
}

static int write_active_pane(desk_session_t *session,
                             const unsigned char *buffer,
                             size_t length)
{
    int active = session->layout.active_pane_id;
    if (active <= 0 || (size_t)active > session->pane_count) {
        return -1;
    }
    desk_pane_t *pane = &session->panes[(size_t)active - 1];
    if (desk_pane_return_to_live(pane)) {
        session->redraw_requested = true;
    }
    if (pane->attachment == NULL) {
        desk_debug_log("event=write_skipped pane=%d process=%s reason=detached",
                       active, pane->process.friendly_name);
        return 0;
    }
    return cubicle_attachment_write(pane->attachment, buffer, length) < 0 ? -1
                                                                         : 0;
}

static int write_pane_by_id(desk_session_t *session,
                            int pane_id,
                            const unsigned char *buffer,
                            size_t length)
{
    if (pane_id <= 0 || (size_t)pane_id > session->pane_count) {
        return write_active_pane(session, buffer, length);
    }
    desk_pane_t *pane = &session->panes[(size_t)pane_id - 1];
    if (pane->attachment == NULL) {
        return write_active_pane(session, buffer, length);
    }
    return cubicle_attachment_write(pane->attachment, buffer, length) < 0 ? -1
                                                                         : 0;
}

static int flush_active_input(desk_session_t *session,
                              const unsigned char *input,
                              size_t start,
                              size_t end)
{
    if (end <= start) {
        return 0;
    }
    return write_active_pane(session, input + start, end - start);
}

static int desk_load_workspace_macros(desk_session_t *session,
                                      char *error,
                                      size_t error_size)
{
    cubicle_workspace_macro_info_t *macros = NULL;
    size_t count = 0;
    cubicle_error_code_t code = cubicle_workspace_macro_list(
        session->manager, session->workspace.id, &macros, &count);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        if (last != NULL && last->code == CUBICLE_ERR_UNSUPPORTED) {
            session->macro_count = 0;
            return 0;
        }
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "macro list failed");
        return -1;
    }
    if (count > CUBICLE_WORKSPACE_MACRO_MAX) {
        count = CUBICLE_WORKSPACE_MACRO_MAX;
    }
    memcpy(session->macros, macros, count * sizeof(session->macros[0]));
    session->macro_count = count;
    cubicle_workspace_macro_list_free(macros);
    return 0;
}

static bool desk_macro_command_ordinal(const char *command,
                                       uint64_t *ordinal)
{
    const char prefix[] = "macro.";
    if (strncmp(command, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    char *end = NULL;
    unsigned long long value =
        strtoull(command + sizeof(prefix) - 1, &end, 10);
    if (end == NULL || *end != '\0' || value == 0) {
        return false;
    }
    *ordinal = (uint64_t)value;
    return true;
}

static cubicle_workspace_macro_info_t *desk_find_macro_by_ordinal(
    desk_session_t *session,
    uint64_t ordinal)
{
    for (size_t i = 0; i < session->macro_count; ++i) {
        if (session->macros[i].ordinal == ordinal) {
            return &session->macros[i];
        }
    }
    return NULL;
}

static cubicle_workspace_macro_info_t *desk_find_macro_by_command(
    desk_session_t *session,
    const char *command)
{
    uint64_t ordinal = 0;
    return desk_macro_command_ordinal(command, &ordinal)
               ? desk_find_macro_by_ordinal(session, ordinal)
               : NULL;
}

static void desk_macro_command_name(uint64_t ordinal,
                                    char *buffer,
                                    size_t size)
{
    snprintf(buffer, size, "macro.%llu", (unsigned long long)ordinal);
}

static int desk_save_macro(desk_session_t *session,
                           const cubicle_workspace_macro_info_t *macro,
                           char *error,
                           size_t error_size)
{
    cubicle_workspace_macro_save_options_t options;
    memset(&options, 0, sizeof(options));
    options.workspace_id = session->workspace.id;
    options.ordinal = macro->ordinal;
    options.name = macro->name;
    options.text = macro->text;
    options.target_pane = macro->target_pane;
    options.key_name = macro->key_name;
    cubicle_workspace_macro_info_t saved;
    cubicle_error_code_t code =
        cubicle_workspace_macro_save(session->manager, &options, &saved);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last = cubicle_client_last_error(session->manager);
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "macro save failed");
        return -1;
    }
    return desk_load_workspace_macros(session, error, error_size);
}

static int desk_run_macro(desk_session_t *session,
                          const cubicle_workspace_macro_info_t *macro)
{
    char buffer[CUBICLE_WORKSPACE_MACRO_TEXT_MAX + 2];
    int length = snprintf(buffer, sizeof(buffer), "%s\n", macro->text);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        return -1;
    }
    int pane_id = macro->target_pane == 0 || macro->target_pane > INT_MAX
                      ? session->layout.active_pane_id
                      : (int)macro->target_pane;
    return write_pane_by_id(session, pane_id, (const unsigned char *)buffer,
                            (size_t)length);
}

static int desk_open_workspace_from_menu(desk_session_t *session,
                                         const desk_terminal_t *terminal,
                                         const cubicle_workspace_info_t *workspace,
                                         bool *menu_closed);

static void desk_begin_new_process_prompt(desk_session_t *session,
                                          const cubicle_workspace_info_t *workspace,
                                          desk_menu_level_t return_level)
{
    desk_open_menu_t *menu = &session->open_menu;
    cubicle_workspace_info_t target_workspace = *workspace;
    memset(menu->command, 0, sizeof(menu->command));
    menu->command_length = 0;
    menu->item_count = 0;
    menu->selected = 0;
    menu->level = DESK_MENU_NEW_COMMAND;
    menu->prompt_return_level = return_level;
    menu->workspace = target_workspace;
    menu->status[0] = '\0';
}

static int desk_generated_process_name(char *buffer,
                                       size_t buffer_size,
                                       const char *command)
{
    while (*command != '\0' && isspace((unsigned char)*command)) {
        ++command;
    }
    if (*command == '\'' || *command == '"') {
        ++command;
    }
    const char *end = command;
    while (*end != '\0' && !isspace((unsigned char)*end) &&
           *end != '\'' && *end != '"') {
        ++end;
    }
    while (end > command && (*(end - 1) == '/' || *(end - 1) == '\\')) {
        --end;
    }
    const char *base = end;
    while (base > command && *(base - 1) != '/' && *(base - 1) != '\\') {
        --base;
    }
    if (base >= end) {
        return -1;
    }

    size_t used = 0;
    for (const char *cursor = base; cursor < end; ++cursor) {
        char c = *cursor;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-') {
            if (used + 1 >= buffer_size) {
                break;
            }
            buffer[used++] = c;
        } else if (used > 0 && buffer[used - 1] != '-') {
            if (used + 1 >= buffer_size) {
                break;
            }
            buffer[used++] = '-';
        }
    }
    while (used > 0 && buffer[used - 1] == '-') {
        --used;
    }
    if (used == 0) {
        return -1;
    }
    buffer[used] = '\0';
    return 0;
}

static int desk_generated_process_name_with_suffix(char *buffer,
                                                   size_t buffer_size,
                                                   const char *base_name,
                                                   int suffix)
{
    int length = suffix == 0
                     ? snprintf(buffer, buffer_size, "%s", base_name)
                     : snprintf(buffer, buffer_size, "%s-%d", base_name,
                                suffix);
    return length < 0 || (size_t)length >= buffer_size ? -1 : 0;
}

static int desk_start_new_process_from_prompt(desk_session_t *session,
                                              const desk_terminal_t *terminal,
                                              bool *menu_closed)
{
    desk_open_menu_t *menu = &session->open_menu;
    const char *command = menu->command;
    while (*command != '\0' && isspace((unsigned char)*command)) {
        ++command;
    }
    if (*command == '\0') {
        snprintf(menu->status, sizeof(menu->status), "command is empty");
        return 0;
    }

    int active = session->layout.active_pane_id;
    unsigned int rows = 0;
    unsigned int cols = 0;
    if (active <= 0 ||
        pane_content_size(terminal, &session->layout, active, &rows, &cols) <
            0) {
        snprintf(menu->status, sizeof(menu->status), "no active pane");
        return 0;
    }

    char base_name[CUBICLE_NAME_MAX];
    if (desk_generated_process_name(base_name, sizeof(base_name), command) <
        0) {
        snprintf(base_name, sizeof(base_name), "cube");
    }

    const char *argv[] = {"sh", "-lc", command};
    cubicle_process_info_t process;
    cubicle_error_code_t code = CUBICLE_ERR_INTERNAL;
    for (int suffix = 0; suffix < 1000; ++suffix) {
        char candidate_name[CUBICLE_NAME_MAX];
        if (desk_generated_process_name_with_suffix(
                candidate_name, sizeof(candidate_name), base_name, suffix) <
            0) {
            continue;
        }

        cubicle_process_start_options_t options;
        memset(&options, 0, sizeof(options));
        options.workspace_id = menu->workspace.id;
        options.friendly_name = candidate_name;
        options.mode = CUBICLE_PROCESS_TTY;
        options.stdin_policy = CUBICLE_STDIN_OPEN;
        options.cwd = menu->workspace.directory[0] != '\0'
                          ? menu->workspace.directory
                          : NULL;
        options.argv = argv;
        options.argc = sizeof(argv) / sizeof(argv[0]);
        options.tty_rows = rows;
        options.tty_cols = cols;

        memset(&process, 0, sizeof(process));
        code = cubicle_process_start(session->manager, &options, &process);
        if (code == CUBICLE_OK) {
            break;
        }
        if (code != CUBICLE_ERR_ALREADY_EXISTS) {
            const cubicle_error_t *last =
                cubicle_client_last_error(session->manager);
            snprintf(menu->status, sizeof(menu->status), "%s",
                     last != NULL && last->message[0] != '\0'
                         ? last->message
                         : "process start failed");
            return 0;
        }
    }
    if (code != CUBICLE_OK) {
        snprintf(menu->status, sizeof(menu->status),
                 "failed to allocate a unique process name");
        return 0;
    }

    char error[256];
    if (desk_commit_pending_split_process(session, terminal, &process, error,
                                          sizeof(error)) < 0) {
        cubicle_process_terminate_options_t terminate_options;
        memset(&terminate_options, 0, sizeof(terminate_options));
        terminate_options.grace_period_ms = 0;
        terminate_options.force_after_grace = true;
        (void)cubicle_process_terminate(session->manager, process.id,
                                        &terminate_options);
        snprintf(menu->status, sizeof(menu->status), "%s", error);
        return 0;
    }

    desk_close_open_menu(session);
    if (!session->mouse_titles) {
        desk_menu_disable_mouse();
    }
    if (menu_closed != NULL) {
        *menu_closed = true;
    }
    return 0;
}

static int desk_open_menu_select(desk_session_t *session,
                                 const desk_terminal_t *terminal,
                                 bool *menu_closed)
{
    desk_open_menu_t *menu = &session->open_menu;
    if (menu->level == DESK_MENU_SAVE_LAYOUT) {
        const char *name = menu->command;
        while (*name != '\0' && isspace((unsigned char)*name)) {
            ++name;
        }
        if (!layout_name_is_safe(name)) {
            snprintf(menu->status, sizeof(menu->status),
                     "use letters, numbers, dash, underscore, or dot");
            return 0;
        }
        if (desk_save_named_layout(session, name) < 0) {
            snprintf(menu->status, sizeof(menu->status),
                     "failed to save layout");
            return 0;
        }
        desk_close_open_menu(session);
        if (!session->mouse_titles) {
            desk_menu_disable_mouse();
        }
        if (menu_closed != NULL) {
            *menu_closed = true;
        }
        return 0;
    }
    if (menu->selected >= menu->item_count ||
        menu->items[menu->selected].disabled) {
        return 0;
    }
    desk_menu_item_t selected = menu->items[menu->selected];
    if (selected.kind == DESK_MENU_ITEM_BINDING) {
        return 0;
    }
    if (selected.kind == DESK_MENU_ITEM_MACRO_NEW) {
        desk_begin_macro_name_prompt(session, session->macro_count);
        return 0;
    }
    if (selected.kind == DESK_MENU_ITEM_MACRO) {
        desk_begin_macro_name_prompt(session, selected.macro_index);
        return 0;
    }
    if (selected.kind == DESK_MENU_ITEM_MACRO_TARGET) {
        return desk_apply_macro_target_selection(session,
                                                 selected.target_pane);
    }
    if (selected.kind == DESK_MENU_ITEM_LAYOUT) {
        char error[256];
        if (desk_apply_named_layout(session, terminal, selected.layout_name,
                                    error, sizeof(error)) < 0) {
            snprintf(menu->status, sizeof(menu->status), "%s", error);
            return 0;
        }
        desk_close_open_menu(session);
        if (!session->mouse_titles) {
            desk_menu_disable_mouse();
        }
        if (menu_closed != NULL) {
            *menu_closed = true;
        }
        return 0;
    }
    if (selected.kind == DESK_MENU_ITEM_WORKSPACE) {
        return desk_open_workspace_from_menu(session, terminal,
                                             &selected.workspace,
                                             menu_closed);
    }
    if (selected.kind == DESK_MENU_ITEM_NEW) {
        desk_begin_new_process_prompt(session, &menu->workspace, menu->level);
        return 0;
    }

    char error[256];
    if (desk_commit_pending_split_process(session, terminal, &selected.process,
                                          error, sizeof(error)) < 0) {
        snprintf(menu->status, sizeof(menu->status), "%s", error);
        return 0;
    }
    desk_close_open_menu(session);
    if (!session->mouse_titles) {
        desk_menu_disable_mouse();
    }
    if (menu_closed != NULL) {
        *menu_closed = true;
    }
    return 0;
}

static bool parse_menu_arrow(const unsigned char *input,
                             size_t length,
                             size_t offset,
                             char *arrow,
                             size_t *consumed)
{
    if (offset + 2 >= length || input[offset] != 0x1b ||
        input[offset + 1] != '[') {
        return false;
    }
    size_t final = offset + 2;
    while (final < length &&
           !(input[final] == 'A' || input[final] == 'B' ||
             input[final] == 'C' || input[final] == 'D')) {
        ++final;
    }
    if (final >= length) {
        return false;
    }
    *arrow = (char)input[final];
    *consumed = final - offset + 1;
    return true;
}

static bool parse_sgr_mouse(const unsigned char *input,
                            size_t length,
                            size_t offset,
                            int *button,
                            int *row,
                            int *col,
                            bool *press,
                            size_t *consumed)
{
    if (offset + 6 >= length || input[offset] != 0x1b ||
        input[offset + 1] != '[' || input[offset + 2] != '<') {
        return false;
    }
    size_t cursor = offset + 3;
    int values[3] = {0, 0, 0};
    for (int index = 0; index < 3; ++index) {
        if (cursor >= length || input[cursor] < '0' ||
            input[cursor] > '9') {
            return false;
        }
        while (cursor < length && input[cursor] >= '0' &&
               input[cursor] <= '9') {
            values[index] = values[index] * 10 + (input[cursor] - '0');
            ++cursor;
        }
        if (index < 2) {
            if (cursor >= length || input[cursor] != ';') {
                return false;
            }
            ++cursor;
        }
    }
    if (cursor >= length || (input[cursor] != 'M' && input[cursor] != 'm')) {
        return false;
    }
    *button = values[0];
    *col = values[1];
    *row = values[2];
    *press = input[cursor] == 'M';
    *consumed = cursor - offset + 1;
    return true;
}

static bool is_incomplete_sgr_mouse(const unsigned char *input,
                                    size_t length,
                                    size_t offset)
{
    const unsigned char prefix[] = {0x1b, '[', '<'};
    size_t available = length - offset;
    if (available == 1) {
        return false;
    }
    if (available == 0 || available > sizeof(prefix)) {
        available = sizeof(prefix);
    }
    if (available < sizeof(prefix)) {
        return memcmp(input + offset, prefix, available) == 0;
    }
    if (memcmp(input + offset, prefix, sizeof(prefix)) != 0) {
        return false;
    }

    size_t cursor = offset + sizeof(prefix);
    int separators = 0;
    while (cursor < length) {
        unsigned char ch = input[cursor];
        if (ch == 'M' || ch == 'm') {
            return false;
        }
        if (ch == ';') {
            ++separators;
        } else if (ch < '0' || ch > '9') {
            return false;
        }
        ++cursor;
    }
    return separators <= 2;
}

static void desk_save_pending_input(desk_session_t *session,
                                    const unsigned char *input,
                                    size_t length,
                                    size_t offset)
{
    size_t pending = length - offset;
    if (pending > sizeof(session->pending_input)) {
        pending = 0;
    }
    if (pending > 0) {
        memcpy(session->pending_input, input + offset, pending);
    }
    session->pending_input_length = pending;
    session->pending_input_since_ms = pending > 0 ? desk_monotonic_ms() : 0;
}

static desk_menu_item_t *desk_menu_item_at_position(
    desk_session_t *session,
    const desk_terminal_t *terminal,
    int row,
    int col)
{
    desk_menu_geometry_t geometry;
    if (!desk_menu_geometry(terminal, session, &geometry) ||
        row < geometry.item_row ||
        row >= geometry.item_row + geometry.visible_items ||
        col < geometry.col + 1 ||
        col > geometry.col + geometry.cols) {
        return NULL;
    }
    size_t index =
        session->open_menu.scroll_offset + (size_t)(row - geometry.item_row);
    if (index >= session->open_menu.item_count) {
        return NULL;
    }
    session->open_menu.selected = index;
    return &session->open_menu.items[index];
}

static int desk_open_workspace_from_menu(desk_session_t *session,
                                         const desk_terminal_t *terminal,
                                         const cubicle_workspace_info_t *workspace,
                                         bool *menu_closed)
{
    char error[256];
    if (desk_switch_workspace(session, terminal, workspace, error,
                              sizeof(error)) != 0) {
        snprintf(session->open_menu.status, sizeof(session->open_menu.status),
                 "%s", error);
        return 0;
    }
    desk_close_open_menu(session);
    desk_menu_disable_mouse();
    if (menu_closed != NULL) {
        *menu_closed = true;
    }
    return 0;
}

static int handle_open_menu_input(desk_session_t *session,
                                  const desk_terminal_t *terminal,
                                  const unsigned char *input,
                                  size_t length,
                                  bool *menu_closed)
{
    for (size_t i = 0; i < length; ++i) {
        if (session->open_menu.level == DESK_MENU_NEW_COMMAND ||
            session->open_menu.level == DESK_MENU_SAVE_LAYOUT ||
            session->open_menu.level == DESK_MENU_BINDING_EDIT ||
            session->open_menu.level == DESK_MENU_MACRO_NAME ||
            session->open_menu.level == DESK_MENU_MACRO_TEXT) {
            unsigned char ch = input[i];
            if (ch == '\r' || ch == '\n') {
                int result = 0;
                if (session->open_menu.level == DESK_MENU_NEW_COMMAND) {
                    result = desk_start_new_process_from_prompt(
                        session, terminal, menu_closed);
                } else if (session->open_menu.level ==
                           DESK_MENU_BINDING_EDIT) {
                    result = desk_apply_binding_edit(session);
                } else if (session->open_menu.level ==
                           DESK_MENU_MACRO_NAME) {
                    desk_begin_macro_text_prompt(session);
                } else if (session->open_menu.level ==
                           DESK_MENU_MACRO_TEXT) {
                    desk_begin_macro_target_prompt(session);
                } else {
                    result = desk_open_menu_select(session, terminal,
                                                  menu_closed);
                }
                if (result < 0) {
                    return -1;
                }
                continue;
            }
            if (ch == 0x1b) {
                if (session->open_menu.level == DESK_MENU_BINDING_EDIT) {
                    char command[CUBICLE_DESK_COMMAND_NAME_MAX];
                    snprintf(command, sizeof(command), "%s",
                             session->open_menu.edit_command);
                    desk_begin_bindings_overlay(session);
                    desk_select_binding_command(session, command);
                } else if (session->open_menu.level == DESK_MENU_MACRO_NAME ||
                           session->open_menu.level == DESK_MENU_MACRO_TEXT ||
                           session->open_menu.level ==
                               DESK_MENU_MACRO_TARGET) {
                    desk_begin_macro_list(session);
                } else if (session->open_menu.level == DESK_MENU_SAVE_LAYOUT) {
                    desk_close_open_menu(session);
                    if (!session->mouse_titles) {
                        desk_menu_disable_mouse();
                    }
                    if (menu_closed != NULL) {
                        *menu_closed = true;
                    }
                } else if (session->open_menu.prompt_return_level ==
                    DESK_MENU_WORKSPACE) {
                    cubicle_workspace_info_t workspace =
                        session->open_menu.workspace;
                    (void)desk_load_process_menu(session, &workspace);
                } else {
                    (void)desk_open_root_menu(session);
                }
                continue;
            }
            if (ch == 0x08 || ch == 0x7f) {
                if (session->open_menu.command_length > 0) {
                    session->open_menu.command
                        [--session->open_menu.command_length] = '\0';
                }
                session->open_menu.status[0] = '\0';
                continue;
            }
            if (ch == 0x15) {
                memset(session->open_menu.command, 0,
                       sizeof(session->open_menu.command));
                session->open_menu.command_length = 0;
                session->open_menu.status[0] = '\0';
                continue;
            }
            if (ch >= 0x20 && ch < 0x7f &&
                session->open_menu.command_length + 1 <
                    sizeof(session->open_menu.command)) {
                session->open_menu.command
                    [session->open_menu.command_length++] = (char)ch;
                session->open_menu.command
                    [session->open_menu.command_length] = '\0';
                session->open_menu.status[0] = '\0';
            }
            continue;
        }
        if (session->open_menu.level == DESK_MENU_LAYOUT_PICKER) {
            size_t picker_consumed = 0;
            char picker_arrow = '\0';
            if (parse_menu_arrow(input, length, i, &picker_arrow,
                                 &picker_consumed)) {
                desk_menu_geometry_t geometry;
                int visible_items =
                    desk_menu_geometry(terminal, session, &geometry)
                        ? geometry.visible_items
                        : 0;
                if (picker_arrow == 'A') {
                    desk_menu_move_selection(&session->open_menu, -1,
                                             visible_items);
                } else if (picker_arrow == 'B') {
                    desk_menu_move_selection(&session->open_menu, 1,
                                             visible_items);
                }
                i += picker_consumed - 1;
                continue;
            }
            unsigned char ch = input[i];
            if (ch == '\r' || ch == '\n') {
                if (desk_open_menu_select(session, terminal, menu_closed) < 0) {
                    return -1;
                }
                continue;
            }
            if (ch == 0x08 || ch == 0x7f) {
                if (session->open_menu.command_length > 0) {
                    session->open_menu.command
                        [--session->open_menu.command_length] = '\0';
                    (void)desk_load_layout_picker_items(session);
                }
                continue;
            }
            if (ch == 0x15) {
                memset(session->open_menu.command, 0,
                       sizeof(session->open_menu.command));
                session->open_menu.command_length = 0;
                (void)desk_load_layout_picker_items(session);
                continue;
            }
            if (ch == 0x1b) {
                desk_close_open_menu(session);
                if (!session->mouse_titles) {
                    desk_menu_disable_mouse();
                }
                if (menu_closed != NULL) {
                    *menu_closed = true;
                }
                continue;
            }
            if (ch >= 0x20 && ch < 0x7f &&
                (ch != 'q' || session->open_menu.command_length > 0) &&
                session->open_menu.command_length + 1 <
                    sizeof(session->open_menu.command)) {
                session->open_menu.command
                    [session->open_menu.command_length++] = (char)ch;
                session->open_menu.command
                    [session->open_menu.command_length] = '\0';
                (void)desk_load_layout_picker_items(session);
                continue;
            }
        }
        if (is_incomplete_sgr_mouse(input, length, i)) {
            desk_save_pending_input(session, input, length, i);
            break;
        }
        size_t consumed = 0;
        int button = 0;
        int mouse_row = 0;
        int mouse_col = 0;
        bool mouse_press = false;
        if (parse_sgr_mouse(input, length, i, &button, &mouse_row,
                            &mouse_col, &mouse_press, &consumed)) {
            if (mouse_press && button == 0) {
                desk_menu_item_t *item = desk_menu_item_at_position(
                    session, terminal, mouse_row, mouse_col);
                if (item != NULL && !item->disabled) {
                    if (item->kind == DESK_MENU_ITEM_WORKSPACE) {
                        (void)desk_open_workspace_from_menu(
                            session, terminal, &item->workspace, menu_closed);
                    } else {
                        (void)desk_open_menu_select(session, terminal,
                                                   menu_closed);
                    }
                }
            }
            i += consumed - 1;
            continue;
        }
        char arrow = '\0';
        if (parse_menu_arrow(input, length, i, &arrow, &consumed)) {
            desk_menu_geometry_t geometry;
            int visible_items =
                desk_menu_geometry(terminal, session, &geometry)
                    ? geometry.visible_items
                    : 0;
            if (arrow == 'A') {
                desk_menu_move_selection(&session->open_menu, -1,
                                         visible_items);
            } else if (arrow == 'B') {
                desk_menu_move_selection(&session->open_menu, 1,
                                         visible_items);
            } else if (arrow == 'C') {
                desk_menu_item_t *item =
                    session->open_menu.selected < session->open_menu.item_count
                        ? &session->open_menu.items[session->open_menu.selected]
                        : NULL;
                if (item != NULL &&
                    item->kind == DESK_MENU_ITEM_WORKSPACE &&
                    !item->disabled) {
                    cubicle_workspace_info_t workspace = item->workspace;
                    (void)desk_load_process_menu(session, &workspace);
                }
            } else if (arrow == 'D') {
                if (session->open_menu.level == DESK_MENU_WORKSPACE &&
                    session->pending_split != DESK_PENDING_SPLIT_NONE) {
                    desk_close_open_menu(session);
                    if (!session->mouse_titles) {
                        desk_menu_disable_mouse();
                    }
                    if (menu_closed != NULL) {
                        *menu_closed = true;
                    }
                } else if (session->open_menu.level == DESK_MENU_WORKSPACE) {
                    (void)desk_open_root_menu(session);
                }
            }
            i += consumed - 1;
            continue;
        }
        if (input[i] == 'j') {
            desk_menu_geometry_t geometry;
            int visible_items =
                desk_menu_geometry(terminal, session, &geometry)
                    ? geometry.visible_items
                    : 0;
            desk_menu_move_selection(&session->open_menu, 1, visible_items);
            continue;
        }
        if (input[i] == 'k') {
            desk_menu_geometry_t geometry;
            int visible_items =
                desk_menu_geometry(terminal, session, &geometry)
                    ? geometry.visible_items
                    : 0;
            desk_menu_move_selection(&session->open_menu, -1, visible_items);
            continue;
        }
        if (session->open_menu.level == DESK_MENU_ROOT &&
            input[i] == 'm') {
            desk_begin_macro_list(session);
            continue;
        }
        if (session->open_menu.level == DESK_MENU_BINDINGS &&
            input[i] == 'e') {
            desk_menu_item_t *item =
                session->open_menu.selected < session->open_menu.item_count
                    ? &session->open_menu.items[session->open_menu.selected]
                    : NULL;
            if (item != NULL && item->kind == DESK_MENU_ITEM_BINDING) {
                char command[CUBICLE_DESK_COMMAND_NAME_MAX];
                snprintf(command, sizeof(command), "%s", item->command_name);
                desk_begin_binding_edit_prompt(session, command);
            }
            continue;
        }
        if (session->open_menu.level == DESK_MENU_MACROS &&
            input[i] == 'd') {
            desk_menu_item_t *item =
                session->open_menu.selected < session->open_menu.item_count
                    ? &session->open_menu.items[session->open_menu.selected]
                    : NULL;
            if (item != NULL && item->kind == DESK_MENU_ITEM_MACRO &&
                item->macro_index < session->macro_count) {
                uint64_t ordinal = session->macros[item->macro_index].ordinal;
                cubicle_error_code_t code = cubicle_workspace_macro_delete(
                    session->manager, session->workspace.id, ordinal);
                if (code != CUBICLE_OK) {
                    const cubicle_error_t *last =
                        cubicle_client_last_error(session->manager);
                    snprintf(session->open_menu.status,
                             sizeof(session->open_menu.status), "%s",
                             last != NULL && last->message[0] != '\0'
                                 ? last->message
                                 : "macro delete failed");
                } else {
                    char error[256];
                    (void)desk_load_workspace_macros(session, error,
                                                     sizeof(error));
                    desk_begin_macro_list(session);
                    snprintf(session->open_menu.status,
                             sizeof(session->open_menu.status),
                             "deleted macro");
                }
            }
            continue;
        }
        if (session->open_menu.level == DESK_MENU_MACROS &&
            (input[i] == 'u' || input[i] == 'n')) {
            desk_menu_item_t *item =
                session->open_menu.selected < session->open_menu.item_count
                    ? &session->open_menu.items[session->open_menu.selected]
                    : NULL;
            if (item != NULL && item->kind == DESK_MENU_ITEM_MACRO &&
                item->macro_index < session->macro_count) {
                cubicle_workspace_macro_info_t *macro =
                    &session->macros[item->macro_index];
                uint64_t target =
                    input[i] == 'u'
                        ? (macro->ordinal > 1 ? macro->ordinal - 1 : 1)
                        : macro->ordinal + 1;
                if (target != macro->ordinal) {
                    cubicle_error_code_t code =
                        cubicle_workspace_macro_reorder(
                            session->manager, session->workspace.id,
                            macro->ordinal, target);
                    if (code != CUBICLE_OK) {
                        const cubicle_error_t *last =
                            cubicle_client_last_error(session->manager);
                        snprintf(session->open_menu.status,
                                 sizeof(session->open_menu.status), "%s",
                                 last != NULL && last->message[0] != '\0'
                                     ? last->message
                                     : "macro reorder failed");
                    } else {
                        char error[256];
                        (void)desk_load_workspace_macros(session, error,
                                                         sizeof(error));
                        desk_begin_macro_list(session);
                    }
                }
            }
            continue;
        }
        if (input[i] == '\r' || input[i] == '\n') {
            if (desk_open_menu_select(session, terminal, menu_closed) < 0) {
                return -1;
            }
            continue;
        }
        if (input[i] == 'q') {
            desk_close_open_menu(session);
            if (!session->mouse_titles) {
                desk_menu_disable_mouse();
            }
            if (menu_closed != NULL) {
                *menu_closed = true;
            }
            continue;
        }
        if (input[i] == 0x1b) {
            if (session->open_menu.level == DESK_MENU_WORKSPACE &&
                session->pending_split != DESK_PENDING_SPLIT_NONE) {
                desk_close_open_menu(session);
                if (!session->mouse_titles) {
                    desk_menu_disable_mouse();
                }
                if (menu_closed != NULL) {
                    *menu_closed = true;
                }
            } else if (session->open_menu.level == DESK_MENU_WORKSPACE) {
                (void)desk_open_root_menu(session);
            } else {
                desk_close_open_menu(session);
                if (!session->mouse_titles) {
                    desk_menu_disable_mouse();
                }
                if (menu_closed != NULL) {
                    *menu_closed = true;
                }
            }
            continue;
        }
    }
    return 0;
}

static const cubicle_desk_key_binding_t *desk_find_key_binding(
    const desk_session_t *session,
    bool uses_prefix,
    const unsigned char *input,
    size_t length,
    size_t *consumed,
    bool *partial)
{
    *partial = false;
    for (size_t i = 0; i < session->key_binding_count; ++i) {
        const cubicle_desk_key_binding_t *binding = &session->key_bindings[i];
        if (binding->unbind ||
            binding->uses_prefix != (uses_prefix ? 1 : 0)) {
            continue;
        }
        size_t compare_length = binding->sequence_length < length
                                    ? binding->sequence_length
                                    : length;
        if (compare_length > 0 &&
            memcmp(binding->sequence, input, compare_length) == 0) {
            if (binding->sequence_length <= length) {
                *consumed = binding->sequence_length;
                return binding;
            }
            *partial = true;
        }
    }
    return NULL;
}

static cubicle_workspace_macro_info_t *desk_find_custom_macro_binding(
    desk_session_t *session,
    bool uses_prefix,
    const unsigned char *input,
    size_t length,
    size_t *consumed,
    bool *partial)
{
    for (size_t i = 0; i < session->macro_count; ++i) {
        cubicle_workspace_macro_info_t *macro = &session->macros[i];
        if (macro->key_name[0] == '\0') {
            continue;
        }
        cubicle_desk_key_binding_t binding;
        char parse_error[128];
        if (desk_parse_binding_key_text(macro->key_name, &binding,
                                        parse_error,
                                        sizeof(parse_error)) < 0 ||
            binding.uses_prefix != uses_prefix) {
            continue;
        }
        size_t compare_length =
            length < binding.sequence_length ? length : binding.sequence_length;
        if (compare_length > 0 &&
            memcmp(binding.sequence, input, compare_length) == 0) {
            if (length >= binding.sequence_length) {
                *consumed = binding.sequence_length;
                return macro;
            }
            *partial = true;
        }
    }
    return NULL;
}

static cubicle_workspace_macro_info_t *desk_find_default_macro_binding(
    desk_session_t *session,
    const unsigned char *input,
    size_t length,
    size_t *consumed)
{
    if (length == 0 || input[0] < '0' || input[0] > '9') {
        return NULL;
    }
    uint64_t ordinal = input[0] == '0' ? 10 : (uint64_t)(input[0] - '0');
    cubicle_workspace_macro_info_t *macro =
        desk_find_macro_by_ordinal(session, ordinal);
    if (macro == NULL) {
        return NULL;
    }
    *consumed = 1;
    return macro;
}

static bool desk_sequence_matches(const unsigned char *expected,
                                  size_t expected_length,
                                  const unsigned char *input,
                                  size_t length,
                                  size_t *consumed,
                                  bool *partial)
{
    *partial = false;
    if (expected_length == 0) {
        return false;
    }
    size_t compare_length = expected_length < length ? expected_length : length;
    if (memcmp(expected, input, compare_length) != 0) {
        return false;
    }
    if (expected_length <= length) {
        *consumed = expected_length;
        return true;
    }
    *partial = true;
    return false;
}

static bool desk_escape_sequence_incomplete(const unsigned char *input,
                                            size_t length,
                                            size_t offset)
{
    if (offset >= length || input[offset] != 0x1b) {
        return false;
    }
    if (offset + 1 >= length) {
        return true;
    }
    if (input[offset + 1] == 'O') {
        return offset + 2 >= length;
    }
    if (input[offset + 1] != '[') {
        return false;
    }
    for (size_t i = offset + 2; i < length; ++i) {
        if (input[i] >= 0x40 && input[i] <= 0x7e) {
            return false;
        }
    }
    return true;
}

static void desk_save_pending_from(desk_session_t *session,
                                   const unsigned char *input,
                                   size_t length,
                                   size_t offset)
{
    if (offset >= length) {
        session->pending_input_length = 0;
        session->pending_input_since_ms = 0;
        return;
    }
    size_t remaining = length - offset;
    if (remaining > sizeof(session->pending_input)) {
        remaining = sizeof(session->pending_input);
    }
    memcpy(session->pending_input, input + offset, remaining);
    session->pending_input_length = remaining;
    session->pending_input_since_ms = remaining > 0 ? desk_monotonic_ms() : 0;
}

static int desk_write_prefix_sequence(desk_session_t *session)
{
    return write_active_pane(session, session->prefix_sequence,
                             session->prefix_sequence_length);
}

static int desk_write_input_sequence(desk_session_t *session,
                                     const unsigned char *input,
                                     size_t length)
{
    return write_active_pane(session, input, length);
}

static int desk_flush_expired_pending_input(desk_session_t *session)
{
    if (session->pending_input_length == 0 ||
        session->pending_input_since_ms <= 0) {
        return 0;
    }
    long long now_ms = desk_monotonic_ms();
    if (now_ms <= 0 ||
        now_ms - session->pending_input_since_ms <
            DESK_PENDING_INPUT_TIMEOUT_MS) {
        return 0;
    }
    size_t length = session->pending_input_length;
    unsigned char pending[sizeof(session->pending_input)];
    memcpy(pending, session->pending_input, length);
    session->pending_input_length = 0;
    session->pending_input_since_ms = 0;
    return desk_write_input_sequence(session, pending, length);
}

static int desk_handle_prefixed_sequence(desk_session_t *session,
                                         desk_terminal_t *terminal,
                                         const unsigned char *input,
                                         size_t length,
                                         size_t offset,
                                         size_t *consumed,
                                         bool *layout_changed,
                                         bool *quit_requested,
                                         bool *need_more)
{
    *need_more = false;
    bool partial = false;
    cubicle_workspace_macro_info_t *macro =
        desk_find_default_macro_binding(session, input + offset,
                                        length - offset, consumed);
    if (macro == NULL) {
        macro = desk_find_custom_macro_binding(session, true, input + offset,
                                              length - offset, consumed,
                                              &partial);
    }
    if (macro != NULL) {
        session->prefix_pending = false;
        if (desk_run_macro(session, macro) < 0) {
            return -1;
        }
        return 0;
    }
    const cubicle_desk_key_binding_t *binding =
        desk_find_key_binding(session, true, input + offset, length - offset,
                              consumed, &partial);
    if (binding != NULL) {
        session->prefix_pending = false;
        (void)desk_execute_command(session, terminal, binding->command,
                                   layout_changed, quit_requested);
        return 0;
    }
    if (partial || desk_escape_sequence_incomplete(input, length, offset)) {
        desk_save_pending_from(session, input, length, offset);
        *need_more = true;
        return 0;
    }

    if (desk_write_prefix_sequence(session) < 0) {
        return -1;
    }
    session->prefix_pending = false;
    if (desk_write_input_sequence(session, input + offset, 1) < 0) {
        return -1;
    }
    *consumed = 1;
    return 0;
}

static int desk_handle_direct_sequence(desk_session_t *session,
                                       desk_terminal_t *terminal,
                                       const unsigned char *input,
                                       size_t length,
                                       size_t offset,
                                       size_t *consumed,
                                       bool *layout_changed,
                                       bool *quit_requested,
                                       bool *need_more,
                                       bool *handled)
{
    *need_more = false;
    *handled = false;
    bool partial = false;
    if (desk_sequence_matches(session->prefix_sequence,
                              session->prefix_sequence_length,
                              input + offset, length - offset, consumed,
                              &partial)) {
        session->prefix_pending = true;
        *handled = true;
        return 0;
    }
    if (partial) {
        desk_save_pending_from(session, input, length, offset);
        *need_more = true;
        *handled = true;
        return 0;
    }

    cubicle_workspace_macro_info_t *macro =
        desk_find_custom_macro_binding(session, false, input + offset,
                                      length - offset, consumed, &partial);
    if (macro != NULL) {
        if (desk_run_macro(session, macro) < 0) {
            return -1;
        }
        *handled = true;
        return 0;
    }
    const cubicle_desk_key_binding_t *binding =
        desk_find_key_binding(session, false, input + offset, length - offset,
                              consumed, &partial);
    if (binding != NULL) {
        (void)desk_execute_command(session, terminal, binding->command,
                                   layout_changed, quit_requested);
        *handled = true;
        return 0;
    }
    if (partial || desk_escape_sequence_incomplete(input, length, offset)) {
        desk_save_pending_from(session, input, length, offset);
        *need_more = true;
        *handled = true;
        return 0;
    }
    return 0;
}

static bool desk_execute_command(desk_session_t *session,
                                 desk_terminal_t *terminal,
                                 const char *command,
                                 bool *layout_changed,
                                 bool *quit_requested)
{
    if (strncmp(command, "scroll.", 7) == 0) {
        if (desk_scroll_active_pane(session, terminal, command)) {
            render_all_panes(terminal, session);
        }
        return true;
    }
    bool keep_zoom = session->zoomed;
    desk_zoom_t previous_zoom = session->layout.zoom;
    cubicle_workspace_macro_info_t *macro =
        desk_find_macro_by_command(session, command);
    if (macro != NULL) {
        (void)terminal;
        return desk_run_macro(session, macro) == 0;
    }
    if (strcmp(command, "pane.next") == 0) {
        pane_layout_next(&session->layout);
        session->layout.zoom = keep_zoom ? DESK_ZOOM_FULL : previous_zoom;
        *layout_changed = true;
        return true;
    }
    if (strcmp(command, "pane.previous") == 0) {
        pane_layout_previous(&session->layout);
        session->layout.zoom = keep_zoom ? DESK_ZOOM_FULL : previous_zoom;
        *layout_changed = true;
        return true;
    }
    desk_pane_direction_t direction = DESK_PANE_LEFT;
    bool directional = true;
    if (strcmp(command, "pane.left") == 0) {
        direction = DESK_PANE_LEFT;
    } else if (strcmp(command, "pane.right") == 0) {
        direction = DESK_PANE_RIGHT;
    } else if (strcmp(command, "pane.above") == 0) {
        direction = DESK_PANE_ABOVE;
    } else if (strcmp(command, "pane.below") == 0) {
        direction = DESK_PANE_BELOW;
    } else {
        directional = false;
    }
    if (directional) {
        int previous = session->layout.active_pane_id;
        if (pane_layout_select_direction(&session->layout, terminal,
                                         session->pane_count, direction) &&
            session->layout.active_pane_id != previous) {
            session->layout.zoom = keep_zoom ? DESK_ZOOM_FULL : previous_zoom;
            *layout_changed = true;
        }
        return true;
    }
    if (strcmp(command, "layout.zoom") == 0) {
        session->zoomed = !session->zoomed;
        session->layout.zoom = session->zoomed ? DESK_ZOOM_FULL
                                               : DESK_ZOOM_NONE;
        session->layout.zoom_pane_id = 0;
        *layout_changed = true;
        return true;
    }
    if (strcmp(command, "layout.resize.toggle") == 0) {
        session->layout.resize_mode = !session->layout.resize_mode;
        *layout_changed = true;
        return true;
    }
    if (strcmp(command, "layout.transpose") == 0) {
        if (pane_layout_transpose(&session->layout, terminal) == 0) {
            session->zoomed = false;
            *layout_changed = true;
        }
        return true;
    }
    if (strcmp(command, "layout.split.horizontal") == 0 ||
        strcmp(command, "layout.split.vertical") == 0) {
        char error[256];
        desk_pending_split_t split =
            strcmp(command, "layout.split.horizontal") == 0
                ? DESK_PENDING_SPLIT_HORIZONTAL
                : DESK_PENDING_SPLIT_VERTICAL;
        if (desk_begin_split_process_menu(session, split, error,
                                          sizeof(error)) < 0) {
            pane_layout_status(&session->layout, error);
            *layout_changed = true;
            return true;
        }
        desk_menu_enable_mouse();
        desk_cursor_erase(terminal, session);
        desk_render_open_menu(terminal, session);
        return true;
    }
    if (strcmp(command, "layout.delete") == 0) {
        char error[256];
        if (desk_delete_active_pane(session, terminal, error,
                                    sizeof(error)) < 0) {
            pane_layout_status(&session->layout, error);
        }
        *layout_changed = true;
        return true;
    }
    if (strcmp(command, "layout.reset") == 0) {
        int active_pane_id = session->layout.active_pane_id;
        bool resize_mode = session->layout.resize_mode;
        if (pane_layout_auto(&session->layout, session->pane_count) == 0) {
            desk_apply_pane_labels(session);
            if (active_pane_id > 0 &&
                (size_t)active_pane_id <= session->pane_count) {
                session->layout.active_pane_id = active_pane_id;
            }
            session->layout.resize_mode = resize_mode;
            session->zoomed = false;
            session->layout.zoom = DESK_ZOOM_NONE;
            session->layout.zoom_pane_id = 0;
            pane_layout_clear_status(&session->layout);
            *layout_changed = true;
        }
        return true;
    }
    if (strcmp(command, "layout.zoom.horizontal") == 0) {
        session->zoomed = false;
        if (session->layout.zoom == DESK_ZOOM_HORIZONTAL) {
            session->layout.zoom = DESK_ZOOM_NONE;
            session->layout.zoom_pane_id = 0;
        } else {
            session->layout.zoom = DESK_ZOOM_HORIZONTAL;
            session->layout.zoom_pane_id = session->layout.active_pane_id;
        }
        *layout_changed = true;
        return true;
    }
    if (strcmp(command, "layout.zoom.vertical") == 0) {
        session->zoomed = false;
        if (session->layout.zoom == DESK_ZOOM_VERTICAL) {
            session->layout.zoom = DESK_ZOOM_NONE;
            session->layout.zoom_pane_id = 0;
        } else {
            session->layout.zoom = DESK_ZOOM_VERTICAL;
            session->layout.zoom_pane_id = session->layout.active_pane_id;
        }
        *layout_changed = true;
        return true;
    }
    if (strcmp(command, "mouse.toggle") == 0) {
        session->mouse_titles = !session->mouse_titles;
        session->mouse_suspended_until_ms = 0;
        if (session->mouse_titles) {
            desk_menu_enable_mouse();
        } else {
            desk_menu_disable_mouse();
        }
        desk_debug_log("event=mouse_mode enabled=%d",
                       session->mouse_titles ? 1 : 0);
        *layout_changed = true;
        return true;
    }
    if (strcmp(command, "menu.open") == 0) {
        (void)desk_open_root_menu(session);
        desk_menu_enable_mouse();
        desk_cursor_erase(terminal, session);
        desk_render_open_menu(terminal, session);
        return true;
    }
    if (strcmp(command, "layout.save") == 0) {
        desk_begin_save_layout_prompt(session);
        desk_menu_enable_mouse();
        desk_cursor_erase(terminal, session);
        desk_render_open_menu(terminal, session);
        return true;
    }
    if (strcmp(command, "layout.load") == 0) {
        desk_begin_layout_picker(session);
        desk_menu_enable_mouse();
        desk_cursor_erase(terminal, session);
        desk_render_open_menu(terminal, session);
        return true;
    }
    if (strcmp(command, "bindings.show") == 0) {
        desk_begin_bindings_overlay(session);
        desk_menu_enable_mouse();
        desk_cursor_erase(terminal, session);
        desk_render_open_menu(terminal, session);
        return true;
    }
    if (strcmp(command, "quit") == 0) {
        *quit_requested = true;
        return true;
    }
    return false;
}

static bool is_terminal_csi_response(const unsigned char *input,
                                     size_t length,
                                     size_t offset,
                                     size_t *consumed)
{
    size_t cursor = offset;
    if (cursor + 3 >= length || input[cursor] != 0x1b ||
        input[cursor + 1] != '[') {
        return false;
    }
    cursor += 2;
    while (cursor < length &&
           ((input[cursor] >= '0' && input[cursor] <= '9') ||
            input[cursor] == ';' || input[cursor] == '?' ||
            input[cursor] == '>')) {
        ++cursor;
    }
    if (cursor >= length) {
        return false;
    }
    if (input[cursor] != 'R' && input[cursor] != 'c' &&
        input[cursor] != 'I' && input[cursor] != 'O') {
        return false;
    }
    *consumed = cursor - offset + 1;
    return true;
}

static bool is_terminal_osc_response(const unsigned char *input,
                                     size_t length,
                                     size_t offset,
                                     size_t *consumed)
{
    size_t cursor = offset;
    if (cursor + 2 >= length || input[cursor] != 0x1b ||
        input[cursor + 1] != ']') {
        return false;
    }
    cursor += 2;
    while (cursor < length) {
        if (input[cursor] == '\a') {
            *consumed = cursor - offset + 1;
            return true;
        }
        if (input[cursor] == 0x1b && cursor + 1 < length &&
            input[cursor + 1] == '\\') {
            *consumed = cursor - offset + 2;
            return true;
        }
        ++cursor;
    }
    return false;
}

static bool is_terminal_response_sequence(const unsigned char *input,
                                          size_t length,
                                          size_t offset,
                                          size_t *consumed)
{
    return is_terminal_csi_response(input, length, offset, consumed) ||
           is_terminal_osc_response(input, length, offset, consumed);
}

static int flush_pending_layout_resize(desk_session_t *session,
                                       desk_terminal_t *terminal,
                                       bool *pending_resize,
                                       bool *layout_changed)
{
    if (pending_resize == NULL || !*pending_resize) {
        return 0;
    }
    long long start_ms = desk_monotonic_ms();
    bool sizes_changed = false;
    if (refresh_pane_sizes(session, terminal, false, &sizes_changed) < 0) {
        return -1;
    }
    long long elapsed_ms = desk_monotonic_ms() - start_ms;
    desk_debug_log("event=layout_resize_flush elapsed_ms=%lld sizes_changed=%d",
                   elapsed_ms, (int)sizes_changed);
    *pending_resize = false;
    if (layout_changed != NULL) {
        *layout_changed = true;
    }
    return 0;
}

static int handle_input(desk_session_t *session,
                        desk_terminal_t *terminal,
                        const unsigned char *input,
                        size_t length,
                        bool *layout_changed,
                        bool *quit_requested)
{
    size_t start = 0;
    bool pending_resize = false;
    for (size_t i = 0; i < length; ++i) {
        if (is_incomplete_sgr_mouse(input, length, i)) {
            desk_save_pending_input(session, input, length, i);
            break;
        }
        size_t consumed = 0;
        int button = 0;
        int mouse_row = 0;
        int mouse_col = 0;
        bool mouse_press = false;
        if (session->mouse_titles &&
            parse_sgr_mouse(input, length, i, &button, &mouse_row,
                            &mouse_col, &mouse_press, &consumed)) {
            if (flush_active_input(session, input, start, i) < 0) {
                return -1;
            }
            desk_debug_log("event=mouse_normal button=%d row=%d col=%d press=%d",
                           button, mouse_row, mouse_col, (int)mouse_press);
            if (mouse_press && sgr_mouse_is_left_button(button)) {
                int pane_id = 0;
                if (!sgr_mouse_has_modifier(button) &&
                    desk_title_hit_test(terminal, session, mouse_row,
                                        mouse_col, &pane_id)) {
                    desk_debug_log("event=mouse_title_select pane=%d", pane_id);
                    desk_zoom_t previous_zoom = session->layout.zoom;
                    session->layout.active_pane_id = pane_id;
                    session->layout.zoom =
                        session->zoomed ? DESK_ZOOM_FULL : previous_zoom;
                    *layout_changed = true;
                } else {
                    desk_suspend_mouse_for_selection(session);
                }
            }
            i += consumed - 1;
            start = i + 1;
            continue;
        }
        if (is_terminal_response_sequence(input, length, i, &consumed)) {
            if (flush_active_input(session, input, start, i) < 0) {
                return -1;
            }
            i += consumed - 1;
            start = i + 1;
            continue;
        }
        if (session->prefix_pending) {
            if (flush_active_input(session, input, start, i) < 0) {
                return -1;
            }
            bool need_more = false;
            if (desk_handle_prefixed_sequence(session, terminal, input, length,
                                             i, &consumed, layout_changed,
                                             quit_requested, &need_more) < 0) {
                return -1;
            }
            if (need_more) {
                return 0;
            }
            i += consumed - 1;
            start = i + 1;
            continue;
        }
        bool need_more = false;
        bool handled = false;
        if (desk_handle_direct_sequence(session, terminal, input, length, i,
                                        &consumed, layout_changed,
                                        quit_requested, &need_more,
                                        &handled) < 0) {
            return -1;
        }
        if (need_more) {
            if (flush_active_input(session, input, start, i) < 0) {
                return -1;
            }
            return 0;
        }
        if (handled) {
            if (flush_active_input(session, input, start, i) < 0) {
                return -1;
            }
            i += consumed - 1;
            start = i + 1;
            continue;
        }
        if (session->layout.resize_mode &&
            (input[i] == 's' || input[i] == 'q' || input[i] == 't' ||
             input[i] == 'H' || input[i] == 'V' || input[i] == 'D' ||
             input[i] == 'r' || input[i] == 'h' || input[i] == 'v')) {
            if (flush_active_input(session, input, start, i) < 0) {
                return -1;
            }
            if (flush_pending_layout_resize(session, terminal,
                                            &pending_resize,
                                            layout_changed) < 0) {
                return -1;
            }
            const char *command = NULL;
            if (input[i] == 's' || input[i] == 'q') {
                session->layout.resize_mode = false;
                *layout_changed = true;
            } else if (input[i] == 't') {
                command = "layout.transpose";
            } else if (input[i] == 'H') {
                command = "layout.split.horizontal";
            } else if (input[i] == 'V') {
                command = "layout.split.vertical";
            } else if (input[i] == 'D') {
                command = "layout.delete";
            } else if (input[i] == 'r') {
                command = "layout.reset";
            } else if (input[i] == 'h') {
                command = "layout.zoom.horizontal";
            } else if (input[i] == 'v') {
                command = "layout.zoom.vertical";
            }
            if (command != NULL) {
                (void)desk_execute_command(session, terminal, command,
                                           layout_changed, quit_requested);
            }
            start = i + 1;
            continue;
        }
        desk_resize_side_t side;
        int delta = 0;
        if (session->layout.resize_mode &&
            parse_resize_arrow(input, length, i, &side, &delta, &consumed)) {
            if (flush_active_input(session, input, start, i) < 0) {
                return -1;
            }
            if (pane_layout_resize_side(&session->layout, terminal, side,
                                        delta) == 0) {
                pending_resize = true;
                *layout_changed = true;
            }
            i += consumed - 1;
            start = i + 1;
            continue;
        }
        if (session->layout.resize_mode && input[i] == 0x1b) {
            if (flush_active_input(session, input, start, i) < 0) {
                return -1;
            }
            if (flush_pending_layout_resize(session, terminal,
                                            &pending_resize,
                                            layout_changed) < 0) {
                return -1;
            }
            session->layout.resize_mode = false;
            *layout_changed = true;
            start = i + 1;
            continue;
        }
    }
    if (flush_pending_layout_resize(session, terminal, &pending_resize,
                                    layout_changed) < 0) {
        return -1;
    }
    return flush_active_input(session, input, start, length);
}

static void desk_session_cleanup(desk_session_t *session)
{
    for (size_t i = 0; i < session->pane_count; ++i) {
        cubicle_attachment_disconnect(session->panes[i].attachment);
        session->panes[i].attachment = NULL;
        cubicle_terminal_model_destroy(session->panes[i].terminal_model);
        session->panes[i].terminal_model = NULL;
        grid_cleanup(&session->panes[i].grid);
        desk_scrollback_clear(&session->panes[i]);
    }
    cubicle_client_disconnect(session->manager);
    session->manager = NULL;
}

static int desk_run_workspace(const char *workspace_arg,
                              unsigned char prefix_key,
                              bool prefix_override,
                              bool mouse_titles)
{
    char error[512];
    desk_session_t session;
    memset(&session, 0, sizeof(session));
    session.mouse_titles = mouse_titles;
    session.terminal_size_dirty = true;

    const char *effective_workspace_arg = workspace_arg;
    char remote_manager_uri[CUBICLE_ENDPOINT_URI_MAX];
    char remote_workspace[CUBICLE_NAME_MAX];
    char remote_name[CUBICLE_NAME_MAX];
    if (workspace_arg != NULL && workspace_arg[0] != '\0') {
        int remote_result = desk_split_remote_workspace(
            workspace_arg, remote_workspace, remote_name);
        if (remote_result < 0) {
            fprintf(stderr, "desk: invalid remote workspace reference\n");
            return 2;
        }
        if (remote_result > 0) {
            if (desk_remote_uri_for_name(remote_name, remote_manager_uri) < 0) {
                fprintf(stderr, "desk: remote '%s' is not configured\n",
                        remote_name);
                return 2;
            }
            effective_workspace_arg = remote_workspace;
            snprintf(session.manager_uri, sizeof(session.manager_uri), "%s",
                     remote_manager_uri);
            session.manager_relay_attachments =
                strncmp(remote_manager_uri, "tls://", 6) == 0;
        }
    }

    cubicle_config_t config;
    cubicle_error_code_t code = connect_client(
        &session.manager, &config,
        session.manager_uri[0] == '\0' ? NULL : session.manager_uri,
        error, sizeof(error));
    if (code != CUBICLE_OK) {
        fprintf(stderr, "desk: %s\n", error);
        return 2;
    }
    desk_crash_configure(&config);
    if (session.manager_uri[0] == '\0') {
        char configured_endpoint[CUBICLE_ENDPOINT_URI_MAX];
        const char *resolved = cubeui_resolve_manager_endpoint(
            NULL, &config, configured_endpoint, sizeof(configured_endpoint));
        if (resolved != NULL) {
            snprintf(session.manager_uri, sizeof(session.manager_uri), "%s",
                     resolved);
            session.manager_relay_attachments =
                strncmp(resolved, "tls://", 6) == 0;
        }
    }
    snprintf(g_desk_crash_workspace, sizeof(g_desk_crash_workspace), "%s",
             workspace_arg != NULL ? workspace_arg : "");
    snprintf(g_desk_crash_manager, sizeof(g_desk_crash_manager), "%s",
             session.manager_uri);
    session.prefix_key = prefix_override ? prefix_key : config.desk_prefix_key;
    if (prefix_override) {
        session.prefix_sequence[0] = prefix_key;
        session.prefix_sequence_length = 1;
    } else {
        memcpy(session.prefix_sequence, config.desk_prefix_sequence,
               config.desk_prefix_sequence_length);
        session.prefix_sequence_length = config.desk_prefix_sequence_length;
    }
    session.key_binding_count = config.desk_key_binding_count;
    session.scrollback_limit = config.desk_scrollback_lines;
    memcpy(session.key_bindings, config.desk_key_bindings,
           session.key_binding_count * sizeof(session.key_bindings[0]));
    session.startup_key_binding_count = session.key_binding_count;
    memcpy(session.startup_key_bindings, session.key_bindings,
           session.startup_key_binding_count *
               sizeof(session.startup_key_bindings[0]));
    desk_debug_log("event=run_start workspace_arg=%s manager=%s relay=%d",
                   effective_workspace_arg != NULL ? effective_workspace_arg : "",
                   session.manager_uri, session.manager_relay_attachments);

    desk_crash_set_phase("resolve-workspace");
    int result = resolve_workspace(&session, effective_workspace_arg, error,
                                   sizeof(error));
    if (result == 0) {
        snprintf(g_desk_crash_workspace, sizeof(g_desk_crash_workspace), "%s",
                 session.workspace.name);
        desk_crash_set_phase("load-macros");
        snprintf(session.last_opened_workspace_id,
                 sizeof(session.last_opened_workspace_id), "%s",
                 session.workspace.id);
        result = desk_load_workspace_macros(&session, error, sizeof(error));
    }
    if (result == 0) {
        desk_crash_set_phase("load-processes");
        result = load_workspace_processes(&session, error, sizeof(error));
    }
    if (result == 0) {
        desk_crash_set_phase("load-layout");
        result = load_or_create_layout(&session, error, sizeof(error));
    }
    if (result != 0) {
        desk_debug_log("event=startup_failed result=%d message=\"%s\"",
                       result, error);
        fprintf(stderr, "desk: %s\n", error);
        desk_session_cleanup(&session);
        return result;
    }

    desk_terminal_t terminal;
    if (cubeui_terminal_enter_alt_raw(&terminal) < 0) {
        desk_debug_log("event=terminal_enter_failed errno=%d", errno);
        desk_session_cleanup(&session);
        return -1;
    }
    desk_debug_log("event=terminal_entered rows=%d cols=%d",
                   terminal.rows, terminal.cols);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGWINCH, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    bool initial_size_changed = false;
    desk_crash_set_phase("initial-resize");
    if (resize_all_panes(&session, &terminal, &initial_size_changed) < 0) {
        cubeui_terminal_leave_alt_raw(&terminal);
        desk_debug_log("event=initial_resize_failed errno=%d", errno);
        fprintf(stderr, "desk: terminal too small for desk\n");
        desk_session_cleanup(&session);
        return 2;
    }
    desk_crash_set_phase("attach-panes");
    result = attach_all_panes(&session, error, sizeof(error));
    if (result != 0) {
        cubeui_terminal_leave_alt_raw(&terminal);
        desk_debug_log("event=attach_all_failed result=%d message=\"%s\"",
                       result, error);
        fprintf(stderr, "desk: %s\n", error);
        desk_session_cleanup(&session);
        return result;
    }
    render_all_panes(&terminal, &session);
    if (session.mouse_titles) {
        desk_menu_enable_mouse();
    }
    desk_crash_set_phase("event-loop");
    desk_debug_log("event=loop_start panes=%zu", session.pane_count);
    while (!g_stop_requested) {
        bool layout_changed = false;
        bool quit_requested = false;
        bool pane_ready[32] = {false};
        bool has_stream_fd = false;
        bool has_polling_pane = false;
        desk_resume_mouse_if_ready(&session);
        long long now_ms = desk_monotonic_ms();
        if (session.notice[0] != '\0' && now_ms > 0 &&
            session.notice_until_ms > 0 && now_ms >= session.notice_until_ms) {
            session.notice[0] = '\0';
            session.notice_until_ms = 0;
            render_all_panes(&terminal, &session);
        }

        if (g_resize_requested) {
            g_resize_requested = 0;
            session.terminal_size_dirty = true;
            if (flush_pending_terminal_resize(&session, &terminal,
                                              &layout_changed) < 0) {
                result = 2;
                desk_debug_log("event=resize_flush_failed errno=%d", errno);
                break;
            }
            if (session.open_menu.level != DESK_MENU_CLOSED) {
                render_all_panes(&terminal, &session);
                desk_render_open_menu(&terminal, &session);
                layout_changed = false;
            }
        }

        if (session.open_menu.level == DESK_MENU_CLOSED &&
            desk_refresh_ended_panes(&session, &terminal, &layout_changed) < 0) {
            result = 2;
            desk_debug_log("event=process_refresh_failed errno=%d", errno);
            break;
        }

        if (session.open_menu.level == DESK_MENU_CLOSED) {
            desk_cursor_tick(&terminal, &session);
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        int max_fd = STDIN_FILENO;
        if (session.open_menu.level == DESK_MENU_CLOSED) {
            for (size_t i = 0; i < session.pane_count; ++i) {
                int stream_fd = cubicle_attachment_stream_fd(
                    session.panes[i].attachment, CUBICLE_STREAM_TTY);
                if (stream_fd >= 0) {
                    FD_SET(stream_fd, &read_set);
                    if (stream_fd > max_fd) {
                        max_fd = stream_fd;
                    }
                    has_stream_fd = true;
                } else if (session.panes[i].attachment != NULL) {
                    has_polling_pane = true;
                }
            }
        }
        struct timeval timeout = {0, 50000};
        int ready = select(max_fd + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = 2;
            desk_debug_log("event=select_failed errno=%d", errno);
            break;
        }
        if (ready == 0 && session.open_menu.level == DESK_MENU_CLOSED &&
            session.pending_input_length > 0) {
            if (desk_flush_expired_pending_input(&session) < 0) {
                result = 2;
                desk_debug_log("event=pending_input_flush_failed errno=%d",
                               errno);
                break;
            }
            if (session.redraw_requested) {
                session.redraw_requested = false;
                render_all_panes(&terminal, &session);
            }
        }
        if (ready > 0 && session.open_menu.level == DESK_MENU_CLOSED) {
            for (size_t i = 0; i < session.pane_count; ++i) {
                int stream_fd = cubicle_attachment_stream_fd(
                    session.panes[i].attachment, CUBICLE_STREAM_TTY);
                if (stream_fd >= 0 && FD_ISSET(stream_fd, &read_set)) {
                    pane_ready[i] = true;
                }
            }
        }
        if (session.open_menu.level == DESK_MENU_CLOSED) {
            bool read_polling_fallback = !has_stream_fd || has_polling_pane;
            for (size_t i = 0; i < session.pane_count; ++i) {
                if (!pane_ready[i] && !read_polling_fallback) {
                    continue;
                }
                if (!pane_ready[i]) {
                    int stream_fd = cubicle_attachment_stream_fd(
                        session.panes[i].attachment, CUBICLE_STREAM_TTY);
                    if (stream_fd >= 0) {
                        continue;
                    }
                }
                if (read_and_render_pane_output(&terminal, &session, i,
                                                NULL) < 0) {
                    result = 2;
                    desk_debug_log("event=loop_read_failed pane=%zu", i + 1);
                    break;
                }
            }
        }
        if (result != 0) {
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
                desk_debug_log("event=input_read_failed errno=%d", errno);
                break;
            }
            if (input_length == 0) {
                desk_debug_log("event=input_eof action=quit");
                quit_requested = true;
            } else {
                desk_debug_log("event=input_read bytes=%zd", input_length);
                unsigned char combined_input[sizeof(session.pending_input) +
                                             sizeof(input)];
                const unsigned char *handled_input = input;
                size_t handled_length = (size_t)input_length;
                if (session.pending_input_length > 0) {
                    memcpy(combined_input, session.pending_input,
                           session.pending_input_length);
                    memcpy(combined_input + session.pending_input_length,
                           input, (size_t)input_length);
                    handled_input = combined_input;
                    handled_length = session.pending_input_length +
                                     (size_t)input_length;
                    session.pending_input_length = 0;
                    session.pending_input_since_ms = 0;
                }
                if (flush_pending_terminal_resize(&session, &terminal,
                                                  &layout_changed) < 0) {
                    result = 2;
                    desk_debug_log("event=input_resize_flush_failed errno=%d",
                                   errno);
                    break;
                }
                bool menu_closed = false;
                if (session.open_menu.level != DESK_MENU_CLOSED) {
                    if (handle_open_menu_input(&session, &terminal,
                                               handled_input,
                                               handled_length,
                                               &menu_closed) < 0) {
                        result = 2;
                        desk_debug_log("event=handle_menu_input_failed errno=%d",
                                       errno);
                        break;
                    }
                } else if (handle_input(&session, &terminal, handled_input,
                                        handled_length, &layout_changed,
                                        &quit_requested) < 0) {
                    result = 2;
                    desk_debug_log("event=handle_input_failed errno=%d",
                                   errno);
                    break;
                }
                if (layout_changed &&
                    session.open_menu.level == DESK_MENU_CLOSED) {
                    if (desk_apply_layout_change(&session, &terminal) < 0) {
                        result = 2;
                        desk_debug_log("event=input_layout_apply_failed errno=%d",
                                       errno);
                        break;
                    }
                    layout_changed = false;
                } else if (session.open_menu.level != DESK_MENU_CLOSED) {
                    render_all_panes(&terminal, &session);
                    desk_render_open_menu(&terminal, &session);
                } else if (menu_closed) {
                    render_all_panes(&terminal, &session);
                } else if (session.redraw_requested) {
                    session.redraw_requested = false;
                    render_all_panes(&terminal, &session);
                } else {
                    desk_cursor_reset_blink(&session);
                    desk_cursor_render(&terminal, &session);
                }
            }
        }

        if (layout_changed) {
            if (desk_apply_layout_change(&session, &terminal) < 0) {
                result = 2;
                desk_debug_log("event=layout_apply_failed errno=%d", errno);
                break;
            }
        }
        if (quit_requested) {
            desk_debug_log("event=quit_requested");
            break;
        }
    }

    desk_menu_disable_mouse();
    cubeui_terminal_leave_alt_raw(&terminal);
    desk_debug_log("event=run_end result=%d stop_requested=%d",
                   result, (int)g_stop_requested);
    desk_session_cleanup(&session);
    return result;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [--config PATH] [--workspace NAME|ID] [--prefix KEY] [--mouse|--no-mouse]\n",
            program);
    fprintf(stream, "Render the Cubicle desk terminal view.\n");
    fprintf(stream,
            "  --mouse       Enable mouse pane title selection.\n");
    fprintf(stream,
            "  --no-mouse    Disable mouse pane title selection.\n");
    fprintf(stream,
            "  Prefix-m      Toggle mouse pane title selection while running.\n");
    fprintf(stream,
            "  Prefix-s      Enter layout mode; arrows resize, H/V split, D deletes.\n");
    fprintf(stream, "  Prefix-:      Save the current layout by name.\n");
    fprintf(stream, "  Prefix-;      Load a saved layout by name.\n");
    fprintf(stream, "  Prefix-?      Show or edit configured key bindings.\n");
    fprintf(stream, "  Prefix-PageUp/PageDown scroll the active pane.\n");
}

static bool desk_string_equals_case(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int parse_control_key_char(char ch, unsigned char *key)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch == ' ' || ch == '@') {
        *key = 0;
        return 0;
    }
    if (ch < 'A' || ch > '_') {
        return -1;
    }
    *key = (unsigned char)(ch - '@');
    return 0;
}

static int parse_prefix_key(const char *text, unsigned char *key)
{
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    if (text[0] == '^' && text[1] != '\0') {
        return text[2] == '\0' ? parse_control_key_char(text[1], key) : -1;
    }
    const char *control = NULL;
    if (strncmp(text, "Control-", 8) == 0 ||
        strncmp(text, "control-", 8) == 0) {
        control = text + 8;
    } else if (strncmp(text, "Ctrl-", 5) == 0 ||
               strncmp(text, "ctrl-", 5) == 0) {
        control = text + 5;
    } else if (strncmp(text, "C-", 2) == 0 ||
               strncmp(text, "c-", 2) == 0) {
        control = text + 2;
    }
    if (control != NULL) {
        if (desk_string_equals_case(control, "Space")) {
            *key = 0;
            return 0;
        }
        return control[0] != '\0' && control[1] == '\0'
                   ? parse_control_key_char(control[0], key)
                   : -1;
    }
    if (desk_string_equals_case(text, "Space")) {
        *key = ' ';
        return 0;
    }
    if (desk_string_equals_case(text, "Enter") ||
        desk_string_equals_case(text, "Return")) {
        *key = '\r';
        return 0;
    }
    if (desk_string_equals_case(text, "Escape") ||
        desk_string_equals_case(text, "Esc")) {
        *key = 0x1b;
        return 0;
    }
    if (desk_string_equals_case(text, "Backspace")) {
        *key = 0x7f;
        return 0;
    }
    if (desk_string_equals_case(text, "Tab")) {
        *key = '\t';
        return 0;
    }
    if (text[1] == '\0') {
        *key = (unsigned char)text[0];
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *workspace = NULL;
    unsigned char prefix_key = 0x18;
    bool prefix_override = false;
    bool mouse_titles = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                print_usage(stderr, argv[0]);
                return 2;
            }
            if (setenv("CUBICLE_CONFIG", argv[++i], 1) < 0) {
                fprintf(stderr, "desk: failed to set config override: %s\n",
                        strerror(errno));
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--workspace") == 0) {
            if (i + 1 >= argc) {
                print_usage(stderr, argv[0]);
                return 2;
            }
            workspace = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--prefix") == 0) {
            if (i + 1 >= argc ||
                parse_prefix_key(argv[++i], &prefix_key) < 0) {
                print_usage(stderr, argv[0]);
                return 2;
            }
            prefix_override = true;
            continue;
        }
        if (strcmp(argv[i], "--mouse") == 0) {
            mouse_titles = true;
            continue;
        }
        if (strcmp(argv[i], "--no-mouse") == 0) {
            mouse_titles = false;
            continue;
        }
        print_usage(stderr, argv[0]);
        return 2;
    }

    int result = desk_run_workspace(workspace, prefix_key, prefix_override,
                                    mouse_titles);
    if (result < 0) {
        fprintf(stderr, "desk: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
