# RTSP Player

Windows RTSP 播放器，基于 FFmpeg 解码，支持 SDL/OpenGL/Vulkan 渲染、音频播放、音视频同步、断线重连、截图录制、双路分屏和 SCRFD ONNX 人脸检测 overlay。

## 功能特性

- RTSP over TCP/UDP，支持低延迟连接参数。
- SDL、OpenGL、Vulkan 三种渲染后端。
- NV12 渲染路径，OpenGL/Vulkan 保持视频宽高比。
- 可选 NVIDIA NVDEC/CUDA 硬解，失败时回退到 d3d11va/dxva2 或软件解码。
- 音频解码、48 kHz stereo S16 重采样和 SDL 播放。
- 以音频时钟为主的音视频同步、jitter buffer 和晚帧丢弃。
- 双路 RTSP 左右分屏，第二路断开后自动隐藏并后台重连。
- 截图、录制、运行时滤镜切换。
- SCRFD ONNX 人脸检测，支持 ONNX Runtime CPU/CUDA provider。
- 本机摄像头 MediaMTX + FFmpeg 推流辅助脚本。

## 环境要求

- Windows 10/11
- Visual Studio 2022 C++ toolchain
- CMake 3.21 或更新版本
- vcpkg
- Vulkan SDK 或可用的 Vulkan loader/runtime
- 可选：NVIDIA GPU、CUDA Toolkit、cuDNN、带 `nvcodec` feature 的 FFmpeg

项目依赖由 [vcpkg.json](vcpkg.json) 管理。确保 `vcpkg` 已在 `PATH` 中，或设置了 `VCPKG_ROOT`：

```powershell
$env:VCPKG_ROOT = "E:\vcpkg"
```

## 构建

推荐使用项目脚本：

```powershell
.\scripts\build.ps1
```

只重新构建，不重新配置：

```powershell
.\scripts\build.ps1 -SkipConfigure
```

也可以直接使用 CMake presets：

```powershell
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
ctest --preset windows-vcpkg-release
```

Debug 构建：

```powershell
cmake --build --preset windows-vcpkg-debug
```

如果 `build-vcpkg` 是在加入 `vcpkg.json` 之前生成的，vcpkg 不能把该目录原地切换到 manifest mode。删除 `build-vcpkg` 后重新运行 `.\scripts\build.ps1`，即可使用 `vcpkg.json` 自动恢复依赖。

## 启用 CUDA/NVDEC

Windows/vcpkg 构建需要 FFmpeg 的 `nvcodec` feature，否则 `h264_cuvid` 等 CUDA decoder 不会出现在播放器实际加载的 `avcodec-*.dll` 中。

```powershell
.\scripts\build.ps1 -EnableCudaFfmpeg
```

然后在 [config/config.yaml](config/config.yaml) 中启用：

```yaml
hw_decode: "cuda"
```

播放器会优先尝试 CUDA，失败时自动尝试 d3d11va/dxva2，再回退到软件解码。启动后可在日志中确认：

```text
Selected hardware decoder: h264_cuvid
Hardware decode active: cuda
```

## 运行

使用配置文件中的默认 RTSP 地址启动：

```powershell
.\build-vcpkg\bin\Release\rtsp_player.exe
```

启动时传入单路 RTSP 地址：

```powershell
.\build-vcpkg\bin\Release\rtsp_player.exe rtsp://你的摄像头IP:554/你的路径
```

启动时传入双路 RTSP 地址：

```powershell
.\build-vcpkg\bin\Release\rtsp_player.exe rtsp://第一个摄像头IP:554/你的路径 rtsp://第二个摄像头IP:554/你的路径
```

当前多路管理只使用前两个 RTSP 地址。第一路是主视频；第二路是可选视频，启动或运行中不可用时会临时隐藏，并按退避策略后台重连，恢复后自动回到分屏。

## 配置

主要配置位于 [config/config.yaml](config/config.yaml)。

### 渲染后端

```yaml
renderer: "vulkan" # sdl, opengl, vulkan
```

Vulkan 后端支持 CPU NV12 帧上传和 shader 转 RGB。启用 CUDA 硬解时，Vulkan 会尝试通过 CUDA/Vulkan external memory buffer 在 GPU 侧拷贝 NV12 平面，再拷入现有 Y/UV 采样纹理。OpenGL/Vulkan 双路显示均支持 `CUDA_NV12` 帧。

### RTSP 连接

```yaml
rtsp:
  transport: "tcp"        # tcp 或 udp
  timeout_ms: 5000        # 连接和读写超时
  buffer_size: 262144     # FFmpeg 输入缓冲区
  low_latency:
    enabled: true
    max_delay_ms: 50
    analyze_duration_ms: 0
    probe_size_bytes: 32768
    reorder_queue_size: 0
```

### 音频与同步

```yaml
audio:
  enabled: true
  target_latency_ms: 30
  max_queue_ms: 300
  hard_reset_queue_ms: 1500

sync:
  enabled: true
  max_wait_ms: 16
  late_drop_ms: 250
  audio_offset_ms: 0
```

开启音频时，视频以音频时钟为主时钟：视频早到会短暂等待，晚到超过 `late_drop_ms` 会丢帧追赶。`audio_offset_ms` 为正数时让视频相对音频晚显示，为负数时让视频更早显示。

