// Scheduling, buffer-management, drift and recovery tests. The engine is
// driven with a simulated clock, so minutes of streaming run in well under a
// second and every scenario is exactly reproducible.
#include <cmath>

#include "TestFramework.h"
#include "core/engine/BitstreamEngine.h"
#include "core/pipeline/Pipeline.h"

using namespace bs;

namespace {

// Verifies the structural integrity of a rendered IEC 61937 stream: every
// burst is complete, bursts never overlap and only zeros lie between them.
struct StreamChecker {
    uint32_t burstFrames;
    uint64_t pos = 0;            // frames seen
    uint64_t bursts = 0;
    uint64_t gapFrames = 0;      // zero frames between bursts (must never occur: the cadence is kept)
    uint64_t errors = 0;
    uint32_t inBurst = 0;        // frames remaining of the current burst
    uint32_t payloadFrames = 0;
    bool started = false;

    void feed(const uint8_t* p, uint32_t frames) {
        for (uint32_t i = 0; i < frames; ++i, ++pos) {
            const uint8_t* f = p + static_cast<size_t>(i) * 4;
            const bool preamble = f[0] == 0x72 && f[1] == 0xF8 && f[2] == 0x1F && f[3] == 0x4E;
            if (inBurst == 0) {
                if (preamble) {
                    started = true;
                    ++bursts;
                    inBurst = burstFrames - 1;
                    payloadFrames = 0;
                    if (i + 1 < frames) {
                        // Pc/Pd are in the next frame: burst length in bits.
                        const uint32_t bits = f[6] | (static_cast<uint32_t>(f[7]) << 8);
                        payloadFrames = (bits / 8 + 8 + 3) / 4;
                    }
                } else {
                    if (f[0] | f[1] | f[2] | f[3]) ++errors;  // garbage between bursts
                    if (started) ++gapFrames;
                }
            } else {
                --inBurst;
            }
        }
    }
};

struct Sim {
    BitstreamEngine engine;
    StreamChecker checker{1536};
    std::vector<float> pcm;
    std::vector<uint8_t> out;
    uint32_t capFrames, outFrames;
    double capRate;              // capture clock rate relative to the output clock
    double tCap = 0, tOut = 0;   // next event times in output-clock seconds
    uint64_t capSamples = 0;
    double jitterMs = 0;         // deterministic scheduling jitter applied to capture reads
    uint32_t lcg = 12345;

    Sim(uint32_t captureFrames, uint32_t outputFrames, double driftPpm, EngineConfig cfg = {})
        : capFrames(captureFrames), outFrames(outputFrames), capRate(1.0 + driftPpm * 1e-6) {
        std::string err;
        cfg.capturePeriodFrames = captureFrames;
        cfg.outputPeriodFrames = outputFrames;
        if (!engine.initialize(cfg, err)) std::printf("    engine init failed: %s\n", err.c_str());
        checker.burstFrames = engine.transport().burstFrames;
        pcm.assign(static_cast<size_t>(captureFrames) * 6, 0.0f);
        out.assign(static_cast<size_t>(outputFrames) * 4, 0);
        tCap = static_cast<double>(captureFrames) / (48000.0 * capRate);
        tOut = 0.001;
    }

    double nextJitter() {
        lcg = lcg * 1664525u + 1013904223u;
        return jitterMs * 1e-3 * static_cast<double>(lcg >> 8) / 16777216.0;
    }

    void capture(double tEvent, uint32_t packets = 1) {
        for (uint32_t k = 0; k < packets; ++k) {
            for (uint32_t i = 0; i < capFrames; ++i) {
                const float v = 0.2f * static_cast<float>(std::sin(static_cast<double>(capSamples + i) * 0.02));
                for (int c = 0; c < 6; ++c) pcm[static_cast<size_t>(i) * 6 + static_cast<size_t>(c)] = v;
            }
            // Capture timestamps are in the capture clock mapped to output time.
            const double tFirst = static_cast<double>(capSamples) / (48000.0 * capRate);
            engine.pushCapture(pcm.data(), capFrames, static_cast<int64_t>(tFirst * 1e9), static_cast<int64_t>(tEvent * 1e9), false);
            capSamples += capFrames;
        }
    }

