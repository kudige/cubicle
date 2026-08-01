#define _POSIX_C_SOURCE 200809L

#include "auth_crypto.h"

#include "cubicle/util.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int path_join(char *path, size_t path_size, const char *dir,
                     const char *name)
{
    int length = snprintf(path, path_size, "%s/%s", dir, name);
    if (length < 0 || (size_t)length >= path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int cubicle_auth_random_bytes(unsigned char *buffer, size_t length)
{
    if (buffer == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }
    if (RAND_bytes(buffer, (int)length) != 1) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int cubicle_auth_hex_encode(const unsigned char *bytes, size_t length,
                            char *hex, size_t hex_size)
{
    if (bytes == NULL || hex == NULL || hex_size < length * 2 + 1) {
        errno = EINVAL;
        return -1;
    }
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    hex[length * 2] = '\0';
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int cubicle_auth_hex_decode(const char *hex, unsigned char *bytes,
                            size_t bytes_size)
{
    if (hex == NULL || bytes == NULL || strlen(hex) != bytes_size * 2) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < bytes_size; ++i) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            errno = EINVAL;
            return -1;
        }
        bytes[i] = (unsigned char)((high << 4) | low);
    }
    return 0;
}

int cubicle_auth_key_fingerprint(const unsigned char *public_key,
                                 size_t public_key_length,
                                 char key_id[CUBICLE_ID_STRING_LENGTH],
                                 char fingerprint[CUBICLE_AUTH_FINGERPRINT_LENGTH])
{
    if (public_key == NULL || public_key_length == 0 || key_id == NULL ||
        fingerprint == NULL) {
        errno = EINVAL;
        return -1;
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(public_key, public_key_length, digest);
    char digest_hex[SHA256_DIGEST_LENGTH * 2 + 1];
    if (cubicle_auth_hex_encode(digest, sizeof(digest), digest_hex,
                                sizeof(digest_hex)) < 0) {
        return -1;
    }
    memcpy(key_id, digest_hex, CUBICLE_ID_STRING_LENGTH - 1);
    key_id[CUBICLE_ID_STRING_LENGTH - 1] = '\0';
    snprintf(fingerprint, CUBICLE_AUTH_FINGERPRINT_LENGTH, "%s", digest_hex);
    return 0;
}

static int check_private_key_permissions(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) != 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static EVP_PKEY *generate_ed25519_key(void)
{
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (context == NULL) {
        return NULL;
    }
    EVP_PKEY *key = NULL;
    if (EVP_PKEY_keygen_init(context) <= 0 ||
        EVP_PKEY_keygen(context, &key) <= 0) {
        EVP_PKEY_free(key);
        key = NULL;
    }
    EVP_PKEY_CTX_free(context);
    return key;
}

static EVP_PKEY *read_private_key(const char *path)
{
    if (check_private_key_permissions(path) < 0) {
        return NULL;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }
    EVP_PKEY *key = PEM_read_PrivateKey(file, NULL, NULL, NULL);
    fclose(file);
    if (key == NULL) {
        errno = EINVAL;
    }
    return key;
}

static int write_identity_files(const char *private_path,
                                const char *public_path,
                                EVP_PKEY *key)
{
    int fd = open(private_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }
    FILE *private_file = fdopen(fd, "w");
    if (private_file == NULL) {
        int saved_errno = errno;
        close(fd);
        unlink(private_path);
        errno = saved_errno;
        return -1;
    }
    if (PEM_write_PrivateKey(private_file, key, NULL, NULL, 0, NULL, NULL) != 1 ||
        fclose(private_file) != 0) {
        int saved_errno = errno == 0 ? EIO : errno;
        unlink(private_path);
        errno = saved_errno;
        return -1;
    }

    unsigned char public_key[CUBICLE_AUTH_PUBLIC_KEY_BYTES];
    size_t public_key_length = sizeof(public_key);
    if (EVP_PKEY_get_raw_public_key(key, public_key, &public_key_length) != 1 ||
        public_key_length != sizeof(public_key)) {
        unlink(private_path);
        errno = EIO;
        return -1;
    }

    char public_hex[CUBICLE_AUTH_HEX_PUBLIC_KEY_LENGTH];
    if (cubicle_auth_hex_encode(public_key, sizeof(public_key), public_hex,
                                sizeof(public_hex)) < 0) {
        unlink(private_path);
        return -1;
    }

    FILE *public_file = fopen(public_path, "wx");
    if (public_file == NULL) {
        unlink(private_path);
        return -1;
    }
    int failed = fprintf(public_file, "%s\n", public_hex) < 0 ||
                 fclose(public_file) != 0;
    if (failed) {
        unlink(private_path);
        unlink(public_path);
        errno = EIO;
        return -1;
    }
    return 0;
}

static int identity_from_key(EVP_PKEY *key, cubicle_auth_identity_t *identity)
{
    memset(identity, 0, sizeof(*identity));
    size_t public_key_length = sizeof(identity->public_key);
    if (EVP_PKEY_get_raw_public_key(key, identity->public_key,
                                    &public_key_length) != 1 ||
        public_key_length != sizeof(identity->public_key) ||
        cubicle_auth_hex_encode(identity->public_key,
                                sizeof(identity->public_key),
                                identity->public_key_hex,
                                sizeof(identity->public_key_hex)) < 0 ||
        cubicle_auth_key_fingerprint(identity->public_key,
                                     sizeof(identity->public_key),
                                     identity->key_id,
                                     identity->fingerprint) < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int cubicle_auth_ensure_identity(const char *key_dir,
                                 const char *private_key_name,
                                 const char *public_key_name,
                                 cubicle_auth_identity_t *identity)
{
    if (key_dir == NULL || private_key_name == NULL || public_key_name == NULL ||
        identity == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (cubicle_mkdir_p(key_dir) < 0 || chmod(key_dir, 0700) < 0) {
        return -1;
    }

    char private_path[CUBICLE_PATH_MAX];
    char public_path[CUBICLE_PATH_MAX];
    if (path_join(private_path, sizeof(private_path), key_dir,
                  private_key_name) < 0 ||
        path_join(public_path, sizeof(public_path), key_dir,
                  public_key_name) < 0) {
        return -1;
    }

    EVP_PKEY *key = read_private_key(private_path);
    if (key == NULL && errno == ENOENT) {
        key = generate_ed25519_key();
        if (key == NULL) {
            errno = EIO;
            return -1;
        }
        if (write_identity_files(private_path, public_path, key) < 0) {
            EVP_PKEY_free(key);
            return -1;
        }
    }
    if (key == NULL) {
        return -1;
    }

    int result = identity_from_key(key, identity);
    EVP_PKEY_free(key);
    return result;
}

int cubicle_auth_sign_file_key(const char *private_key_path,
                               const unsigned char *message,
                               size_t message_length,
                               unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES])
{
    if (private_key_path == NULL || message == NULL || signature == NULL) {
        errno = EINVAL;
        return -1;
    }
    EVP_PKEY *key = read_private_key(private_key_path);
    if (key == NULL) {
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    size_t signature_length = CUBICLE_AUTH_SIGNATURE_BYTES;
    int ok = context != NULL &&
             EVP_DigestSignInit(context, NULL, NULL, NULL, key) == 1 &&
             EVP_DigestSign(context, signature, &signature_length, message,
                            message_length) == 1 &&
             signature_length == CUBICLE_AUTH_SIGNATURE_BYTES;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!ok) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int cubicle_auth_verify(const unsigned char public_key[CUBICLE_AUTH_PUBLIC_KEY_BYTES],
                        const unsigned char *message,
                        size_t message_length,
                        const unsigned char signature[CUBICLE_AUTH_SIGNATURE_BYTES])
{
    if (public_key == NULL || message == NULL || signature == NULL) {
        errno = EINVAL;
        return -1;
    }
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                public_key,
                                                CUBICLE_AUTH_PUBLIC_KEY_BYTES);
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    int ok = key != NULL && context != NULL &&
             EVP_DigestVerifyInit(context, NULL, NULL, NULL, key) == 1 &&
             EVP_DigestVerify(context, signature, CUBICLE_AUTH_SIGNATURE_BYTES,
                              message, message_length) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!ok) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int cubicle_auth_hmac_sha256(const unsigned char *key, size_t key_length,
                             const unsigned char *message,
                             size_t message_length,
                             unsigned char out[CUBICLE_AUTH_SECRET_BYTES])
{
    if (key == NULL || message == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    unsigned int out_length = CUBICLE_AUTH_SECRET_BYTES;
    if (HMAC(EVP_sha256(), key, (int)key_length, message, message_length, out,
             &out_length) == NULL ||
        out_length != CUBICLE_AUTH_SECRET_BYTES) {
        errno = EIO;
        return -1;
    }
    return 0;
}
