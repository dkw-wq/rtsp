cd C:\Users\dkw\.a_dkwrtc\rtsp

D:\vcpkg\vcpkg.exe install ffmpeg:x64-windows sdl2:x64-windows yaml-cpp:x64-windows spdlog:x64-windows

.\scripts\build.ps1

只重新构建：

.\scripts\build.ps1 -SkipConfigure

切换 OpenGL 渲染：

编辑 config\config.yaml：

renderer: "opengl"

OpenGL 滤镜：

opengl_filter: "warm"

可选值：none, grayscale, warm, invert。运行时按 F 可以切换滤镜。


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
