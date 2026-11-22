// Codec, packetizer, channel-mapping and sample-conversion tests.
#include <cmath>

#include "TestFramework.h"
#include "core/codec/Codec.h"
#include "core/dsp/DriftResampler.h"
#include "core/transport/Iec61937.h"
#include "core/wasapi/Capture.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

using namespace bs;

namespace {

const AudioFormat kCanonical{48000, 6, spk::Surround51Side, SampleFormat::Float32, 32};

// Decodes a sequence of encoded frames and returns the RMS level per speaker
// position (indexed FL FR FC LFE SL SR), skipping the first frames.
struct Decoder {
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    double energy[6] = {};
    uint64_t samples = 0;
    int decodedFrames = 0;
    int channels = 0;

    explicit Decoder(const char* name) {
        const AVCodec* c = avcodec_find_decoder_by_name(name);
        if (!c) return;
        ctx = avcodec_alloc_context3(c);
        if (avcodec_open2(ctx, c, nullptr) < 0) avcodec_free_context(&ctx);
    }
    ~Decoder() {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
    }
    bool feed(std::span<const uint8_t> data, int skipFrames) {
        if (!ctx) return false;
        if (av_new_packet(pkt, static_cast<int>(data.size())) < 0) return false;
        std::memcpy(pkt->data, data.data(), data.size());
        const int rc = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (rc < 0) return false;
        while (avcodec_receive_frame(ctx, frame) == 0) {
            ++decodedFrames;
            channels = frame->ch_layout.nb_channels;
            if (decodedFrames > skipFrames) {
                static const AVChannel order[6] = {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
                                                   AV_CHAN_LOW_FREQUENCY, AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT};
                static const AVChannel alt[6] = {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
                                                 AV_CHAN_LOW_FREQUENCY, AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT};
                for (int s = 0; s < 6; ++s) {
                    int idx = av_channel_layout_index_from_channel(&frame->ch_layout, order[s]);
                    if (idx < 0) idx = av_channel_layout_index_from_channel(&frame->ch_layout, alt[s]);
                    if (idx < 0) continue;
                    for (int i = 0; i < frame->nb_samples; ++i) {
                        double v = 0;
                        switch (frame->format) {
                            case AV_SAMPLE_FMT_FLTP: v = reinterpret_cast<const float*>(frame->extended_data[idx])[i]; break;
                            case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<const float*>(frame->extended_data[0])[i * channels + idx]; break;
                            case AV_SAMPLE_FMT_S32P: v = reinterpret_cast<const int32_t*>(frame->extended_data[idx])[i] / 2147483648.0; break;
                            case AV_SAMPLE_FMT_S32: v = reinterpret_cast<const int32_t*>(frame->extended_data[0])[i * channels + idx] / 2147483648.0; break;
                            case AV_SAMPLE_FMT_S16P: v = reinterpret_cast<const int16_t*>(frame->extended_data[idx])[i] / 32768.0; break;
                            default: break;
                        }
                        energy[s] += v * v;
                    }
                }
                samples += static_cast<uint64_t>(frame->nb_samples);
            }
            av_frame_unref(frame);
        }
        return true;
    }
    double rms(int s) const { return samples ? std::sqrt(energy[s] / static_cast<double>(samples)) : 0.0; }
};

void fillTone(float* const* planes, const EncoderCapabilities& caps, int activeChannel, uint64_t& pos) {
    for (uint32_t i = 0; i < caps.frameSamples; ++i, ++pos) {
        for (uint16_t c = 0; c < caps.channels; ++c) planes[c][i] = 0.0f;
        const double freq = activeChannel == 3 ? 60.0 : 1000.0;
        planes[activeChannel][i] = 0.5f * static_cast<float>(std::sin(6.283185307179586 * freq * static_cast<double>(pos) / 48000.0));
    }
}

// Encodes a tone on one channel at a time and verifies, by decoding the
// bitstream again, that the energy arrives on exactly that speaker.
void roundTripChannels(CodecId id, const char* decoderName, uint32_t bitrate, bool strictLfeLevel = true) {
    for (int ch = 0; ch < 6; ++ch) {
        auto enc = createEncoder(id);
        std::string err;
        REQUIRE(enc->initialize(kCanonical, {bitrate}, err));
        const EncoderCapabilities caps = enc->capabilities();
        Decoder dec(decoderName);
        REQUIRE(dec.ctx != nullptr);
        uint64_t pos = 0;
        const int frames = static_cast<int>(48000 / caps.frameSamples) + 4;  // ~1 s
        for (int f = 0; f < frames; ++f) {
            float* const* planes = enc->inputPlanes();
            fillTone(planes, caps, ch, pos);
            const EncodeResult r = enc->encode({planes, caps.channels, caps.frameSamples});
            REQUIRE(r.status == EncodeResult::Status::Ok);
            CHECK(r.data.size() <= caps.maxFrameBytes);
            REQUIRE(dec.feed(r.data, 3));
        }
        CHECK(dec.channels == 6);
        const double active = dec.rms(ch);
        if (ch == 3 && !strictLfeLevel) {
            std::printf("    note: LFE round-trip level %.4f (input 0.354)\n", active);
            CHECK_MSG(active > 0.05, "LFE rms %.4f", active);
        } else {
            CHECK_MSG(active > 0.25 && active < 0.45, "channel %d rms %.4f (expected ~0.354)", ch, active);
        }
        for (int other = 0; other < 6; ++other) {
            if (other == ch) continue;
            CHECK_MSG(dec.rms(other) < 0.01, "tone on channel %d leaked into channel %d (rms %.4f)", ch, other, dec.rms(other));
        }
    }
}

}  // namespace