### Jitter Buffer

```yaml
jitter_buffer:
  max_size: 40
  latency_ms: 50
```

低延迟场景可降低 `latency_ms`；弱网或抖动明显时建议把视频 jitter buffer 和 `audio.target_latency_ms` 一起调高，例如 `300` 或 `1000`。

### 双路分屏

```yaml
renderer: "vulkan"
rtsp_urls:
  - "rtsp://第一个摄像头IP:554/你的路径"
  - "rtsp://第二个摄像头IP:554/你的路径"
```

双路本机推流时，音频可以单独走一路 RTSP，避免拖慢第一路视频：

```yaml
rtsp_urls:
  - "rtsp://127.0.0.1:8554/webcam"
  - "rtsp://127.0.0.1:8554/webcam2"
audio_rtsp_url: "rtsp://127.0.0.1:8554/audio"
```

### OpenGL 滤镜

```yaml
opengl_filters:
  - warm
  - contrast
```

可选值：`none`、`grayscale`、`warm`、`invert`、`contrast`、`saturation`。OpenGL 和 Vulkan 运行时都可以按 `F` 切换单滤镜预览。

### 断线重连

```yaml
reconnect:
  enabled: true
  initial_delay_ms: 1000
  max_delay_ms: 5000
```

## 人脸检测

启用 SCRFD ONNX 后端：

```yaml
face_detection:
  enabled: true
  backend: "onnx_cuda" # onnx_cpu, onnx_cuda, tensorrt
  model: "models/det_500m.onnx"
  input_width: 640
  input_height: 640
  detect_every_n_frames: 5
  score_threshold: 0.5
  nms_threshold: 0.4
```

`onnx_cuda` 会先检查 ONNX Runtime 的 `CUDAExecutionProvider`。可用时使用 GPU 推理；不可用、初始化失败或缺少 provider DLL 时记录 warning 并自动回退到 `onnx_cpu`。

检查 ONNX Runtime CUDA provider：

```powershell
cmake --build build-vcpkg --config Release --target onnx_cuda_probe
$env:Path = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\bin\x64;C:\Program Files\NVIDIA\CUDNN\v9.22\bin\13.2\x64;$env:Path"
.\build-vcpkg\bin\Release\onnx_cuda_probe.exe models\det_500m.onnx
```

成功时会看到：

```text
available_providers=CUDAExecutionProvider,CPUExecutionProvider
cuda_session=ok
```

Windows 下需要确保 `onnxruntime_providers_cuda.dll`、`onnxruntime_providers_shared.dll` 以及匹配的 CUDA/cuDNN 运行库能被 exe 找到。

## 本机摄像头 RTSP 推流

辅助脚本假设 MediaMTX 位于仓库同级目录的 `..\mediamtx`。

启动本机摄像头 RTSP 流：

```powershell
.\scripts\start_webcam_rtsp.ps1
```

关闭音频：

```powershell
.\scripts\start_webcam_rtsp.ps1 -NoAudio
```

同时推送内置摄像头和第二个 USB 摄像头：

```powershell
.\scripts\start_webcam_rtsp.ps1 -Dual
```

指定第二个摄像头设备名：

```powershell
.\scripts\start_webcam_rtsp.ps1 -Dual -SecondCameraName "Logi C270 HD WebCam"
```

如果启动双路但第二个摄像头未插入，脚本会先按单路推流，并启动后台 watcher 等待第二摄像头；第二摄像头恢复后会自动推送到 `webcam2`。

停止本机摄像头 RTSP 流：

```powershell
.\scripts\stop_webcam_rtsp.ps1
```

## 快捷键

| 按键 | 功能 |
| --- | --- |
| `F` | 切换单滤镜预览 |
| `S` | 保存当前最终画面截图到 `captures/` |
| `R` | 开始/停止录制，输出到 `captures/` |
| `ESC` / `Q` | 退出 |

## 输出目录

- `captures/`：截图和录制文件
- `logs/`：运行日志
- `build-vcpkg/`：CMake 构建目录

这些目录默认被 `.gitignore` 忽略。

## 开发与测试

运行单元测试：

```powershell
ctest --preset windows-vcpkg-release
```

构建 ONNX CUDA provider 探针：

```powershell
cmake --build build-vcpkg --config Release --target onnx_cuda_probe
```

主要代码结构：

- `src/rtsp_client.cpp`：RTSP 输入、FFmpeg 解复用、音视频解码、硬解回退和录制。
- `src/player_runner.cpp`：单路/双路播放主循环、同步、重连和渲染调度。
- `src/rendering/`：SDL、OpenGL、Vulkan 渲染后端。
- `src/onnx_scrfd_detector.cpp`：SCRFD ONNX 推理与后处理。
- `tests/unit_tests.cpp`：配置、jitter buffer 和同步逻辑测试。


作者一般运行流程：

```powershell
cmake --preset windows-vcpkg
.\scripts\start_webcam_rtsp.ps1 -Dual
.\build-vcpkg\bin\Release\rtsp_player.exe
.\scripts\stop_webcam_rtsp.ps1
```