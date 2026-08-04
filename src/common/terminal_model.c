#define _POSIX_C_SOURCE 200809L

#include "terminal_model.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>

struct cubicle_terminal_model {
    VTerm *term;
    VTermScreen *screen;
    unsigned int rows;
    unsigned int cols;
    bool cursor_visible;
    char response[4096];
    size_t response_length;
};

static int ignore_damage(VTermRect rect, void *user)
{
    (void)rect;
    (void)user;
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

static int ignore_scrollback_push(int cols, const VTermScreenCell *cells,
                                  void *user)
{
    (void)cols;
    (void)cells;
    (void)user;
    return 0;
}

static int ignore_scrollback_pop(int cols, VTermScreenCell *cells, void *user)
{
    (void)cols;
    (void)cells;
    (void)user;
    return 0;
}

static int ignore_scrollback_clear(void *user)
{
    (void)user;
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
    .damage = ignore_damage,
    .moverect = ignore_moverect,
    .movecursor = track_cursor,
    .settermprop = track_termprop,
    .bell = ignore_bell,
    .resize = track_resize,
    .sb_pushline = ignore_scrollback_push,
    .sb_popline = ignore_scrollback_pop,
    .sb_clear = ignore_scrollback_clear,
};

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
    vterm_screen_set_damage_merge(model->screen, VTERM_DAMAGE_SCREEN);
    vterm_screen_reset(model->screen, 1);

    *model_out = model;
    return 0;
}

void cubicle_terminal_model_destroy(cubicle_terminal_model_t *model)
{
    if (model != NULL) {
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
    return 0;
}

int cubicle_terminal_model_feed(cubicle_terminal_model_t *model,
                                const void *data, size_t length)
{
    if (model == NULL || (data == NULL && length > 0)) {
        errno = EINVAL;
        return -1;
    }
    const char *cursor = data;
    size_t remaining = length;
    while (remaining > 0) {
        size_t written = vterm_input_write(model->term, cursor, remaining);
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        cursor += written;
        remaining -= written;
    }
    vterm_screen_flush_damage(model->screen);
    return 0;
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
    snapshot_out->offset = offset;
    snapshot_out->cells = cells;
    return 0;
}

void cubicle_terminal_snapshot_cleanup(cubicle_terminal_snapshot_t *snapshot)
{
    if (snapshot != NULL) {
        free(snapshot->cells);
        memset(snapshot, 0, sizeof(*snapshot));
    }
}
