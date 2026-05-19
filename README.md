cd C:\Users\dkw\.a_dkwrtc\rtsp

E:\vcpkg\vcpkg.exe install ffmpeg:x64-windows sdl2:x64-windows yaml-cpp:x64-windows spdlog:x64-windows vulkan-headers:x64-windows vulkan-loader:x64-windows shaderc:x64-windows

.\scripts\build.ps1

只重新构建：

.\scripts\build.ps1 -SkipConfigure

切换 OpenGL 渲染：

编辑 config\config.yaml：

renderer: "opengl"

切换 Vulkan 渲染：

编辑 config\config.yaml：

renderer: "vulkan"

Vulkan 后端支持 CPU NV12 帧上传和 shader 转 RGB，使用 SDL 创建 Vulkan 窗口并保持视频宽高比。启用 CUDA 硬解时，Vulkan 会通过 CUDA/Vulkan external memory buffer 在 GPU 侧拷贝 NV12 平面，再拷入现有 Y/UV 采样纹理。Vulkan 也支持截图、录制和运行时滤镜切换。

启用 NVIDIA NVDEC/CUDA 硬件解码：

hw_decode: "cuda"

如果当前 FFmpeg、驱动或显卡不支持 CUDA 硬解，会自动尝试 d3d11va/dxva2，再回退到软件解码。

启用 SCRFD ONNX CUDA 人脸检测实验后端：

```yaml
face_detection:
  enabled: true
  backend: "onnx_cuda"
```

`onnx_cuda` 会先检查 ONNX Runtime 的 `CUDAExecutionProvider`，可用时使用 GPU 推理；不可用、初始化失败或缺少 provider DLL 时会记录 warning 并自动回退到 `onnx_cpu`。Windows 下需要确保 `onnxruntime_providers_cuda.dll`、`onnxruntime_providers_shared.dll` 以及匹配的 CUDA/cuDNN 运行库能被 exe 找到。

检查 CUDA provider 是否真的可用：

```powershell
cmake --build build-vcpkg --config Release --target onnx_cuda_probe
$env:Path = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\bin\x64;C:\Program Files\NVIDIA\CUDNN\v9.22\bin\13.2\x64;$env:Path"
.\build-vcpkg\bin\Release\onnx_cuda_probe.exe models\det_500m.onnx
```

看到下面两行就表示 ONNX Runtime CUDA session 已经创建成功：

```text
available_providers=CUDAExecutionProvider,CPUExecutionProvider
cuda_session=ok
```

OpenGL 会保持视频原始宽高比，窗口比例不匹配时自动居中并显示黑边。

OpenGL 和 Vulkan 渲染路径都使用 NV12 两纹理：Y 平面 + 交错 UV 平面。NV12 更贴近硬件解码输出，避免把 UV 拆成两个纹理后再上传。

RTSP 连接参数：

```yaml
rtsp:
  transport: "tcp"        # tcp 或 udp
  timeout_ms: 5000        # 连接和读写超时
  buffer_size: 262144     # FFmpeg 输入缓冲区
  low_latency:
    enabled: true         # 启用 nobuffer/low_delay 等低延迟选项
    max_delay_ms: 50
    analyze_duration_ms: 0
    probe_size_bytes: 32768
    reorder_queue_size: 0
```

音频与音视频同步：

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
```

播放器会解码 RTSP 中的音频 track，重采样为 48kHz stereo S16 后交给 SDL 播放。开启音频时，视频以音频时钟为主时钟：视频早到会短暂等待，晚到超过 `late_drop_ms` 会丢帧追赶。

视频 jitter buffer 默认 `latency_ms: 30`，音频也保留很短的 `target_latency_ms: 30` 预缓冲；音视频同步逻辑仍然启用。如果想更稳，可以把二者一起调到 `300` 或 `1000`。

OpenGL 初始滤镜：

opengl_filters:
  - warm
  - contrast

可选值：none, grayscale, warm, invert, contrast, saturation。OpenGL 和 Vulkan 运行时按 F 都可以切换单滤镜预览。

热键：

- F：切换单滤镜预览
- S：保存当前最终画面截图到 captures 目录
- R：开始/停止录制，输出带滤镜的 MJPEG AVI 到 captures 目录
- ESC/Q：退出

断线重连：

reconnect:
  enabled: true
  initial_delay_ms: 1000
  max_delay_ms: 5000

只重新构建：

.\scripts\build.ps1 -SkipConfigure

启动时传真实 RTSP 地址：

.\build-vcpkg\bin\Release\rtsp_player.exe rtsp://你的摄像头IP:554/你的路径

双路 Vulkan 分屏显示：

```yaml
renderer: "vulkan"
rtsp_urls:
  - "rtsp://第一个摄像头IP:554/你的路径"
  - "rtsp://第二个摄像头IP:554/你的路径"
