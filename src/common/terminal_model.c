#define _POSIX_C_SOURCE 200809L

#include "terminal_model.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>

typedef struct terminal_scrollback_line {
    int cols;
    VTermScreenCell *cells;
} terminal_scrollback_line_t;

struct cubicle_terminal_model {
    VTerm *term;
    VTermScreen *screen;
    unsigned int rows;
    unsigned int cols;
    bool cursor_visible;
    bool application_cursor;
    unsigned char input_tail[16];
    size_t input_tail_length;
    bool dirty_rows[1000];
    char response[4096];
    size_t response_length;
    terminal_scrollback_line_t *scrollback;
    size_t scrollback_count;
    size_t scrollback_capacity;
    cubicle_terminal_scrollback_line_t *captured_scrollback;
    size_t captured_scrollback_head;
    size_t captured_scrollback_count;
    size_t captured_scrollback_capacity;
    size_t scrollback_capture_limit;
};

static void cell_text_from_vterm(const VTermScreenCell *source,
                                 cubicle_terminal_cell_t *target);
static int cell_sgr_from_vterm(const VTermScreenCell *source,
                               cubicle_terminal_cell_t *target);

static int track_damage(VTermRect rect, void *user)
{
    cubicle_terminal_model_t *model = user;
    int start = rect.start_row < 0 ? 0 : rect.start_row;
    int end = rect.end_row > (int)model->rows ? (int)model->rows
                                              : rect.end_row;
    for (int row = start; row < end; ++row) {
        model->dirty_rows[row] = true;
    }
    return 1;
}

static int ignore_moverect(VTermRect dest, VTermRect src, void *user)
{
    (void)dest;
    (void)src;
    (void)user;
    return 1;
}

static int track_cursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    (void)pos;
    (void)oldpos;
    cubicle_terminal_model_t *model = user;
    model->cursor_visible = visible != 0;
    return 1;
}

static int track_termprop(VTermProp prop, VTermValue *val, void *user)
{
    cubicle_terminal_model_t *model = user;
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        model->cursor_visible = val->boolean != 0;
    }
    return 1;
}

static int track_resize(int rows, int cols, void *user)
{
    cubicle_terminal_model_t *model = user;
    model->rows = (unsigned int)rows;
    model->cols = (unsigned int)cols;
    return 1;
}

static int ignore_bell(void *user)
{
    (void)user;
    return 1;
}

static void clear_scrollback(cubicle_terminal_model_t *model)
{
    if (model == NULL) {
        return;
    }
    for (size_t i = 0; i < model->scrollback_count; ++i) {
        free(model->scrollback[i].cells);
    }
    free(model->scrollback);
    model->scrollback = NULL;
    model->scrollback_count = 0;
    model->scrollback_capacity = 0;
}

static void clear_captured_scrollback(cubicle_terminal_model_t *model)
{
    if (model == NULL) {
        return;
    }
    for (size_t i = 0; i < model->captured_scrollback_count; ++i) {
        size_t index = (model->captured_scrollback_head + i) %
                       model->captured_scrollback_capacity;
        free(model->captured_scrollback[index].cells);
        model->captured_scrollback[index].cells = NULL;
        model->captured_scrollback[index].cols = 0;
    }
    model->captured_scrollback_head = 0;
    model->captured_scrollback_count = 0;
}

static void free_captured_scrollback(cubicle_terminal_model_t *model)
{
    if (model == NULL) {
        return;
    }
    clear_captured_scrollback(model);
    free(model->captured_scrollback);
    model->captured_scrollback = NULL;
    model->captured_scrollback_capacity = 0;
    model->scrollback_capture_limit = 0;
}

