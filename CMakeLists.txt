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

# CUDA/OpenGL interop is optional. When the CUDA Toolkit is present, the
# OpenGL renderer can upload CUDA decoded frames into GL textures without a
# CPU round-trip.
if(NOT DEFINED CUDAToolkit_ROOT)
    file(GLOB CUDA_TOOLKIT_CANDIDATES
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*")
    if(CUDA_TOOLKIT_CANDIDATES)
        list(SORT CUDA_TOOLKIT_CANDIDATES ORDER DESCENDING)
        list(GET CUDA_TOOLKIT_CANDIDATES 0 CUDAToolkit_ROOT)
    endif()
endif()
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    message(STATUS "CUDA Toolkit found: ${CUDAToolkit_VERSION}")
else()
    message(STATUS "CUDA Toolkit not found; CUDA/OpenGL interop disabled")
endif()

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
    src/audio_player.cpp
    src/frame_capture.cpp
    src/decoder.cpp
    src/video_renderer.cpp
    src/opengl_video_renderer.cpp
)

set(HEADERS
    include/rtsp_client.hpp
    include/rtp_parser.hpp
    include/h264_depacketizer.hpp
    include/jitter_buffer.hpp
    include/audio_player.hpp
    include/frame_capture.hpp
    include/decoder.hpp
    include/video_renderer.hpp
)

# Executable
add_executable(rtsp_player ${SOURCES} ${HEADERS})
target_compile_definitions(rtsp_player PRIVATE SDL_MAIN_HANDLED)
target_link_directories(rtsp_player PRIVATE ${FFMPEG_LIBRARY_DIRS})

if(CUDAToolkit_FOUND)
    target_compile_definitions(rtsp_player PRIVATE RTSP_ENABLE_CUDA_INTEROP=1)
endif()

# Link libraries
target_link_libraries(rtsp_player
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    OpenGL::GL
    yaml-cpp::yaml-cpp
    spdlog::spdlog
)

if(CUDAToolkit_FOUND)
    target_link_libraries(rtsp_player CUDA::cudart)
endif()

# Install target
install(TARGETS rtsp_player DESTINATION bin)
