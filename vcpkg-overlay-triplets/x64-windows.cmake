set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_C_FLAGS "/Zc:preprocessor /wd4996")
set(VCPKG_CXX_FLAGS "/Zc:preprocessor /wd4996")

# CUDA 13.x no longer accepts older architectures such as compute_60. The
# project target machine uses an RTX 4060 Laptop GPU, which is Ada / sm_89.
# The face detector model only needs standard ONNX CUDA kernels, so keep this
# experiment away from ORT's transformer/FlashAttention contrib kernels that
# currently trip CUDA 13.2 deprecation errors in vcpkg's ORT 1.23.2 port.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_CUDA_ARCHITECTURES=89"
    "-DCMAKE_CUDA_FLAGS=-Xcudafe --diag_suppress=2803 -Wno-deprecated-gpu-targets -Xcompiler=/Zc:preprocessor -Xcompiler=/wd4996"
    "-Donnxruntime_DISABLE_CONTRIB_OPS=ON"
    "-Donnxruntime_USE_FLASH_ATTENTION=OFF"
    "-Donnxruntime_USE_MEMORY_EFFICIENT_ATTENTION=OFF"
)