static int capture_scrollback_line(cubicle_terminal_model_t *model,
                                   int cols,
                                   const VTermScreenCell *cells)
{
    if (model == NULL || model->scrollback_capture_limit == 0 ||
        cols <= 0 || cells == NULL) {
        return 1;
    }
    if (model->captured_scrollback_capacity == 0) {
        errno = EINVAL;
        return 0;
    }
    cubicle_terminal_cell_t *copy =
        calloc((size_t)cols, sizeof(*copy));
    if (copy == NULL) {
        return 0;
    }
    for (int i = 0; i < cols; ++i) {
        cell_text_from_vterm(&cells[i], &copy[i]);
        if (cell_sgr_from_vterm(&cells[i], &copy[i]) < 0) {
            free(copy);
            return 0;
        }
    }

    size_t slot = 0;
    if (model->captured_scrollback_count ==
        model->captured_scrollback_capacity) {
        slot = model->captured_scrollback_head;
        free(model->captured_scrollback[slot].cells);
        model->captured_scrollback_head =
            (model->captured_scrollback_head + 1) %
            model->captured_scrollback_capacity;
    } else {
        slot = (model->captured_scrollback_head +
                model->captured_scrollback_count) %
               model->captured_scrollback_capacity;
        model->captured_scrollback_count++;
    }
    model->captured_scrollback[slot].cols = (unsigned int)cols;
    model->captured_scrollback[slot].cells = copy;
    return 1;
}

static int save_scrollback_line(cubicle_terminal_model_t *model,
                                int cols,
                                const VTermScreenCell *cells)
{
    if (model == NULL || cols <= 0 || cells == NULL) {
        return 0;
    }
    VTermScreenCell *copy = malloc((size_t)cols * sizeof(*copy));
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, cells, (size_t)cols * sizeof(*copy));

    if (model->scrollback_count == model->scrollback_capacity) {
        size_t next_capacity = model->scrollback_capacity == 0
                                   ? 64
                                   : model->scrollback_capacity * 2;
        terminal_scrollback_line_t *next =
            realloc(model->scrollback, next_capacity * sizeof(*next));
        if (next == NULL) {
            free(copy);
            return 0;
        }
        model->scrollback = next;
        model->scrollback_capacity = next_capacity;
    }
    model->scrollback[model->scrollback_count].cols = cols;
    model->scrollback[model->scrollback_count].cells = copy;
    model->scrollback_count++;
    return 1;
}

static int restore_scrollback_line(cubicle_terminal_model_t *model,
                                   int cols,
                                   VTermScreenCell *cells)
{
    if (model == NULL || cols <= 0 || cells == NULL) {
        return 0;
    }
    while (model->scrollback_count > 0) {
        terminal_scrollback_line_t *line =
            &model->scrollback[model->scrollback_count - 1];
        if (line->cols == cols) {
            memcpy(cells, line->cells, (size_t)cols * sizeof(*cells));
            free(line->cells);
            model->scrollback_count--;
            return 1;
        }
        free(line->cells);
        model->scrollback_count--;
    }
    return 0;
}

static int track_scrollback_push(int cols, const VTermScreenCell *cells,
                                 void *user)
{
    cubicle_terminal_model_t *model = user;
    int saved = save_scrollback_line(model, cols, cells);
    int captured = capture_scrollback_line(model, cols, cells);
    return saved && captured;
}

static int track_scrollback_pop(int cols, VTermScreenCell *cells, void *user)
{
    return restore_scrollback_line(user, cols, cells);
}

static int track_scrollback_clear(void *user)
{
    clear_scrollback(user);
    return 1;
}

static void capture_response(const char *data, size_t length, void *user)
{
    cubicle_terminal_model_t *model = user;
    size_t available = sizeof(model->response) - model->response_length;
    if (length > available) {
        length = available;
    }
    if (length > 0) {
        memcpy(model->response + model->response_length, data, length);
        model->response_length += length;
    }
}

static const VTermScreenCallbacks screen_callbacks = {
    .damage = track_damage,
    .moverect = ignore_moverect,
    .movecursor = track_cursor,
    .settermprop = track_termprop,
    .bell = ignore_bell,
    .resize = track_resize,
    .sb_pushline = track_scrollback_push,
    .sb_popline = track_scrollback_pop,
    .sb_clear = track_scrollback_clear,
};

