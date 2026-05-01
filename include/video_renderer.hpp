#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "jitter_buffer.hpp"

namespace rtsp {

struct PlaybackStats {
    double fps = 0.0;
    uint64_t decodedFrames = 0;
    uint64_t droppedFrames = 0;
    size_t jitterBufferSize = 0;
    uint32_t latencyMs = 0;
};

/**
 * @brief 视频渲染器
 * @note 渲染后端接口，当前默认实现位于video_renderer.cpp中的SDL2后端
 */
class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;

    /**
     * @brief 初始化渲染器
     * @param width 视频宽度
     * @param height 视频高度
     * @param title 窗口标题
     * @return true if initialized successfully
     */
    virtual bool initialize(int width, int height,
                            const std::string& title = "RTSP Player") = 0;

    /**
     * @brief 渲染帧
     * @param frame 媒体帧
     * @return true if rendered successfully
     */
    virtual bool render(const std::shared_ptr<MediaFrame>& frame) = 0;

    /**
     * @brief 更新播放状态叠加层数据
     */
    virtual void setPlaybackStats(const PlaybackStats& stats) = 0;

    /**
     * @brief 处理SDL事件
     * @return false if should quit
     */
    virtual bool handleEvents() = 0;

    /**
     * @brief 关闭渲染器
     */
    virtual void close() = 0;

    /**
     * @brief 获取窗口宽度
     */
    virtual int getWidth() const = 0;

    /**
     * @brief 获取窗口高度
     */
    virtual int getHeight() const = 0;

    /**
     * @brief 检查是否已初始化
     */
    virtual bool isInitialized() const = 0;
};

std::unique_ptr<VideoRenderer> createSdlVideoRenderer();
std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer();
std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer(const std::string& filterName);
std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer(const std::vector<std::string>& filterNames);

} // namespace rtsp
