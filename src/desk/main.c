#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
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

static volatile sig_atomic_t g_resize_requested = 1;
static volatile sig_atomic_t g_stop_requested = 0;

typedef cubeui_terminal_t desk_terminal_t;

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
    unsigned int rows;
    unsigned int cols;
} desk_pane_t;

typedef struct desk_session {
    cubicle_client_t *manager;
    cubicle_workspace_info_t workspace;
    desk_pane_t panes[32];
    size_t pane_count;
    desk_pane_layout_t layout;
    char layout_path[PATH_MAX];
    unsigned char prefix_key;
    bool prefix_pending;
    bool zoomed;
    bool terminal_size_dirty;
    bool cursor_drawn;
    bool cursor_blink_visible;
    long long cursor_next_blink_ms;
    int cursor_pane_id;
    int cursor_row;
    int cursor_col;
} desk_session_t;

static void desk_render_cube_grid(const desk_terminal_t *terminal,
                                  const desk_pane_layout_t *panes,
                                  int pane_id,
                                  desk_grid_t *grid,
                                  const char *title);

static void handle_signal(int signo)
{
    if (signo == SIGWINCH) {
        g_resize_requested = 1;
    } else {
        g_stop_requested = 1;
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
    panes->zoom = DESK_ZOOM_NONE;
}

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

static int DESK_UNUSED pane_layout_split(desk_pane_layout_t *panes,
                                         desk_split_t split)
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
    panes->zoom = DESK_ZOOM_NONE;
    return 0;
}

static int DESK_UNUSED pane_layout_delete_active(desk_pane_layout_t *panes)
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

    bool first_has_active =
        pane_subtree_contains(panes, entry->first, panes->active_pane_id);
    bool second_has_active =
        pane_subtree_contains(panes, entry->second, panes->active_pane_id);
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

    bool first_has_active =
        pane_subtree_contains(panes, entry->first, panes->active_pane_id);
    bool second_has_active =
        pane_subtree_contains(panes, entry->second, panes->active_pane_id);
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

static int DESK_UNUSED pane_layout_transpose(desk_pane_layout_t *panes,
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

        panes->zoom = DESK_ZOOM_NONE;
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

static int pane_layout_save(const desk_pane_layout_t *panes,
                            const char *path)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }
    fprintf(file, "desk-layout-v1\n");
    fprintf(file, "root %d\n", panes->root);
    fprintf(file, "active %d\n", panes->active_pane_id);
    fprintf(file, "next %d\n", panes->next_pane_id);
    fprintf(file, "zoom %d\n", (int)panes->zoom);
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
    return fclose(file);
}