static void log_vterm_no_progress(const unsigned char *cursor,
                                  size_t remaining)
{
    char hex[3 * 10 + 1];
    size_t sample = remaining < 10 ? remaining : 10;
    size_t used = 0;
    for (size_t i = 0; i < sample; ++i) {
        int length = snprintf(hex + used, sizeof(hex) - used, "%s%02x",
                              i == 0 ? "" : " ", cursor[i]);
        if (length < 0 || (size_t)length >= sizeof(hex) - used) {
            break;
        }
        used += (size_t)length;
    }
    fprintf(stderr,
            "[WARN] terminal_model: vterm_input_write made no progress; remaining=%zu first_bytes_hex=%s\n",
            remaining, sample == 0 ? "" : hex);
}

static void track_application_cursor_mode(cubicle_terminal_model_t *model,
                                          const unsigned char *data,
                                          size_t length)
{
    unsigned char buffer[sizeof(model->input_tail) + 4096];
    size_t prefix_length = model->input_tail_length;
    size_t data_length = length;
    if (data_length > 4096) {
        data += data_length - 4096;
        data_length = 4096;
    }
    memcpy(buffer, model->input_tail, prefix_length);
    if (data_length > 0) {
        memcpy(buffer + prefix_length, data, data_length);
    }
    size_t total = prefix_length + data_length;

    for (size_t i = 0; i + 4 < total; ++i) {
        if (buffer[i] != 0x1b || buffer[i + 1] != '[' ||
            buffer[i + 2] != '?') {
            continue;
        }
        size_t cursor = i + 3;
        bool has_mode_one = false;
        unsigned int value = 0;
        bool have_digits = false;
        while (cursor < total) {
            unsigned char ch = buffer[cursor];
            if (ch >= '0' && ch <= '9') {
                have_digits = true;
                value = value * 10 + (unsigned int)(ch - '0');
                cursor++;
                continue;
            }
            if (ch == ';') {
                if (have_digits && value == 1) {
                    has_mode_one = true;
                }
                value = 0;
                have_digits = false;
                cursor++;
                continue;
            }
            if (ch == 'h' || ch == 'l') {
                if (have_digits && value == 1) {
                    has_mode_one = true;
                }
                if (has_mode_one) {
                    model->application_cursor = ch == 'h';
                }
                break;
            }
            if (ch >= 0x40 && ch <= 0x7e) {
                break;
            }
            cursor++;
        }
    }

    model->input_tail_length =
        total < sizeof(model->input_tail) ? total : sizeof(model->input_tail);
    if (model->input_tail_length > 0) {
        memcpy(model->input_tail,
               buffer + total - model->input_tail_length,
               model->input_tail_length);
    }
}