BS_TEST(ac3_frame_format) {
    for (uint32_t bitrate : {384000u, 448000u, 640000u}) {
        auto enc = createEncoder(CodecId::Ac3);
        std::string err;
        REQUIRE(enc->initialize(kCanonical, {bitrate}, err));
        const EncoderCapabilities caps = enc->capabilities();
        CHECK(caps.frameSamples == 1536);
        CHECK(caps.channels == 6);
        CHECK(caps.algorithmicDelaySamples == 256);
        uint64_t pos = 0;
        for (int f = 0; f < 5; ++f) {
            float* const* planes = enc->inputPlanes();
            fillTone(planes, caps, 0, pos);
            const EncodeResult r = enc->encode({planes, caps.channels, caps.frameSamples});
            REQUIRE(r.status == EncodeResult::Status::Ok);
            CHECK(r.data.size() == bitrate / 8 * 1536 / 48000);  // CBR: exact frame size
            CHECK(r.data[0] == 0x0B && r.data[1] == 0x77);
            const int acmod = r.data[6] >> 5;
            CHECK(acmod == 7);  // 3/2 channel mode
        }
    }
}

BS_TEST(ac3_rejects_unsupported_input) {
    auto enc = createEncoder(CodecId::Ac3);
    std::string err;
    AudioFormat stereo{48000, 2, spk::Stereo, SampleFormat::Float32, 32};
    CHECK(!enc->initialize(stereo, {}, err));
    CHECK(!err.empty());
    CHECK(!enc->initialize(kCanonical, {123456}, err));
}

BS_TEST(ac3_channel_mapping_roundtrip) { roundTripChannels(CodecId::Ac3, "ac3", 640000); }

