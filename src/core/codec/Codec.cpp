// libavcodec-backed encoders and the codec registry.
#include "Codec.h"

#include <algorithm>
#include <cmath>

#include "../transport/Iec61937.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

namespace bs {

namespace {

struct LavcDesc {
    CodecId id;
    const char* lavcName;
    const char* displayName;
    AVSampleFormat sampleFormat;
    bool experimental;
    std::vector<uint32_t> bitrates;
    uint32_t defaultBitrate;
    const char* note;
};

const LavcDesc& lavcDesc(CodecId id) {
    static const LavcDesc ac3{CodecId::Ac3, "ac3", "Dolby Digital (AC-3)", AV_SAMPLE_FMT_FLTP, false,
                              {384000, 448000, 640000}, 640000,
                              "FFmpeg native AC-3 encoder"};
    // FFmpeg's DTS Coherent Acoustics core encoder is flagged experimental
    // upstream. 1411.2 kbit/s is the highest rate whose 512-sample frame still
    // fits a type-I IEC 61937 burst together with the preamble.
    static const LavcDesc dts{CodecId::Dts, "dca", "DTS Digital Surround", AV_SAMPLE_FMT_S32, true,
                              {768000, 1411200}, 768000,
                              "FFmpeg DCA core encoder (flagged experimental upstream)"};
    return id == CodecId::Ac3 ? ac3 : dts;
}

std::string avErr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

class LavcEncoder final : public ISurroundEncoder {
public:
    explicit LavcEncoder(const LavcDesc& d) : desc_(d) {}
    ~LavcEncoder() override { close(); }

    bool initialize(const AudioFormat& input, const EncoderSettings& settings, std::string& error) override {
        close();
        if (input.channels != 6 || input.sampleRate != 48000) {
            error = "encoder input must be 48 kHz 5.1 (got " + input.describe() + ")";
            return false;
        }
        const AVCodec* codec = avcodec_find_encoder_by_name(desc_.lavcName);
        if (!codec) {
            error = std::string("libavcodec was built without the '") + desc_.lavcName + "' encoder";
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) { error = "avcodec_alloc_context3 failed"; return false; }

        uint32_t bitrate = settings.bitrate ? settings.bitrate : desc_.defaultBitrate;
        if (std::find(desc_.bitrates.begin(), desc_.bitrates.end(), bitrate) == desc_.bitrates.end()) {
            error = "unsupported bitrate " + std::to_string(bitrate);
            return false;
        }
        ctx_->sample_rate = static_cast<int>(input.sampleRate);
        ctx_->bit_rate = bitrate;
        ctx_->sample_fmt = desc_.sampleFormat;
        ctx_->thread_count = 1;  // one frame is far too small to benefit from threading
        av_channel_layout_from_mask(&ctx_->ch_layout, AV_CH_LAYOUT_5POINT1);  // FL FR FC LFE SL SR
        if (desc_.experimental) ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

        int rc = avcodec_open2(ctx_, codec, nullptr);
        if (rc < 0) { error = "avcodec_open2: " + avErr(rc); close(); return false; }
        if (ctx_->frame_size <= 0) { error = "encoder reported no fixed frame size"; close(); return false; }

        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!frame_ || !packet_) { error = "out of memory"; close(); return false; }
        frame_->nb_samples = ctx_->frame_size;
        frame_->format = ctx_->sample_fmt;
        frame_->sample_rate = ctx_->sample_rate;
        av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout);
        rc = av_frame_get_buffer(frame_, 0);
        if (rc < 0) { error = "av_frame_get_buffer: " + avErr(rc); close(); return false; }

        caps_ = {};
        caps_.codec = desc_.id;
        caps_.name = desc_.displayName;
        caps_.sampleRate = input.sampleRate;
        caps_.channels = 6;
        caps_.channelMask = spk::Surround51Side;
        caps_.frameSamples = static_cast<uint32_t>(ctx_->frame_size);
        caps_.algorithmicDelaySamples = static_cast<uint32_t>(std::max(0, ctx_->initial_padding));
        // libavcodec's DCA encoder does not report its delay. The codec's
        // 512-tap, 32-band analysis/synthesis filter bank delays the signal
        // by one filter length end to end; use that as the estimate.
        if (desc_.id == CodecId::Dts && caps_.algorithmicDelaySamples == 0) {
            caps_.algorithmicDelaySamples = 512;
            caps_.algorithmicDelayEstimated = true;
        }
        caps_.bitrate = bitrate;
        const uint64_t bits = static_cast<uint64_t>(bitrate) * caps_.frameSamples / caps_.sampleRate;
        caps_.maxFrameBytes = static_cast<uint32_t>(((bits + 31) / 32) * 4);

        planar_ = desc_.sampleFormat == AV_SAMPLE_FMT_FLTP;
        if (!planar_) {
            staging_.assign(static_cast<size_t>(caps_.channels) * caps_.frameSamples, 0.0f);
        }
        refreshPlanes();
        pts_ = 0;
        return true;
    }

