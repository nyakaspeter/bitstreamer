// Codec-neutral encoder and bitstream-packetizer interfaces plus the registry
// that the pipeline, CLI and GUI use to discover codecs. Nothing outside the
// codec/transport modules may assume a specific frame size, bitrate or burst
// type: everything is queried from EncoderCapabilities / TransportDescriptor.
#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../Common.h"

namespace bs {

enum class CodecId { Ac3, Dts };

struct EncoderSettings {
    uint32_t bitrate = 0;  // 0 = codec default
};

struct EncoderCapabilities {
    CodecId codec = CodecId::Ac3;
    std::string name;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint32_t channelMask = 0;             // canonical input channel order
    uint32_t frameSamples = 0;            // PCM frames consumed per encoded frame
    uint32_t algorithmicDelaySamples = 0; // codec look-ahead (not frame duration)
    bool algorithmicDelayEstimated = false; // true if not reported by the encoder
    uint32_t bitrate = 0;
    uint32_t maxFrameBytes = 0;
};

struct EncodeResult {
    enum class Status { Ok, NeedMoreData, Error };
    Status status = Status::Error;
    std::span<const uint8_t> data;  // valid until the next encode()/reset()
    int errorCode = 0;
};

class ISurroundEncoder {
public:
    virtual ~ISurroundEncoder() = default;

    virtual bool initialize(const AudioFormat& input, const EncoderSettings& settings, std::string& error) = 0;
    virtual EncoderCapabilities capabilities() const = 0;

    // Encoder-owned planar float staging buffer holding one frame
    // (capabilities().frameSamples per channel). The pipeline accumulates
    // captured PCM directly into it so no extra accumulation buffer exists.
    // Must be re-queried after every encode().
    virtual float* const* inputPlanes() = 0;

    // Encodes exactly one frame. `input` may be the buffer from inputPlanes()
    // (zero copy) or any other planar float buffer of frameSamples frames.
    virtual EncodeResult encode(const AudioBufferView& input) = 0;

    virtual void flush() = 0;
    virtual void reset() = 0;
};

// Identifies how an encoded stream is carried on a digital audio link.
struct Guid128 {
    uint32_t d1; uint16_t d2; uint16_t d3; uint8_t d4[8];
};

struct TransportDescriptor {
    std::string name;                 // e.g. "IEC 61937-3 (AC-3)"
    Guid128 waveSubFormat{};          // KSDATAFORMAT_SUBTYPE_IEC61937_*
    uint16_t legacyFormatTag = 0;     // e.g. WAVE_FORMAT_DOLBY_AC3_SPDIF, 0 = none
    uint32_t transportSampleRate = 0; // IEC 60958 frame rate
    uint16_t transportChannels = 2;
    uint32_t burstFrames = 0;         // burst repetition period in IEC 60958 frames
    uint32_t encodedSampleRate = 0;
    uint32_t encodedChannels = 0;
    size_t burstBytes() const { return static_cast<size_t>(burstFrames) * transportChannels * 2; }
};

class IBitstreamPacketizer {
public:
    virtual ~IBitstreamPacketizer() = default;
    virtual bool initialize(const EncoderCapabilities& encoder, std::string& error) = 0;
    virtual TransportDescriptor descriptor() const = 0;
    // Writes exactly descriptor().burstBytes() bytes into `burst`.
    virtual bool packetize(std::span<const uint8_t> frame, std::span<uint8_t> burst) = 0;
};

struct CodecInfo {
    CodecId id;
    std::string displayName;
    bool available = false;        // encoder present in this build
    std::vector<uint32_t> bitrates;
    uint32_t defaultBitrate = 0;
};

const std::vector<CodecInfo>& codecRegistry();
const CodecInfo* findCodec(CodecId id);
const char* codecShortName(CodecId id);                  // "ac3", "dts"
bool parseCodec(std::string_view s, CodecId& out);
std::unique_ptr<ISurroundEncoder> createEncoder(CodecId id);
std::unique_ptr<IBitstreamPacketizer> createPacketizer(CodecId id);

}  // namespace bs
