#include "cubicle/cubicle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_endpoint(cubicle_endpoint_t *endpoint, const char *socket_path)
{
    memset(endpoint, 0, sizeof(*endpoint));
    int length = snprintf(endpoint->uri, sizeof(endpoint->uri), "unix://%s",
                          socket_path);
    assert(length > 0 && (size_t)length < sizeof(endpoint->uri));
}

int main(int argc, char **argv)
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

    cubicle_terminal_snapshot_t snapshot;
    assert(cubicle_attachment_snapshot(attachment, &snapshot) == CUBICLE_OK);
    assert(snapshot.rows > 0);
    assert(snapshot.cols > 0);
    assert(snapshot.offset > 0);
    size_t first_hello = (size_t)1 * snapshot.cols + 2;
    assert(first_hello + 4 < (size_t)snapshot.rows * snapshot.cols);
    assert(strcmp(snapshot.cells[first_hello].text, "h") == 0);
    assert(strcmp(snapshot.cells[first_hello + 1].text, "e") == 0);
    assert(strcmp(snapshot.cells[first_hello + 2].text, "l") == 0);

    cubicle_terminal_snapshot_cleanup(&snapshot);
    cubicle_attachment_disconnect(attachment);
    return 0;
}
