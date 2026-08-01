#define _POSIX_C_SOURCE 200809L

#include "../src/common/auth_crypto.h"

#include "cubicle/util.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_temp_dir(char *path, size_t path_size)
{
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }
    int length = snprintf(path, path_size, "%s/cubicle-auth-test-XXXXXX",
                          tmpdir);
    assert(length >= 0 && (size_t)length < path_size);
    assert(mkdtemp(path) != NULL);
}

static void private_key_path(char *path, size_t path_size, const char *dir)
{
    int length = snprintf(path, path_size, "%s/keys/client.key", dir);
    assert(length >= 0 && (size_t)length < path_size);
}

static void test_identity_round_trip(void)
{
    char dir[CUBICLE_PATH_MAX];
    make_temp_dir(dir, sizeof(dir));
    char key_dir[CUBICLE_PATH_MAX];
    int length = snprintf(key_dir, sizeof(key_dir), "%s/keys", dir);
    assert(length >= 0 && (size_t)length < sizeof(key_dir));

    cubicle_auth_identity_t first;
    assert(cubicle_auth_ensure_identity(key_dir, "client.key", "client.pub",
                                        &first) == 0);
    assert(strlen(first.public_key_hex) == 64);
    assert(strlen(first.key_id) == 32);
    assert(strlen(first.fingerprint) == 64);

    struct stat st;
    char private_path_buffer[CUBICLE_PATH_MAX];
    private_key_path(private_path_buffer, sizeof(private_path_buffer), dir);
    assert(stat(private_path_buffer, &st) == 0);
    assert((st.st_mode & 0777) == 0600);
    assert(stat(key_dir, &st) == 0);
    assert((st.st_mode & 0777) == 0700);

    cubicle_auth_identity_t second;
    assert(cubicle_auth_ensure_identity(key_dir, "client.key", "client.pub",
                                        &second) == 0);
    assert(strcmp(first.public_key_hex, second.public_key_hex) == 0);
    assert(strcmp(first.key_id, second.key_id) == 0);
    assert(strcmp(first.fingerprint, second.fingerprint) == 0);

    const unsigned char message[] = "auth transcript";
    unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES];
    assert(cubicle_auth_sign_file_key(private_path_buffer, message,
                                      sizeof(message) - 1, signature) == 0);
    assert(cubicle_auth_verify(first.public_key, message, sizeof(message) - 1,
                               signature) == 0);
    signature[0] ^= 0x01;
    assert(cubicle_auth_verify(first.public_key, message, sizeof(message) - 1,
                               signature) < 0);
}

static void test_hex_and_hmac(void)
{
    unsigned char random_bytes[16];
    assert(cubicle_auth_random_bytes(random_bytes, sizeof(random_bytes)) == 0);

    const unsigned char bytes[] = {0x00, 0xab, 0xff};
    char hex[7];
    assert(cubicle_auth_hex_encode(bytes, sizeof(bytes), hex,
                                   sizeof(hex)) == 0);
    assert(strcmp(hex, "00abff") == 0);
    unsigned char decoded[3];
    assert(cubicle_auth_hex_decode(hex, decoded, sizeof(decoded)) == 0);
    assert(memcmp(bytes, decoded, sizeof(bytes)) == 0);
    assert(cubicle_auth_hex_decode("00abfg", decoded, sizeof(decoded)) < 0);

    unsigned char mac1[CUBICLE_AUTH_SECRET_BYTES];
    unsigned char mac2[CUBICLE_AUTH_SECRET_BYTES];
    const unsigned char key[] = "resume-key";
    const unsigned char message[] = "resume-message";
    assert(cubicle_auth_hmac_sha256(key, sizeof(key) - 1, message,
                                    sizeof(message) - 1, mac1) == 0);
    assert(cubicle_auth_hmac_sha256(key, sizeof(key) - 1, message,
                                    sizeof(message) - 1, mac2) == 0);
    assert(memcmp(mac1, mac2, sizeof(mac1)) == 0);
}

static void test_rejects_unsafe_private_key(void)
{
    char dir[CUBICLE_PATH_MAX];
    make_temp_dir(dir, sizeof(dir));
    char key_dir[CUBICLE_PATH_MAX];
    int length = snprintf(key_dir, sizeof(key_dir), "%s/keys", dir);
    assert(length >= 0 && (size_t)length < sizeof(key_dir));

    cubicle_auth_identity_t identity;
    assert(cubicle_auth_ensure_identity(key_dir, "client.key", "client.pub",
                                        &identity) == 0);
    char private_path_buffer[CUBICLE_PATH_MAX];
    private_key_path(private_path_buffer, sizeof(private_path_buffer), dir);
    assert(chmod(private_path_buffer, 0644) == 0);
    errno = 0;
    assert(cubicle_auth_ensure_identity(key_dir, "client.key", "client.pub",
                                        &identity) < 0);
    assert(errno == EACCES);
}

int main(void)
{
    test_identity_round_trip();
    test_hex_and_hmac();
    test_rejects_unsafe_private_key();
    return 0;
}
