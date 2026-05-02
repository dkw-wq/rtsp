cd C:\Users\dkw\.a_dkwrtc\rtsp

D:\vcpkg\vcpkg.exe install ffmpeg:x64-windows sdl2:x64-windows yaml-cpp:x64-windows spdlog:x64-windows

.\scripts\build.ps1

只重新构建：

.\scripts\build.ps1 -SkipConfigure

切换 OpenGL 渲染：

编辑 config\config.yaml：

renderer: "opengl"

启用 NVIDIA NVDEC/CUDA 硬件解码：

hw_decode: "cuda"

如果当前 FFmpeg、驱动或显卡不支持 CUDA 硬解，会自动尝试 d3d11va/dxva2，再回退到软件解码。

OpenGL 会保持视频原始宽高比，窗口比例不匹配时自动居中并显示黑边。

OpenGL 渲染路径使用 NV12 两纹理：Y 平面 + 交错 UV 平面。NV12 更贴近硬件解码输出，避免把 UV 拆成两个纹理后再上传。

OpenGL 滤镜：

opengl_filters:
  - warm
  - contrast

可选值：none, grayscale, warm, invert, contrast, saturation。运行时按 F 可以切换单滤镜预览。

断线重连：

reconnect:
  enabled: true
  initial_delay_ms: 1000
  max_delay_ms: 5000


启动时传真实 RTSP 地址：

.\build-vcpkg\bin\Release\rtsp_player.exe rtsp://你的摄像头IP:554/你的路径

本机摄像头已经配置为 MediaMTX + FFmpeg 推流，MediaMTX 位于：

..\mediamtx

启动本机摄像头 RTSP 流：

.\scripts\start_webcam_rtsp.ps1

然后运行播放器：

.\build-vcpkg\bin\Release\rtsp_player.exe

停止本机摄像头 RTSP 流：

.\scripts\stop_webcam_rtsp.ps1
