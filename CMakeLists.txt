cmake_minimum_required(VERSION 3.20)
project(cubicle VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CUBICLE_ENABLE_COVERAGE "Build with gcov/lcov coverage instrumentation" OFF)
option(CUBICLE_ENABLE_SANITIZERS "Build with ASan/UBSan instrumentation" OFF)
option(CUBICLE_ENABLE_FUZZERS "Build libFuzzer fuzz targets" OFF)
option(CUBICLE_LINK_LIBFUZZER "Link fuzz targets with Clang libFuzzer" OFF)

if(CUBICLE_ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "CUBICLE_ENABLE_COVERAGE requires GCC or Clang")
    endif()

    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
endif()

if(CUBICLE_ENABLE_SANITIZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "CUBICLE_ENABLE_SANITIZERS requires GCC or Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
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
target_include_directories(cubicle-manager PRIVATE third_party/yyjson)

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
target_include_directories(cubicle-controller PRIVATE third_party/yyjson)

target_compile_options(cubicle-controller PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(cube
    src/cli/main.c
)

target_link_libraries(cube PRIVATE cubicle_common)
target_include_directories(cube PRIVATE third_party/yyjson)

target_compile_options(cube PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

install(TARGETS
    cube
    cubicle-manager
    cubicle-controller
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
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

if(CUBICLE_ENABLE_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "CUBICLE_ENABLE_FUZZERS requires Clang")
    endif()

    add_executable(fuzz-json-parser
        tests/fuzz_json_parser.c
    )
    target_link_libraries(fuzz-json-parser PRIVATE cubicle_common)
    target_include_directories(fuzz-json-parser PRIVATE third_party/yyjson)
    target_compile_options(fuzz-json-parser PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    if(CUBICLE_LINK_LIBFUZZER)
        target_compile_options(fuzz-json-parser PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(fuzz-json-parser PRIVATE -fsanitize=fuzzer,address,undefined)
    else()
        target_compile_definitions(fuzz-json-parser PRIVATE CUBICLE_FUZZ_STANDALONE=1)
    endif()

    add_executable(fuzz-rpc-envelope
        tests/fuzz_rpc_envelope.c
    )
    target_link_libraries(fuzz-rpc-envelope PRIVATE cubicle_common)
    target_include_directories(fuzz-rpc-envelope PRIVATE third_party/yyjson)
    target_compile_options(fuzz-rpc-envelope PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    if(CUBICLE_LINK_LIBFUZZER)
        target_compile_options(fuzz-rpc-envelope PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(fuzz-rpc-envelope PRIVATE -fsanitize=fuzzer,address,undefined)
    else()
        target_compile_definitions(fuzz-rpc-envelope PRIVATE CUBICLE_FUZZ_STANDALONE=1)
    endif()
endif()

set(CPACK_PACKAGE_NAME "cubicle")
set(CPACK_PACKAGE_VENDOR "Cubicle")
set(CPACK_PACKAGE_CONTACT "kudige")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Persistent local development runtime")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/kudige/cubicle")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "kudige")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(CUBICLE_PACMAN_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(CUBICLE_PACMAN_ARCH "aarch64")
else()
    set(CUBICLE_PACMAN_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/PKGBUILD.in
    ${CMAKE_CURRENT_BINARY_DIR}/pacman/PKGBUILD
    @ONLY
)

find_program(MAKEPKG_EXECUTABLE makepkg)
if(MAKEPKG_EXECUTABLE)
    add_custom_target(pacman-package
        COMMAND ${CMAKE_COMMAND} -E rm -f
                ${CMAKE_CURRENT_BINARY_DIR}/${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-1-${CUBICLE_PACMAN_ARCH}.pkg.tar.*
        COMMAND ${MAKEPKG_EXECUTABLE} --force --nodeps
        COMMAND ${CMAKE_COMMAND}
                -D SOURCE_DIR=${CMAKE_CURRENT_BINARY_DIR}/pacman
                -D DEST_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -D PACKAGE_NAME=${CPACK_PACKAGE_NAME}
                -D PACKAGE_VERSION=${PROJECT_VERSION}
                -D PACKAGE_ARCH=${CUBICLE_PACMAN_ARCH}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/packaging/copy-pacman-package.cmake
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/pacman
        DEPENDS cube cubicle-manager cubicle-controller
        COMMENT "Building pacman package"
        VERBATIM
    )
else()
    add_custom_target(pacman-package
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "makepkg was not found; install pacman/package tools to build pacman packages"
        VERBATIM
    )
endif()

add_test(
    NAME cubicle-cli-help-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBE=$<TARGET_FILE:cube>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_help_test.sh
)

add_test(
    NAME cube-workspace-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBE=$<TARGET_FILE:cube>
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_API_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/api_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/cube_workspace_test.sh
)

add_test(
    NAME cube-process-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBE=$<TARGET_FILE:cube>
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_API_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/api_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/cube_process_test.sh
)

add_test(
    NAME cube-tty-attach-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBE=$<TARGET_FILE:cube>
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_API_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/api_client.py
            CUBICLE_TERMINAL_RECORDER=${CMAKE_CURRENT_SOURCE_DIR}/tests/terminal_event_recorder.py
            python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cube_tty_attach_test.py
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
    NAME cubicle-manager-event-aggregation-error-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_API_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/api_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/manager_event_aggregation_error_test.sh
)

add_test(
    NAME cubicle-libcubicle-real-manager-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_MANAGER=$<TARGET_FILE:cubicle-manager>
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
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
    NAME cubicle-controller-term-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_term_test.sh
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
    NAME cubicle-controller-api-test
    COMMAND ${CMAKE_COMMAND} -E env
            CUBICLE_CONTROLLER=$<TARGET_FILE:cubicle-controller>
            CUBICLE_API_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/api_client.py
            CUBICLE_CONTROL_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/tests/control_socket_client.py
            sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_api_test.sh
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
            cube
            cubicle-process-test
            cubicle-rpc-test
            cubicle-util-test
            cubicle-rpc-test
            cubicle-manager-registry-unit-test
    )
endif()