    void render(double tEvent) {
        engine.render(out.data(), outFrames, static_cast<int64_t>(tEvent * 1e9), static_cast<int64_t>(outFrames));
        checker.feed(out.data(), outFrames);
    }

    // Runs until output time `until` (seconds). captureEnabled=false simulates a stalled source.
    void run(double until, bool captureEnabled = true) {
        while (std::min(tCap, tOut) < until) {
            if (tOut <= tCap) {
                render(tOut);
                tOut += static_cast<double>(outFrames) / 48000.0;
            } else {
                if (captureEnabled) capture(tCap + nextJitter());
                else capSamples += capFrames;  // the samples are lost
                tCap += static_cast<double>(capFrames) / (48000.0 * capRate);
            }
        }
    }
};

}  // namespace

BS_TEST(engine_steady_state_low_latency) {
    // 10 ms capture packets, 3 ms output period, no drift.
    Sim sim(480, 144, 0.0);
    sim.run(10.0);
    sim.engine.requestStatsReset();
    sim.run(40.0);
    const EngineStats s = sim.engine.stats();
    CHECK(s.underruns == 0);
    CHECK(s.overrunDrops == 0);
    CHECK(s.encodeErrors == 0);
    CHECK(sim.checker.errors == 0);
    CHECK(s.framesEncoded > 900);
    // The queue must converge to the margin, not sit at the alignment padding
    // (up to 15 ms). The slack includes the real encoding time, which varies
    // with CPU load, so the tolerance is 2 ms rather than 1.
    CHECK(s.minHeadroomValid);
    CHECK_MSG(std::abs(s.minHeadroomFrames - static_cast<int32_t>(s.marginFrames)) <= 96,
              "min headroom %d frames, margin %u", s.minHeadroomFrames, s.marginFrames);
    // Latency: one frame of accumulation plus only a few ms of queueing.
    const double submitMs = s.submitLatencyUs.mean / 1000.0;
    CHECK_MSG(submitMs > 32.0 && submitMs < 46.0, "capture->submit %.2f ms", submitMs);
    CHECK_MSG(s.queueUs.mean / 1000.0 < 12.0, "queue residence %.2f ms", s.queueUs.mean / 1000.0);
    CHECK(s.totalUs.count > 0);
}

BS_TEST(engine_small_capture_period_lowers_latency) {
    // Whole-pipeline effect: finer capture granularity lets frames complete
    // on time, so less safety queueing is needed.
    Sim coarse(480, 144, 0.0);
    Sim fine(96, 144, 0.0);
    for (Sim* s : {&coarse, &fine}) {
        s->run(12.0);
        s->engine.requestStatsReset();
        s->run(30.0);
    }
    const EngineStats a = coarse.engine.stats(), b = fine.engine.stats();
    CHECK(a.underruns == 0 && b.underruns == 0);
    CHECK_MSG(b.submitLatencyUs.mean < a.submitLatencyUs.mean - 2000.0, "coarse %.2f ms, fine %.2f ms",
              a.submitLatencyUs.mean / 1000.0, b.submitLatencyUs.mean / 1000.0);
}

BS_TEST(engine_clock_drift_is_absorbed) {
    for (double ppm : {+300.0, -300.0}) {
        Sim sim(480, 144, ppm);
        sim.run(30.0);
        sim.engine.requestStatsReset();
        const uint64_t gapsBefore = sim.checker.gapFrames;
        sim.run(240.0);  // without correction 300 ppm would slip > 2 frames in this time
        const EngineStats s = sim.engine.stats();
        CHECK_MSG(s.underruns == 0, "drift %+.0f ppm: %llu underruns", ppm, static_cast<unsigned long long>(s.underruns));
        CHECK_MSG(s.overrunDrops == 0, "drift %+.0f ppm: %llu drops", ppm, static_cast<unsigned long long>(s.overrunDrops));
        CHECK(sim.checker.gapFrames == gapsBefore);  // bit stream stayed gapless
        CHECK(sim.checker.errors == 0);
        // The controller's correction must match the simulated drift.
        CHECK_MSG(std::fabs(s.resampleAdjustPpm - ppm) < 150.0, "drift %+.0f ppm, correction %+.1f ppm", ppm, s.resampleAdjustPpm);
        CHECK_MSG(std::fabs(s.driftEstimatePpm - ppm) < 80.0, "drift %+.0f ppm, estimate %+.1f ppm", ppm, s.driftEstimatePpm);
        // Queue stays bounded: latency does not creep.
        CHECK_MSG(s.queueLevelUs.max / 1000.0 < 33.0, "max queue level %.2f ms", s.queueLevelUs.max / 1000.0);
        CHECK_MSG(s.submitLatencyUs.max - s.submitLatencyUs.min < 15000.0, "latency spread %.2f ms",
                  (s.submitLatencyUs.max - s.submitLatencyUs.min) / 1000.0);
    }
}

