#include "../src/common/terminal_model.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_true(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        ++failures;
    }
}

static const cubicle_terminal_cell_t *snapshot_cell(
    const cubicle_terminal_snapshot_t *snapshot,
    unsigned int row,
    unsigned int col)
{
    return &snapshot->cells[(size_t)row * (size_t)snapshot->cols + col];
}

static void test_text_cursor_and_attrs(void)
{
    cubicle_terminal_model_t *model = NULL;
    cubicle_terminal_snapshot_t snapshot;

    expect_true(cubicle_terminal_model_create(4, 12, &model) == 0,
                "expected terminal model creation to succeed");
    if (model == NULL) {
        return;
    }

    const char *input = "hello\r\n\x1b[31;1mX\x1b[0m";
    expect_true(cubicle_terminal_model_feed(model, input, strlen(input)) == 0,
                "expected terminal input feed to succeed");
    expect_true(cubicle_terminal_model_snapshot(model, 123, &snapshot) == 0,
                "expected terminal snapshot to succeed");
    expect_true(snapshot.rows == 4 && snapshot.cols == 12,
                "expected snapshot dimensions to match model");
    expect_true(snapshot.offset == 123, "expected snapshot offset to match");
    expect_true(strcmp(snapshot_cell(&snapshot, 0, 0)->text, "h") == 0,
                "expected first cell text");
    expect_true(strcmp(snapshot_cell(&snapshot, 1, 0)->text, "X") == 0,
                "expected second line colored text");
    expect_true(strstr(snapshot_cell(&snapshot, 1, 0)->sgr, "38;5;1") != NULL,
                "expected foreground SGR in colored cell");
    expect_true(strstr(snapshot_cell(&snapshot, 1, 0)->sgr, "1") != NULL,
                "expected bold SGR in colored cell");
    cubicle_terminal_snapshot_cleanup(&snapshot);
    cubicle_terminal_model_destroy(model);
}

static void test_resize_and_terminal_response(void)
{
    cubicle_terminal_model_t *model = NULL;
    cubicle_terminal_snapshot_t snapshot;
    char response[64];

    expect_true(cubicle_terminal_model_create(2, 4, &model) == 0,
                "expected terminal model creation to succeed");
    if (model == NULL) {
        return;
    }
    expect_true(cubicle_terminal_model_resize(model, 3, 5) == 0,
                "expected terminal resize to succeed");
    expect_true(cubicle_terminal_model_feed(model, "\x1b[2;3H\x1b[6n", 10) == 0,
                "expected DSR input feed to succeed");
    ssize_t response_length =
        cubicle_terminal_model_take_response(model, response, sizeof(response));
    expect_true(response_length > 0, "expected terminal response bytes");
    if (response_length > 0) {
        response[response_length] = '\0';
        expect_true(strcmp(response, "\x1b[2;3R") == 0,
                    "expected cursor-position DSR response");
    }
    expect_true(cubicle_terminal_model_snapshot(model, 0, &snapshot) == 0,
                "expected resized terminal snapshot to succeed");
    expect_true(snapshot.rows == 3 && snapshot.cols == 5,
                "expected resized snapshot dimensions");
    cubicle_terminal_snapshot_cleanup(&snapshot);
    cubicle_terminal_model_destroy(model);
}

static void test_dirty_rows(void)
{
    cubicle_terminal_model_t *model = NULL;
    bool dirty[4] = {false, false, false, false};

    expect_true(cubicle_terminal_model_create(4, 8, &model) == 0,
                "expected terminal model creation to succeed");
    if (model == NULL) {
        return;
    }
    cubicle_terminal_model_clear_dirty_rows(model);
    expect_true(cubicle_terminal_model_feed(model, "\x1b[3;1HX", 7) == 0,
                "expected dirty-row feed to succeed");
    expect_true(cubicle_terminal_model_get_dirty_rows(model, dirty,
                                                     sizeof(dirty) /
                                                         sizeof(dirty[0])) == 0,
                "expected dirty rows to be readable");
    expect_true(!dirty[0] && !dirty[1] && dirty[2] && !dirty[3],
                "expected only row 3 to be dirty");
    cubicle_terminal_model_destroy(model);
}

static void test_incomplete_utf8_is_discarded(void)
{
    cubicle_terminal_model_t *model = NULL;
    cubicle_terminal_snapshot_t snapshot;
    const unsigned char input[] = {0xf0};

    expect_true(cubicle_terminal_model_create(2, 4, &model) == 0,
                "expected terminal model creation to succeed");
    if (model == NULL) {
        return;
    }
    expect_true(cubicle_terminal_model_feed(model, input, sizeof(input)) == 0,
                "expected incomplete UTF-8 feed to be discarded");
    expect_true(cubicle_terminal_model_snapshot(model, 0, &snapshot) == 0,
                "expected snapshot after discarded UTF-8 to succeed");
    expect_true(strcmp(snapshot_cell(&snapshot, 0, 0)->text, " ") == 0,
                "expected discarded UTF-8 not to alter screen");
    cubicle_terminal_snapshot_cleanup(&snapshot);
    cubicle_terminal_model_destroy(model);
}

int main(void)
{
    test_text_cursor_and_attrs();
    test_resize_and_terminal_response();
    test_dirty_rows();
    test_incomplete_utf8_is_discarded();
    return failures == 0 ? 0 : 1;
}
