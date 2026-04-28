#include "rtp_parser.hpp"
#include <cstring>
#include <spdlog/spdlog.h>

namespace rtsp {

RtpParser::RtpParser()
    : ssrcFilter_(0)
    , lastSequence_(0)
    , initialized_(false)
{}

RtpParser::~RtpParser() = default;

std::shared_ptr<RtpPacket> RtpParser::parse(const uint8_t* data, size_t size) {
    if (!validateRtpHeader(data, size)) {
        return nullptr;
    }

    auto packet = std::make_shared<RtpPacket>();

    // RTP版本(V): 2 bits
    // 填充(P): 1 bit
    // 扩展(X): 1 bit
    uint8_t vpx = data[0];
    packet->csrcCount = vpx & 0x0F;  // CSRC count (4 bits)

    // 负载类型(PT): 7 bits
    packet->payloadType = data[1] & 0x7F;

    // 标记位(M): 1 bit
    packet->marker = (data[1] >> 7) & 0x01;

    // 序列号: 16 bits (big-endian)
    packet->sequenceNumber = (data[2] << 8) | data[3];

    // 时间戳: 32 bits (big-endian)
    packet->timestamp = (static_cast<uint32_t>(data[4]) << 24) |
                        (static_cast<uint32_t>(data[5]) << 16) |
                        (static_cast<uint32_t>(data[6]) << 8) |
                        static_cast<uint32_t>(data[7]);

    // SSRC: 32 bits (big-endian)
    packet->ssrc = (static_cast<uint32_t>(data[8]) << 24) |
                   (static_cast<uint32_t>(data[9]) << 16) |
                   (static_cast<uint32_t>(data[10]) << 8) |
                   static_cast<uint32_t>(data[11]);

    // 如果设置了SSRC过滤器，检查是否匹配
    if (ssrcFilter_ != 0 && packet->ssrc != ssrcFilter_) {
        return nullptr;
    }

    // 计算负载起始位置
    size_t headerSize = 12 + (packet->csrcCount * 4);
    if (size <= headerSize) {
        SPDLOG_WARN("RTP packet too small");
        return nullptr;
    }

    // 复制负载数据
    packet->payload.assign(data + headerSize, data + size);

    // 记录接收时间
    packet->receiveTime = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch());

    initialized_ = true;
    lastSequence_ = packet->sequenceNumber;

    return packet;
}

void RtpParser::reset() {
    ssrcFilter_ = 0;
    lastSequence_ = 0;
    initialized_ = false;
}

uint32_t RtpParser::getSsrc() const {
    return ssrcFilter_;
}

void RtpParser::setSsrcFilter(uint32_t ssrc) {
    ssrcFilter_ = ssrc;
}

bool RtpParser::validateRtpHeader(const uint8_t* data, size_t size) {
    // 最小的RTP头大小是12字节
    if (size < 12) {
        return false;
    }

    // 检查版本(V) = 2
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) {
        return false;
    }

    return true;
}

} // namespace rtsp