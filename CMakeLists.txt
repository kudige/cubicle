cmake_minimum_required(VERSION 3.20)
project(cubicle VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(cubicle_common
    src/common/log.c
    src/common/process.c
)

target_include_directories(cubicle_common PUBLIC include)

target_compile_options(cubicle_common PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(cubicle-manager
    src/manager/main.c
)

target_link_libraries(cubicle-manager PRIVATE cubicle_common)

target_compile_options(cubicle-manager PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(cubicle-controller
    src/controller/main.c
)

target_link_libraries(cubicle-controller PRIVATE cubicle_common)

target_compile_options(cubicle-controller PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

enable_testing()

add_executable(cubicle-process-test
    tests/process_test.c
)

target_link_libraries(cubicle-process-test PRIVATE cubicle_common)

target_compile_options(cubicle-process-test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME cubicle-process-test COMMAND cubicle-process-test)

add_test(
    NAME cubicle-controller-stream-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_stream_test.sh
)

add_test(
    NAME cubicle-controller-control-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_control_test.sh
)

add_test(
    NAME cubicle-controller-attach-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_attach_test.sh
)

add_test(
    NAME cubicle-controller-stdin-attach-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_stdin_attach_test.sh
)

add_test(
    NAME cubicle-controller-stdin-policy-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_stdin_policy_test.sh
)

add_test(
    NAME cubicle-controller-daemon-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_daemon_test.sh
)