    EncoderCapabilities capabilities() const override { return caps_; }

    float* const* inputPlanes() override { return planes_.data(); }

    EncodeResult encode(const AudioBufferView& input) override {
        EncodeResult r;
        if (!ctx_ || input.channels != caps_.channels || input.frames != caps_.frameSamples) {
            r.errorCode = AVERROR(EINVAL);
            return r;
        }
        // The frame stays writable as long as the encoder did not retain a
        // reference to it; the native audio encoders copy their input.
        int rc = av_frame_make_writable(frame_);
        if (rc < 0) { r.errorCode = rc; return r; }
        const bool ownBuffer = input.planes == planes_.data() || input.planes[0] == planes_[0];
        const size_t n = caps_.frameSamples;
        if (planar_) {
            for (uint16_t c = 0; c < caps_.channels; ++c) {
                auto* dst = reinterpret_cast<float*>(frame_->extended_data[c]);
                if (dst != input.planes[c]) std::memcpy(dst, input.planes[c], n * sizeof(float));
            }
        } else {
            (void)ownBuffer;
            auto* dst = reinterpret_cast<int32_t*>(frame_->extended_data[0]);
            const uint16_t ch = caps_.channels;
            for (uint16_t c = 0; c < ch; ++c) {
                const float* src = input.planes[c];
                for (size_t i = 0; i < n; ++i) {
                    double v = static_cast<double>(src[i]) * 2147483648.0;
                    v = std::clamp(v, -2147483648.0, 2147483647.0);
                    dst[i * ch + c] = static_cast<int32_t>(std::lrint(v));
                }
            }
        }
        frame_->pts = pts_;
        pts_ += static_cast<int64_t>(n);

        av_packet_unref(packet_);
        rc = avcodec_send_frame(ctx_, frame_);
        if (rc < 0) { r.errorCode = rc; refreshPlanes(); return r; }
        rc = avcodec_receive_packet(ctx_, packet_);
        refreshPlanes();
        if (rc == AVERROR(EAGAIN)) { r.status = EncodeResult::Status::NeedMoreData; return r; }
        if (rc < 0) { r.errorCode = rc; return r; }
        r.status = EncodeResult::Status::Ok;
        r.data = {packet_->data, static_cast<size_t>(packet_->size)};
        return r;
    }

    void flush() override {
        if (ctx_) avcodec_flush_buffers(ctx_);
    }

    void reset() override {
        // The native encoders keep only overlap state; flushing is not
        // supported by all of them, so a reset is a no-op apart from timing.
        pts_ = 0;
    }

private:
    void refreshPlanes() {
        planes_.resize(caps_.channels);
        for (uint16_t c = 0; c < caps_.channels; ++c) {
            planes_[c] = planar_ ? reinterpret_cast<float*>(frame_->extended_data[c])
                                 : staging_.data() + static_cast<size_t>(c) * caps_.frameSamples;
        }
    }
    void close() {
        if (packet_) av_packet_free(&packet_);
        if (frame_) av_frame_free(&frame_);
        if (ctx_) avcodec_free_context(&ctx_);
    }

    const LavcDesc& desc_;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    EncoderCapabilities caps_;
    std::vector<float*> planes_;
    std::vector<float> staging_;
    bool planar_ = true;
    int64_t pts_ = 0;
};

CodecInfo makeInfo(CodecId id) {
    const LavcDesc& d = lavcDesc(id);
    CodecInfo info;
    info.id = id;
    info.displayName = d.displayName;
    info.available = avcodec_find_encoder_by_name(d.lavcName) != nullptr;
    info.bitrates = d.bitrates;
    info.defaultBitrate = d.defaultBitrate;
    return info;
}

}  // namespace

const std::vector<CodecInfo>& codecRegistry() {
    static const std::vector<CodecInfo> reg = {makeInfo(CodecId::Ac3), makeInfo(CodecId::Dts)};
    return reg;
}

const CodecInfo* findCodec(CodecId id) {
    for (const auto& c : codecRegistry())
        if (c.id == id) return &c;
    return nullptr;
}

const char* codecShortName(CodecId id) { return id == CodecId::Ac3 ? "ac3" : "dts"; }

bool parseCodec(std::string_view s, CodecId& out) {
    if (s == "ac3" || s == "dd" || s == "dolby") { out = CodecId::Ac3; return true; }
    if (s == "dts" || s == "dca") { out = CodecId::Dts; return true; }
    return false;
}

std::unique_ptr<ISurroundEncoder> createEncoder(CodecId id) {
    return std::make_unique<LavcEncoder>(lavcDesc(id));
}

std::unique_ptr<IBitstreamPacketizer> createPacketizer(CodecId) {
    return std::make_unique<Iec61937Packetizer>();
}
}  // namespace bs