BS_TEST(engine_recovers_from_capture_stall_without_stale_audio) {
    Sim sim(480, 144, 0.0);
    sim.run(15.0);
    const EngineStats before = sim.engine.stats();
    CHECK(before.underruns == 0);
    const double baseline = before.submitLatencyUs.mean;

    // The source stalls for 300 ms, then delivers a 300 ms backlog at once.
    sim.run(15.3, false);
    sim.capSamples -= static_cast<uint64_t>(0.3 * 48000);  // backlog was buffered, not lost
    sim.capture(15.3, static_cast<uint32_t>(0.3 * 48000 / 480));
    sim.run(16.0);
    const EngineStats during = sim.engine.stats();
    CHECK(during.underruns >= 1);
    CHECK_MSG(during.overrunDrops >= 5, "stale bursts dropped: %llu", static_cast<unsigned long long>(during.overrunDrops));
    CHECK(sim.checker.errors == 0);

    // After recovery the latency must be back at the baseline, not 300 ms higher.
    sim.run(30.0);
    sim.engine.requestStatsReset();
    sim.run(45.0);
    const EngineStats after = sim.engine.stats();
    CHECK(after.underruns == 0);
    CHECK_MSG(std::fabs(after.submitLatencyUs.mean - baseline) < 6000.0, "latency before %.2f ms, after %.2f ms",
              baseline / 1000.0, after.submitLatencyUs.mean / 1000.0);
    CHECK_MSG(after.submitLatencyUs.max / 1000.0 < 60.0, "max latency after recovery %.2f ms", after.submitLatencyUs.max / 1000.0);
}

BS_TEST(engine_adaptive_margin_reacts_to_jitter) {
    // Heavy scheduling jitter with a tiny margin: underruns must make the
    // margin grow until the stream is stable.
    EngineConfig cfg;
    cfg.marginFrames = 24;  // 0.5 ms
    cfg.adaptiveMargin = true;
    Sim sim(480, 144, 0.0, cfg);
    sim.jitterMs = 6.0;
    sim.run(90.0);
    const EngineStats mid = sim.engine.stats();
    CHECK(mid.marginFrames >= cfg.marginFrames);
    sim.engine.requestStatsReset();
    sim.run(150.0);
    const EngineStats s = sim.engine.stats();
    CHECK_MSG(s.underruns == 0, "underruns after adaptation: %llu (margin %u frames)",
              static_cast<unsigned long long>(s.underruns), s.marginFrames);
    CHECK(sim.checker.errors == 0);
}

BS_TEST(engine_burst_stream_integrity) {
    // Odd period combinations must never split or corrupt a burst.
    for (auto [cap, outp] : {std::pair<uint32_t, uint32_t>{441, 133}, {128, 480}, {1024, 96}, {480, 1056}}) {
        Sim sim(cap, outp, +120.0);
        sim.run(20.0);
        CHECK(sim.checker.errors == 0);
        CHECK_MSG(sim.checker.bursts > 550, "cap %u out %u: bursts %llu", cap, outp,
                  static_cast<unsigned long long>(sim.checker.bursts));
        const EngineStats s = sim.engine.stats();
        CHECK(s.encodeErrors == 0);
        // The stream also contains silence bursts (before the first frame and
        // for late frames), and the statistics snapshot lags by up to 200 ms.
        const uint64_t silence = s.padFrames / sim.checker.burstFrames + 4;
        CHECK_MSG(s.burstsPlayed + 8 + silence >= sim.checker.bursts && s.burstsPlayed <= sim.checker.bursts,
                  "played %llu, seen in stream %llu", static_cast<unsigned long long>(s.burstsPlayed),
                  static_cast<unsigned long long>(sim.checker.bursts));
    }
}

