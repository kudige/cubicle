#include "cubicle/cubicle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>

static void make_endpoint(cubicle_endpoint_t *endpoint, const char *socket_path)
{
    memset(endpoint, 0, sizeof(*endpoint));
    int length = snprintf(endpoint->uri, sizeof(endpoint->uri), "unix://%s",
                          socket_path);
    assert(length > 0 && (size_t)length < sizeof(endpoint->uri));
}

static int snapshot_has_hello(const cubicle_terminal_snapshot_t *snapshot)
{
    size_t first_hello = (size_t)1 * snapshot->cols + 2;
    if (first_hello + 4 >= (size_t)snapshot->rows * snapshot->cols) {
        return 0;
    }
    return strcmp(snapshot->cells[first_hello].text, "h") == 0 &&
           strcmp(snapshot->cells[first_hello + 1].text, "e") == 0 &&
           strcmp(snapshot->cells[first_hello + 2].text, "l") == 0;
}

static void assert_snapshot_hello(cubicle_attachment_t *attachment)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        cubicle_terminal_snapshot_t snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        cubicle_error_code_t code =
            cubicle_attachment_snapshot(attachment, &snapshot);
        if (code == CUBICLE_OK) {
            assert(snapshot.rows > 0);
            assert(snapshot.cols > 0);
            assert(snapshot.offset > 0);
            if (snapshot_has_hello(&snapshot)) {
                cubicle_terminal_snapshot_cleanup(&snapshot);
                return;
            }
            cubicle_terminal_snapshot_cleanup(&snapshot);
        }
        struct timeval delay = {
            .tv_sec = 0,
            .tv_usec = 50000,
        };
        select(0, NULL, NULL, NULL, &delay);
    }
    assert(!"snapshot did not contain expected hello text");
}

static int run_direct_snapshot(int argc, char **argv)
{
    assert(argc == 2);

    cubicle_attachment_grant_t grant;
    memset(&grant, 0, sizeof(grant));
    snprintf(grant.grant_id, sizeof(grant.grant_id),
             "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    snprintf(grant.token, sizeof(grant.token), "local:test:process");
    make_endpoint(&grant.endpoint, argv[1]);
    grant.granted_channels = CUBICLE_CHANNEL_TTY;
    grant.mode = CUBICLE_ATTACHMENT_OBSERVER;

    cubicle_attachment_t *attachment = NULL;
    assert(cubicle_attachment_connect(&grant, NULL, &attachment) ==
           CUBICLE_OK);

    assert_snapshot_hello(attachment);
    cubicle_attachment_disconnect(attachment);
    return 0;
}

static int run_relay_snapshot(int argc, char **argv)
{
    assert(argc == 4);

    cubicle_client_t *client = NULL;
    cubicle_error_code_t code = cubicle_client_connect_uri(argv[2], NULL,
                                                           &client);
    if (code != CUBICLE_OK) {
        fprintf(stderr, "failed to connect to manager: %d\n", code);
        return 1;
    }

    cubicle_attachment_request_t request;
    memset(&request, 0, sizeof(request));
    request.process_id = argv[3];
    request.channels = CUBICLE_CHANNEL_TTY;
    request.mode = CUBICLE_ATTACHMENT_OBSERVER;
    request.rows = 24;
    request.cols = 80;

    cubicle_attachment_grant_t grant;
    memset(&grant, 0, sizeof(grant));
    code = cubicle_attachment_request(client, &request, &grant);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *error = cubicle_client_last_error(client);
        fprintf(stderr, "attachment request failed: %s\n",
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "unknown error");
        cubicle_client_disconnect(client);
        return 1;
    }

    cubicle_attachment_t *attachment = NULL;
    code = cubicle_attachment_connect_relay(client, &grant, NULL, &attachment);
    if (code != CUBICLE_OK) {
        const cubicle_error_t *error =
            attachment != NULL ? cubicle_attachment_last_error(attachment)
                               : NULL;
        fprintf(stderr, "relay attachment failed: %s\n",
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "unknown error");
        cubicle_client_disconnect(client);
        return 1;
    }

    assert_snapshot_hello(attachment);
    cubicle_attachment_disconnect(attachment);
    cubicle_client_disconnect(client);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--relay") == 0) {
        return run_relay_snapshot(argc, argv);
    }
    return run_direct_snapshot(argc, argv);
}
