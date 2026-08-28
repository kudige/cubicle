#ifndef CUBICLE_TERMINAL_MODEL_H
#define CUBICLE_TERMINAL_MODEL_H

#include "cubicle/terminal_snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct cubicle_terminal_model cubicle_terminal_model_t;

typedef struct cubicle_terminal_scrollback_line {
    unsigned int cols;
    cubicle_terminal_cell_t *cells;
} cubicle_terminal_scrollback_line_t;

typedef enum cubicle_terminal_key {
    CUBICLE_TERMINAL_KEY_UP = 1,
    CUBICLE_TERMINAL_KEY_DOWN,
    CUBICLE_TERMINAL_KEY_LEFT,
    CUBICLE_TERMINAL_KEY_RIGHT,
} cubicle_terminal_key_t;

int cubicle_terminal_model_create(unsigned int rows, unsigned int cols,
                                  cubicle_terminal_model_t **model_out);
void cubicle_terminal_model_destroy(cubicle_terminal_model_t *model);
int cubicle_terminal_model_resize(cubicle_terminal_model_t *model,
                                  unsigned int rows, unsigned int cols);
int cubicle_terminal_model_feed(cubicle_terminal_model_t *model,
                                const void *data, size_t length);
int cubicle_terminal_model_set_scrollback_capture_limit(
    cubicle_terminal_model_t *model,
    size_t line_limit);
int cubicle_terminal_model_take_scrollback(
    cubicle_terminal_model_t *model,
    cubicle_terminal_scrollback_line_t **lines_out,
    size_t *line_count_out);
void cubicle_terminal_scrollback_cleanup(
    cubicle_terminal_scrollback_line_t *lines,
    size_t line_count);
int cubicle_terminal_model_get_dirty_rows(cubicle_terminal_model_t *model,
                                          bool *rows,
                                          size_t row_count);
void cubicle_terminal_model_clear_dirty_rows(cubicle_terminal_model_t *model);
ssize_t cubicle_terminal_model_take_response(cubicle_terminal_model_t *model,
                                             void *buffer, size_t length);
int cubicle_terminal_model_encode_key(cubicle_terminal_model_t *model,
                                      cubicle_terminal_key_t key,
                                      void *buffer,
                                      size_t length,
                                      size_t *written_out);
int cubicle_terminal_model_snapshot(cubicle_terminal_model_t *model,
                                    uint64_t offset,
                                    cubicle_terminal_snapshot_t *snapshot_out);
int cubicle_terminal_model_load_snapshot(
    cubicle_terminal_model_t *model,
    const cubicle_terminal_snapshot_t *snapshot);
void cubicle_terminal_snapshot_cleanup(cubicle_terminal_snapshot_t *snapshot);

#endif