BS_TEST(ac3_external_buffer_encode) {
    // Encoding from a caller-owned buffer must give the same bitstream as
    // encoding from the encoder's own staging buffer.
    auto a = createEncoder(CodecId::Ac3);
    auto b = createEncoder(CodecId::Ac3);
    std::string err;
    REQUIRE(a->initialize(kCanonical, {448000}, err));
    REQUIRE(b->initialize(kCanonical, {448000}, err));
    const EncoderCapabilities caps = a->capabilities();
    std::vector<std::vector<float>> ext(6, std::vector<float>(caps.frameSamples));
    uint64_t posA = 0, posB = 0;
    for (int f = 0; f < 4; ++f) {
        fillTone(a->inputPlanes(), caps, 2, posA);
        float* extPlanes[6];
        for (int c = 0; c < 6; ++c) extPlanes[c] = ext[static_cast<size_t>(c)].data();
        fillTone(extPlanes, caps, 2, posB);
        const EncodeResult ra = a->encode({a->inputPlanes(), 6, caps.frameSamples});
        const EncodeResult rb = b->encode({extPlanes, 6, caps.frameSamples});
        REQUIRE(ra.status == EncodeResult::Status::Ok && rb.status == EncodeResult::Status::Ok);
        REQUIRE(ra.data.size() == rb.data.size());
        CHECK(std::memcmp(ra.data.data(), rb.data.data(), ra.data.size()) == 0);
    }
}

BS_TEST(iec61937_ac3_burst) {
    auto enc = createEncoder(CodecId::Ac3);
    auto pk = createPacketizer(CodecId::Ac3);
    std::string err;
    REQUIRE(enc->initialize(kCanonical, {640000}, err));
    REQUIRE(pk->initialize(enc->capabilities(), err));
    const TransportDescriptor d = pk->descriptor();
    CHECK(d.burstFrames == 1536);
    CHECK(d.transportChannels == 2);
    CHECK(d.transportSampleRate == 48000);
    CHECK(d.burstBytes() == 6144);
    CHECK(d.waveSubFormat.d1 == 0x00000092);
    CHECK(d.legacyFormatTag == 0x0092);

    const EncoderCapabilities caps = enc->capabilities();
    uint64_t pos = 0;
    fillTone(enc->inputPlanes(), caps, 1, pos);
    const EncodeResult r = enc->encode({enc->inputPlanes(), 6, caps.frameSamples});
    REQUIRE(r.status == EncodeResult::Status::Ok);
    std::vector<uint8_t> burst(d.burstBytes(), 0xAA);
    REQUIRE(pk->packetize(r.data, burst));

    // Preamble Pa Pb Pc Pd as little-endian 16-bit words.
    CHECK(burst[0] == 0x72 && burst[1] == 0xF8);
    CHECK(burst[2] == 0x1F && burst[3] == 0x4E);
    CHECK((burst[4] & 0x7F) == 0x01);                 // data type: AC-3
    CHECK(burst[5] == (r.data[5] & 0x07));            // bsmod in bits 8..10
    const uint32_t lengthBits = burst[6] | (burst[7] << 8);
    CHECK(lengthBits == r.data.size() * 8);
    // Payload: byte-swapped 16-bit words, so the AC-3 syncword reads 0x0B77.
    CHECK(burst[8] == 0x77 && burst[9] == 0x0B);
    bool payloadOk = true;
    for (size_t i = 0; i + 1 < r.data.size(); i += 2)
        payloadOk = payloadOk && burst[8 + i] == r.data[i + 1] && burst[8 + i + 1] == r.data[i];
    CHECK(payloadOk);
    bool padOk = true;
    for (size_t i = 8 + r.data.size(); i < burst.size(); ++i) padOk = padOk && burst[i] == 0;
    CHECK(padOk);

    // Malformed input is rejected rather than transmitted.
    std::vector<uint8_t> bad(r.data.begin(), r.data.end());
    bad[0] = 0;
    CHECK(!pk->packetize(bad, burst));
    std::vector<uint8_t> tooBig(d.burstBytes(), 0);
    tooBig[0] = 0x0B; tooBig[1] = 0x77;
    CHECK(!pk->packetize(tooBig, burst));
    std::vector<uint8_t> wrongSize(100);
    CHECK(!pk->packetize(r.data, wrongSize));
}

