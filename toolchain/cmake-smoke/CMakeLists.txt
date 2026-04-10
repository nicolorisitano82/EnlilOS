cmake_minimum_required(VERSION 3.20)

project(arkshsmoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(arkshsmoke main.c)
set_target_properties(arkshsmoke PROPERTIES
    OUTPUT_NAME arkshsmoke
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
