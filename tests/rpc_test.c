#include "cubicle/rpc.h"
#include "../src/common/json.h"
#include "../src/common/rpc_internal.h"

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

    const unsigned char binary_value[] = {'A', 0xff, '\033', '\0', 'Z'};
    expect_int(cubicle_json_builder_append_string_n(
                   &builder, binary_value, sizeof(binary_value)),
               0, "builder counted binary string");
    expect_string(builder.data, "\"A\\u00ff\\u001b\\u0000Z\"",
                  "builder counted binary result");
    cubicle_json_builder_cleanup(&builder);

    const unsigned char utf8_value[] = {
        'L', 0xe2, 0x94, 0x80, 'R', 0xe2, 0x94
    };
    expect_int(cubicle_json_builder_append_string_n(
                   &builder, utf8_value, sizeof(utf8_value)),
               0, "builder counted utf8 string");
    expect_string(builder.data, "\"L─R\\u00e2\\u0094\"",
                  "builder counted utf8 result");
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

    expect_int(cubicle_rpc_response_error(
                   "{\"request_id\":\"req-2\",\"success\":true,"
                   "\"result\":{}}",
                   &error),
               -1, "success is not error");
    expect_int(cubicle_rpc_response_error(
                   "{\"request_id\":\"req-2\",\"success\":false,"
                   "\"result\":{},\"error\":{\"code\":\"timeout\","
                   "\"message\":\"too slow\",\"retryable\":true,"
                   "\"system_errno\":110}}",
                   &error),
               -1, "error response rejects result");
    expect_int(cubicle_rpc_response_error(
                   "{\"request_id\":\"req-2\",\"success\":false,"
                   "\"error\":null}",
                   &error),
               -1, "error response rejects null error");
    expect_int(cubicle_rpc_response_error(
                   "{\"request_id\":\"req-2\",\"success\":false,"
                   "\"error\":{\"code\":\"timeout\",\"message\":\"too slow\","
                   "\"retryable\":\"yes\",\"system_errno\":110}}",
                   &error),
               -1, "error response rejects invalid retryable");
    expect_int(cubicle_rpc_response_error(
                   "{\"request_id\":\"req-2\",\"success\":false,"
                   "\"error\":{\"code\":\"timeout\",\"message\":\"too slow\","
                   "\"retryable\":true,\"system_errno\":2147483648}}",
                   &error),
               -1, "error response rejects errno overflow");
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
               -1, "duplicate field");
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

static void test_json_hardening(void)
{
    cubicle_json_doc_t parsed;
    expect_int(cubicle_json_parse(&parsed,
                                  "{\"outer\":{\"id\":\"a\",\"id\":\"b\"}}"),
               -1, "nested duplicate field");

    uint64_t number = 0;
    expect_int(cubicle_rpc_get_uint64("{\"n\":-1}", "n", &number), -1,
               "negative unsigned");
    expect_int(cubicle_rpc_get_uint64("{\"n\":1.5}", "n", &number), -1,
               "fraction unsigned");
    expect_int(cubicle_rpc_get_uint64("{\"n\":1e2}", "n", &number), -1,
               "exponent unsigned");
    expect_int(cubicle_rpc_get_uint64("{\"n\":18446744073709551616}", "n",
                                      &number),
               -1, "overflow unsigned");

    char nested[256];
    size_t used = 0;
    nested[used++] = '{';
    for (unsigned i = 0; i < CUBICLE_JSON_MAX_DEPTH + 1; ++i) {
        nested[used++] = '"';
        nested[used++] = 'a';
        nested[used++] = '"';
        nested[used++] = ':';
        nested[used++] = '{';
    }
    nested[used++] = '}';
    for (unsigned i = 0; i < CUBICLE_JSON_MAX_DEPTH + 1; ++i) {
        nested[used++] = '}';
    }
    nested[used] = '\0';
    expect_int(cubicle_json_parse(&parsed, nested), -1,
               "maximum depth");

    cubicle_json_builder_t builder = {0};
    expect_int(cubicle_json_builder_append(&builder, "{\"s\":\""), 0,
               "oversized string prefix");
    int append_failed = 0;
    for (size_t i = 0; i < CUBICLE_JSON_MAX_STRING_BYTES + 1; ++i) {
        if (cubicle_json_builder_append(&builder, "x") < 0) {
            append_failed = 1;
            break;
        }
    }
    expect_int(append_failed, 0, "oversized string append");
    expect_int(cubicle_json_builder_append(&builder, "\"}"), 0,
               "oversized string suffix");
    expect_int(cubicle_json_parse(&parsed, builder.data), -1,
               "oversized string");
    cubicle_json_builder_cleanup(&builder);
}

