#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "jitter_buffer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace rtsp {

/**
 * @brief FFmpeg解码器
 * @note 使用FFmpeg解码H.264视频流
 */
class Decoder {
public:
    Decoder();
    ~Decoder();

    /**
     * @brief 初始化解码器
     * @param codecId 编解码器ID (AV_CODEC_ID_H264)
     * @param width 视频宽度
     * @param height 视频高度
     * @return true if initialized successfully
     */
    bool initialize(AVCodecID codecId, int width, int height);

    /**
     * @brief 解码数据
     * @param data 输入数据(H.264 NAL单元)
     * @param size 数据大小
     * @param pts 显示时间戳
     * @param dts 解码时间戳
     * @return 解码成功返回MediaFrame，失败返回nullptr
     */
    std::shared_ptr<MediaFrame> decode(const uint8_t* data, size_t size, 
                                        int64_t pts, int64_t dts);

    /**
     * @brief 解码数据(带标记位)
     */
    std::shared_ptr<MediaFrame> decode(const uint8_t* data, size_t size,
                                        int64_t pts, int64_t dts, bool keyFrame);

    /**
     * @brief 刷新解码器
     */
    void flush();

    /**
     * @brief 关闭解码器
     */
    void close();

    /**
     * @brief 获取解码后的图像宽度
     */
    int getWidth() const;

    /**
     * @brief 获取解码后的图像高度
     */
    int getHeight() const;

    /**
     * @brief 检查是否已初始化
     */
    bool isInitialized() const;

private:
    AVCodecContext* codecContext_;
    AVFrame* frame_;
    AVPacket* packet_;
    SwsContext* swsContext_;

    int width_;
    int height_;
    bool initialized_;
};

} // namespace rtsp