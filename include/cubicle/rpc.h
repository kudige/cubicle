#ifndef CUBICLE_RPC_H
#define CUBICLE_RPC_H

#include "cubicle/client_error.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUBICLE_PROTOCOL_MAJOR 0
#define CUBICLE_PROTOCOL_MINOR 1

typedef struct cubicle_json_builder {
    char *data;
    size_t length;
    size_t capacity;
} cubicle_json_builder_t;

int cubicle_json_escape(char *buffer, size_t buffer_size, const char *value);

int cubicle_json_builder_init(cubicle_json_builder_t *builder);
void cubicle_json_builder_cleanup(cubicle_json_builder_t *builder);
int cubicle_json_builder_reserve(cubicle_json_builder_t *builder,
                                 size_t extra);
int cubicle_json_builder_append(cubicle_json_builder_t *builder,
                                const char *text);
int cubicle_json_builder_appendf(cubicle_json_builder_t *builder,
                                 const char *format, ...);
int cubicle_json_builder_append_escaped(cubicle_json_builder_t *builder,
                                        const char *value);
int cubicle_json_builder_append_string(cubicle_json_builder_t *builder,
                                       const char *value);

#ifdef __cplusplus
}
#endif

#endif