BS_TEST(dts_roundtrip) {
    const CodecInfo* info = findCodec(CodecId::Dts);
    REQUIRE(info != nullptr);
    if (!info->available) {
        std::printf("    (skipped: libavcodec has no DCA encoder)\n");
        return;
    }
    auto enc = createEncoder(CodecId::Dts);
    auto pk = createPacketizer(CodecId::Dts);
    std::string err;
    REQUIRE(enc->initialize(kCanonical, {1411200}, err));
    const EncoderCapabilities caps = enc->capabilities();
    CHECK(caps.frameSamples == 512);
    REQUIRE(pk->initialize(caps, err));
    const TransportDescriptor d = pk->descriptor();
    CHECK(d.burstBytes() == 2048);
    uint64_t pos = 0;
    fillTone(enc->inputPlanes(), caps, 0, pos);
    const EncodeResult r = enc->encode({enc->inputPlanes(), 6, caps.frameSamples});
    REQUIRE(r.status == EncodeResult::Status::Ok);
    CHECK(r.data.size() + 8 <= d.burstBytes());
    std::vector<uint8_t> burst(d.burstBytes());
    REQUIRE(pk->packetize(r.data, burst));
    CHECK((burst[4] & 0x7F) == 0x0B);  // DTS type I (512 samples)
    CHECK(burst[8] == 0xFE && burst[9] == 0x7F && burst[10] == 0x01 && burst[11] == 0x80);
    // FFmpeg's experimental DCA encoder reproduces the LFE channel at a lower
    // level than the input when decoded with FFmpeg's own decoder; routing is
    // verified strictly, the LFE level only loosely.
    roundTripChannels(CodecId::Dts, "dca", 1411200, false);
}

BS_TEST(codec_registry) {
    const CodecInfo* ac3 = findCodec(CodecId::Ac3);
    REQUIRE(ac3 != nullptr);
    CHECK(ac3->available);
    CHECK(ac3->defaultBitrate == 640000);
    CodecId id;
    CHECK(parseCodec("ac3", id) && id == CodecId::Ac3);
    CHECK(parseCodec("dts", id) && id == CodecId::Dts);
    CHECK(!parseCodec("mp3", id));
}

BS_TEST(channel_map_layouts) {
    int map[6];
    std::string err;
    // Both 5.1 layouts Windows offers are six discrete channels in the same order.
    for (uint32_t mask : {spk::Surround51Back, spk::Surround51Side, 0u}) {
        AudioFormat f{48000, 6, mask, SampleFormat::Float32, 32};
        REQUIRE(buildChannelMap(f, map, err));
        for (int i = 0; i < 6; ++i) CHECK(map[i] == i);
    }
    // Anything else is refused; nothing is ever mixed.
    AudioFormat stereo{48000, 2, spk::Stereo, SampleFormat::Float32, 32};
    CHECK(!buildChannelMap(stereo, map, err));
    CHECK(!err.empty());
    AudioFormat s71{48000, 8, spk::Surround71, SampleFormat::Float32, 32};
    CHECK(!buildChannelMap(s71, map, err));
}

