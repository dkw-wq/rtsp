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

# Vulkan
find_package(Vulkan REQUIRED)
find_program(GLSLC_EXECUTABLE
    NAMES glslc
    HINTS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shaderc"
        "D:/vcpkg/installed/x64-windows/tools/shaderc"
)
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found. Install shaderc with vcpkg: vcpkg install shaderc:x64-windows")
endif()

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

# Face detection
find_package(onnxruntime CONFIG REQUIRED)

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
    src/app_logging.cpp
    src/player_runner.cpp
    src/rtsp_client.cpp
    src/rtsp_audio_decoder.cpp
    src/rtsp_ffmpeg_utils.cpp
    src/rtsp_frame_converter.cpp
    src/rtsp_hardware_decoder.cpp
    src/rtsp_recorder.cpp
    src/jitter_buffer.cpp
    src/audio_player.cpp
    src/frame_capture.cpp
    src/config_loader.cpp
    src/face_analyzer.cpp
    src/face_detector.cpp
    src/onnx_scrfd_detector.cpp
    src/sync_controller.cpp
    src/stream_session.cpp
    src/rendering/common/overlay_font.cpp
    src/rendering/common/renderer_input.cpp
    src/rendering/sdl_video_renderer.cpp
    src/rendering/opengl/opengl_video_renderer.cpp
    src/rendering/opengl/opengl_capture.cpp
    src/rendering/opengl/opengl_filters.cpp
    src/rendering/opengl/opengl_gl_api.cpp
    src/rendering/opengl/opengl_layout.cpp
    src/rendering/opengl/opengl_overlay.cpp
    src/rendering/opengl/opengl_programs.cpp
    src/rendering/opengl/opengl_upload.cpp
    src/rendering/vulkan/vulkan_video_renderer.cpp
    src/rendering/vulkan/vulkan_capture.cpp
    src/rendering/vulkan/vulkan_helpers.cpp
    src/rendering/vulkan/vulkan_overlay.cpp
    src/rendering/vulkan/vulkan_render_commands.cpp
    src/rendering/vulkan/vulkan_resources.cpp
    src/rendering/vulkan/vulkan_setup.cpp
)

set(HEADERS
    include/app_logging.hpp
    include/player_runner.hpp
    include/rtsp_client.hpp
    include/jitter_buffer.hpp
    include/audio_player.hpp
    include/frame_capture.hpp
    include/config_loader.hpp
    include/face_analyzer.hpp
    include/face_detector.hpp
    include/face_types.hpp
    include/onnx_scrfd_detector.hpp
    include/sync_controller.hpp
    include/stream_session.hpp
    include/video_renderer.hpp
)

# Executable
add_executable(rtsp_player ${SOURCES} ${HEADERS})
target_compile_definitions(rtsp_player PRIVATE SDL_MAIN_HANDLED)
target_link_directories(rtsp_player PRIVATE ${FFMPEG_LIBRARY_DIRS})

set(VULKAN_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders/vulkan)
set(VULKAN_SHADER_SOURCES
    ${CMAKE_SOURCE_DIR}/shaders/vulkan/video.vert
    ${CMAKE_SOURCE_DIR}/shaders/vulkan/nv12.frag
    ${CMAKE_SOURCE_DIR}/shaders/vulkan/overlay.vert
    ${CMAKE_SOURCE_DIR}/shaders/vulkan/overlay.frag
)
set(VULKAN_SHADER_OUTPUTS)
foreach(SHADER_SOURCE ${VULKAN_SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
    set(SHADER_OUTPUT ${VULKAN_SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_SHADER_OUTPUT_DIR}
        COMMAND ${GLSLC_EXECUTABLE} ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER_SOURCE}
        VERBATIM
    )
    list(APPEND VULKAN_SHADER_OUTPUTS ${SHADER_OUTPUT})
endforeach()
add_custom_target(vulkan_shaders DEPENDS ${VULKAN_SHADER_OUTPUTS})
add_dependencies(rtsp_player vulkan_shaders)
add_custom_command(TARGET rtsp_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:rtsp_player>/shaders/vulkan"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VULKAN_SHADER_OUTPUTS} "$<TARGET_FILE_DIR:rtsp_player>/shaders/vulkan"
    VERBATIM
)

if(CUDAToolkit_FOUND)
    target_compile_definitions(rtsp_player PRIVATE RTSP_ENABLE_CUDA_INTEROP=1)
endif()

# Link libraries
target_link_libraries(rtsp_player
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    OpenGL::GL
    Vulkan::Vulkan
    yaml-cpp::yaml-cpp
    spdlog::spdlog
    onnxruntime::onnxruntime
)

if(CUDAToolkit_FOUND)
    target_link_libraries(rtsp_player CUDA::cudart)
endif()

# ONNX Runtime CUDA/TensorRT providers are loaded dynamically at runtime. They
# are not always copied as target dependencies, so keep any provider DLLs next
# to executables that need to probe or use ORT execution providers.
function(copy_onnxruntime_provider_dlls target_name)
    get_target_property(ONNXRUNTIME_RUNTIME_DLL onnxruntime::onnxruntime IMPORTED_LOCATION_RELEASE)
    if(NOT ONNXRUNTIME_RUNTIME_DLL)
        get_target_property(ONNXRUNTIME_RUNTIME_DLL onnxruntime::onnxruntime IMPORTED_LOCATION)
    endif()
    if(ONNXRUNTIME_RUNTIME_DLL)
        get_filename_component(ONNXRUNTIME_RUNTIME_DIR "${ONNXRUNTIME_RUNTIME_DLL}" DIRECTORY)
        file(GLOB ONNXRUNTIME_PROVIDER_DLLS CONFIGURE_DEPENDS
            "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_*.dll")
        if(ONNXRUNTIME_PROVIDER_DLLS)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${ONNXRUNTIME_PROVIDER_DLLS}
                    "$<TARGET_FILE_DIR:${target_name}>"
                VERBATIM
            )
        endif()
    endif()
endfunction()
copy_onnxruntime_provider_dlls(rtsp_player)

add_executable(onnx_cuda_probe EXCLUDE_FROM_ALL tools/onnx_cuda_probe.cpp)
target_link_libraries(onnx_cuda_probe PRIVATE onnxruntime::onnxruntime)
copy_onnxruntime_provider_dlls(onnx_cuda_probe)

enable_testing()

add_executable(rtsp_unit_tests
    tests/unit_tests.cpp
    src/jitter_buffer.cpp
    src/config_loader.cpp
    src/sync_controller.cpp
    src/audio_player.cpp
)
target_compile_definitions(rtsp_unit_tests PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(rtsp_unit_tests
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    yaml-cpp::yaml-cpp
    spdlog::spdlog
)
add_test(NAME rtsp_unit_tests COMMAND rtsp_unit_tests)

# Install target
install(TARGETS rtsp_player DESTINATION bin)
