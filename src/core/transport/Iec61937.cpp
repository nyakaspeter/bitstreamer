#include "Iec61937.h"

namespace bs {

namespace {
// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL / _DTS
constexpr Guid128 kSubtypeDolbyDigital{0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr Guid128 kSubtypeDts{0x00000008, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

inline void putLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}
}  // namespace

bool Iec61937Packetizer::initialize(const EncoderCapabilities& enc, std::string& error) {
    codec_ = enc.codec;
    desc_ = {};
    desc_.transportChannels = 2;
    desc_.encodedSampleRate = enc.sampleRate;
    desc_.encodedChannels = enc.channels;
    // For AC-3 and DTS core the IEC 60958 frame rate equals the audio sample
    // rate and one burst spans exactly one codec frame.
    desc_.transportSampleRate = enc.sampleRate;
    desc_.burstFrames = enc.frameSamples;

    switch (enc.codec) {
        case CodecId::Ac3:
            if (enc.frameSamples != 1536) {
                error = "AC-3 requires 1536-sample frames";
                return false;
            }
            desc_.name = "IEC 61937-3 (AC-3)";
            desc_.waveSubFormat = kSubtypeDolbyDigital;
            desc_.legacyFormatTag = 0x0092;  // WAVE_FORMAT_DOLBY_AC3_SPDIF
            break;
        case CodecId::Dts:
            if (enc.frameSamples != 512 && enc.frameSamples != 1024 && enc.frameSamples != 2048) {
                error = "DTS requires 512/1024/2048-sample frames";
                return false;
            }
            desc_.name = "IEC 61937-5 (DTS type I-III)";
            desc_.waveSubFormat = kSubtypeDts;
            desc_.legacyFormatTag = 0x0008;  // WAVE_FORMAT_DTS
            break;
        default:
            error = "codec has no IEC 61937 mapping";
            return false;
    }
    if (enc.maxFrameBytes + iec61937::kPreambleBytes > desc_.burstBytes()) {
        error = "encoded frame does not fit into one IEC 61937 burst at this bitrate";
        return false;
    }
    return true;
}

bool Iec61937Packetizer::packetize(std::span<const uint8_t> frame, std::span<uint8_t> burst) {
    const size_t burstBytes = desc_.burstBytes();
    if (burst.size() != burstBytes) return false;
    if (frame.size() < 8 || frame.size() + iec61937::kPreambleBytes > burstBytes) return false;

    uint16_t pc = 0;
    if (codec_ == CodecId::Ac3) {
        if (frame[0] != 0x0B || frame[1] != 0x77) return false;  // syncword
        const uint16_t bsmod = frame[5] & 0x07;
        pc = static_cast<uint16_t>(iec61937::kTypeAc3 | (bsmod << 8));
    } else {
        if (frame[0] != 0x7F || frame[1] != 0xFE || frame[2] != 0x80 || frame[3] != 0x01) return false;
        // NBLKS: 7 bits following SYNC(32) FTYPE(1) SHORT(5) CPF(1).
        const uint32_t nblks = ((static_cast<uint32_t>(frame[4]) << 8 | frame[5]) >> 2) & 0x7F;
        const uint32_t samples = (nblks + 1) * 32;
        if (samples != desc_.burstFrames) return false;
        pc = samples == 512 ? iec61937::kTypeDts1 : samples == 1024 ? iec61937::kTypeDts2 : iec61937::kTypeDts3;
    }

    uint8_t* out = burst.data();
    putLe16(out + 0, iec61937::kPa);
    putLe16(out + 2, iec61937::kPb);
    putLe16(out + 4, pc);
    putLe16(out + 6, static_cast<uint16_t>(frame.size() * 8));  // Pd: length in bits
    out += iec61937::kPreambleBytes;

    // The codec bitstream is a big-endian sequence of 16-bit words; on the
    // little-endian PCM carrier each byte pair is swapped.
    const size_t pairs = frame.size() / 2;
    const uint8_t* in = frame.data();
    for (size_t i = 0; i < pairs; ++i) {
        out[2 * i] = in[2 * i + 1];
        out[2 * i + 1] = in[2 * i];
    }
    size_t written = pairs * 2;
    if (frame.size() & 1) {
        out[written] = 0;
        out[written + 1] = in[frame.size() - 1];
        written += 2;
    }
    std::memset(out + written, 0, burstBytes - iec61937::kPreambleBytes - written);
    return true;
}

}  // namespace bs