BS_TEST(pcm_sample_preservation) {
    const int map[6] = {0, 1, 2, 3, 4, 5};
    const uint32_t frames = 64;
    std::vector<float> out(frames * 6);

    // float32 passes through bit-exactly.
    AudioFormat f32{48000, 6, spk::Surround51Back, SampleFormat::Float32, 32};
    std::vector<float> src(frames * 6);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.37)) * 0.9f;
    convertToCanonical(reinterpret_cast<const uint8_t*>(src.data()), f32, map, frames, out.data());
    CHECK(std::memcmp(src.data(), out.data(), src.size() * sizeof(float)) == 0);

    // Integer formats scale exactly and keep every channel in place.
    AudioFormat i16{48000, 6, spk::Surround51Back, SampleFormat::Int16, 16};
    std::vector<int16_t> s16(frames * 6);
    for (size_t i = 0; i < s16.size(); ++i) s16[i] = static_cast<int16_t>((static_cast<int>(i) * 977) % 65536 - 32768);
    convertToCanonical(reinterpret_cast<const uint8_t*>(s16.data()), i16, map, frames, out.data());
    bool ok = true;
    for (size_t i = 0; i < s16.size(); ++i) ok = ok && out[i] == static_cast<float>(s16[i]) / 32768.0f;
    CHECK(ok);

    AudioFormat i24{48000, 6, spk::Surround51Back, SampleFormat::Int24Packed, 24};
    std::vector<uint8_t> s24(frames * 6 * 3);
    std::vector<int32_t> ref24(frames * 6);
    for (size_t i = 0; i < ref24.size(); ++i) {
        const int32_t v = static_cast<int32_t>((static_cast<int64_t>(i) * 104729) % 16777216) - 8388608;
        ref24[i] = v;
        s24[i * 3] = static_cast<uint8_t>(v);
        s24[i * 3 + 1] = static_cast<uint8_t>(v >> 8);
        s24[i * 3 + 2] = static_cast<uint8_t>(v >> 16);
    }
    convertToCanonical(s24.data(), i24, map, frames, out.data());
    ok = true;
    for (size_t i = 0; i < ref24.size(); ++i) ok = ok && out[i] == static_cast<float>(ref24[i]) / 8388608.0f;
    CHECK(ok);

    // Channel routing: side-surround source with a permuted map.
    const int swapped[6] = {1, 0, 2, 3, 5, 4};
    convertToCanonical(reinterpret_cast<const uint8_t*>(src.data()), f32, swapped, frames, out.data());
    ok = true;
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < 6; ++c) ok = ok && out[i * 6 + static_cast<uint32_t>(c)] == src[i * 6 + static_cast<uint32_t>(swapped[c])];
    CHECK(ok);
}

BS_TEST(resampler_unity_is_pure_delay) {
    DriftResampler rs;
    rs.configure(2);
    std::vector<float> in(1000 * 2);
    for (size_t i = 0; i < 1000; ++i) {
        in[i * 2] = static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
        in[i * 2 + 1] = static_cast<float>(i % 17) / 17.0f;
    }
    std::vector<float> l(1100), r(1100);
    float* planes[2] = {l.data(), r.data()};
    rs.push(in.data(), 1000);
    const size_t got = rs.pull(planes, 0, 1100);
    // The newest half kernel of input is still needed as look-ahead.
    CHECK(got == 1000 - static_cast<size_t>(DriftResampler::latencyFrames()));
    bool exact = true;
    for (size_t i = 0; i < got; ++i) exact = exact && l[i] == in[i * 2] && r[i] == in[i * 2 + 1];
    CHECK(exact);
}

BS_TEST(resampler_ratio_and_quality) {
    for (double step : {1.002, 0.998}) {
        DriftResampler rs;
        rs.configure(1);
        rs.setStep(step);
        const double freq = 1000.0;
        std::vector<float> out;
        std::vector<float> block(480), dst(1024);
        float* planes[1] = {dst.data()};
        uint64_t n = 0;
        for (int b = 0; b < 200; ++b) {
            for (auto& v : block) v = 0.5f * static_cast<float>(std::sin(6.283185307179586 * freq * static_cast<double>(n++) / 48000.0));
            rs.push(block.data(), block.size());
            size_t got;
            while ((got = rs.pull(planes, 0, dst.size())) > 0) out.insert(out.end(), dst.begin(), dst.begin() + static_cast<ptrdiff_t>(got));
        }
        const double expected = 200.0 * 480.0 / step;
        CHECK_MSG(std::fabs(static_cast<double>(out.size()) - expected) < 40, "produced %zu, expected %.0f", out.size(), expected);
        // The output must be a clean sine at freq*step: fit and measure the residual.
        const double w = 6.283185307179586 * freq * step / 48000.0;
        double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
        const size_t start = 2000, len = 40000;
        for (size_t i = start; i < start + len; ++i) {
            const double s = std::sin(w * static_cast<double>(i)), c = std::cos(w * static_cast<double>(i));
            ss += s * s; sc += s * c; cc += c * c; ys += out[i] * s; yc += out[i] * c;
        }
        const double det = ss * cc - sc * sc;
        const double a = (ys * cc - yc * sc) / det, bq = (yc * ss - ys * sc) / det;
        double err = 0, sig = 0;
        for (size_t i = start; i < start + len; ++i) {
            const double fit = a * std::sin(w * static_cast<double>(i)) + bq * std::cos(w * static_cast<double>(i));
            err += (out[i] - fit) * (out[i] - fit);
            sig += fit * fit;
        }
        const double snr = 10.0 * std::log10(sig / (err + 1e-30));
        CHECK_MSG(snr > 70.0, "resampler SNR %.1f dB at step %.3f", snr, step);
        CHECK(std::fabs(std::sqrt(a * a + bq * bq) - 0.5) < 0.005);
    }
}


