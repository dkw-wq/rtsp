#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <memory>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace rtsp {

/**
 * @brief 媒体帧结构
 */
struct MediaFrame {
    enum class Type {
        VIDEO,
        AUDIO
    };

    enum class PixelFormat {
        YUV420P,
        NV12,
        CUDA_NV12
    };

    Type type;
    PixelFormat pixelFormat;
    std::vector<uint8_t> data;        // 解码后的原始数据或压缩数据
    std::array<uintptr_t, 2> gpuData; // GPU端Y/UV平面指针(CUDA_NV12)
    std::array<int, 2> gpuLinesize;   // GPU端Y/UV平面pitch(CUDA_NV12)
    std::shared_ptr<void> hardwareFrameRef; // 持有底层硬件帧生命周期
    int width;                         // 视频宽度
    int height;                        // 视频高度
    int sampleRate;                    // 音频采样率
    int channels;                      // 音频声道数
    int bytesPerSample;                // 单样本字节数
    uint64_t pts;                      // 显示时间戳
    uint64_t dts;                      // 解码时间戳
    double ptsSeconds;                 // 秒级显示时间戳
    double durationSeconds;            // 帧持续时间
    bool keyFrame;                     // 是否为关键帧
    std::chrono::microseconds recvTime; // 接收时间

    MediaFrame() 
        : type(Type::VIDEO)
        , pixelFormat(PixelFormat::YUV420P)
        , gpuData{0, 0}
        , gpuLinesize{0, 0}
        , hardwareFrameRef(nullptr)
        , width(0)
        , height(0)
        , sampleRate(0)
        , channels(0)
        , bytesPerSample(0)
        , pts(0)
        , dts(0)
        , ptsSeconds(0.0)
        , durationSeconds(0.0)
        , keyFrame(false)
        , recvTime(0)
    {}
};

/**
 * @brief Jitter Buffer
 * @note 缓冲已解码的视频帧，按显示时间戳进行有限重排
 */
class JitterBuffer {
public:
    /**
     * @brief 构造函数
     * @param maxSize 最大缓冲帧数
     * @param latencyMs 基础延迟(毫秒)
     * @param adaptive 是否根据帧到达抖动调整延迟
     * @param maxLatencyMs 自适应延迟上限(毫秒)
     */
    JitterBuffer(size_t maxSize = 12, uint32_t latencyMs = 30,
                 bool adaptive = true, uint32_t maxLatencyMs = 200);

    ~JitterBuffer();

    /**
     * @brief 添加帧到缓冲区
     * @param frame 媒体帧
     * @return true if added successfully
     */
    bool push(const std::shared_ptr<MediaFrame>& frame);

    /**
     * @brief 从缓冲区取出帧
     * @param frame 输出参数，获取的帧
     * @param timeoutMs 超时时间(毫秒)
     * @return true if got frame successfully
     */
    bool pop(std::shared_ptr<MediaFrame>& frame, uint32_t timeoutMs = 100);

    /**
     * @brief 清空缓冲区
     */
    void clear();

    /**
     * @brief 获取当前缓冲区大小
     */
    size_t size() const;

    /**
     * @brief 检查缓冲区是否为空
     */
    bool empty() const;

    /**
     * @brief 设置目标延迟
     */
    void setLatency(uint32_t latencyMs);

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        size_t bufferSize;
        double avgJitter; // 接收间隔与显示间隔之差的 EWMA，单位 ms
        uint32_t targetLatencyMs;
        uint64_t droppedFrames;
        uint64_t overflowDroppedFrames;
        uint64_t lateDroppedFrames;
        uint64_t totalFrames;
    };
    Stats getStats() const;

private:
    bool shouldRelease(const std::shared_ptr<MediaFrame>& frame) const;
    void updateTargetLatency();

    size_t maxSize_;
    uint32_t baseLatencyMs_;
    uint32_t targetLatencyMs_;
    uint32_t maxLatencyMs_;
    bool adaptive_;
    std::deque<std::shared_ptr<MediaFrame>> buffer_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // 统计信息
    uint64_t overflowDroppedFrames_;
    uint64_t lateDroppedFrames_;
    uint64_t totalFrames_;
    double lastPtsSeconds_;
    double lastReleasedPtsSeconds_;
    std::chrono::microseconds lastRecvTime_;
    double jitterEstimateMs_;
    bool hasLastArrival_;
    bool hasLastRelease_;
};

} // namespace rtsp
