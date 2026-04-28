cmake_minimum_required(VERSION 3.16)
project(RtspPlayer VERSION 1.0.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/obj)

# Find packages
find_package(Threads REQUIRED)

# FFmpeg
find_package(FFMPEG REQUIRED)

# SDL2
find_package(SDL2 CONFIG REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

# yaml-cpp
find_package(yaml-cpp REQUIRED)

# spdlog
find_package(spdlog REQUIRED)

# Compiler options
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Header directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${FFMPEG_INCLUDE_DIRS}
)

# Source files
set(SOURCES
    src/main.cpp
    src/rtsp_client.cpp
    src/rtp_parser.cpp
    src/h264_depacketizer.cpp
    src/jitter_buffer.cpp
    src/decoder.cpp
    src/video_renderer.cpp
    src/opengl_video_renderer.cpp
)

set(HEADERS
    include/rtsp_client.hpp
    include/rtp_parser.hpp
    include/h264_depacketizer.hpp
    include/jitter_buffer.hpp
    include/decoder.hpp
    include/video_renderer.hpp
)

# Executable
add_executable(rtsp_player ${SOURCES} ${HEADERS})
target_compile_definitions(rtsp_player PRIVATE SDL_MAIN_HANDLED)
target_link_directories(rtsp_player PRIVATE ${FFMPEG_LIBRARY_DIRS})

# Link libraries
target_link_libraries(rtsp_player
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    OpenGL::GL
    yaml-cpp::yaml-cpp
    spdlog::spdlog
)

# Install target
install(TARGETS rtsp_player DESTINATION bin)
