#include "cubicle/manager_registry.h"

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

static void expect_string(const char *actual, const char *expected,
                          const char *message)
{
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "%s: expected '%s', got '%s'\n", message, expected,
                actual);
        ++failures;
    }
}

static void test_workspace_record(void)
{
    char line[] = "workspace-id\tProject A\n";
    cubicle_workspace_record_t record;

    expect_true(cubicle_parse_workspace_record(line, &record) == 0,
                "expected valid workspace record to parse");
    expect_string(record.id, "workspace-id", "workspace id");
    expect_string(record.name, "Project A", "workspace name");

    char malformed[] = "workspace-id-only\n";
    expect_true(cubicle_parse_workspace_record(malformed, &record) < 0,
                "expected malformed workspace record to fail");
}

static void test_process_record(void)
{
    char line[] = "process-id\tworkspace-id\tmake-1\tstream\trunning\tcontroller-id\t/tmp/control.sock\n";
    cubicle_process_record_t record;

    expect_true(cubicle_parse_process_record(line, &record) == 0,
                "expected valid process record to parse");
    expect_string(record.process_id, "process-id", "process id");
    expect_string(record.workspace_id, "workspace-id", "workspace id");
    expect_string(record.friendly_name, "make-1", "friendly name");
    expect_string(record.mode, "stream", "mode");
    expect_string(record.state, "running", "state");
    expect_string(record.controller_id, "controller-id", "controller id");
    expect_string(record.control_socket, "/tmp/control.sock", "control socket");

    char malformed[] = "process-id\tworkspace-id\tmake-1\n";
    expect_true(cubicle_parse_process_record(malformed, &record) < 0,
                "expected malformed process record to fail");
}

static void test_cursor_record(void)
{
    char line[] = "process-id\t42\n";
    cubicle_cursor_record_t record;

    expect_true(cubicle_parse_cursor_record(line, &record) == 0,
                "expected valid cursor record to parse");
    expect_string(record.process_id, "process-id", "cursor process id");
    expect_true(record.sequence == 42, "expected cursor sequence 42");
    expect_true(record.offset == 0, "expected legacy cursor offset 0");

    char offset_line[] = "process-id\t43\t128\n";
    expect_true(cubicle_parse_cursor_record(offset_line, &record) == 0,
                "expected offset cursor record to parse");
    expect_string(record.process_id, "process-id", "offset cursor process id");
    expect_true(record.sequence == 43, "expected offset cursor sequence 43");
    expect_true(record.offset == 128, "expected cursor offset 128");

    char malformed[] = "process-id-only\n";
    expect_true(cubicle_parse_cursor_record(malformed, &record) < 0,
                "expected malformed cursor record to fail");
}

static void test_event_sequence(void)
{
    long long sequence = 0;

    expect_true(cubicle_parse_event_sequence(
                    "seq=17 type=output stream=stdout start=0 length=5",
                    &sequence) == 0,
                "expected valid event sequence to parse");
    expect_true(sequence == 17, "expected event sequence 17");

    expect_true(cubicle_parse_event_sequence("type=output seq=17", &sequence) < 0,
                "expected event without leading sequence to fail");
}

int main(void)
{
    test_workspace_record();
    test_process_record();
    test_cursor_record();
    test_event_sequence();

    return failures == 0 ? 0 : 1;
}
