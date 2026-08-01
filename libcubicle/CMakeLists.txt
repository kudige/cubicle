cmake_minimum_required(VERSION 3.20)
project(libcubicle VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(cubicle_client STATIC
    ../src/client/client.c
    ../src/client/error.c
    ../src/client/transport_tcp.c
    ../src/client/transport_unix.c
    ../src/common/process.c
    ../src/common/rpc.c
)

add_library(Cubicle::Client ALIAS cubicle_client)

target_include_directories(cubicle_client PUBLIC ../include)
target_compile_options(cubicle_client PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(libcubicle-scaffold-test
        ../tests/client_scaffold_test.c
    )
    target_link_libraries(libcubicle-scaffold-test PRIVATE Cubicle::Client)
    target_compile_options(libcubicle-scaffold-test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME libcubicle-scaffold-test COMMAND libcubicle-scaffold-test)

    add_executable(libcubicle-unix-transport-test
        ../tests/unix_transport_test.c
    )
    target_link_libraries(libcubicle-unix-transport-test PRIVATE Cubicle::Client)
    target_compile_options(libcubicle-unix-transport-test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME libcubicle-unix-transport-test
             COMMAND libcubicle-unix-transport-test)

    add_executable(libcubicle-public-headers-test
        ../tests/public_headers_test.c
    )
    target_link_libraries(libcubicle-public-headers-test PRIVATE Cubicle::Client)
    target_compile_options(libcubicle-public-headers-test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME libcubicle-public-headers-test
             COMMAND libcubicle-public-headers-test)

    add_executable(libcubicle-rpc-test
        ../tests/rpc_test.c
    )
    target_link_libraries(libcubicle-rpc-test PRIVATE Cubicle::Client)
    target_compile_options(libcubicle-rpc-test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME libcubicle-rpc-test COMMAND libcubicle-rpc-test)

    add_executable(libcubicle-client-rpc-test
        ../tests/libcubicle_rpc_test.c
    )
    target_link_libraries(libcubicle-client-rpc-test PRIVATE Cubicle::Client)
    target_compile_options(libcubicle-client-rpc-test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME libcubicle-client-rpc-test COMMAND libcubicle-client-rpc-test)

    add_executable(cubicle-mock-api-server
        ../tests/mock_api_server.c
    )
    target_compile_options(cubicle-mock-api-server PRIVATE
        -Wall -Wextra -Wpedantic
    )

    add_executable(libcubicle-mock-integration-test
        ../tests/libcubicle_mock_integration_test.c
    )
    target_link_libraries(libcubicle-mock-integration-test PRIVATE Cubicle::Client)
    target_compile_options(libcubicle-mock-integration-test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(
        NAME libcubicle-mock-integration-test
        COMMAND ${CMAKE_COMMAND} -E env
                CUBICLE_MOCK_API_SERVER=$<TARGET_FILE:cubicle-mock-api-server>
                $<TARGET_FILE:libcubicle-mock-integration-test> all
    )
    add_test(
        NAME libcubicle-mock-integration-unix-test
        COMMAND ${CMAKE_COMMAND} -E env
                CUBICLE_MOCK_API_SERVER=$<TARGET_FILE:cubicle-mock-api-server>
                $<TARGET_FILE:libcubicle-mock-integration-test> unix
    )
    add_test(
        NAME libcubicle-mock-integration-tcp-test
        COMMAND ${CMAKE_COMMAND} -E env
                CUBICLE_MOCK_API_SERVER=$<TARGET_FILE:cubicle-mock-api-server>
                $<TARGET_FILE:libcubicle-mock-integration-test> tcp
    )
endif()
