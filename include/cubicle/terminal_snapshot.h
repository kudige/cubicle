#ifndef CUBICLE_TERMINAL_SNAPSHOT_H
#define CUBICLE_TERMINAL_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cubicle_terminal_cell {
    char text[32];
    char sgr[96];
} cubicle_terminal_cell_t;

typedef struct cubicle_terminal_snapshot {
    unsigned int rows;
    unsigned int cols;
    unsigned int cursor_row;
    unsigned int cursor_col;
    bool cursor_visible;
    uint64_t offset;
    cubicle_terminal_cell_t *cells;
} cubicle_terminal_snapshot_t;

void cubicle_terminal_snapshot_cleanup(cubicle_terminal_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
