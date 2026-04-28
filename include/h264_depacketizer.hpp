#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include "rtp_parser.hpp"

namespace rtsp {

/**
 * @brief H.264 NAL单元类型
 */
enum class NalUnitType : uint8_t {
    UNSPECIFIED = 0,
    NON_IDR = 1,
    PARTITION_A = 2,
    PARTITION_B = 3,
    PARTITION_C = 4,
    IDR = 5,
    SEI = 6,
    SPS = 7,
    PPS = 8,
    ACCESS_UNIT_DELIMITER = 9,
    END_OF_SEQUENCE = 10,
    END_OF_STREAM = 11,
    FILLER_DATA = 12
};

/**
 * @brief H.264解包器
 * @note 将RTP负载解析为完整的H.264 NAL单元
 */
class H264Depacketizer {
public:
    H264Depacketizer();
    ~H264Depacketizer();

    /**
     * @brief 解包RTP包
     * @param rtp RTP包
     * @return 解包成功返回NAL单元数据，失败返回空vector
     */
    std::vector<uint8_t> depacketize(const std::shared_ptr<RtpPacket>& rtp);

    /**
     * @brief 重置状态
     */
    void reset();

    /**
     * @brief 检查是否收到SPS
     */
    bool hasSps() const;

    /**
     * @brief 检查是否收到PPS
     */
    bool hasPps() const;

    /**
     * @brief 获取SPS数据
     */
    std::vector<uint8_t> getSps() const;

    /**
     * @brief 获取PPS数据
     */
    std::vector<uint8_t> getPps() const;

private:
    // 处理FU-A分片
    std::vector<uint8_t> handleFuA(const std::shared_ptr<RtpPacket>& rtp);

    // 处理STAP-A聚合
    std::vector<uint8_t> handleStapA(const std::shared_ptr<RtpPacket>& rtp);

    // 处理单NAL单元
    std::vector<uint8_t> handleSingleNalu(const std::shared_ptr<RtpPacket>& rtp);

    // 提取NAL单元类型
    static NalUnitType getNalUnitType(uint8_t nalHeader);

    // 保存分片数据
    struct FragmentData {
        uint8_t nalHeader;
        std::vector<uint8_t> buffer;
        uint16_t expectedSeq;
    };

    std::unique_ptr<FragmentData> fragmentData_;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    bool hasSps_;
    bool hasPps_;
};

} // namespace rtsp