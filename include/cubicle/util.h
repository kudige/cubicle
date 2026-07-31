#ifndef CUBICLE_UTIL_H
#define CUBICLE_UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CUBICLE_PATH_MAX
#define CUBICLE_PATH_MAX 4096
#endif

int cubicle_generate_hex_id(char *id, size_t id_size);
int cubicle_mkdir_p(const char *path);
int cubicle_write_all(int fd, const char *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
