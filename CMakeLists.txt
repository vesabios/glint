cmake_minimum_required(VERSION 3.16)
project(glint C)

set(CMAKE_C_STANDARD 11)

# SDL2
find_package(SDL2 REQUIRED)

# wasm3 sources
file(GLOB W3_SRC wasm3/*.c)

add_executable(glint glint.c ${W3_SRC})

target_include_directories(glint PRIVATE wasm3)
target_link_libraries(glint PRIVATE SDL2::SDL2 SDL2::SDL2main m)

if(MSVC)
    target_compile_options(glint PRIVATE /W3)
    # Link math lib is automatic on MSVC
    target_link_libraries(glint PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
    target_compile_options(glint PRIVATE -Wall -Wno-unused-function -O2)
    target_link_libraries(glint PRIVATE SDL2::SDL2 SDL2::SDL2main m)
endif()

# On Windows, copy SDL2.dll next to the executable
if(WIN32)
    add_custom_command(TARGET glint POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:glint>
    )
endif()
