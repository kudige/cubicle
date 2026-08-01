#include "../src/common/rpc_internal.h"

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

    cubicle_rpc_request_envelope_t request;
    if (cubicle_rpc_decode_request(&request, json) == 0) {
        cubicle_rpc_request_envelope_cleanup(&request);
    }

    cubicle_rpc_response_envelope_t response;
    if (cubicle_rpc_decode_response(&response, json, "req-1") == 0) {
        cubicle_rpc_response_envelope_cleanup(&response);
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
        "{\"protocol_major\":0,\"protocol_minor\":1,\"request_id\":\"req-1\","
        "\"method\":\"manager.ping\",\"params\":{}}",
        "{\"request_id\":\"req-1\",\"success\":true,\"result\":{}}",
        "{\"request_id\":\"req-1\",\"success\":false,\"error\":{"
        "\"code\":\"protocol\",\"message\":\"bad\",\"retryable\":false,"
        "\"system_errno\":0}}",
    };
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        (void)LLVMFuzzerTestOneInput((const uint8_t *)seeds[i],
                                     strlen(seeds[i]));
    }
    return 0;
}
#endif