// ---------------------------------------------------------------- upmixing

#include "core/dsp/Upmixer.h"

namespace {
// Runs three seconds of a signal through the upmixer and returns the RMS of
// each output channel over the last half second (after detection and fades).
// `amp` is the amplitude of the `probeHz` tone in each channel over that time.
struct UpmixResult { double rms[6]; double amp[6]; };
template <typename Fill>
UpmixResult runUpmix(UpmixMode mode, Fill fill, double probeHz = 0.0) {
    Upmixer up(48000);
    up.setMode(mode);
    std::vector<float> block(480 * 6);
    double energy[6] = {}, inPhase[6] = {}, quadrature[6] = {};
    uint64_t counted = 0, n = 0;
    for (int b = 0; b < 300; ++b) {
        for (int i = 0; i < 480; ++i, ++n) {
            float* f = &block[static_cast<size_t>(i) * 6];
            for (int c = 0; c < 6; ++c) f[c] = 0.0f;
            fill(f, n);
        }
        up.process(block.data(), 480);
        if (b >= 250) {
            for (int i = 0; i < 480; ++i) {
                const double phase = 6.283185307179586 * probeHz * static_cast<double>(n - 480 + static_cast<uint64_t>(i)) / 48000.0;
                for (int c = 0; c < 6; ++c) {
                    const double v = block[static_cast<size_t>(i) * 6 + c];
                    energy[c] += v * v;
                    inPhase[c] += v * std::sin(phase);
                    quadrature[c] += v * std::cos(phase);
                }
            }
            counted += 480;
        }
    }
    UpmixResult r;
    for (int c = 0; c < 6; ++c) {
        r.rms[c] = std::sqrt(energy[c] / static_cast<double>(counted));
        r.amp[c] = 2.0 * std::sqrt(inPhase[c] * inPhase[c] + quadrature[c] * quadrature[c]) / static_cast<double>(counted);
    }
    return r;
}
float tone(uint64_t n, double hz) { return 0.5f * static_cast<float>(std::sin(6.283185307179586 * hz * static_cast<double>(n) / 48000.0)); }
}  // namespace

BS_TEST(upmix_off_leaves_stereo_alone) {
    const UpmixResult r = runUpmix(UpmixMode::Off, [](float* f, uint64_t n) { f[0] = tone(n, 440); f[1] = tone(n, 660); });
    CHECK(std::fabs(r.rms[0] - 0.3536) < 0.005 && std::fabs(r.rms[1] - 0.3536) < 0.005);
    for (int c = 2; c < 6; ++c) CHECK(r.rms[c] == 0.0);
}

BS_TEST(upmix_quad_mirrors_fronts_to_surrounds) {
    const UpmixResult r = runUpmix(UpmixMode::Quad, [](float* f, uint64_t n) { f[0] = tone(n, 440); f[1] = tone(n, 660); });
    CHECK(std::fabs(r.rms[0] - 0.3536) < 0.005 && std::fabs(r.rms[1] - 0.3536) < 0.005);  // fronts unchanged
    CHECK_MSG(std::fabs(r.rms[4] - r.rms[0]) < 0.005 && std::fabs(r.rms[5] - r.rms[1]) < 0.005,
              "surround rms %.4f / %.4f", r.rms[4], r.rms[5]);
    CHECK(r.rms[2] == 0.0 && r.rms[3] == 0.0);  // no centre, no LFE
}