BS_TEST(engine_discontinuity_resets_frame_alignment) {
    Sim sim(480, 144, 0.0);
    sim.run(5.0);
    sim.engine.pushCapture(sim.pcm.data(), 480, static_cast<int64_t>(sim.tCap * 1e9), static_cast<int64_t>(sim.tCap * 1e9), true);
    sim.run(10.0);
    const EngineStats s = sim.engine.stats();
    CHECK(s.discontinuities == 1);
    CHECK(sim.checker.errors == 0);
}

BS_TEST(histogram_and_seqlock) {
    Histogram h(10.0);
    for (int i = 1; i <= 1000; ++i) h.add(i);
    const StatSummary s = h.summary();
    CHECK(s.count == 1000);
    CHECK(std::fabs(s.mean - 500.5) < 1e-6);
    CHECK(s.min == 1 && s.max == 1000);
    CHECK(std::fabs(s.p50 - 500) < 15);
    CHECK(std::fabs(s.p99 - 990) < 15);
    SeqLock<StatSummary> lock;
    lock.write(s);
    CHECK(lock.read().count == 1000);
}

BS_TEST(pipeline_synthetic_end_to_end) {
    // Real threads, real timers, real encoder; no audio hardware required.
    PipelineConfig cfg;
    cfg.captureBackend = CaptureBackend::Synthetic;
    cfg.capturePeriodHns = 50'000;
    cfg.nullSink = true;
    cfg.outputPeriodHns = 30'000;
    cfg.autoReconnect = false;
    Pipeline p;
    p.start(cfg);
    std::string err;
    REQUIRE(p.waitUntilRunning(5000, err));
    Sleep(2500);
    const PipelineStatus st = p.status();
    p.stop();
    REQUIRE(st.sessionValid);
    CHECK(st.state == PipelineState::Running);
    CHECK_MSG(st.stats.framesEncoded > 50, "frames encoded: %llu", static_cast<unsigned long long>(st.stats.framesEncoded));
    CHECK(st.stats.burstsPlayed > 40);
    CHECK(st.stats.encodeErrors == 0);
    CHECK(st.encoder.frameSamples == 1536);
    const LatencyBreakdown b = computeLatency(st);
    CHECK(b.valid);
    CHECK_MSG(b.totalMs > 32.0 && b.totalMs < 120.0, "latency %.1f ms", b.totalMs);
    CHECK(p.status().state == PipelineState::Stopped);
}
BS_TEST(pipeline_reports_errors_and_retries) {
    // Unknown output device: must fail cleanly without auto-reconnect...
    PipelineConfig cfg;
    cfg.captureBackend = CaptureBackend::Synthetic;
    cfg.outputDeviceId = L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}";
    cfg.autoReconnect = false;
    {
        Pipeline p;
        p.start(cfg);
        std::string err;
        CHECK(!p.waitUntilRunning(5000, err));
        CHECK(!err.empty());
        p.stop();
        CHECK(p.status().state == PipelineState::Error);
    }
    // ...and keep retrying (reconnection path) with it.
    cfg.autoReconnect = true;
    {
        Pipeline p;
        p.start(cfg);
        Sleep(2600);
        const PipelineStatus st = p.status();
        CHECK(st.state == PipelineState::Recovering);
        CHECK(st.restarts >= 1);
        CHECK(!st.message.empty());
        p.stop();
        CHECK(p.status().state == PipelineState::Stopped);
    }
    // DTS runs through the same pipeline without any opt-in.
    PipelineConfig dts;
    dts.captureBackend = CaptureBackend::Synthetic;
    dts.nullSink = true;
    dts.codec = CodecId::Dts;
    dts.autoReconnect = false;
    dts.marginUs = 4000;
    if (findCodec(CodecId::Dts)->available) {
        Pipeline p;
        p.start(dts);
        std::string err;
        REQUIRE(p.waitUntilRunning(5000, err));
        Sleep(1500);
        const PipelineStatus st = p.status();
        p.stop();
        CHECK(st.encoder.frameSamples == 512);
        CHECK(st.stats.framesEncoded > 80);
        CHECK(st.stats.encodeErrors == 0);
    }
}
