#include "cubicle/rpc.h"
#include "../src/common/json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void expect_int(int actual, int expected, const char *name)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d got %d\n", name, expected, actual);
        ++failures;
    }
}

static void expect_string(const char *actual, const char *expected,
                          const char *name)
{
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "%s: expected %s got %s\n", name, expected, actual);
        ++failures;
    }
}

static void test_escape(void)
{
    char buffer[128];
    expect_int(cubicle_json_escape(buffer, sizeof(buffer), "a\"b\\c\n"), 0,
               "escape");
    expect_string(buffer, "a\\\"b\\\\c\\n", "escaped string");
    expect_int(cubicle_json_escape(buffer, sizeof(buffer), ""), 0,
               "escape empty");
    expect_string(buffer, "", "escaped empty string");

    errno = 0;
    expect_int(cubicle_json_escape(buffer, 4, "abcdef"), -1,
               "escape overflow");
    expect_int(errno, ENOSPC, "escape overflow errno");
}

static void test_builder(void)
{
    cubicle_json_builder_t builder = {0};
    expect_int(cubicle_json_builder_init(&builder), 0, "builder init");
    expect_int(cubicle_json_builder_append(&builder, "{\"name\":"), 0,
               "builder append");
    expect_int(cubicle_json_builder_append_string(&builder, "a\"b"), 0,
               "builder string");
    expect_int(cubicle_json_builder_appendf(&builder, ",\"n\":%d}", 42), 0,
               "builder appendf");
    expect_string(builder.data, "{\"name\":\"a\\\"b\",\"n\":42}",
                  "builder result");
    expect_int((int)builder.length, (int)strlen(builder.data),
               "builder length");
    cubicle_json_builder_cleanup(&builder);
    expect_int(builder.data == NULL, 1, "builder cleanup data");
    expect_int((int)builder.length, 0, "builder cleanup length");

    expect_int(cubicle_json_builder_append_escaped(&builder, "line\n"), 0,
               "builder escaped");
    expect_string(builder.data, "line\\n", "builder escaped result");
    cubicle_json_builder_cleanup(&builder);

    expect_int(cubicle_json_builder_append_string(&builder, NULL), 0,
               "builder null string");
    expect_string(builder.data, "\"\"", "builder null string result");
    cubicle_json_builder_cleanup(&builder);

    expect_int(cubicle_json_builder_reserve(&builder, 4096), 0,
               "builder reserve");
    expect_int(builder.capacity >= 4097, 1, "builder reserve capacity");
    cubicle_json_builder_cleanup(&builder);

    char long_value[1200];
    memset(long_value, 'x', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    expect_int(cubicle_json_builder_append_escaped(&builder, long_value), 0,
               "builder long escaped");
    expect_int((int)builder.length, (int)strlen(long_value),
               "builder long escaped length");
    cubicle_json_builder_cleanup(&builder);
}

static void test_envelopes(void)
{
    char request[512];
    expect_int(cubicle_rpc_request(request, sizeof(request), "req-1", "sess-1",
                                   "manager.ping", "{}"),
               0, "request");
    char value[64];
    expect_int(cubicle_rpc_get_string(request, "request_id", value,
                                      sizeof(value)),
               0, "request id");
    expect_string(value, "req-1", "request id value");
    expect_int(cubicle_rpc_get_string(request, "method", value,
                                      sizeof(value)),
               0, "method");
    expect_string(value, "manager.ping", "method value");

    char params[64];
    expect_int(cubicle_rpc_get_object(request, "params", params,
                                      sizeof(params)),
               0, "params object");
    expect_string(params, "{}", "params value");

    char response[512];
    expect_int(cubicle_rpc_success(response, sizeof(response), "req-1",
                                   "{\"answer\":42,\"okish\":false}"),
               0, "success response");
    int ok = 0;
    expect_int(cubicle_rpc_response_ok(response, &ok), 0, "success ok");
    expect_int(ok, 1, "success ok value");
    char result[128];
    expect_int(cubicle_rpc_get_object(response, "result", result,
                                      sizeof(result)),
               0, "success result");
    uint64_t answer = 0;
    expect_int(cubicle_rpc_get_uint64(result, "answer", &answer), 0,
               "answer");
    expect_int((int)answer, 42, "answer value");
}

static void test_error(void)
{
    char response[512];
    expect_int(cubicle_rpc_error(response, sizeof(response), "req-2",
                                 CUBICLE_ERR_TIMEOUT, "too slow", 1, 110),
               0, "error response");
    int ok = 1;
    expect_int(cubicle_rpc_response_ok(response, &ok), 0, "error ok");
    expect_int(ok, 0, "error ok value");

    cubicle_error_t error;
    memset(&error, 0, sizeof(error));
    expect_int(cubicle_rpc_response_error(response, &error), 0,
               "parse error");
    expect_int(error.code, CUBICLE_ERR_TIMEOUT, "error code");
    expect_int(error.retryable, 1, "error retryable");
    expect_int(error.system_errno, 110, "error errno");
    expect_string(error.message, "too slow", "error message");
}

static void test_invalid(void)
{
    char value[8];
    expect_int(cubicle_rpc_get_string("{}", "missing", value, sizeof(value)),
               -1, "missing string");
    uint64_t number = 0;
    expect_int(cubicle_rpc_get_uint64("{\"n\":\"x\"}", "n", &number), -1,
               "invalid uint");
    int boolean = 0;
    expect_int(cubicle_rpc_get_bool("{\"b\":0}", "b", &boolean), -1,
               "invalid bool");
    char object[4];
    expect_int(cubicle_rpc_get_object("{\"o\":{\"a\":1}}", "o", object,
                                      sizeof(object)),
               -1, "object overflow");
    expect_int(cubicle_rpc_get_string("{\"s\":\"unterminated}", "s", value,
                                      sizeof(value)),
               -1, "malformed json");
    char request[128];
    expect_int(cubicle_rpc_request(request, sizeof(request), "req-3",
                                   "sess-1", "manager.ping", "{bad"),
               -1, "invalid params fragment");
    expect_int(cubicle_rpc_success(request, sizeof(request), "req-4",
                                   "{bad"),
               -1, "invalid result fragment");
}

static void test_direct_field_lookup(void)
{
    const char *json =
        "{\"message\":\"invalid process_id\","
        "\"nested\":{\"process_id\":\"wrong-value\"},"
        "\"process_id\":\"correct-value\"}";
    char value[32];
    expect_int(cubicle_rpc_get_string(json, "process_id", value,
                                      sizeof(value)),
               0, "direct process id");
    expect_string(value, "correct-value", "direct process id value");

    uint64_t number = 0;
    expect_int(cubicle_rpc_get_uint64(
                   "{\"nested\":{\"answer\":7},\"answer\":42}", "answer",
                   &number),
               0, "direct number");
    expect_int((int)number, 42, "direct number value");

    expect_int(cubicle_rpc_get_string(
                   "{\"process_id\":\"first\",\"process_id\":\"second\"}",
                   "process_id", value, sizeof(value)),
               0, "duplicate field");
    expect_string(value, "first", "duplicate field first wins");
}

static void test_json_helpers(void)
{
    cubicle_json_doc_t parsed;
    expect_int(cubicle_json_parse(&parsed,
                                  "{\"n\":-7,\"items\":[{\"id\":\"a\"}],"
                                  "\"copy\":{\"ok\":true}}"),
               0, "json helper parse");
    int64_t signed_number = 0;
    expect_int(cubicle_json_get_i64(parsed.root, "n", &signed_number), 0,
               "json helper i64");
    expect_int((int)signed_number, -7, "json helper i64 value");

    yyjson_val *items = cubicle_json_get_array(parsed.root, "items");
    expect_int(items != NULL, 1, "json helper array");
    expect_int((int)cubicle_json_array_size(items), 1, "json helper array size");
    yyjson_val *item = cubicle_json_array_get(items, 0);
    char id[8];
    expect_int(cubicle_json_get_string(item, "id", id, sizeof(id)), 0,
               "json helper array item");
    expect_string(id, "a", "json helper array item value");

    char *copy = cubicle_json_copy_field(parsed.root, "copy");
    expect_int(copy != NULL, 1, "json helper copy field");
    if (copy != NULL) {
        expect_string(copy, "{\"ok\":true}", "json helper copy field value");
        free(copy);
    }
    cubicle_json_cleanup(&parsed);
}

int main(void)
{
    test_escape();
    test_builder();
    test_envelopes();
    test_error();
    test_invalid();
    test_direct_field_lookup();
    test_json_helpers();
    return failures == 0 ? 0 : 1;
}