static void test_rpc_envelope_validation(void)
{
    cubicle_rpc_request_envelope_t request;
    const char *valid_request =
        "{\"protocol_major\":0,\"protocol_minor\":1,\"request_id\":\"req-1\","
        "\"session_id\":\"sess-1\",\"method\":\"manager.ping\","
        "\"params\":{}}";
    expect_int(cubicle_rpc_decode_request(&request, valid_request), 0,
               "decode request envelope");
    expect_string(request.request_id, "req-1", "decoded request id");
    expect_string(request.method, "manager.ping", "decoded method");
    expect_int(request.params != NULL, 1, "decoded params");
    cubicle_rpc_request_envelope_cleanup(&request);

    expect_int(cubicle_rpc_decode_request(
                   &request,
                   "{\"protocol_major\":0,\"protocol_minor\":1,"
                   "\"request_id\":\"req-1\",\"method\":\"manager.ping\","
                   "\"params\":{},\"surprise\":true}"),
               -1, "request unknown top-level field");
    expect_int(cubicle_rpc_decode_request(
                   &request,
                   "{\"protocol_major\":1,\"protocol_minor\":0,"
                   "\"request_id\":\"req-1\",\"method\":\"manager.ping\","
                   "\"params\":{}}"),
               -1, "request major mismatch");
    expect_int(cubicle_rpc_decode_request(
                   &request,
                   "{\"protocol_major\":0,\"protocol_minor\":2,"
                   "\"request_id\":\"req-1\",\"method\":\"manager.ping\","
                   "\"params\":{}}"),
               -1, "request unsupported minor");
    expect_int(cubicle_rpc_decode_request(
                   &request,
                   "{\"protocol_major\":0,\"protocol_minor\":1,"
                   "\"request_id\":\"req-1\",\"method\":\"manager.ping\","
                   "\"params\":null}"),
               -1, "request null params");

    cubicle_rpc_response_envelope_t response;
    expect_int(cubicle_rpc_decode_response(
                   &response,
                   "{\"request_id\":\"req-1\",\"success\":true,"
                   "\"result\":{}}",
                   "req-1"),
               0, "decode response envelope");
    expect_int(response.success, 1, "decoded success");
    cubicle_rpc_response_envelope_cleanup(&response);

    expect_int(cubicle_rpc_decode_response(
                   &response,
                   "{\"request_id\":\"wrong\",\"success\":true,"
                   "\"result\":{}}",
                   "req-1"),
               -1, "response request mismatch");
    expect_int(cubicle_rpc_decode_response(
                   &response,
                   "{\"request_id\":\"req-1\",\"success\":true,"
                   "\"result\":{},\"error\":{}}",
                   "req-1"),
               -1, "response result and error");
    expect_int(cubicle_rpc_decode_response(
                   &response,
                   "{\"request_id\":\"req-1\",\"success\":false,"
                   "\"result\":{}}",
                   "req-1"),
               -1, "error response with result");
    expect_int(cubicle_rpc_decode_response(
                   &response,
                   "{\"request_id\":\"req-1\",\"success\":true,"
                   "\"result\":{},\"surprise\":true}",
                   "req-1"),
               -1, "response unknown top-level field");
}