static size_t append_utf8(char *buffer, size_t used, size_t capacity,
                          uint32_t codepoint)
{
    if (codepoint == 0) {
        return used;
    }
    if (codepoint < 0x80) {
        if (used + 1 < capacity) {
            buffer[used++] = (char)codepoint;
        }
    } else if (codepoint < 0x800) {
        if (used + 2 < capacity) {
            buffer[used++] = (char)(0xc0 | (codepoint >> 6));
            buffer[used++] = (char)(0x80 | (codepoint & 0x3f));
        }
    } else if (codepoint < 0x10000) {
        if (used + 3 < capacity) {
            buffer[used++] = (char)(0xe0 | (codepoint >> 12));
            buffer[used++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
            buffer[used++] = (char)(0x80 | (codepoint & 0x3f));
        }
    } else if (codepoint <= 0x10ffff) {
        if (used + 4 < capacity) {
            buffer[used++] = (char)(0xf0 | (codepoint >> 18));
            buffer[used++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
            buffer[used++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
            buffer[used++] = (char)(0x80 | (codepoint & 0x3f));
        }
    }
    buffer[used] = '\0';
    return used;
}

static void cell_text_from_vterm(const VTermScreenCell *source,
                                 cubicle_terminal_cell_t *target)
{
    size_t used = 0;
    target->text[0] = '\0';
    for (size_t i = 0; i < VTERM_MAX_CHARS_PER_CELL; ++i) {
        used = append_utf8(target->text, used, sizeof(target->text),
                           source->chars[i]);
    }
    if (target->text[0] == '\0') {
        snprintf(target->text, sizeof(target->text), " ");
    }
}

static int append_sgr_number(char *buffer, size_t capacity, size_t *used,
                             const char *prefix, int value)
{
    int length = snprintf(buffer + *used, capacity - *used, "%s%d", prefix,
                          value);
    if (length < 0 || (size_t)length >= capacity - *used) {
        errno = ENOSPC;
        return -1;
    }
    *used += (size_t)length;
    return 0;
}

static int append_sgr_color(char *buffer, size_t capacity, size_t *used,
                            const char *prefix, VTermColor color)
{
    if (VTERM_COLOR_IS_INDEXED(&color)) {
        int length = snprintf(buffer + *used, capacity - *used, "%s5;%u",
                              prefix, (unsigned int)color.indexed.idx);
        if (length < 0 || (size_t)length >= capacity - *used) {
            errno = ENOSPC;
            return -1;
        }
        *used += (size_t)length;
    } else if (VTERM_COLOR_IS_RGB(&color)) {
        int length = snprintf(buffer + *used, capacity - *used, "%s2;%u;%u;%u",
                              prefix, (unsigned int)color.rgb.red,
                              (unsigned int)color.rgb.green,
                              (unsigned int)color.rgb.blue);
        if (length < 0 || (size_t)length >= capacity - *used) {
            errno = ENOSPC;
            return -1;
        }
        *used += (size_t)length;
    }
    return 0;
}

static int cell_sgr_from_vterm(const VTermScreenCell *source,
                               cubicle_terminal_cell_t *target)
{
    size_t used = 0;
    target->sgr[0] = '\0';

    if (!source->attrs.bold && source->attrs.underline == VTERM_UNDERLINE_OFF &&
        !source->attrs.italic && !source->attrs.blink &&
        !source->attrs.reverse && !source->attrs.conceal &&
        !source->attrs.strike && VTERM_COLOR_IS_DEFAULT_FG(&source->fg) &&
        VTERM_COLOR_IS_DEFAULT_BG(&source->bg)) {
        return 0;
    }

    int length = snprintf(target->sgr, sizeof(target->sgr), "\x1b[0");
    if (length < 0 || (size_t)length >= sizeof(target->sgr)) {
        errno = ENOSPC;
        return -1;
    }
    used = (size_t)length;
    if (source->attrs.bold &&
        append_sgr_number(target->sgr, sizeof(target->sgr), &used, ";", 1) < 0) {
        return -1;
    }
    if (source->attrs.italic &&
        append_sgr_number(target->sgr, sizeof(target->sgr), &used, ";", 3) < 0) {
        return -1;
    }
    if (source->attrs.underline != VTERM_UNDERLINE_OFF &&
        append_sgr_number(target->sgr, sizeof(target->sgr), &used, ";", 4) < 0) {
        return -1;
    }
    if (source->attrs.blink &&
        append_sgr_number(target->sgr, sizeof(target->sgr), &used, ";", 5) < 0) {
        return -1;
    }
    if (source->attrs.reverse &&
        append_sgr_number(target->sgr, sizeof(target->sgr), &used, ";", 7) < 0) {
        return -1;
    }
    if (source->attrs.conceal &&
        append_sgr_number(target->sgr, sizeof(target->sgr), &used, ";", 8) < 0) {
        return -1;
    }
    if (source->attrs.strike &&
        append_sgr_number(target->sgr, sizeof(target->sgr), &used, ";", 9) < 0) {
        return -1;
    }
    if (!VTERM_COLOR_IS_DEFAULT_FG(&source->fg) &&
        append_sgr_color(target->sgr, sizeof(target->sgr), &used, ";38;",
                         source->fg) < 0) {
        return -1;
    }
    if (!VTERM_COLOR_IS_DEFAULT_BG(&source->bg) &&
        append_sgr_color(target->sgr, sizeof(target->sgr), &used, ";48;",
                         source->bg) < 0) {
        return -1;
    }
    if (used + 1 >= sizeof(target->sgr)) {
        errno = ENOSPC;
        return -1;
    }
    target->sgr[used++] = 'm';
    target->sgr[used] = '\0';
    return 0;
}

int cubicle_terminal_model_create(unsigned int rows, unsigned int cols,
                                  cubicle_terminal_model_t **model_out)
{
    if (rows == 0 || cols == 0 || rows > 1000 || cols > 1000 ||
        model_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    cubicle_terminal_model_t *model = calloc(1, sizeof(*model));
    if (model == NULL) {
        return -1;
    }
    model->term = vterm_new((int)rows, (int)cols);
    if (model->term == NULL) {
        free(model);
        errno = ENOMEM;
        return -1;
    }
    model->screen = vterm_obtain_screen(model->term);
    model->rows = rows;
    model->cols = cols;
    model->cursor_visible = true;
    vterm_set_utf8(model->term, 1);
    vterm_output_set_callback(model->term, capture_response, model);
    vterm_screen_set_callbacks(model->screen, &screen_callbacks, model);
    vterm_screen_enable_altscreen(model->screen, 1);
    vterm_screen_set_damage_merge(model->screen, VTERM_DAMAGE_ROW);
    vterm_screen_reset(model->screen, 1);
    for (unsigned int row = 0; row < rows; ++row) {
        model->dirty_rows[row] = true;
    }

    *model_out = model;
    return 0;
}

void cubicle_terminal_model_destroy(cubicle_terminal_model_t *model)
{
    if (model != NULL) {
        clear_scrollback(model);
        free_captured_scrollback(model);
        vterm_free(model->term);
        free(model);
    }
}

int cubicle_terminal_model_resize(cubicle_terminal_model_t *model,
                                  unsigned int rows, unsigned int cols)
{
    if (model == NULL || rows == 0 || cols == 0 || rows > 1000 ||
        cols > 1000) {
        errno = EINVAL;
        return -1;
    }
    vterm_set_size(model->term, (int)rows, (int)cols);
    model->rows = rows;
    model->cols = cols;
    vterm_screen_flush_damage(model->screen);
    for (unsigned int row = 0; row < rows; ++row) {
        model->dirty_rows[row] = true;
    }
    return 0;
}

int cubicle_terminal_model_feed(cubicle_terminal_model_t *model,
                                const void *data, size_t length)
{
    if (model == NULL || (data == NULL && length > 0)) {
        errno = EINVAL;
        return -1;
    }
    track_application_cursor_mode(model, data, length);
    const char *cursor = data;
    size_t remaining = length;
    while (remaining > 0) {
        size_t written = vterm_input_write(model->term, cursor, remaining);
        if (written == 0) {
            log_vterm_no_progress((const unsigned char *)cursor, remaining);
            break;
        }
        cursor += written;
        remaining -= written;
    }
    vterm_screen_flush_damage(model->screen);
    return 0;
}

int cubicle_terminal_model_set_scrollback_capture_limit(
    cubicle_terminal_model_t *model,
    size_t line_limit)
{
    if (model == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (line_limit == 0) {
        free_captured_scrollback(model);
        return 0;
    }
    cubicle_terminal_scrollback_line_t *next =
        calloc(line_limit, sizeof(*next));
    if (next == NULL) {
        return -1;
    }
    free_captured_scrollback(model);
    model->captured_scrollback = next;
    model->captured_scrollback_capacity = line_limit;
    model->scrollback_capture_limit = line_limit;
    return 0;
}

int cubicle_terminal_model_take_scrollback(
    cubicle_terminal_model_t *model,
    cubicle_terminal_scrollback_line_t **lines_out,
    size_t *line_count_out)
{
    if (model == NULL || lines_out == NULL || line_count_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *lines_out = NULL;
    *line_count_out = 0;
    if (model->captured_scrollback_count == 0) {
        return 0;
    }
    cubicle_terminal_scrollback_line_t *lines =
        calloc(model->captured_scrollback_count, sizeof(*lines));
    if (lines == NULL) {
        return -1;
    }
    for (size_t i = 0; i < model->captured_scrollback_count; ++i) {
        size_t index = (model->captured_scrollback_head + i) %
                       model->captured_scrollback_capacity;
        lines[i] = model->captured_scrollback[index];
        model->captured_scrollback[index].cells = NULL;
        model->captured_scrollback[index].cols = 0;
    }
    *lines_out = lines;
    *line_count_out = model->captured_scrollback_count;
    model->captured_scrollback_head = 0;
    model->captured_scrollback_count = 0;
    return 0;
}

void cubicle_terminal_scrollback_cleanup(
    cubicle_terminal_scrollback_line_t *lines,
    size_t line_count)
{
    if (lines == NULL) {
        return;
    }
    for (size_t i = 0; i < line_count; ++i) {
        free(lines[i].cells);
    }
    free(lines);
}

int cubicle_terminal_model_get_dirty_rows(cubicle_terminal_model_t *model,
                                          bool *rows,
                                          size_t row_count)
{
    if (model == NULL || rows == NULL || row_count < model->rows) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned int row = 0; row < model->rows; ++row) {
        rows[row] = model->dirty_rows[row];
    }
    return 0;
}

void cubicle_terminal_model_clear_dirty_rows(cubicle_terminal_model_t *model)
{
    if (model != NULL) {
        memset(model->dirty_rows, 0, sizeof(model->dirty_rows));
    }
}

ssize_t cubicle_terminal_model_take_response(cubicle_terminal_model_t *model,
                                             void *buffer, size_t length)
{
    if (model == NULL || buffer == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t copied = model->response_length < length ? model->response_length
                                                    : length;
    memcpy(buffer, model->response, copied);
    if (copied < model->response_length) {
        memmove(model->response, model->response + copied,
                model->response_length - copied);
    }
    model->response_length -= copied;
    return (ssize_t)copied;
}

static VTermKey terminal_key_to_vterm(cubicle_terminal_key_t key)
{
    switch (key) {
    case CUBICLE_TERMINAL_KEY_UP:
        return VTERM_KEY_UP;
    case CUBICLE_TERMINAL_KEY_DOWN:
        return VTERM_KEY_DOWN;
    case CUBICLE_TERMINAL_KEY_LEFT:
        return VTERM_KEY_LEFT;
    case CUBICLE_TERMINAL_KEY_RIGHT:
        return VTERM_KEY_RIGHT;
    }
    return VTERM_KEY_NONE;
}

int cubicle_terminal_model_encode_key(cubicle_terminal_model_t *model,
                                      cubicle_terminal_key_t key,
                                      void *buffer,
                                      size_t length,
                                      size_t *written_out)
{
    if (model == NULL || buffer == NULL || length == 0 ||
        written_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    VTermKey vterm_key = terminal_key_to_vterm(key);
    if (vterm_key == VTERM_KEY_NONE) {
        errno = EINVAL;
        return -1;
    }

    if (key == CUBICLE_TERMINAL_KEY_UP ||
        key == CUBICLE_TERMINAL_KEY_DOWN ||
        key == CUBICLE_TERMINAL_KEY_LEFT ||
        key == CUBICLE_TERMINAL_KEY_RIGHT) {
        if (length < 3) {
            errno = ENOSPC;
            return -1;
        }
        unsigned char final = 'A';
        if (key == CUBICLE_TERMINAL_KEY_DOWN) {
            final = 'B';
        } else if (key == CUBICLE_TERMINAL_KEY_RIGHT) {
            final = 'C';
        } else if (key == CUBICLE_TERMINAL_KEY_LEFT) {
            final = 'D';
        }
        unsigned char *out = buffer;
        out[0] = 0x1b;
        out[1] = model->application_cursor ? 'O' : '[';
        out[2] = final;
        *written_out = 3;
        return 0;
    }

    size_t previous_response_length = model->response_length;
    vterm_keyboard_key(model->term, vterm_key, VTERM_MOD_NONE);
    size_t generated = model->response_length - previous_response_length;
    if (generated > length) {
        model->response_length = previous_response_length;
        errno = ENOSPC;
        return -1;
    }
    memcpy(buffer, model->response + previous_response_length, generated);
    model->response_length = previous_response_length;
    *written_out = generated;
    return 0;
}

int cubicle_terminal_model_snapshot(cubicle_terminal_model_t *model,
                                    uint64_t offset,
                                    cubicle_terminal_snapshot_t *snapshot_out)
{
    if (model == NULL || snapshot_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    size_t count = (size_t)model->rows * (size_t)model->cols;
    cubicle_terminal_cell_t *cells = calloc(count, sizeof(*cells));
    if (cells == NULL) {
        return -1;
    }

    for (unsigned int row = 0; row < model->rows; ++row) {
        for (unsigned int col = 0; col < model->cols; ++col) {
            VTermScreenCell cell;
            memset(&cell, 0, sizeof(cell));
            VTermPos pos = {(int)row, (int)col};
            cubicle_terminal_cell_t *target =
                &cells[(size_t)row * (size_t)model->cols + (size_t)col];
            if (vterm_screen_get_cell(model->screen, pos, &cell)) {
                cell_text_from_vterm(&cell, target);
                if (cell_sgr_from_vterm(&cell, target) < 0) {
                    free(cells);
                    return -1;
                }
            } else {
                snprintf(target->text, sizeof(target->text), " ");
            }
        }
    }

    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(model->term), &cursor);
    snapshot_out->rows = model->rows;
    snapshot_out->cols = model->cols;
    snapshot_out->cursor_row = cursor.row < 0 ? 0 : (unsigned int)cursor.row;
    snapshot_out->cursor_col = cursor.col < 0 ? 0 : (unsigned int)cursor.col;
    snapshot_out->cursor_visible = model->cursor_visible;
    snapshot_out->application_cursor = model->application_cursor;
    snapshot_out->offset = offset;
    snapshot_out->cells = cells;
    return 0;
}

int cubicle_terminal_model_load_snapshot(
    cubicle_terminal_model_t *model,
    const cubicle_terminal_snapshot_t *snapshot)
{
    if (model == NULL || snapshot == NULL || snapshot->cells == NULL ||
        snapshot->rows == 0 || snapshot->cols == 0 ||
        snapshot->rows > 1000 || snapshot->cols > 1000) {
        errno = EINVAL;
        return -1;
    }
    clear_scrollback(model);
    clear_captured_scrollback(model);
    if (cubicle_terminal_model_resize(model, snapshot->rows,
                                      snapshot->cols) < 0) {
        return -1;
    }
    vterm_screen_reset(model->screen, 1);

    char active_sgr[96] = "";
    for (unsigned int row = 0; row < snapshot->rows; ++row) {
        char cursor[32];
        int length = snprintf(cursor, sizeof(cursor), "\x1b[%u;1H", row + 1);
        if (length < 0 || (size_t)length >= sizeof(cursor) ||
            cubicle_terminal_model_feed(model, cursor, (size_t)length) < 0) {
            return -1;
        }
        for (unsigned int col = 0; col < snapshot->cols; ++col) {
            const cubicle_terminal_cell_t *cell =
                &snapshot->cells[(size_t)row * (size_t)snapshot->cols + col];
            const char *sgr = cell->sgr;
            if (strcmp(active_sgr, sgr) != 0) {
                const char *sequence = sgr[0] == '\0' ? "\x1b[0m" : sgr;
                if (cubicle_terminal_model_feed(model, sequence,
                                                strlen(sequence)) < 0) {
                    return -1;
                }
                snprintf(active_sgr, sizeof(active_sgr), "%s", sgr);
            }
            const char *text = cell->text[0] == '\0' ? " " : cell->text;
            if (cubicle_terminal_model_feed(model, text, strlen(text)) < 0) {
                return -1;
            }
        }
    }

    char cursor[32];
    int length = snprintf(cursor, sizeof(cursor), "\x1b[%u;%uH",
                          snapshot->cursor_row + 1,
                          snapshot->cursor_col + 1);
    if (length < 0 || (size_t)length >= sizeof(cursor) ||
        cubicle_terminal_model_feed(model, cursor, (size_t)length) < 0) {
        return -1;
    }
    if (snapshot->application_cursor &&
        cubicle_terminal_model_feed(model, "\x1b[?1h", 5) < 0) {
        return -1;
    }
    model->cursor_visible = snapshot->cursor_visible;
    for (unsigned int row = 0; row < model->rows; ++row) {
        model->dirty_rows[row] = true;
    }
    return 0;
}

void cubicle_terminal_snapshot_cleanup(cubicle_terminal_snapshot_t *snapshot)
{
    if (snapshot != NULL) {
        free(snapshot->cells);
        memset(snapshot, 0, sizeof(*snapshot));
    }
}
