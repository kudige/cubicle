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

static void test_keyboard_encoding_tracks_cursor_mode(void)
{
    cubicle_terminal_model_t *model = NULL;
    char output[16];
    size_t output_length = 0;

    expect_true(cubicle_terminal_model_create(2, 4, &model) == 0,
                "expected terminal model creation to succeed");
    if (model == NULL) {
        return;
    }

    expect_true(cubicle_terminal_model_encode_key(
                    model, CUBICLE_TERMINAL_KEY_UP, output, sizeof(output),
                    &output_length) == 0,
                "expected normal cursor key encoding to succeed");
    expect_true(output_length == 3 &&
                    memcmp(output, "\x1b[A", output_length) == 0,
                "expected normal cursor Up sequence");

    expect_true(cubicle_terminal_model_feed(model, "\x1b[?1h", 5) == 0,
                "expected application cursor mode feed to succeed");
    output_length = 0;
    expect_true(cubicle_terminal_model_encode_key(
                    model, CUBICLE_TERMINAL_KEY_UP, output, sizeof(output),
                    &output_length) == 0,
                "expected application cursor key encoding to succeed");
    expect_true(output_length == 3 &&
                    memcmp(output, "\x1bOA", output_length) == 0,
                "expected application cursor Up sequence");

    cubicle_terminal_snapshot_t snapshot;
    cubicle_terminal_model_t *restored = NULL;
    expect_true(cubicle_terminal_model_snapshot(model, 0, &snapshot) == 0,
                "expected application cursor snapshot to succeed");
    expect_true(snapshot.application_cursor,
                "expected snapshot to preserve application cursor mode");
    expect_true(cubicle_terminal_model_create(2, 4, &restored) == 0,
                "expected restored terminal model creation to succeed");
    expect_true(cubicle_terminal_model_load_snapshot(restored, &snapshot) == 0,
                "expected application cursor snapshot load to succeed");
    output_length = 0;
    expect_true(cubicle_terminal_model_encode_key(
                    restored, CUBICLE_TERMINAL_KEY_DOWN, output,
                    sizeof(output), &output_length) == 0,
                "expected restored application cursor encoding to succeed");
    expect_true(output_length == 3 &&
                    memcmp(output, "\x1bOB", output_length) == 0,
                "expected restored application cursor Down sequence");
    cubicle_terminal_model_destroy(restored);
    cubicle_terminal_snapshot_cleanup(&snapshot);

    expect_true(cubicle_terminal_model_feed(model, "\x1b[?1l", 5) == 0,
                "expected normal cursor mode feed to succeed");
    output_length = 0;
    expect_true(cubicle_terminal_model_encode_key(
                    model, CUBICLE_TERMINAL_KEY_DOWN, output, sizeof(output),
                    &output_length) == 0,
                "expected restored cursor key encoding to succeed");
    expect_true(output_length == 3 &&
                    memcmp(output, "\x1b[B", output_length) == 0,
                "expected normal cursor Down sequence");

    cubicle_terminal_model_destroy(model);
}

static void test_resize_growth_restores_scrollback(void)
{
    cubicle_terminal_model_t *model = NULL;
    cubicle_terminal_snapshot_t snapshot;

    expect_true(cubicle_terminal_model_create(4, 8, &model) == 0,
                "expected terminal model creation to succeed");
    if (model == NULL) {
        return;
    }
    const char *input = "1\r\n2\r\n3\r\n4\r\n5\r\n6";
    expect_true(cubicle_terminal_model_feed(model, input, strlen(input)) == 0,
                "expected scrollback input feed to succeed");
    expect_true(cubicle_terminal_model_resize(model, 6, 8) == 0,
                "expected terminal growth to succeed");
    expect_true(cubicle_terminal_model_snapshot(model, 0, &snapshot) == 0,
                "expected grown terminal snapshot to succeed");
    expect_true(strcmp(snapshot_cell(&snapshot, 0, 0)->text, "1") == 0,
                "expected first scrolled line to return after growth");
    expect_true(strcmp(snapshot_cell(&snapshot, 1, 0)->text, "2") == 0,
                "expected second scrolled line to return after growth");
    expect_true(strcmp(snapshot_cell(&snapshot, 5, 0)->text, "6") == 0,
                "expected prompt-side content to remain at bottom");
    cubicle_terminal_snapshot_cleanup(&snapshot);
    cubicle_terminal_model_destroy(model);
}

static void test_scrollback_capture_limit_and_take(void)
{
    cubicle_terminal_model_t *model = NULL;
    cubicle_terminal_scrollback_line_t *lines = NULL;
    size_t line_count = 0;

    expect_true(cubicle_terminal_model_create(2, 6, &model) == 0,
                "expected terminal model creation to succeed");
    if (model == NULL) {
        return;
    }
    expect_true(cubicle_terminal_model_set_scrollback_capture_limit(model, 2) == 0,
                "expected scrollback capture limit to be set");
    const char *input = "one\r\ntwo\r\nthree\r\nfour";
    expect_true(cubicle_terminal_model_feed(model, input, strlen(input)) == 0,
                "expected scrollback-producing input to succeed");
    expect_true(cubicle_terminal_model_take_scrollback(model, &lines,
                                                       &line_count) == 0,
                "expected captured scrollback to be drained");
    expect_true(line_count == 2, "expected capture to retain last two lines");
    if (line_count == 2) {
        expect_true(lines[0].cols == 6 && lines[0].cells != NULL,
                    "expected older retained scrollback cells");
        expect_true(lines[1].cols == 6 && lines[1].cells != NULL,
                    "expected newest retained scrollback cells");
    }
    cubicle_terminal_scrollback_cleanup(lines, line_count);
    lines = NULL;
    line_count = 0;
    expect_true(cubicle_terminal_model_take_scrollback(model, &lines,
                                                       &line_count) == 0,
                "expected empty second drain to succeed");
    expect_true(line_count == 0 && lines == NULL,
                "expected second drain to be empty");
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
    test_keyboard_encoding_tracks_cursor_mode();
    test_resize_growth_restores_scrollback();
    test_scrollback_capture_limit_and_take();
    test_dirty_rows();
    test_incomplete_utf8_is_discarded();
    return failures == 0 ? 0 : 1;
}
