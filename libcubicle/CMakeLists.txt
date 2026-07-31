cmake_minimum_required(VERSION 3.20)
project(libcubicle VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(cubicle_client STATIC
    ../src/client/client.c
    ../src/client/error.c
    ../src/client/transport_unix.c
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
endif()