BS_TEST(upmix_virtual_surround_steers_centre_and_ambience) {
    // Identical on both channels (like centred speech): it must move to the centre.
    const UpmixResult mono = runUpmix(UpmixMode::VirtualSurround, [](float* f, uint64_t n) { f[0] = f[1] = tone(n, 1000); });
    CHECK_MSG(mono.rms[2] > 0.3, "centre rms %.4f", mono.rms[2]);
    CHECK_MSG(mono.rms[0] < 0.12 && mono.rms[1] < 0.12, "front rms %.4f / %.4f", mono.rms[0], mono.rms[1]);
    CHECK_MSG(mono.rms[4] < 0.01 && mono.rms[5] < 0.01, "surround rms %.4f / %.4f", mono.rms[4], mono.rms[5]);

    // Opposite on the two channels (how matrix surround is encoded): most of
    // it must move from the fronts to both surrounds, none to the centre.
    const UpmixResult wide = runUpmix(UpmixMode::VirtualSurround, [](float* f, uint64_t n) { f[0] = tone(n, 1000); f[1] = -tone(n, 1000); });
    CHECK_MSG(wide.rms[4] > 0.2 && wide.rms[5] > 0.2, "surround rms %.4f / %.4f", wide.rms[4], wide.rms[5]);
    CHECK_MSG(wide.rms[2] < 0.01, "centre rms %.4f", wide.rms[2]);
    CHECK_MSG(wide.rms[0] < 0.15 && wide.rms[1] < 0.15, "front rms %.4f / %.4f", wide.rms[0], wide.rms[1]);

    // Low frequencies reach the subwoofer channel.
    const UpmixResult bass = runUpmix(UpmixMode::VirtualSurround, [](float* f, uint64_t n) { f[0] = f[1] = tone(n, 50); });
    CHECK_MSG(bass.rms[3] > 0.05, "LFE rms %.4f", bass.rms[3]);
    const UpmixResult treble = runUpmix(UpmixMode::VirtualSurround, [](float* f, uint64_t n) { f[0] = f[1] = tone(n, 2000); });
    CHECK_MSG(treble.rms[3] < 0.005, "LFE rms for 2 kHz %.4f", treble.rms[3]);
}

BS_TEST(upmix_virtual_surround_steers_each_band_on_its_own) {
    // A centred voice (1.5 kHz, same on both channels) over wide music in
    // other bands (400 Hz and 4 kHz, opposite on the two channels). A
    // single-band upmixer sees a half-correlated mix and compromises; here
    // the voice band must still go to the centre in full.
    const UpmixResult r = runUpmix(UpmixMode::VirtualSurround, [](float* f, uint64_t n) {
        const float voice = tone(n, 1500), wide = 0.5f * (tone(n, 400) + tone(n, 4000));
        f[0] = voice + wide;
        f[1] = voice - wide;
    });
    CHECK_MSG(r.rms[2] > 0.27, "centre rms %.4f", r.rms[2]);  // a single-band upmixer reaches about 0.13 here
    CHECK_MSG(r.rms[4] > 0.15 && r.rms[5] > 0.15, "surround rms %.4f / %.4f", r.rms[4], r.rms[5]);
}

