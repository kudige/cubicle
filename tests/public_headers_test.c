#include "cubicle/cubicle.h"
#include "cubicle/transport_tcp.h"
#include "cubicle/transport_unix.h"

int main(void)
{
    cubicle_workspace_info_t workspace = {0};
    cubicle_process_info_t process = {0};
    cubicle_attachment_request_t attachment = {0};
    cubicle_event_query_t events = {0};
    cubicle_manager_status_t manager = {0};
    cubicle_manager_cleanup_result_t cleanup = {0};
    cubicle_json_builder_t builder = {0};

    (void)workspace;
    (void)process;
    (void)attachment;
    (void)events;
    (void)manager;
    (void)cleanup;
    (void)builder;
    return 0;
}
