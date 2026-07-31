#define _POSIX_C_SOURCE 200809L

#include "cubicle/util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    ++failures;
}

static void expect_true(int condition, const char *message)
{
    if (!condition) {
        fail(message);
    }
}

static int is_directory(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void test_generate_hex_id(void)
{
    char id[33];
    memset(id, 'x', sizeof(id));

    expect_true(cubicle_generate_hex_id(id, sizeof(id)) == 0,
                "expected id generation to succeed");
    expect_true(id[32] == '\0', "expected generated id to be null terminated");

    for (size_t i = 0; i < 32; ++i) {
        expect_true(isxdigit((unsigned char)id[i]) != 0,
                    "expected generated id to contain only hex digits");
        expect_true(id[i] == (char)tolower((unsigned char)id[i]),
                    "expected generated id to use lowercase hex digits");
    }

    char too_small[32];
    expect_true(cubicle_generate_hex_id(too_small, sizeof(too_small)) < 0,
                "expected id generation to reject wrong buffer size");
    expect_true(errno == EINVAL, "expected wrong id buffer size to set EINVAL");
}

static void test_mkdir_p(void)
{
    char template[] = "/tmp/cubicle-util-test-XXXXXX";
    char *root = mkdtemp(template);
    expect_true(root != NULL, "expected mkdtemp to succeed");
    if (root == NULL) {
        return;
    }

    char nested[CUBICLE_PATH_MAX];
    int length = snprintf(nested, sizeof(nested), "%s/a/b/c", root);
    expect_true(length > 0 && (size_t)length < sizeof(nested),
                "expected nested path to fit");

    expect_true(cubicle_mkdir_p(nested) == 0,
                "expected recursive directory creation to succeed");
    expect_true(is_directory(nested), "expected nested directory to exist");
    expect_true(cubicle_mkdir_p(nested) == 0,
                "expected recursive directory creation to be idempotent");

    char file_path[CUBICLE_PATH_MAX];
    length = snprintf(file_path, sizeof(file_path), "%s/file", root);
    expect_true(length > 0 && (size_t)length < sizeof(file_path),
                "expected file path to fit");

    FILE *file = fopen(file_path, "w");
    expect_true(file != NULL, "expected test file creation to succeed");
    if (file != NULL) {
        fclose(file);
    }

    expect_true(cubicle_mkdir_p(file_path) < 0,
                "expected mkdir_p to reject an existing file");

    unlink(file_path);
    rmdir(nested);
    char parent[CUBICLE_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s/a/b", root);
    rmdir(parent);
    snprintf(parent, sizeof(parent), "%s/a", root);
    rmdir(parent);
    rmdir(root);
}

static void test_write_all(void)
{
    int fds[2] = {-1, -1};
    expect_true(pipe(fds) == 0, "expected pipe creation to succeed");
    if (fds[0] < 0 || fds[1] < 0) {
        return;
    }

    const char *message = "hello through pipe";
    expect_true(cubicle_write_all(fds[1], message, strlen(message)) == 0,
                "expected write_all to write complete buffer");
    close(fds[1]);

    char buffer[64];
    ssize_t read_result = read(fds[0], buffer, sizeof(buffer));
    expect_true(read_result == (ssize_t)strlen(message),
                "expected to read the full written message");
    if (read_result > 0) {
        buffer[read_result] = '\0';
        expect_true(strcmp(buffer, message) == 0,
                    "expected read message to match written message");
    }
    close(fds[0]);
}

int main(void)
{
    test_generate_hex_id();
    test_mkdir_p();
    test_write_all();

    return failures == 0 ? 0 : 1;
}
