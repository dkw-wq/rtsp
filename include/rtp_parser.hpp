#pragma once

#include <cstdint>
#include <chrono>
#include <vector>
#include <memory>

namespace rtsp {

/**
 * @brief RTP包结构
 */
struct RtpPacket {
    uint16_t sequenceNumber;      // RTP序列号
    uint32_t timestamp;            // RTP时间戳
    uint32_t ssrc;                // 同步源标识
    uint8_t payloadType;          // 负载类型
    uint8_t marker;                // 标记位
    uint32_t csrcCount;            // CSRC计数
    
    std::vector<uint8_t> payload; // RTP负载数据
    std::chrono::microseconds receiveTime; // 接收时间

    RtpPacket() 
        : sequenceNumber(0)
        , timestamp(0)
        , ssrc(0)
        , payloadType(0)
        , marker(0)
        , csrcCount(0)
    {}
};

/**
 * @brief RTP解析器
 * @note 解析RTP协议包
 */
class RtpParser {
public:
    RtpParser();
    ~RtpParser();

    /**
     * @brief 解析RTP包
     * @param data UDP数据
     * @param size 数据大小
     * @return 解析成功返回RtpPacket智能指针，失败返回nullptr
     */
    std::shared_ptr<RtpPacket> parse(const uint8_t* data, size_t size);

    /**
     * @brief 重置解析器状态
     */
    void reset();

    /**
     * @brief 获取SSRC
     */
    uint32_t getSsrc() const;

    /**
     * @brief 设置SSRC过滤器
     */
    void setSsrcFilter(uint32_t ssrc);

private:
    bool validateRtpHeader(const uint8_t* data, size_t size);

    uint32_t ssrcFilter_;
    uint32_t lastSequence_;
    bool initialized_;
};

} // namespace rtsp
