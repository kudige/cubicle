#include "cubicle/rpc.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void expect_string(const char *actual, const char *expected)
{
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "expected [%s], got [%s]\n", expected, actual);
        assert(!"string mismatch");
    }
}

static void test_escape(void)
{
    char buffer[128];
    assert(cubicle_json_escape(buffer, sizeof(buffer),
                               "a\"b\\c\n\r\t\b\f") == 0);
    expect_string(buffer, "a\\\"b\\\\c\\n\\r\\t\\b\\f");

    char control[] = { 'x', 0x01, 'y', '\0' };
    assert(cubicle_json_escape(buffer, sizeof(buffer), control) == 0);
    expect_string(buffer, "x\\u0001y");

    errno = 0;
    assert(cubicle_json_escape(buffer, 4, "abcdef") < 0);
    assert(errno == ENOSPC);
}

static void test_builder_append(void)
{
    cubicle_json_builder_t builder = {0};
    assert(cubicle_json_builder_init(&builder) == 0);
    assert(cubicle_json_builder_append(&builder, "{\"name\":") == 0);
    assert(cubicle_json_builder_append_string(&builder, "a\"b") == 0);
    assert(cubicle_json_builder_appendf(&builder, ",\"n\":%d}", 42) == 0);
    expect_string(builder.data, "{\"name\":\"a\\\"b\",\"n\":42}");
    assert(builder.length == strlen(builder.data));
    cubicle_json_builder_cleanup(&builder);
    assert(builder.data == NULL && builder.length == 0 && builder.capacity == 0);
}

static void test_builder_escaped_without_quotes(void)
{
    cubicle_json_builder_t builder = {0};
    assert(cubicle_json_builder_append_escaped(&builder, "line\n") == 0);
    expect_string(builder.data, "line\\n");
    cubicle_json_builder_cleanup(&builder);
}

static void test_builder_null_string_compatibility(void)
{
    cubicle_json_builder_t builder = {0};
    assert(cubicle_json_builder_append_string(&builder, NULL) == 0);
    expect_string(builder.data, "\"\"");
    cubicle_json_builder_cleanup(&builder);
}

static void test_builder_reserve(void)
{
    cubicle_json_builder_t builder = {0};
    assert(cubicle_json_builder_reserve(&builder, 4096) == 0);
    assert(builder.capacity >= 4097);
    assert(cubicle_json_builder_append(&builder, "ok") == 0);
    expect_string(builder.data, "ok");
    cubicle_json_builder_cleanup(&builder);
}

int main(void)
{
    test_escape();
    test_builder_append();
    test_builder_escaped_without_quotes();
    test_builder_null_string_compatibility();
    test_builder_reserve();
    puts("rpc test passed");
    return 0;
}
