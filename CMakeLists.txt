cmake_minimum_required(VERSION 3.20)
project(cubicle VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(cubicle_common
    src/common/log.c
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
