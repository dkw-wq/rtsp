#include "h264_depacketizer.hpp"
#include <spdlog/spdlog.h>
#include <cstring>

namespace rtsp {

// NAL单元类型常量
static const uint8_t NAL_HEADER_FU_INDICATOR = 0x1C;  // 1110 1100
static const uint8_t NAL_HEADER_FU_START = 0x80;    // 1000 0000
static const uint8_t NAL_HEADER_FU_END = 0x40;      // 0100 0000
static const uint8_t NAL_HEADER_FU_MIDDLE = 0x00;  // 0000 0000

H264Depacketizer::H264Depacketizer()
    : hasSps_(false)
    , hasPps_(false)
{
    fragmentData_ = std::make_unique<FragmentData>();
}

H264Depacketizer::~H264Depacketizer() = default;

std::vector<uint8_t> H264Depacketizer::depacketize(const std::shared_ptr<RtpPacket>& rtp) {
    if (!rtp || rtp->payload.empty()) {
        return {};
    }

    const uint8_t* payload = rtp->payload.data();

    // 获取NAL单元类型
    uint8_t nalHeader = payload[0];

    // 检查是否是分片单元(FU-A)
    if ((nalHeader & 0x1F) == 28) {  // FU-A type = 28
        return handleFuA(rtp);
    }
    // 检查是否是STAP-A聚合
    else if ((nalHeader & 0x1F) == 24) {  // STAP-A type = 24
        return handleStapA(rtp);
    }
    // 单NAL单元
    else {
        return handleSingleNalu(rtp);
    }
}

std::vector<uint8_t> H264Depacketizer::handleFuA(const std::shared_ptr<RtpPacket>& rtp) {
    const uint8_t* payload = rtp->payload.data();
    size_t payloadSize = rtp->payload.size();

    if (payloadSize < 2) {
        return {};
    }

    // FU indicator
    uint8_t fuIndicator = payload[0];
    // FU header
    uint8_t fuHeader = payload[1];

    // 提取NAL单元类型
    uint8_t nalType = fuHeader & 0x1F;
    // 提取开始位和结束位
    bool start = (fuHeader & 0x80) != 0;
    bool end = (fuHeader & 0x40) != 0;

    // 构造新的NAL头
    uint8_t newNalHeader = (fuIndicator & 0xE0) | nalType;

    if (start) {
        // 开始新的分片
        fragmentData_->nalHeader = newNalHeader;
        fragmentData_->buffer.clear();
        fragmentData_->buffer.push_back(newNalHeader);
        fragmentData_->expectedSeq = rtp->sequenceNumber;
    }

    // 添加FU负载(跳过FU indicator和header)
    fragmentData_->buffer.insert(fragmentData_->buffer.end(), 
                                  payload + 2, 
                                  payload + payloadSize);

    if (end) {
        // 分片结束，返回完整NAL单元
        std::vector<uint8_t> result = std::move(fragmentData_->buffer);
        fragmentData_->buffer.clear();
        return result;
    }

    // 分片未完成，返回空
    return {};
}

std::vector<uint8_t> H264Depacketizer::handleStapA(const std::shared_ptr<RtpPacket>& rtp) {
    const uint8_t* payload = rtp->payload.data();
    size_t payloadSize = rtp->payload.size();

    if (payloadSize < 3) {
        return {};
    }

    // STAP-A: 跳过NRI和类型(1 byte)，然后是2 byte的size
    size_t offset = 1;
    std::vector<uint8_t> result;

    while (offset + 2 < payloadSize) {
        // 读取NAL单元大小
        uint16_t naluSize = (static_cast<uint16_t>(payload[offset]) << 8) |
                           payload[offset + 1];
        offset += 2;

        if (offset + naluSize > payloadSize) {
            break;
        }

        // 复制NAL单元
        result.insert(result.end(), payload + offset, payload + offset + naluSize);
        offset += naluSize;
    }

    return result;
}

std::vector<uint8_t> H264Depacketizer::handleSingleNalu(const std::shared_ptr<RtpPacket>& rtp) {
    const uint8_t* payload = rtp->payload.data();
    size_t payloadSize = rtp->payload.size();

    uint8_t nalHeader = payload[0];
    NalUnitType nalType = getNalUnitType(nalHeader);

    // 保存SPS和PPS
    if (nalType == NalUnitType::SPS) {
        sps_.assign(payload, payload + payloadSize);
        hasSps_ = true;
        SPDLOG_DEBUG("Received SPS, size: {}", payloadSize);
    } else if (nalType == NalUnitType::PPS) {
        pps_.assign(payload, payload + payloadSize);
        hasPps_ = true;
        SPDLOG_DEBUG("Received PPS, size: {}", payloadSize);
    }

    return std::vector<uint8_t>(payload, payload + payloadSize);
}

NalUnitType H264Depacketizer::getNalUnitType(uint8_t nalHeader) {
    return static_cast<NalUnitType>(nalHeader & 0x1F);
}

void H264Depacketizer::reset() {
    fragmentData_->buffer.clear();
    sps_.clear();
    pps_.clear();
    hasSps_ = false;
    hasPps_ = false;
}

bool H264Depacketizer::hasSps() const { return hasSps_; }
bool H264Depacketizer::hasPps() const { return hasPps_; }

std::vector<uint8_t> H264Depacketizer::getSps() const { return sps_; }
std::vector<uint8_t> H264Depacketizer::getPps() const { return pps_; }

} // namespace rtsp