static int pane_layout_load_file(desk_pane_layout_t *panes, const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    desk_pane_layout_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.root = -1;
    loaded.active_pane_id = 1;
    loaded.next_pane_id = 1;
    char line[256];
    if (fgets(line, sizeof(line), file) == NULL ||
        strcmp(line, "desk-layout-v1\n") != 0) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        int value = 0;
        if (sscanf(line, "root %d", &value) == 1) {
            loaded.root = value;
            continue;
        }
        if (sscanf(line, "active %d", &value) == 1) {
            loaded.active_pane_id = value;
            continue;
        }
        if (sscanf(line, "next %d", &value) == 1) {
            loaded.next_pane_id = value;
            continue;
        }
        if (sscanf(line, "zoom %d", &value) == 1) {
            loaded.zoom = (desk_zoom_t)value;
            continue;
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
                index >= (int)(sizeof(loaded.nodes) / sizeof(loaded.nodes[0])) ||
                split < DESK_SPLIT_NONE || split > DESK_SPLIT_VERTICAL ||
                (split == DESK_SPLIT_NONE && matched != 7)) {
                fclose(file);
                errno = EINVAL;
                return -1;
            }
            loaded.nodes[index].used = true;
            loaded.nodes[index].pane_id = pane_id;
            snprintf(loaded.nodes[index].label,
                     sizeof(loaded.nodes[index].label), "%s", label);
            loaded.nodes[index].split = (desk_split_t)split;
            loaded.nodes[index].first = first;
            loaded.nodes[index].second = second;
            loaded.nodes[index].split_size = split_size;
            continue;
        }

        fclose(file);
        errno = EINVAL;
        return -1;
    }
    fclose(file);

    if (loaded.root < 0 ||
        loaded.root >= (int)(sizeof(loaded.nodes) / sizeof(loaded.nodes[0])) ||
        !loaded.nodes[loaded.root].used ||
        pane_find_leaf_node(&loaded, loaded.root, loaded.active_pane_id) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (loaded.next_pane_id <= 0) {
        loaded.next_pane_id = pane_layout_next_id(&loaded);
    }
    loaded.resize_mode = false;
    *panes = loaded;
    return 0;
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

    char configured_endpoint[CUBICLE_ENDPOINT_URI_MAX];
    const char *manager_uri = cubeui_resolve_manager_endpoint(
        NULL, &config, configured_endpoint, sizeof(configured_endpoint));

    cubicle_error_code_t code = cubicle_client_connect_uri(manager_uri, NULL,
                                                           client_out);
    if (code != CUBICLE_OK) {
        snprintf(error, error_size, "failed to connect to manager");
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

    bool first_has_active =
        pane_subtree_contains(panes, entry->first, panes->active_pane_id);
    bool second_has_active =
        pane_subtree_contains(panes, entry->second, panes->active_pane_id);
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

static void grid_clear_row(desk_grid_t *grid, int row)
{
    if (row < 0 || row >= grid->rows) {
        return;
    }
    desk_cell_t *line = grid->cells + (size_t)row * (size_t)grid->cols;
    for (int col = 0; col < grid->cols; ++col) {
        snprintf(line[col].text, sizeof(line[col].text), " ");
        snprintf(line[col].sgr, sizeof(line[col].sgr), "%s",
                 grid->current_sgr);
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

static void grid_put_text(desk_grid_t *grid, const char *text)
{
    if (grid->cursor_row < 0 || grid->cursor_row >= grid->rows ||
        grid->cursor_col < 0 || grid->cursor_col >= grid->cols) {
        return;
    }
    desk_cell_t *cell =
        &grid->cells[(size_t)grid->cursor_row * (size_t)grid->cols +
                     (size_t)grid->cursor_col];
    snprintf(cell->text, sizeof(cell->text), "%s", text);
    snprintf(cell->sgr, sizeof(cell->sgr), "%s", grid->current_sgr);
    if (grid->cursor_col + 1 >= grid->cols) {
        grid_newline(grid);
    } else {
        grid->cursor_col++;
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
            grid_put_text(grid, " ");
        }
        return;
    }
    if (ch < 0x20) {
        return;
    }

    char text[2] = {(char)ch, '\0'};
    grid_put_text(grid, text);
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
    snprintf(cell->text, sizeof(cell->text), " ");
    snprintf(cell->sgr, sizeof(cell->sgr), "%s", grid->current_sgr);
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

static size_t grid_apply_csi(desk_grid_t *grid,
                             unsigned char *response,
                             size_t response_size)
{
    if (grid->csi_length == 0) {
        return 0;
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
        return 0;
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
        return 0;
    }
    if (command == 'm') {
        grid_set_sgr(grid);
        return 0;
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
        return 0;
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
        return 0;
    }
    if (command == 'L') {
        int amount = parse_csi_number(grid->csi, 1);
        int bottom = grid->scroll_bottom;
        if (grid->cursor_row >= grid->scroll_top && grid->cursor_row <= bottom) {
            grid_scroll_down_region(grid, grid->cursor_row, bottom, amount);
        }
        return 0;
    }
    if (command == 'M') {
        int amount = parse_csi_number(grid->csi, 1);
        int bottom = grid->scroll_bottom;
        if (grid->cursor_row >= grid->scroll_top && grid->cursor_row <= bottom) {
            grid_scroll_up_region(grid, grid->cursor_row, bottom, amount);
        }
        return 0;
    }
    if (command == 'S') {
        int amount = parse_csi_number(grid->csi, 1);
        grid_scroll_up_region(grid, grid->scroll_top, grid->scroll_bottom,
                              amount);
        return 0;
    }
    if (command == 'T') {
        int amount = parse_csi_number(grid->csi, 1);
        grid_scroll_down_region(grid, grid->scroll_top, grid->scroll_bottom,
                                amount);
        return 0;
    }
    if (command == 'n' && strcmp(grid->csi, "6") == 0 &&
        response != NULL && response_size > 0) {
        int length = snprintf((char *)response, response_size, "\x1b[%d;%dR",
                              grid->cursor_row + 1, grid->cursor_col + 1);
        return length > 0 && (size_t)length < response_size ? (size_t)length
                                                            : 0;
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
    return 0;
}

static size_t utf8_expected_length(unsigned char ch)
{
    if (ch < 0x80) {
        return 1;
    }
    if (ch >= 0xc2 && ch <= 0xdf) {
        return 2;
    }
    if (ch >= 0xe0 && ch <= 0xef) {
        return 3;
    }
    if (ch >= 0xf0 && ch <= 0xf4) {
        return 4;
    }
    return 0;
}

static bool utf8_is_continuation(unsigned char ch)
{
    return ch >= 0x80 && ch <= 0xbf;
}

static void grid_reset_utf8(desk_grid_t *grid)
{
    grid->utf8_length = 0;
    grid->utf8_expected = 0;
    grid->utf8[0] = '\0';
}

static void grid_put_utf8_byte(desk_grid_t *grid, unsigned char ch)
{
    if (grid->utf8_expected > 0) {
        if (!utf8_is_continuation(ch) ||
            grid->utf8_length + 1 >= sizeof(grid->utf8)) {
            grid_reset_utf8(grid);
            grid_put_char(grid, ch);
            return;
        }
        grid->utf8[grid->utf8_length++] = (char)ch;
        if (grid->utf8_length == grid->utf8_expected) {
            grid->utf8[grid->utf8_length] = '\0';
            grid_put_text(grid, grid->utf8);
            grid_reset_utf8(grid);
        }
        return;
    }

    size_t expected = utf8_expected_length(ch);
    if (expected <= 1) {
        grid_put_char(grid, ch);
        return;
    }
    grid->utf8[0] = (char)ch;
    grid->utf8[1] = '\0';
    grid->utf8_length = 1;
    grid->utf8_expected = expected;
}

static void grid_feed(desk_grid_t *grid, const unsigned char *data,
                      size_t length,
                      cubicle_attachment_t *attachment)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = data[i];
        if (grid->csi_active) {
            if (grid->csi_length + 1 < sizeof(grid->csi)) {
                grid->csi[grid->csi_length++] = (char)ch;
                grid->csi[grid->csi_length] = '\0';
            }
            if (ch >= 0x40 && ch <= 0x7e) {
                unsigned char response[64];
                size_t response_length = grid_apply_csi(
                    grid, response, sizeof(response));
                if (response_length > 0 && attachment != NULL) {
                    (void)cubicle_attachment_write(attachment, response,
                                                   response_length);
                }
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
            grid_reset_utf8(grid);
            grid->escape_active = true;
            continue;
        }
        grid_put_utf8_byte(grid, ch);
    }
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
    if (pane == NULL || !pane->grid.cursor_visible ||
        !pane_layout_rect_for_pane(&session->layout, terminal, active,
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
    if (!pane_layout_rect_for_pane(panes, terminal, pane_id, &rect) ||
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

static void desk_render_cube_grid_rows(const desk_terminal_t *terminal,
                                       const desk_pane_layout_t *panes,
                                       int pane_id,
                                       desk_grid_t *grid,
                                       const char *title,
                                       bool dirty_only)
{
    (void)title;
    char frame[65536];
    size_t used = 0;
    desk_rect_t rect;

    if (!pane_layout_rect_for_pane(panes, terminal, pane_id, &rect)) {
        return;
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
            if (row < grid->rows && col < grid->cols) {
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
}

static void desk_render_cube_grid(const desk_terminal_t *terminal,
                                  const desk_pane_layout_t *panes,
                                  int pane_id,
                                  desk_grid_t *grid,
                                  const char *title)
{
    desk_render_cube_grid_rows(terminal, panes, pane_id, grid, title, false);
}

static void desk_render_dirty_cube_grid(const desk_terminal_t *terminal,
                                        const desk_pane_layout_t *panes,
                                        int pane_id,
                                        desk_grid_t *grid,
                                        const char *title)
{
    desk_render_cube_grid_rows(terminal, panes, pane_id, grid, title, true);
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
    session->layout.zoom = DESK_ZOOM_NONE;
    int result = pane_layout_save(&session->layout, session->layout_path);
    session->layout.zoom = saved_zoom;
    return result;
}

static bool process_is_attachable(const cubicle_process_info_t *process)
{
    return process->state == CUBICLE_PROCESS_RUNNING &&
           (process->mode == CUBICLE_PROCESS_TTY ||
            process->mode == CUBICLE_PROCESS_TTY_CAPTURED_STDERR);
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
    if (pane_content_size(terminal, &session->layout, (int)pane_index + 1,
                          &rows, &cols) < 0 ||
        grid_resize(&pane->grid, (int)rows, (int)cols) < 0) {
        return -1;
    }
    if (pane->terminal_model != NULL &&
        cubicle_terminal_model_resize(pane->terminal_model, rows, cols) < 0) {
        return -1;
    }
    bool size_changed = pane->rows != rows || pane->cols != cols;
    if (pane->attachment != NULL) {
        bool sent = false;
        cubicle_error_code_t code = cubicle_attachment_resize_tracked(
            pane->attachment, &pane->resize, rows, cols, false, &sent);
        if (code != CUBICLE_OK) {
            return -1;
        }
        size_changed = size_changed || sent;
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
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "attachment request failed");
        return code;
    }

    cubicle_attachment_options_t options;
    memset(&options, 0, sizeof(options));
    code = cubicle_attachment_connect(&grant, &options, &pane->attachment);
    if (code != CUBICLE_OK) {
        snprintf(error, error_size, "controller attachment failed");
        return code;
    }
    code = cubicle_attachment_resize_tracked(pane->attachment, &pane->resize,
                                             pane->rows, pane->cols, true,
                                             NULL);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *last =
            cubicle_attachment_last_error(pane->attachment);
        snprintf(error, error_size, "%s",
                 last != NULL && last->message[0] != '\0'
                     ? last->message
                     : "initial controller resize failed");
        return code;
    }

    cubicle_terminal_snapshot_t snapshot;
    code = cubicle_attachment_snapshot(pane->attachment, &snapshot);
    if (code != CUBICLE_OK) {
        (void)error;
        (void)error_size;
        return CUBICLE_OK;
    }
    if (cubicle_terminal_model_create(snapshot.rows, snapshot.cols,
                                      &pane->terminal_model) < 0 ||
        cubicle_terminal_model_load_snapshot(pane->terminal_model,
                                             &snapshot) < 0) {
        cubicle_terminal_snapshot_cleanup(&snapshot);
        snprintf(error, error_size, "terminal model initialization failed");
        return CUBICLE_ERR_INTERNAL;
    }
    grid_apply_snapshot(&pane->grid, &snapshot);
    cubicle_terminal_model_clear_dirty_rows(pane->terminal_model);
    cubicle_terminal_snapshot_cleanup(&snapshot);
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

static void render_all_panes(const desk_terminal_t *terminal,
                             desk_session_t *session)
{
    desk_cursor_erase(terminal, session);
    desk_render_layout(terminal, &session->layout);
    for (size_t i = 0; i < session->pane_count; ++i) {
        desk_render_cube_grid(terminal, &session->layout, (int)i + 1,
                              &session->panes[i].grid,
                              session->panes[i].process.friendly_name);
    }
    desk_cursor_reset_blink(session);
    desk_cursor_render(terminal, session);
}

static int read_and_render_pane_output(const desk_terminal_t *terminal,
                                       desk_session_t *session,
                                       size_t pane_index,
                                       bool *output_seen)
{
    desk_pane_t *pane = &session->panes[pane_index];
    bool pane_changed = false;

    for (int burst = 0; burst < DESK_OUTPUT_READ_BURST; ++burst) {
        unsigned char output[4096];
        ssize_t nread = cubicle_attachment_read(pane->attachment, output,
                                                sizeof(output));
        if (nread < 0) {
            return -1;
        }
        if (nread == 0) {
            break;
        }
        grid_feed(&pane->grid, output, (size_t)nread, pane->attachment);
        if (pane->terminal_model != NULL &&
            cubicle_terminal_model_feed(pane->terminal_model, output,
                                        (size_t)nread) < 0) {
            return -1;
        }
        pane_changed = true;
        if (output_seen != NULL) {
            *output_seen = true;
        }
    }

    if (pane_changed && refresh_pane_from_model(pane) < 0) {
        return -1;
    }

    if (pane_changed) {
        desk_cursor_erase(terminal, session);
        desk_render_dirty_cube_grid(terminal, &session->layout,
                                    (int)pane_index + 1, &pane->grid,
                                    pane->process.friendly_name);
        desk_cursor_reset_blink(session);
        desk_cursor_render(terminal, session);
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
    return cubicle_attachment_write(session->panes[(size_t)active - 1].attachment,
                                    buffer, length) < 0
               ? -1
               : 0;
}

static bool handle_prefix_command(desk_session_t *session,
                                  desk_terminal_t *terminal,
                                  unsigned char key,
                                  bool *layout_changed,
                                  bool *quit_requested)
{
    bool keep_zoom = session->zoomed;
    switch (key) {
    case 'n':
        pane_layout_next(&session->layout);
        session->layout.zoom = keep_zoom ? DESK_ZOOM_FULL : DESK_ZOOM_NONE;
        *layout_changed = true;
        return true;
    case 'p':
        pane_layout_previous(&session->layout);
        session->layout.zoom = keep_zoom ? DESK_ZOOM_FULL : DESK_ZOOM_NONE;
        *layout_changed = true;
        return true;
    case ' ':
        session->zoomed = !session->zoomed;
        session->layout.zoom = session->zoomed ? DESK_ZOOM_FULL
                                               : DESK_ZOOM_NONE;
        *layout_changed = true;
        return true;
    case 's':
        session->layout.resize_mode = !session->layout.resize_mode;
        *layout_changed = true;
        return true;
    case 'q':
        *quit_requested = true;
        return true;
    default:
        if (key == session->prefix_key) {
            (void)write_active_pane(session, &session->prefix_key, 1);
            return true;
        }
        (void)write_active_pane(session, &session->prefix_key, 1);
        (void)write_active_pane(session, &key, 1);
        (void)terminal;
        return true;
    }
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

static int handle_input(desk_session_t *session,
                        desk_terminal_t *terminal,
                        const unsigned char *input,
                        size_t length,
                        bool *layout_changed,
                        bool *quit_requested)
{
    for (size_t i = 0; i < length; ++i) {
        size_t consumed = 0;
        if (is_terminal_response_sequence(input, length, i, &consumed)) {
            i += consumed - 1;
            continue;
        }
        if (session->prefix_pending) {
            session->prefix_pending = false;
            (void)handle_prefix_command(session, terminal, input[i],
                                        layout_changed, quit_requested);
            continue;
        }
        if (input[i] == session->prefix_key) {
            session->prefix_pending = true;
            continue;
        }
        desk_resize_side_t side;
        int delta = 0;
        if (session->layout.resize_mode &&
            parse_resize_arrow(input, length, i, &side, &delta, &consumed)) {
            if (pane_layout_resize_side(&session->layout, terminal, side,
                                        delta) == 0) {
                bool sizes_changed = false;
                if (refresh_pane_sizes(session, terminal, false,
                                       &sizes_changed) < 0) {
                    return -1;
                }
                *layout_changed = true;
            }
            i += consumed - 1;
            continue;
        }

        if (write_active_pane(session, &input[i], 1) < 0) {
            return -1;
        }
    }
    return 0;
}

static void desk_session_cleanup(desk_session_t *session)
{
    for (size_t i = 0; i < session->pane_count; ++i) {
        cubicle_attachment_disconnect(session->panes[i].attachment);
        session->panes[i].attachment = NULL;
        cubicle_terminal_model_destroy(session->panes[i].terminal_model);
        session->panes[i].terminal_model = NULL;
        grid_cleanup(&session->panes[i].grid);
    }
    cubicle_client_disconnect(session->manager);
    session->manager = NULL;
}

static int desk_run_workspace(const char *workspace_arg,
                              unsigned char prefix_key)
{
    char error[512];
    desk_session_t session;
    memset(&session, 0, sizeof(session));
    session.prefix_key = prefix_key;
    session.terminal_size_dirty = true;

    cubicle_error_code_t code = connect_client(&session.manager, error,
                                               sizeof(error));
    if (code != CUBICLE_OK) {
        fprintf(stderr, "desk: %s\n", error);
        return 2;
    }

    int result = resolve_workspace(&session, workspace_arg, error,
                                   sizeof(error));
    if (result == 0) {
        result = load_workspace_processes(&session, error, sizeof(error));
    }
    if (result == 0) {
        result = load_or_create_layout(&session, error, sizeof(error));
    }
    if (result != 0) {
        fprintf(stderr, "desk: %s\n", error);
        desk_session_cleanup(&session);
        return result;
    }

    desk_terminal_t terminal;
    if (cubeui_terminal_enter_alt_raw(&terminal) < 0) {
        desk_session_cleanup(&session);
        return -1;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGWINCH, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    bool initial_size_changed = false;
    if (resize_all_panes(&session, &terminal, &initial_size_changed) < 0) {
        cubeui_terminal_leave_alt_raw(&terminal);
        fprintf(stderr, "desk: terminal too small for desk\n");
        desk_session_cleanup(&session);
        return 2;
    }
    result = attach_all_panes(&session, error, sizeof(error));
    if (result != 0) {
        cubeui_terminal_leave_alt_raw(&terminal);
        fprintf(stderr, "desk: %s\n", error);
        desk_session_cleanup(&session);
        return result;
    }
    cubicle_client_disconnect(session.manager);
    session.manager = NULL;

    render_all_panes(&terminal, &session);
    while (!g_stop_requested) {
        bool layout_changed = false;
        bool quit_requested = false;
        bool output_seen = false;

        if (g_resize_requested) {
            g_resize_requested = 0;
            session.terminal_size_dirty = true;
            if (flush_pending_terminal_resize(&session, &terminal,
                                              &layout_changed) < 0) {
                result = 2;
                break;
            }
        }

        for (size_t i = 0; i < session.pane_count; ++i) {
            if (read_and_render_pane_output(&terminal, &session, i,
                                            &output_seen) < 0) {
                result = 2;
                break;
            }
        }
        if (result != 0) {
            break;
        }
        (void)output_seen;
        desk_cursor_tick(&terminal, &session);

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval timeout = output_seen ? (struct timeval){0, 0}
                                             : (struct timeval){0, 50000};
        int ready = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
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
                quit_requested = true;
            } else {
                if (flush_pending_terminal_resize(&session, &terminal,
                                                  &layout_changed) < 0) {
                    result = 2;
                    break;
                }
                if (handle_input(&session, &terminal, input,
                                 (size_t)input_length, &layout_changed,
                                 &quit_requested) < 0) {
                    result = 2;
                    break;
                }
                desk_cursor_reset_blink(&session);
                desk_cursor_render(&terminal, &session);
            }
        }

        if (layout_changed) {
            bool sizes_changed = false;
            if (resize_all_panes(&session, &terminal, &sizes_changed) == 0) {
                (void)desk_save_layout(&session);
                render_all_panes(&terminal, &session);
            }
        }
        if (quit_requested) {
            break;
        }
    }

    cubeui_terminal_leave_alt_raw(&terminal);
    desk_session_cleanup(&session);
    return result;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [--workspace NAME|ID] [--prefix KEY]\n",
            program);
    fprintf(stream, "Render the Cubicle desk terminal view.\n");
}

static int parse_prefix_key(const char *text, unsigned char *key)
{
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    if (text[0] == '^' && text[1] != '\0') {
        char ch = text[1];
        if (text[2] != '\0') {
            return -1;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (ch < '@' || ch > '_') {
            return -1;
        }
        *key = (unsigned char)(ch - '@');
        return 0;
    }
    if (text[0] == 'C' && text[1] == '-' && text[2] != '\0') {
        char ch = text[2];
        if (text[3] != '\0') {
            return -1;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (ch < '@' || ch > '_') {
            return -1;
        }
        *key = (unsigned char)(ch - '@');
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

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
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
            continue;
        }
        print_usage(stderr, argv[0]);
        return 2;
    }

    int result = desk_run_workspace(workspace, prefix_key);
    if (result < 0) {
        fprintf(stderr, "desk: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
