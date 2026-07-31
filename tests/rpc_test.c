#include "cubicle/rpc.h"

#include <errno.h>
#include <stdio.h>
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

    errno = 0;
    expect_int(cubicle_json_escape(buffer, 4, "abcdef"), -1,
               "escape overflow");
    expect_int(errno, ENOSPC, "escape overflow errno");
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
    uint64_t answer = 0;
    expect_int(cubicle_rpc_get_uint64(response, "answer", &answer), 0,
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
}

int main(void)
{
    test_escape();
    test_envelopes();
    test_error();
    test_invalid();
    return failures == 0 ? 0 : 1;
}
