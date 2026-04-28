#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "jitter_buffer.hpp"

extern "C" {
#include <SDL2/SDL.h>
}

namespace rtsp {

/**
 * @brief 视频渲染器
 * @note 使用SDL2渲染解码后的视频帧
 */
class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    /**
     * @brief 初始化渲染器
     * @param width 视频宽度
     * @param height 视频高度
     * @param title 窗口标题
     * @return true if initialized successfully
     */
    bool initialize(int width, int height, const std::string& title = "RTSP Player");

    /**
     * @brief 渲染帧
     * @param frame 媒体帧
     * @return true if rendered successfully
     */
    bool render(const std::shared_ptr<MediaFrame>& frame);

    /**
     * @brief 渲染YUV数据
     * @param y Y分量数据
     * @param u U分量数据
     * @param v V分量数据
     * @param width 宽度
     * @param height 高度
     */
    bool renderYuv(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                    int width, int height);

    /**
     * @brief 处理SDL事件
     * @return false if should quit
     */
    bool handleEvents();

    /**
     * @brief 关闭渲染器
     */
    void close();

    /**
     * @brief 获取窗口宽度
     */
    int getWidth() const;

    /**
     * @brief 获取窗口高度
     */
    int getHeight() const;

    /**
     * @brief 检查是否已初始化
     */
    bool isInitialized() const;

private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    SDL_Texture* texture_;

    int width_;
    int height_;
    bool initialized_;
};

} // namespace rtsp