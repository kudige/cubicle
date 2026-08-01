#include "../src/common/json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > CUBICLE_JSON_MAX_DOCUMENT_BYTES) {
        return 0;
    }

    char *json = malloc(size + 1);
    if (json == NULL) {
        return 0;
    }
    memcpy(json, data, size);
    json[size] = '\0';

    cubicle_json_doc_t parsed;
    if (cubicle_json_parse(&parsed, json) == 0) {
        cubicle_json_cleanup(&parsed);
    }
    free(json);
    return 0;
}

#ifdef CUBICLE_FUZZ_STANDALONE
int main(void)
{
    static const char *const seeds[] = {
        "",
        "{}",
        "{\"request_id\":\"req-1\"}",
        "{\"a\":{\"a\":1}}",
        "{\"a\":\"\\ud800\"}",
    };
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        (void)LLVMFuzzerTestOneInput((const uint8_t *)seeds[i],
                                     strlen(seeds[i]));
    }
    return 0;
}
#endif
