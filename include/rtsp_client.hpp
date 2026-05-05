#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <chrono>

namespace rtsp {

// Forward declarations
struct RtpPacket;
struct MediaFrame;

struct RtspConnectionOptions {
    std::string transport = "tcp";
    int timeoutMs = 5000;
    int bufferSize = 262144;
    bool lowLatency = true;
    int maxDelayMs = 50;
    int analyzeDurationMs = 0;
    int probeSizeBytes = 32768;
    int reorderQueueSize = 0;
};

/**
 * @brief RTSP客户端类
 * @note 使用FFmpeg处理RTSP连接和RTP接收
 */
class RtspClient {
public:
    using FrameCallback = std::function<void(const std::shared_ptr<MediaFrame>&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    RtspClient();
    ~RtspClient();

    // 禁止拷贝和移动
    RtspClient(const RtspClient&) = delete;
    RtspClient& operator=(const RtspClient&) = delete;
    RtspClient(RtspClient&&) = delete;
    RtspClient& operator=(RtspClient&&) = delete;

    /**
     * @brief 连接到RTSP流
     * @param url RTSP URL (e.g., rtsp://192.168.1.100:554/stream)
     * @return true if connected successfully
     */
    bool connect(const std::string& url);

    /**
     * @brief 断开连接
     */
    void disconnect();

    /**
     * @brief 开始接收流
     */
    void start();

    /**
     * @brief 停止接收流
     */
    void stop();

    /**
     * @brief 设置帧回调
     */
    void setFrameCallback(FrameCallback callback);

    /**
     * @brief 设置错误回调
     */
    void setErrorCallback(ErrorCallback callback);

    /**
     * @brief 设置RTSP连接参数
     */
    void setConnectionOptions(const RtspConnectionOptions& options);

    /**
     * @brief 启用或关闭音频解码
     */
    void setAudioEnabled(bool enabled);

    /**
     * @brief 设置硬件解码后端
     * @param backend none, cuda
     */
    void setHardwareDecode(const std::string& backend);

    /**
     * @brief 允许输出硬件帧给GPU渲染器
     */
    void setHardwareFrameOutput(bool enabled);

    /**
     * @brief 获取当前实际解码后端
     * @return CPU, CUDA
     */
    std::string getDecodeBackend() const;

    /**
     * @brief 获取硬解状态说明
     */
    std::string getHardwareDecodeStatus() const;

    /**
     * @brief 检查是否正在运行
     */
    bool isRunning() const;

    /**
     * @brief 获取视频宽度
     */
    int getWidth() const;

    /**
     * @brief 获取视频高度
     */
    int getHeight() const;

private:
    void receiveLoop();

    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace rtsp
