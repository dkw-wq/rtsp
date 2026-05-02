#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <queue>
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
        NV12
    };

    Type type;
    PixelFormat pixelFormat;
    std::vector<uint8_t> data;        // 解码后的原始数据或压缩数据
    int width;                         // 视频宽度
    int height;                        // 视频高度
    uint64_t pts;                      // 显示时间戳
    uint64_t dts;                      // 解码时间戳
    bool keyFrame;                     // 是否为关键帧
    std::chrono::microseconds recvTime; // 接收时间

    MediaFrame() 
        : type(Type::VIDEO)
        , pixelFormat(PixelFormat::YUV420P)
        , width(0)
        , height(0)
        , pts(0)
        , dts(0)
        , keyFrame(false)
        , recvTime(0)
    {}
};

/**
 * @brief Jitter Buffer
 * @note 用于平滑RTP数据包的时序抖动
 */
class JitterBuffer {
public:
    /**
     * @brief 构造函数
     * @param maxSize 最大缓冲帧数
     * @param latencyMs 目标延迟(毫秒)
     */
    JitterBuffer(size_t maxSize = 30, uint32_t latencyMs = 100);

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
        uint32_t avgJitter;
        uint64_t droppedFrames;
        uint64_t totalFrames;
    };
    Stats getStats() const;

private:
    bool shouldRelease(const std::shared_ptr<MediaFrame>& frame) const;

    size_t maxSize_;
    uint32_t latencyMs_;
    std::queue<std::shared_ptr<MediaFrame>> buffer_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // 统计信息
    uint64_t droppedFrames_;
    uint64_t totalFrames_;
    uint32_t lastPts_;
    uint32_t jitterSum_;
    uint32_t jitterCount_;
};

} // namespace rtsp
