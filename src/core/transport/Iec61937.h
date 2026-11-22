// IEC 61937 data-burst packetizer for non-linear-PCM bitstreams carried over
// an IEC 60958 (S/PDIF, HDMI) link represented as 16-bit little-endian stereo.
#pragma once

#include "../codec/Codec.h"

namespace bs {

namespace iec61937 {
inline constexpr uint16_t kPa = 0xF872;
inline constexpr uint16_t kPb = 0x4E1F;
inline constexpr uint8_t kTypeAc3 = 0x01;
inline constexpr uint8_t kTypeDts1 = 0x0B;  // 512-sample frames
inline constexpr uint8_t kTypeDts2 = 0x0C;  // 1024-sample frames
inline constexpr uint8_t kTypeDts3 = 0x0D;  // 2048-sample frames
inline constexpr size_t kPreambleBytes = 8;
}  // namespace iec61937

class Iec61937Packetizer final : public IBitstreamPacketizer {
public:
    bool initialize(const EncoderCapabilities& encoder, std::string& error) override;
    TransportDescriptor descriptor() const override { return desc_; }
    bool packetize(std::span<const uint8_t> frame, std::span<uint8_t> burst) override;

private:
    CodecId codec_ = CodecId::Ac3;
    TransportDescriptor desc_;
};

}  // namespace bs
