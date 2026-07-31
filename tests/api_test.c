#include "cubicle/api.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_int(int actual, int expected, const char *name)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
        ++failures;
    }
}

static void expect_string(const char *actual, const char *expected,
                          const char *name)
{
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "%s: expected %s, got %s\n", name, expected, actual);
        ++failures;
    }
}

static void test_error_names(void)
{
    expect_string(cubicle_error_code_name(CUBICLE_OK), "ok", "ok name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_INVALID_ARGUMENT),
                  "invalid_argument", "invalid argument name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_NOT_FOUND), "not_found",
                  "not found name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_ALREADY_EXISTS),
                  "already_exists", "already exists name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_AMBIGUOUS_NAME),
                  "ambiguous_name", "ambiguous name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_PERMISSION_DENIED),
                  "permission_denied", "permission denied name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_AUTHENTICATION_FAILED),
                  "authentication_failed", "authentication failed name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_SESSION_EXPIRED),
                  "session_expired", "session expired name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_UNSUPPORTED),
                  "unsupported", "unsupported name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_INVALID_STATE),
                  "invalid_state", "invalid state name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_CONFLICT), "conflict",
                  "conflict name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_TIMEOUT), "timeout",
                  "timeout name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_MANAGER_UNAVAILABLE),
                  "manager_unavailable", "manager unavailable name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_CONTROLLER_UNAVAILABLE),
                  "controller_unavailable", "controller unavailable name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_PROTOCOL), "protocol",
                  "protocol name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_IO), "io", "io name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_RESOURCE_LIMIT),
                  "resource_limit", "resource limit name");
    expect_string(cubicle_error_code_name(CUBICLE_ERR_INTERNAL), "internal",
                  "internal name");
    expect_string(cubicle_error_code_name((cubicle_error_code_t)999),
                  "unknown", "unknown name");
}

static void test_unix_endpoint(void)
{
    cubicle_endpoint_t endpoint;
    expect_int(cubicle_endpoint_parse(&endpoint, "unix:///tmp/cubicle.sock",
                                      "manager-key"),
               0, "parse unix endpoint");
    expect_string(endpoint.uri, "unix:///tmp/cubicle.sock", "endpoint uri");
    expect_string(endpoint.server_identity, "manager-key", "server identity");
    expect_int(cubicle_endpoint_is_unix(&endpoint), 1, "endpoint is unix");

    char path[64];
    expect_int(cubicle_endpoint_unix_path(&endpoint, path, sizeof(path)), 0,
               "extract unix path");
    expect_string(path, "/tmp/cubicle.sock", "unix path");
}

static void test_endpoint_validation(void)
{
    cubicle_endpoint_t endpoint;
    errno = 0;
    expect_int(cubicle_endpoint_parse(NULL, "unix:///tmp/cubicle.sock", NULL),
               -1, "parse null endpoint");
    expect_int(errno, EINVAL, "null endpoint errno");

    errno = 0;
    expect_int(cubicle_endpoint_parse(&endpoint, NULL, NULL), -1,
               "parse null uri");
    expect_int(errno, EINVAL, "null uri errno");

    errno = 0;
    expect_int(cubicle_endpoint_parse(&endpoint, "", NULL), -1,
               "parse empty uri");
    expect_int(errno, EINVAL, "empty uri errno");

    errno = 0;
    expect_int(cubicle_endpoint_parse(&endpoint, "tcp://127.0.0.1:7443", NULL),
               -1, "parse unsupported endpoint");
    expect_int(errno, EPROTONOSUPPORT, "unsupported endpoint errno");

    errno = 0;
    expect_int(cubicle_endpoint_parse(&endpoint, "unix://", NULL), -1,
               "parse empty unix path");
    expect_int(errno, EPROTONOSUPPORT, "empty unix path errno");

    expect_int(cubicle_endpoint_parse(&endpoint, "unix:///tmp/cubicle.sock",
                                      NULL),
               0, "parse null server identity");
    expect_string(endpoint.server_identity, "", "empty server identity");

    errno = 0;
    char path[4];
    expect_int(cubicle_endpoint_unix_path(&endpoint, path, sizeof(path)), -1,
               "short unix path buffer");
    expect_int(errno, ENAMETOOLONG, "short unix path errno");

    errno = 0;
    expect_int(cubicle_endpoint_unix_path(NULL, path, sizeof(path)), -1,
               "null unix endpoint");
    expect_int(errno, EINVAL, "null unix endpoint errno");
}

int main(void)
{
    expect_int(CUBICLE_PROTOCOL_MAJOR, 0, "protocol major");
    expect_int(CUBICLE_PROTOCOL_MINOR, 1, "protocol minor");
    test_error_names();
    test_unix_endpoint();
    test_endpoint_validation();
    return failures == 0 ? 0 : 1;
}
