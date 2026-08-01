cmake_minimum_required(VERSION 3.20)
project(cubicle VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CUBICLE_ENABLE_COVERAGE "Build with gcov/lcov coverage instrumentation" OFF)

if(CUBICLE_ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "CUBICLE_ENABLE_COVERAGE requires GCC or Clang")
    endif()

    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
endif()

add_library(cubicle_common
    src/client/error.c
    src/common/json.c
    src/common/log.c
    src/common/process.c
    src/common/rpc.c
    src/common/util.c
    third_party/yyjson/yyjson.c
)

target_include_directories(cubicle_common PUBLIC include)
target_include_directories(cubicle_common PRIVATE third_party/yyjson)

target_compile_options(cubicle_common PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(cubicle-manager
    src/manager/main.c
    src/manager/registry.c
)

target_link_libraries(cubicle-manager PRIVATE cubicle_common)

target_compile_options(cubicle-manager PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(cubicle-controller
    src/controller/control.c
    src/controller/io.c
    src/controller/main.c
    src/controller/state.c
    src/controller/stream.c
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

add_executable(cubicle-util-test
    tests/util_test.c
)

target_link_libraries(cubicle-util-test PRIVATE cubicle_common)

target_compile_options(cubicle-util-test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME cubicle-util-test COMMAND cubicle-util-test)

add_executable(cubicle-rpc-test
    tests/rpc_test.c
)

target_link_libraries(cubicle-rpc-test PRIVATE cubicle_common)
target_include_directories(cubicle-rpc-test PRIVATE third_party/yyjson)

target_compile_options(cubicle-rpc-test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME cubicle-rpc-test COMMAND cubicle-rpc-test)

add_executable(cubicle-manager-registry-unit-test
    tests/manager_registry_unit_test.c
    src/manager/registry.c
)

target_link_libraries(cubicle-manager-registry-unit-test PRIVATE cubicle_common)

target_compile_options(cubicle-manager-registry-unit-test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME cubicle-manager-registry-unit-test COMMAND cubicle-manager-registry-unit-test)

add_test(
    NAME cubicle-cli-help-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_help_test.sh
)

add_test(
    NAME cubicle-manager-registry-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/manager_registry_test.sh
)

add_test(
    NAME cubicle-manager-start-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/manager_start_test.sh
)

add_test(
    NAME cubicle-manager-events-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/manager_events_test.sh
)

add_test(
    NAME cubicle-manager-daemon-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_API_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/api_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/manager_daemon_test.sh
)

add_test(
    NAME cubicle-libcubicle-real-manager-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/libcubicle_real_manager_test.sh
)

add_test(
    NAME cubicle-controller-stream-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_stream_test.sh
)

add_test(
    NAME cubicle-controller-tty-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_tty_test.sh
)

add_test(
    NAME cubicle-controller-tty-local-input-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_tty_local_input_test.py
)

add_test(
    NAME cubicle-controller-tty-socket-input-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_tty_socket_input_test.sh
)

add_test(
    NAME cubicle-control-socket-client-tty-attach-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client_tty_attach_test.py
)

add_test(
    NAME cubicle-terminal-resize-e2e-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            CUBICLE_TERMINAL_RECORDER=${CMAKE_CURRENT_SOURCE_DIR}/tests/terminal_event_recorder.py
            python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/terminal_resize_e2e_test.py
)

add_test(
    NAME cubicle-controller-default-state-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_default_state_test.sh
)

add_test(
    NAME cubicle-controller-state-init-failure-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_state_init_failure_test.sh
)

add_test(
    NAME cubicle-controller-state-reuse-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_state_reuse_test.sh
)

add_test(
    NAME cubicle-controller-control-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_control_test.sh
)

add_test(
    NAME cubicle-controller-control-socket-path-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_control_socket_path_test.sh
)

add_test(
    NAME cubicle-controller-completed-retention-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_completed_retention_test.sh
)

add_test(
    NAME cubicle-controller-queued-response-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_queued_response_test.sh
)

add_test(
    NAME cubicle-controller-recovery-read-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_recovery_read_test.sh
)

add_test(
    NAME cubicle-controller-output-eof-lifecycle-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_output_eof_lifecycle_test.sh
)

add_test(
    NAME cubicle-controller-attach-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
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

if(CUBICLE_ENABLE_COVERAGE)
    find_program(LCOV_EXECUTABLE lcov REQUIRED)

    add_custom_target(coverage-summary
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${LCOV_EXECUTABLE}
                --capture
                --directory ${CMAKE_BINARY_DIR}
                --output-file ${CMAKE_BINARY_DIR}/coverage.info
                --ignore-errors mismatch
        COMMAND ${LCOV_EXECUTABLE}
                --remove ${CMAKE_BINARY_DIR}/coverage.info
                '/usr/*'
                '${CMAKE_SOURCE_DIR}/tests/*'
                '${CMAKE_SOURCE_DIR}/third_party/*'
                '${CMAKE_BINARY_DIR}/*'
                --output-file ${CMAKE_BINARY_DIR}/coverage.filtered.info
                --ignore-errors unused
        COMMAND ${LCOV_EXECUTABLE}
                --summary ${CMAKE_BINARY_DIR}/coverage.filtered.info
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL
        COMMENT "Running tests and generating lcov coverage summary"
        DEPENDS
            cubicle-manager
            cubicle-controller
            cubicle-process-test
            cubicle-rpc-test
            cubicle-util-test
            cubicle-rpc-test
            cubicle-manager-registry-unit-test
    )
endif()