```

也可以直接传两个地址启动：

.\build-vcpkg\bin\Release\rtsp_player.exe rtsp://第一个摄像头IP:554/你的路径 rtsp://第二个摄像头IP:554/你的路径

当前多路管理只取前两个 RTSP 地址。第一路是主视频，连接失败会按 `reconnect` 配置重连；第二路是可选视频，启动时或运行中不可用都会临时隐藏，并按退避策略后台重连，恢复后自动回到左右分屏。多路模式会关闭硬件帧直通，让 FFmpeg 输出 CPU NV12 帧给 OpenGL/Vulkan 上传。

双路本机推流会把音频拆成单独一路，视频 RTSP 只包含 H264：

```yaml
rtsp_urls:
  - "rtsp://127.0.0.1:8554/webcam"
  - "rtsp://127.0.0.1:8554/webcam2"
audio_rtsp_url: "rtsp://127.0.0.1:8554/audio"
```

三路 RTSP 的音视频同步使用本机接收时间做软同步。可以在 `config/config.yaml` 中微调：

```yaml
sync:
  enabled: true
  max_wait_ms: 16
  late_drop_ms: 250
  audio_offset_ms: 0
```

`audio_offset_ms` 为正数时会让第一路视频等待更久，适合音频听起来偏晚的情况；为负数时视频会更早显示。

本机摄像头已经配置为 MediaMTX + FFmpeg 推流，MediaMTX 位于：

..\mediamtx

启动本机摄像头 RTSP 流：

.\scripts\start_webcam_rtsp.ps1

脚本默认会把本机摄像头和内置麦克风推到同一个 RTSP URL。若要关闭音频：

.\scripts\start_webcam_rtsp.ps1 -NoAudio

同时推送内置摄像头和罗技 USB 摄像头：

.\scripts\start_webcam_rtsp.ps1 -Dual

如果没有插入第二个 USB 摄像头，脚本会先按单路推流，同时启动后台 watcher 等待第二摄像头；第二摄像头插回后 watcher 会自动恢复 `webcam2` 推流。播放器在配置为双路时会把第二路当作可选流处理，第二路不可用时显示第一路，第二路恢复后自动回到分屏。

CUDA 人脸检测运行方式：

先安装带 NVDEC/NVCODEC 支持的 FFmpeg 依赖：

```powershell
E:\vcpkg\vcpkg.exe install "ffmpeg[nvcodec]:x64-windows" --recurse
.\scripts\build.ps1
```

确认 `config\config.yaml` 中启用 CUDA 硬解和 ONNX CUDA 人脸检测：

```yaml
hw_decode: "cuda"

face_detection:
  enabled: true
  backend: "onnx_cuda"
  model: "models/det_500m.onnx"
```

运行前把 CUDA Toolkit 和 cuDNN 运行库加入当前 PowerShell 的 `PATH`：

```powershell
$env:Path = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\bin\x64;C:\Program Files\NVIDIA\CUDNN\v9.22\bin\13.2\x64;$env:Path"
```

先用探针确认 ONNX Runtime CUDA session 能创建成功：

```powershell
cmake --build build-vcpkg --config Release --target onnx_cuda_probe
.\build-vcpkg\bin\Release\onnx_cuda_probe.exe models\det_500m.onnx
```

看到 `cuda_session=ok` 后，再启动播放器：

```powershell
.\build-vcpkg\bin\Release\rtsp_player.exe
```

默认会输出：

```yaml
rtsp_urls:
  - "rtsp://127.0.0.1:8554/webcam"
  - "rtsp://127.0.0.1:8554/webcam2"
audio_rtsp_url: "rtsp://127.0.0.1:8554/audio"
```

双路推流时两个视频 URL 都只推 H264，音频单独推到 `audio_rtsp_url`。若设备名不同，可以传：

.\scripts\start_webcam_rtsp.ps1 -Dual -SecondCameraName "Logi C270 HD WebCam"

然后运行播放器：

.\build-vcpkg\bin\Release\rtsp_player.exe

停止本机摄像头 RTSP 流：

.\scripts\stop_webcam_rtsp.ps1