BS_TEST(upmix_virtual_surround_places_sound_across_the_front) {
    // A sound on one side only stays exactly where it is.
    const UpmixResult left = runUpmix(UpmixMode::VirtualSurround, [](float* f, uint64_t n) { f[0] = tone(n, 1500); });
    CHECK_MSG(std::fabs(left.rms[0] - 0.3536) < 0.01, "left rms %.4f", left.rms[0]);
    for (int c : {1, 2, 4, 5}) CHECK_MSG(left.rms[c] < 0.01, "channel %d rms %.4f", c, left.rms[c]);

    // A sound halfway between left and centre plays on the left and centre
    // speakers and all but leaves the right one (it had 0.135 rms there).
    const UpmixResult half = runUpmix(UpmixMode::VirtualSurround, [](float* f, uint64_t n) {
        f[0] = 0.924f * tone(n, 1500);
        f[1] = 0.383f * tone(n, 1500);
    });
    CHECK_MSG(half.rms[1] < 0.05, "right rms %.4f", half.rms[1]);
    CHECK_MSG(half.rms[2] > 0.15, "centre rms %.4f", half.rms[2]);
    CHECK_MSG(half.rms[0] > half.rms[2], "left rms %.4f, centre rms %.4f", half.rms[0], half.rms[2]);
    CHECK_MSG(half.rms[4] < 0.01 && half.rms[5] < 0.01, "surround rms %.4f / %.4f", half.rms[4], half.rms[5]);
}

BS_TEST(upmix_virtual_surround_keeps_left_and_right_ambience_apart) {
    // Unrelated sound on the two channels (1 kHz left, 1.3 kHz right) is
    // ambience: each surround must carry its own side and not the other.
    auto fill = [](float* f, uint64_t n) { f[0] = tone(n, 1000); f[1] = tone(n, 1300); };
    const UpmixResult atLeftTone = runUpmix(UpmixMode::VirtualSurround, fill, 1000.0);
    const UpmixResult atRightTone = runUpmix(UpmixMode::VirtualSurround, fill, 1300.0);
    CHECK_MSG(atLeftTone.amp[4] > 0.15 && atLeftTone.amp[5] < 0.05, "1 kHz: LS %.4f, RS %.4f", atLeftTone.amp[4], atLeftTone.amp[5]);
    CHECK_MSG(atRightTone.amp[5] > 0.15 && atRightTone.amp[4] < 0.05, "1.3 kHz: LS %.4f, RS %.4f", atRightTone.amp[4], atRightTone.amp[5]);
    CHECK_MSG(atLeftTone.amp[2] < 0.05 && atRightTone.amp[2] < 0.05, "centre %.4f / %.4f", atLeftTone.amp[2], atRightTone.amp[2]);
    CHECK_MSG(std::fabs(atLeftTone.amp[0] - 0.5) < 0.05, "front left %.4f", atLeftTone.amp[0]);  // fronts keep it too
}

BS_TEST(upmix_virtual_surround_bands_recombine_flat) {
    // A sound on one side is not steered, so that front carries the plain sum
    // of all bands: it must come out at the input level at every frequency,
    // including right on the crossover points.
    for (double hz : {60.0, 200.0, 320.0, 500.0, 800.0, 1200.0, 1800.0, 2800.0, 4000.0, 6500.0, 12000.0}) {
        const UpmixResult r = runUpmix(UpmixMode::VirtualSurround, [hz](float* f, uint64_t n) { f[0] = tone(n, hz); }, hz);
        CHECK_MSG(std::fabs(r.amp[0] - 0.5) < 0.005, "front amplitude %.4f at %.0f Hz", r.amp[0], hz);
    }
}

BS_TEST(upmix_never_touches_real_surround_content) {
    // A source that itself uses the centre and surrounds is a real 5.1 mix.
    for (UpmixMode mode : {UpmixMode::Quad, UpmixMode::VirtualSurround}) {
        const UpmixResult r = runUpmix(mode, [](float* f, uint64_t n) {
            f[0] = f[1] = tone(n, 440);
            f[2] = tone(n, 300);
            f[4] = tone(n, 700) * 0.5f;
        });
        CHECK(std::fabs(r.rms[0] - 0.3536) < 0.001 && std::fabs(r.rms[1] - 0.3536) < 0.001);
        CHECK(std::fabs(r.rms[2] - 0.3536) < 0.001);
        CHECK(std::fabs(r.rms[4] - 0.1768) < 0.001);
        CHECK(r.rms[3] == 0.0 && r.rms[5] == 0.0);
    }
}