static void test_checked_json_accessors(void)
{
    cubicle_json_doc_t parsed;
    expect_int(cubicle_json_parse(&parsed,
                                  "{\"id\":\"abc-123\",\"count\":7,"
                                  "\"enabled\":true,\"items\":[],"
                                  "\"child\":{\"ok\":true},"
                                  "\"optional\":null}"),
               0, "checked accessors parse");

    cubicle_validation_error_t error;
    memset(&error, 0, sizeof(error));
    char id[16];
    expect_int(cubicle_json_get_required_string(parsed.root, "id", id,
                                                sizeof(id), &error),
               0, "required string");
    expect_string(id, "abc-123", "required string value");
    expect_int(cubicle_json_validate_ascii_identifier(id, "id", &error), 0,
               "ascii identifier");

    uint64_t count = 0;
    expect_int(cubicle_json_get_required_u64(parsed.root, "count", &count,
                                             &error),
               0, "required u64");
    expect_int((int)count, 7, "required u64 value");

    int present = 0;
    count = 0;
    present = 0;
    expect_int(cubicle_json_get_optional_u64(parsed.root, "count", &count,
                                             &present, &error),
               0, "optional u64 present");
    expect_int(present, 1, "optional u64 present flag");
    expect_int((int)count, 7, "optional u64 value");

    bool enabled = false;
    present = 0;
    expect_int(cubicle_json_get_optional_bool(parsed.root, "enabled",
                                              &enabled, &present, &error),
               0, "optional bool present");
    expect_int(present, 1, "optional bool present flag");
    expect_int(enabled, 1, "optional bool value");

    present = 1;
    char missing[8] = "keep";
    expect_int(cubicle_json_get_optional_string(parsed.root, "missing",
                                                missing, sizeof(missing),
                                                &present, &error),
               0, "optional missing string");
    expect_int(present, 0, "optional missing present");
    expect_string(missing, "keep", "optional missing keeps value");

    expect_int(cubicle_json_get_optional_string(parsed.root, "optional",
                                                missing, sizeof(missing),
                                                &present, &error),
               -1, "optional null rejected");
    expect_string(error.field_path, "optional", "optional null field");
    expect_string(error.expected, "non-null", "optional null expected");

    expect_int(cubicle_json_get_required_string(parsed.root, "count", id,
                                                sizeof(id), &error),
               -1, "required string wrong type");
    expect_string(error.field_path, "count", "wrong type field");
    expect_string(error.expected, "string", "wrong type expected");

    expect_int(cubicle_json_validate_ascii_identifier("abc def", "id",
                                                      &error),
               -1, "ascii identifier rejects spaces");
    expect_string(error.field_path, "id", "ascii invalid field");

    yyjson_val *items = NULL;
    expect_int(cubicle_json_get_required_array(parsed.root, "items", &items,
                                               &error),
               0, "required array");
    expect_int(items != NULL, 1, "required array value");

    yyjson_val *child = NULL;
    expect_int(cubicle_json_get_required_object(parsed.root, "child", &child,
                                                &error),
               0, "required object");
    expect_int(child != NULL, 1, "required object value");

    child = NULL;
    present = 0;
    expect_int(cubicle_json_get_optional_object(parsed.root, "child", &child,
                                                &present, &error),
               0, "optional object present");
    expect_int(present, 1, "optional object present flag");
    expect_int(child != NULL, 1, "optional object value");

    child = NULL;
    present = 1;
    expect_int(cubicle_json_get_optional_object(parsed.root, "missing_child",
                                                &child, &present, &error),
               0, "optional object missing");
    expect_int(present, 0, "optional object missing flag");
    expect_int(child == NULL, 1, "optional object missing value");

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
    test_json_hardening();
    test_rpc_envelope_validation();
    test_checked_json_accessors();
    return failures == 0 ? 0 : 1;
}
