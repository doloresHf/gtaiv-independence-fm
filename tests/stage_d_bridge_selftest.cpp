#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ipc_ring_protocol.h"
#include "stage_d_timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

bool g_hasFailures = false;

void CheckCond(bool cond, const char* expr, const char* detail, int line) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] line %d: %s (%s)\n", line, expr, detail ? detail : "");
        g_hasFailures = true;
    }
}

#define CHECK(cond, detail) CheckCond((cond), #cond, (detail), __LINE__)

constexpr uint64_t kRealOrigin100ns = 1930000000000ULL;

class CaptureMemorySink final : public staged::IRingFrameSink {
public:
    bool WriteCommittedFrames(uint64_t startFrame,
                              const int16_t* stereoLR,
                              uint32_t frameCount) override {
        if (startFrame != committedFrames_) {
            writeOrderViolated_ = true;
            return false;
        }
        if (frameCount == 0u) {
            return true;
        }
        if (!stereoLR) {
            return false;
        }
        const size_t oldSize = samples_.size();
        const size_t addCount = static_cast<size_t>(frameCount) * 2u;
        samples_.resize(oldSize + addCount);
        std::memcpy(samples_.data() + oldSize, stereoLR, addCount * sizeof(int16_t));
        committedFrames_ += static_cast<uint64_t>(frameCount);
        return true;
    }

    uint64_t CommittedFrames() const noexcept { return committedFrames_; }
    bool WriteOrderViolated() const noexcept { return writeOrderViolated_; }

    int16_t LeftAt(uint64_t frameIdx) const noexcept {
        return samples_[static_cast<size_t>(frameIdx) * 2u];
    }
    int16_t RightAt(uint64_t frameIdx) const noexcept {
        return samples_[static_cast<size_t>(frameIdx) * 2u + 1u];
    }
    const std::vector<int16_t>& RawSamples() const noexcept { return samples_; }

private:
    std::vector<int16_t> samples_;
    uint64_t committedFrames_ = 0ULL;
    bool writeOrderViolated_ = false;
};

class SimulatedIpcSharedSink final : public staged::IRingFrameSink {
public:
    explicit SimulatedIpcSharedSink(stagec::StageCIpcSharedBlock* block) noexcept
        : block_(block) {}

    bool WriteCommittedFrames(uint64_t startFrame,
                              const int16_t* stereoLR,
                              uint32_t frameCount) override {
        if (frameCount == 0u) {
            return true;
        }
        if (!block_ || !stereoLR) {
            return false;
        }

        constexpr uint32_t kMaxChunk = stagec::kRingCapacityFrames / 4u;
        uint64_t curFrame = startFrame;
        const int16_t* src = stereoLR;
        uint32_t remaining = frameCount;

        while (remaining > 0u) {
            const uint32_t chunk = std::min<uint32_t>(remaining, kMaxChunk);
            const uint64_t newReserve = curFrame + static_cast<uint64_t>(chunk);

            stagec::AtomicStoreU64(&block_->header.write_reserve_frame, newReserve);

            const uint32_t slot = static_cast<uint32_t>(curFrame & stagec::kRingFrameMask);
            const uint32_t firstPart = std::min<uint32_t>(chunk, stagec::kRingCapacityFrames - slot);
            const uint32_t secondPart = chunk - firstPart;

            std::memcpy(&block_->pcm_samples[static_cast<size_t>(slot) * 2u],
                        src,
                        static_cast<size_t>(firstPart) * 2u * sizeof(int16_t));
            if (secondPart > 0u) {
                std::memcpy(&block_->pcm_samples[0],
                            src + static_cast<size_t>(firstPart) * 2u,
                            static_cast<size_t>(secondPart) * 2u * sizeof(int16_t));
            }

            stagec::AtomicStoreU64(&block_->header.write_commit_frame, newReserve);

            curFrame = newReserve;
            src += static_cast<size_t>(chunk) * 2u;
            remaining -= chunk;
        }
        return true;
    }

private:
    stagec::StageCIpcSharedBlock* block_ = nullptr;
};

void InitSharedHeaderForTest(stagec::StageCIpcSharedBlock* block, uint64_t evenGen) noexcept {
    std::memset(block, 0, sizeof(stagec::StageCIpcRingHeader));
    stagec::StageCIpcRingHeader* hdr = &block->header;
    stagec::AtomicStore32(&hdr->magic, stagec::kIpcMagic);
    stagec::AtomicStore32(&hdr->version, stagec::kIpcVersion);
    stagec::AtomicStore32(&hdr->header_bytes, stagec::kHeaderBytes);
    stagec::AtomicStore32(&hdr->total_bytes, stagec::kTotalSharedBytes);
    stagec::AtomicStore32(&hdr->sample_rate, stagec::kSampleRate);
    stagec::AtomicStore32(&hdr->channels, stagec::kChannels);
    stagec::AtomicStore32(&hdr->bits_per_sample, stagec::kBitsPerSample);
    stagec::AtomicStore32(&hdr->capacity_frames, stagec::kRingCapacityFrames);
    stagec::AtomicStoreU64(&hdr->session_id, 0x123456789ABCDEF0ULL);
    stagec::AtomicStoreU64(&hdr->qpc_freq, 10000000ULL);
    stagec::AtomicStore32(&hdr->producer_pid, 1001u);
    stagec::AtomicStore32(&hdr->producer_arch, 64u);
    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Streaming));
    stagec::AtomicStoreU64(&hdr->session_generation, evenGen);
}

class SimulatedStageCConsumer {
public:
    explicit SimulatedStageCConsumer(stagec::StageCIpcSharedBlock* block,
                                     uint64_t steadyLeadFrames = 168000ULL) noexcept
        : block_(block)
        , steadyLeadFrames_(steadyLeadFrames) {}

    void Pull24ms(std::vector<int16_t>& outBuf) {
        constexpr uint32_t kPullFrames = 1152u;
        outBuf.resize(static_cast<size_t>(kPullFrames) * 2u);
        stagec::StageCIpcRingHeader* hdr = &block_->header;

        const uint64_t gen1 = stagec::AtomicLoadU64(&hdr->session_generation);
        uint64_t commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
        const uint64_t reserveBefore = stagec::AtomicLoadU64(&hdr->write_reserve_frame);

        if (!anchored_) {
            if (commitBefore >= steadyLeadFrames_) {
                liveReadFrame_ = commitBefore - steadyLeadFrames_;
                startupSilenceDebt_ = 0ULL;
            } else {
                liveReadFrame_ = 0ULL;
                startupSilenceDebt_ = steadyLeadFrames_ - commitBefore;
            }
            anchored_ = true;
            stagec::AtomicStore32(&hdr->consumer_active_readers, 1u);
            stagec::AtomicAddU64(&hdr->session_attach_count, 1ULL);
        }

        int16_t* dst = outBuf.data();
        uint32_t rem = kPullFrames;

        if (startupSilenceDebt_ > 0ULL) {
            const uint32_t debtChunk = static_cast<uint32_t>(std::min<uint64_t>(rem, startupSilenceDebt_));
            std::memset(dst, 0, static_cast<size_t>(debtChunk) * 2u * sizeof(int16_t));
            startupSilenceDebt_ -= static_cast<uint64_t>(debtChunk);
            dst += static_cast<size_t>(debtChunk) * 2u;
            rem -= debtChunk;
            if (rem == 0u) {
                const uint64_t wm = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
                stagec::AtomicStoreU64(&hdr->consumer_read_frame, liveReadFrame_);
                stagec::AtomicStoreU64(&hdr->watermark_frames, wm);
                stagec::AtomicStoreU64(&hdr->last_frame_age_us, (wm * 1000000ULL) / 48000ULL);
                return;
            }
        }

        if (reserveBefore > liveReadFrame_ &&
            (reserveBefore - liveReadFrame_) > (stagec::kRingCapacityFrames - stagec::kGuardFrames)) {
            const uint64_t newPos = (commitBefore > steadyLeadFrames_) ? (commitBefore - steadyLeadFrames_) : 0ULL;
            const uint64_t skipped = (newPos > liveReadFrame_) ? (newPos - liveReadFrame_) : 0ULL;
            liveReadFrame_ = newPos;
            stagec::AtomicAddU64(&hdr->overrun_events, 1ULL);
            stagec::AtomicAddU64(&hdr->overrun_skipped_frames, skipped);
        }

        commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
        const uint64_t avail = (commitBefore > liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
        const uint32_t toCopy = static_cast<uint32_t>(std::min<uint64_t>(rem, avail));

        if (toCopy > 0u) {
            const uint32_t slot = static_cast<uint32_t>(liveReadFrame_ & stagec::kRingFrameMask);
            const uint32_t firstPart = std::min<uint32_t>(toCopy, stagec::kRingCapacityFrames - slot);
            const uint32_t secondPart = toCopy - firstPart;
            std::memcpy(dst,
                        &block_->pcm_samples[static_cast<size_t>(slot) * 2u],
                        static_cast<size_t>(firstPart) * 2u * sizeof(int16_t));
            if (secondPart > 0u) {
                std::memcpy(dst + static_cast<size_t>(firstPart) * 2u,
                            &block_->pcm_samples[0],
                            static_cast<size_t>(secondPart) * 2u * sizeof(int16_t));
            }
        }

        const uint64_t reserveAfter = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
        const uint64_t gen2 = stagec::AtomicLoadU64(&hdr->session_generation);
        if (toCopy > 0u &&
            ((gen2 != gen1) ||
             (reserveAfter > liveReadFrame_ && (reserveAfter - liveReadFrame_) > stagec::kRingCapacityFrames))) {
            stagec::AtomicAddU64(&hdr->overwrite_conflict_events, 1ULL);
        }

        liveReadFrame_ += static_cast<uint64_t>(toCopy);
        if (toCopy < rem) {
            const uint32_t missing = rem - toCopy;
            std::memset(dst + static_cast<size_t>(toCopy) * 2u,
                        0,
                        static_cast<size_t>(missing) * 2u * sizeof(int16_t));
            stagec::AtomicAddU64(&hdr->underrun_events, 1ULL);
            stagec::AtomicAddU64(&hdr->underrun_frames, static_cast<uint64_t>(missing));
        }

        const uint64_t finalCommit = stagec::AtomicLoadU64(&hdr->write_commit_frame);
        const uint64_t wm = (finalCommit >= liveReadFrame_) ? (finalCommit - liveReadFrame_) : 0ULL;
        stagec::AtomicStoreU64(&hdr->consumer_read_frame, liveReadFrame_);
        stagec::AtomicStoreU64(&hdr->watermark_frames, wm);
        stagec::AtomicStoreU64(&hdr->last_frame_age_us, (wm * 1000000ULL) / 48000ULL);
    }

    bool IsWarmupComplete() const noexcept { return anchored_ && startupSilenceDebt_ == 0ULL; }

private:
    stagec::StageCIpcSharedBlock* block_ = nullptr;
    uint64_t steadyLeadFrames_ = 168000ULL;
    bool anchored_ = false;
    uint64_t liveReadFrame_ = 0ULL;
    uint64_t startupSilenceDebt_ = 0ULL;
};

// ============================================================================
// Test 1: 128-bit QPC/100ns Math & Holdback Flush Invariants
// ============================================================================

void RunTest1_MathAndHoldbackFlush() {
    std::printf("[Test 1] 128-bit QPC/100ns math, negative FloorDiv, holdback FIFO & flush...\n");

    // 1a. 128-bit overflow-free conversions at ~1.93e12 scale
    CHECK(staged::QpcTicksTo100ns(1930000000000LL, 10000000ULL) == 1930000000000LL,
          "1.93e12 ticks at 10 MHz (product 1.93e19 > UINT64_MAX)");
    CHECK(staged::FloorMulDivI64(1930000000000LL, 48000ULL, 10000000ULL) == 9264000000LL,
          "1.93e12 100ns to 48kHz exact");

    // Non-multiple QPC freq check: 3579545 Hz
    // floor(1930000000000 * 10000000 / 3579545) = 5391746716412 (rem 1007460)
    CHECK(staged::QpcTicksTo100ns(1930000000000LL, 3579545ULL) == 5391746716412LL,
          "1.93e12 ticks at 3579545 Hz exact");

    // Signed negative strict FloorDiv
    CHECK(staged::FloorMulDivI64(-1LL, 1ULL, 2ULL) == -1LL, "floor(-0.5) == -1");
    CHECK(staged::FloorMulDivI64(-2LL, 1ULL, 2ULL) == -1LL, "floor(-1.0) == -1");
    CHECK(staged::FloorMulDivI64(-3LL, 1ULL, 2ULL) == -2LL, "floor(-1.5) == -2");
    CHECK(staged::FloorMulDivI64(-1930000000000LL, 48000ULL, 10000000ULL) == -9264000000LL,
          "negative 1.93e12 scale exact");
    CHECK(staged::FloorMulDivI64(std::numeric_limits<int64_t>::min(), 1ULL, 2ULL) ==
          (std::numeric_limits<int64_t>::min() / 2LL),
          "INT64_MIN / 2 safe");
    CHECK(staged::FloorMulDivI64(std::numeric_limits<int64_t>::min(), 1ULL, 1ULL) ==
          std::numeric_limits<int64_t>::min(),
          "INT64_MIN identity safe");

    uint64_t rem = 0ULL;
    CHECK(staged::MulDivFloorAddRemU64(1ULL, 48000ULL, 0ULL, 44100ULL, &rem) == 1ULL && rem == 3900ULL,
          "44.1k->48k jump +1");
    CHECK(staged::MulDivFloorAddRemU64(100ULL, 48000ULL, rem, 44100ULL, &rem) == 108ULL && rem == 41100ULL,
          "44.1k->48k jump +100 with carry");
    CHECK(staged::MulDivFloorAddRemU64(46ULL, 48000ULL, rem, 44100ULL, &rem) == 51ULL && rem == 0ULL,
          "44.1k->48k jump +46 exact closure");

    // 1b. Holdback bounded [0, 240] and FlushHoldbackOnStop()
    CaptureMemorySink sink;
    staged::StageDTimelineBridge bridge(sink);
    bridge.Reset(kRealOrigin100ns);
    bridge.OnStreamStarted(48000u, 0u);

    const uint32_t sizes[] = { 100u, 200u, 480u, 111u, 333u, 96u };
    uint64_t cursor48k = 0ULL;
    std::vector<int16_t> commitSnapshot;

    for (uint32_t sz : sizes) {
        std::vector<int16_t> pcm(static_cast<size_t>(sz) * 2u, 16000);
        staged::TimelinePacketInput pkt;
        pkt.sourceFrames = sz;
        pkt.stereo48kLR = pcm.data();
        pkt.outFrames48k = sz;
        pkt.devPos = 0ULL;
        pkt.qpcPos100ns = kRealOrigin100ns + staged::MulDivFloorU64(cursor48k, 10000000ULL, 48000ULL);
        pkt.nowQpc100ns = pkt.qpcPos100ns + staged::MulDivFloorU64(sz, 10000000ULL, 48000ULL);

        CHECK(bridge.IngestPacket(pkt), "IngestPacket Test1");
        cursor48k += sz;

        const auto snap = bridge.Snapshot();
        CHECK(snap.holdbackFrames <= staged::kMaxHoldbackFrames, "holdback <= 240");
        CHECK(snap.VerifyInvariants(false), "invariants mid-stream");

        if (!commitSnapshot.empty()) {
            const size_t checkFrames = commitSnapshot.size() / 2u;
            for (size_t i = 0; i < checkFrames; ++i) {
                CHECK(sink.RawSamples()[i * 2u] == commitSnapshot[i * 2u] &&
                      sink.RawSamples()[i * 2u + 1u] == commitSnapshot[i * 2u + 1u],
                      "committed frames never modified retrospectively");
            }
        }
        commitSnapshot = sink.RawSamples();
    }

    const auto preFlush = bridge.Snapshot();
    CHECK(preFlush.holdbackFrames > 0u && preFlush.holdbackFrames <= 240u, "non-empty holdback pre-flush");
    const std::vector<int16_t> preFlushCommitted = sink.RawSamples();

    CHECK(bridge.FlushHoldbackOnStop(), "FlushHoldbackOnStop");
    const auto postFlush = bridge.Snapshot();
    CHECK(postFlush.holdbackFrames == 0u, "holdback == 0 after flush");
    CHECK(postFlush.committedRingFrames == postFlush.timelineQueuedFrames, "C == Q after flush");
    CHECK(postFlush.VerifyInvariants(true), "VerifyInvariants(true) after flush");
    CHECK(postFlush.fadeInCount == 1ULL && postFlush.fadeOutCount == 1ULL, "fadeIn=1 fadeOut=1");
    CHECK(!sink.WriteOrderViolated(), "monotonic sink writes");

    // Pre-existing committed region strictly preserved byte-for-byte
    for (size_t i = 0; i < preFlushCommitted.size(); ++i) {
        if (sink.RawSamples()[i] != preFlushCommitted[i]) {
            CHECK(false, "committed slot altered during flush");
            break;
        }
    }

    // Newly flushed uncommitted tail is monotonically faded down toward silence
    const uint64_t tailStart = preFlush.committedRingFrames;
    const uint64_t tailEnd = postFlush.committedRingFrames;
    CHECK(tailEnd - tailStart == preFlush.holdbackFrames, "flushed tail equals H");
    CHECK(sink.LeftAt(tailStart) > sink.LeftAt(tailEnd - 1ULL), "tail cosine fade-out slopes down");
    CHECK(sink.LeftAt(tailEnd - 1ULL) >= 0 && sink.LeftAt(tailEnd - 1ULL) <= 50,
          "tail fades close to 0");
}

// ============================================================================
// Test 2: 0 ppm Nominal + 150 ms Stall + Burst Historical Packet Recovery
// ============================================================================

void RunTest2_ZeroPpmStallAndDelayedBurstRecovery() {
    std::printf("[Test 2] 0 ppm + 150ms stall + 15 delayed historical packets recovery...\n");

    CaptureMemorySink sink;
    staged::StageDTimelineBridge bridge(sink);
    bridge.Reset(kRealOrigin100ns);
    bridge.OnStreamStarted(48000u, 0u);

    // Stall for 150 ms with 10ms Tick intervals
    for (uint64_t ms = 10ULL; ms <= 80ULL; ms += 10ULL) {
        CHECK(bridge.Tick(kRealOrigin100ns + ms * 10000ULL), "Tick <= 80ms");
        CHECK(bridge.Snapshot().starvationZeroFrames == 0ULL, "zero starvation within 80ms horizon");
    }
    for (uint64_t ms = 90ULL; ms <= 150ULL; ms += 10ULL) {
        CHECK(bridge.Tick(kRealOrigin100ns + ms * 10000ULL), "Tick 90..150ms");
    }

    const auto at150 = bridge.Snapshot();
    CHECK(at150.wallElapsedFrames48k == 7200ULL, "wallElapsed == 7200 at 150ms");
    CHECK(at150.starvationZeroFrames == 3360ULL, "starvation == 3360 (150ms - 80ms = 70ms)");
    CHECK(at150.timelineQueuedFrames == 3360ULL, "Q == 3360 at 150ms");
    CHECK(at150.VerifyInvariants(false), "invariants after stall");

    // Now burst 15 delayed packets (10ms / 480 frames each, spanning t=0..150ms) at now = 150ms
    const std::vector<int16_t> activePkt(480u * 2u, 12000);
    for (uint64_t i = 0ULL; i < 15ULL; ++i) {
        staged::TimelinePacketInput pkt;
        pkt.sourceFrames = 480u;
        pkt.stereo48kLR = activePkt.data();
        pkt.outFrames48k = 480u;
        pkt.devPos = 0ULL;
        pkt.qpcPos100ns = kRealOrigin100ns + i * 100000ULL;
        pkt.nowQpc100ns = kRealOrigin100ns + 1500000ULL;
        CHECK(bridge.IngestPacket(pkt), "Ingest delayed burst packet");
        CHECK(bridge.Snapshot().VerifyInvariants(false), "invariants during burst drain");
    }

    const auto postBurst = bridge.Snapshot();
    CHECK(postBurst.lateOverlapDroppedFrames == 3360ULL, "first 7 packets (3360 frames) trimmed cleanly");
    CHECK(postBurst.convertedInFrames48k == 7200ULL, "I == 7200");
    CHECK(postBurst.timelineQueuedFrames == 7200ULL, "Q == 7200 synced to wall clock");
    CHECK(postBurst.sourceGapZeroFrames == 0ULL, "no extra gap frames injected");

    const int64_t driftAfterBurst =
        static_cast<int64_t>(postBurst.timelineQueuedFrames) -
        static_cast<int64_t>(postBurst.wallElapsedFrames48k);
    CHECK(std::abs(driftAfterBurst) <= 480, "|Q - wallElapsed| <= N_pkt + L_conv");

    // Continue 15 normal packets up to t = 300ms
    for (uint64_t i = 15ULL; i < 30ULL; ++i) {
        staged::TimelinePacketInput pkt;
        pkt.sourceFrames = 480u;
        pkt.stereo48kLR = activePkt.data();
        pkt.outFrames48k = 480u;
        pkt.devPos = 0ULL;
        pkt.qpcPos100ns = kRealOrigin100ns + i * 100000ULL;
        pkt.nowQpc100ns = kRealOrigin100ns + (i + 1ULL) * 100000ULL;
        CHECK(bridge.IngestPacket(pkt), "Ingest normal post-burst packet");
        CHECK(bridge.Tick(pkt.nowQpc100ns), "Tick post-burst");
    }

    CHECK(bridge.FlushHoldbackOnStop(), "Flush Test2");
    const auto fin = bridge.Snapshot();
    CHECK(fin.VerifyInvariants(true), "VerifyInvariants(true) Test2");
    CHECK(fin.committedRingFrames == 14400ULL && fin.timelineQueuedFrames == 14400ULL,
          "C == Q == 14400 at 300ms");

    // First 3360 frames are digital silence; frames 3360..3599 ramp cleanly via cosine fade-in
    for (uint64_t f = 0ULL; f < 3360ULL; ++f) {
        if (sink.LeftAt(f) != 0 || sink.RightAt(f) != 0) {
            CHECK(false, "starvation window must be exact silence");
            break;
        }
    }
    CHECK(sink.LeftAt(3360ULL) >= 0 && sink.LeftAt(3360ULL) < sink.LeftAt(3599ULL),
          "post-starvation active audio fades in");
    CHECK(sink.LeftAt(3600ULL) == 12000 && sink.RightAt(3600ULL) == 12000,
          "post-fade active audio bit-exact");
}

// ============================================================================
// Test 3: +-200 ppm Continuous Clock Drift over 60s (Dual Mode + Shared Block)
// ============================================================================

struct DriftRunResult {
    staged::TimelineStatsSnapshot snap{};
    staged::IpcTelemetryCounters ipc{};
    uint64_t minWmAfterWarmup = std::numeric_limits<uint64_t>::max();
    uint64_t maxWmAfterWarmup = 0ULL;
    uint64_t firstWmAfterWarmup = 0ULL;
    uint64_t lastWmAfterWarmup = 0ULL;
};

DriftRunResult RunSingleDriftSimulation(int32_t ppmSign,
                                        bool useDevPosIncrement,
                                        const std::wstring* optionalTelemetryPath) {
    DriftRunResult out{};

    auto block = std::make_unique<stagec::StageCIpcSharedBlock>();
    constexpr uint64_t kGen = 2ULL;
    InitSharedHeaderForTest(block.get(), kGen);

    SimulatedIpcSharedSink ipcSink(block.get());
    staged::StageDTimelineBridge bridge(ipcSink);
    bridge.Reset(kRealOrigin100ns);
    bridge.OnStreamStarted(48000u, 0u);

    SimulatedStageCConsumer consumer(block.get(), 168000ULL);
    std::vector<int16_t> pullBuf;
    std::vector<int16_t> pktBuf(480u * 2u, 8000);

    // +200 ppm capture runs faster -> 99,980 ticks (9.998 ms) per 480-frame packet.
    // -200 ppm capture runs slower -> 100,020 ticks (10.002 ms) per 480-frame packet.
    const uint64_t pktPeriod100ns = (ppmSign > 0) ? 99980ULL : 100020ULL;
    constexpr uint64_t kTotalPackets = 6000ULL;
    constexpr uint64_t kConsumerStep100ns = 240000ULL; // 24 ms = 1152 frames

    FILE* telFp = nullptr;
    if (optionalTelemetryPath && !optionalTelemetryPath->empty()) {
        std::filesystem::path p(*optionalTelemetryPath);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        const errno_t openRc = _wfopen_s(&telFp, optionalTelemetryPath->c_str(), L"wb");
        CHECK(openRc == 0 && telFp != nullptr, "open sample telemetry JSONL output");
    }

    auto readIpc = [&]() -> staged::IpcTelemetryCounters {
        staged::IpcTelemetryCounters c{};
        c.sessionGeneration = stagec::AtomicLoadU64(&block->header.session_generation);
        c.producerState = stagec::AtomicLoad32(&block->header.producer_state);
        c.writeReserveFrame = stagec::AtomicLoadU64(&block->header.write_reserve_frame);
        c.writeCommitFrame = stagec::AtomicLoadU64(&block->header.write_commit_frame);
        c.consumerReadFrame = stagec::AtomicLoadU64(&block->header.consumer_read_frame);
        c.watermarkFrames = stagec::AtomicLoadU64(&block->header.watermark_frames);
        c.underrunEvents = stagec::AtomicLoadU64(&block->header.underrun_events);
        c.underrunFrames = stagec::AtomicLoadU64(&block->header.underrun_frames);
        c.overrunEvents = stagec::AtomicLoadU64(&block->header.overrun_events);
        c.overrunSkippedFrames = stagec::AtomicLoadU64(&block->header.overrun_skipped_frames);
        c.overwriteConflictEvents = stagec::AtomicLoadU64(&block->header.overwrite_conflict_events);
        c.lastFrameAgeUs = stagec::AtomicLoadU64(&block->header.last_frame_age_us);
        c.consumerActiveReaders = stagec::AtomicLoad32(&block->header.consumer_active_readers);
        return c;
    };

    uint64_t nextConsumer100ns = kRealOrigin100ns + kConsumerStep100ns;
    uint64_t nextTelemetry100ns = kRealOrigin100ns + 10000000ULL;
    bool capturedFirstWarmupWm = false;

    for (uint64_t i = 0ULL; i < kTotalPackets; ++i) {
        const uint64_t pktQpc = kRealOrigin100ns + i * pktPeriod100ns;
        const uint64_t pktArrival = pktQpc + pktPeriod100ns;

        while (nextConsumer100ns <= pktArrival) {
            bridge.Tick(nextConsumer100ns);
            consumer.Pull24ms(pullBuf);
            if (consumer.IsWarmupComplete()) {
                const uint64_t wm = stagec::AtomicLoadU64(&block->header.watermark_frames);
                if (!capturedFirstWarmupWm) {
                    capturedFirstWarmupWm = true;
                    out.firstWmAfterWarmup = wm;
                }
                out.minWmAfterWarmup = std::min(out.minWmAfterWarmup, wm);
                out.maxWmAfterWarmup = std::max(out.maxWmAfterWarmup, wm);
                out.lastWmAfterWarmup = wm;
            }
            nextConsumer100ns += kConsumerStep100ns;
        }

        staged::TimelinePacketInput pkt;
        pkt.sourceFrames = 480u;
        pkt.stereo48kLR = pktBuf.data();
        pkt.outFrames48k = 480u;
        pkt.devPos = useDevPosIncrement ? (i * 480ULL) : 0ULL;
        pkt.qpcPos100ns = pktQpc;
        pkt.nowQpc100ns = pktArrival;

        CHECK(bridge.IngestPacket(pkt), "IngestPacket Drift");
        CHECK(bridge.Tick(pktArrival), "Tick Drift");

        if (telFp && pktArrival >= nextTelemetry100ns) {
            staged::TelemetryLineContext ctx;
            ctx.kind = "periodic";
            ctx.nowQpc100ns = pktArrival;
            ctx.targetPid = 4242u;
            ctx.targetExeUtf8 = "AMPLibraryAgent.exe";
            ctx.formatLabelUtf8 = "48000Hz_2ch_Float32_Native";
            ctx.ipc = readIpc();
            const std::string line = staged::FormatTelemetryJsonLine(bridge.Snapshot(), ctx);
            std::fwrite(line.data(), 1u, line.size(), telFp);
            std::fputc('\n', telFp);
            nextTelemetry100ns += 10000000ULL;
        }
    }

    bridge.OnStreamEnded();
    CHECK(bridge.FlushHoldbackOnStop(), "FlushHoldbackOnStop Drift");
    stagec::AtomicStore32(&block->header.producer_state,
                          static_cast<uint32_t>(stagec::ProducerState::Stopped));

    out.snap = bridge.Snapshot();
    out.ipc = readIpc();

    if (telFp) {
        staged::TelemetryLineContext ctx;
        ctx.kind = "shutdown_flush";
        ctx.nowQpc100ns = out.snap.lastWallQpc100ns;
        ctx.targetPid = 4242u;
        ctx.targetExeUtf8 = "AMPLibraryAgent.exe";
        ctx.formatLabelUtf8 = "48000Hz_2ch_Float32_Native";
        ctx.ipc = out.ipc;
        const std::string line = staged::FormatTelemetryJsonLine(out.snap, ctx);
        std::fwrite(line.data(), 1u, line.size(), telFp);
        std::fputc('\n', telFp);
        std::fclose(telFp);
    }

    return out;
}

void RunTest3_ClockDriftAndTelemetryEndToEnd(const std::wstring& sampleTelemetryPath) {
    std::printf("[Test 3] +-200 ppm 60s continuous clock drift (devPos==0 loopback & devPos++ modes)...\n");

    struct CaseSpec {
        int32_t ppmSign;
        bool useDevPos;
        const char* label;
        bool writeSample;
    };
    const CaseSpec cases[] = {
        { +1, false, "+200ppm devPos==0 loopback", true },
        { +1, true,  "+200ppm devPos++",          false },
        { -1, false, "-200ppm devPos==0 loopback", false },
        { -1, true,  "-200ppm devPos++",          false },
    };

    for (const auto& c : cases) {
        const std::wstring* pathPtr = (c.writeSample && !sampleTelemetryPath.empty())
            ? &sampleTelemetryPath
            : nullptr;
        const DriftRunResult r = RunSingleDriftSimulation(c.ppmSign, c.useDevPos, pathPtr);

        CHECK(r.snap.VerifyInvariants(true), c.label);
        CHECK(r.snap.sourceGapZeroFrames == 0ULL, c.label);
        CHECK(r.snap.starvationZeroFrames == 0ULL, c.label);
        CHECK(r.snap.lateOverlapDroppedFrames == 0ULL, c.label);
        CHECK(r.snap.timelineQueuedFrames == 2880000ULL, c.label);
        CHECK(r.snap.convertedInFrames48k == 2880000ULL, c.label);
        CHECK(r.snap.committedRingFrames == 2880000ULL, c.label);
        CHECK(r.snap.holdbackFrames == 0u, c.label);

        const int64_t expectedDiff = (c.ppmSign > 0) ? 576LL : -576LL;
        const int64_t actualDiff =
            static_cast<int64_t>(r.snap.timelineQueuedFrames) -
            static_cast<int64_t>(r.snap.wallElapsedFrames48k);
        CHECK(actualDiff == expectedDiff, c.label);

        CHECK(r.ipc.underrunEvents == 0ULL, c.label);
        CHECK(r.ipc.overrunEvents == 0ULL, c.label);
        CHECK(r.ipc.overwriteConflictEvents == 0ULL, c.label);
        CHECK(r.minWmAfterWarmup >= 165000ULL && r.maxWmAfterWarmup <= 171000ULL, c.label);
        if (c.ppmSign > 0) {
            CHECK(r.lastWmAfterWarmup > r.firstWmAfterWarmup, "+200ppm watermark grows");
        } else {
            CHECK(r.lastWmAfterWarmup < r.firstWmAfterWarmup, "-200ppm watermark shrinks");
        }
    }
}

// ============================================================================
// Test 4: Boundaries, SILENT Fade, TIMESTAMP_ERROR, Restart & Ring Wrap-Around
// ============================================================================

void RunTest4_BoundaryAndErrorPaths() {
    std::printf("[Test 4] 80ms threshold, SILENT fade, TIMESTAMP_ERROR, restart, 44.1k jump, wrap-around...\n");

    // 4a. 80ms starvation threshold boundary (75ms -> 0, 80ms -> 0, 85ms -> 240)
    {
        CaptureMemorySink sink;
        staged::StageDTimelineBridge bridge(sink);
        bridge.Reset(kRealOrigin100ns);
        bridge.Tick(kRealOrigin100ns + 750000ULL);
        CHECK(bridge.Snapshot().starvationZeroFrames == 0ULL, "75ms -> 0 starvation");
        bridge.Tick(kRealOrigin100ns + 800000ULL);
        CHECK(bridge.Snapshot().starvationZeroFrames == 0ULL, "80ms -> 0 starvation");
        bridge.Tick(kRealOrigin100ns + 850000ULL);
        CHECK(bridge.Snapshot().starvationZeroFrames == 240ULL, "85ms -> 240 starvation");
        CHECK(bridge.FlushHoldbackOnStop(), "4a flush");
        CHECK(bridge.Snapshot().VerifyInvariants(true), "4a invariants");
    }

    // 4b. SILENT packet fade-out & recovery fade-in without modifying committed slots
    {
        CaptureMemorySink sink;
        staged::StageDTimelineBridge bridge(sink);
        bridge.Reset(kRealOrigin100ns);
        bridge.OnStreamStarted(48000u, 0u);

        const std::vector<int16_t> loud(480u * 2u, 12000);
        uint64_t idx = 0ULL;
        for (int p = 0; p < 5; ++p, ++idx) {
            staged::TimelinePacketInput pkt;
            pkt.sourceFrames = 480u;
            pkt.stereo48kLR = loud.data();
            pkt.outFrames48k = 480u;
            pkt.qpcPos100ns = kRealOrigin100ns + idx * 100000ULL;
            pkt.nowQpc100ns = pkt.qpcPos100ns + 100000ULL;
            CHECK(bridge.IngestPacket(pkt), "4b loud pkt");
        }

        const auto beforeSilent = bridge.Snapshot();
        CHECK(beforeSilent.committedRingFrames == 2160ULL && beforeSilent.holdbackFrames == 240u,
              "4b C=2160 H=240");
        const std::vector<int16_t> committedBefore = sink.RawSamples();

        for (int p = 0; p < 2; ++p, ++idx) {
            staged::TimelinePacketInput pkt;
            pkt.sourceFrames = 480u;
            pkt.stereo48kLR = nullptr;
            pkt.outFrames48k = 480u;
            pkt.qpcPos100ns = kRealOrigin100ns + idx * 100000ULL;
            pkt.nowQpc100ns = pkt.qpcPos100ns + 100000ULL;
            pkt.silentFlag = true;
            CHECK(bridge.IngestPacket(pkt), "4b silent pkt");
        }

        // Verify previously committed [0..2160) remained untouched
        for (size_t i = 0; i < committedBefore.size(); ++i) {
            if (sink.RawSamples()[i] != committedBefore[i]) {
                CHECK(false, "4b committed frames corrupted during silent transition");
                break;
            }
        }
        CHECK(bridge.Snapshot().fadeOutCount == 1ULL, "4b fadeOut == 1");
        CHECK(sink.LeftAt(2160ULL) > sink.LeftAt(2399ULL) && sink.LeftAt(2399ULL) <= 50,
              "4b holdback faded out cleanly");
        for (uint64_t f = 2400ULL; f < 3120ULL; ++f) {
            if (sink.LeftAt(f) != 0 || sink.RightAt(f) != 0) {
                CHECK(false, "4b SILENT region must be zero");
                break;
            }
        }

        for (int p = 0; p < 2; ++p, ++idx) {
            staged::TimelinePacketInput pkt;
            pkt.sourceFrames = 480u;
            pkt.stereo48kLR = loud.data();
            pkt.outFrames48k = 480u;
            pkt.qpcPos100ns = kRealOrigin100ns + idx * 100000ULL;
            pkt.nowQpc100ns = pkt.qpcPos100ns + 100000ULL;
            CHECK(bridge.IngestPacket(pkt), "4b resume loud pkt");
        }
        CHECK(bridge.FlushHoldbackOnStop(), "4b flush");
        const auto fin = bridge.Snapshot();
        CHECK(fin.fadeInCount == 2ULL && fin.fadeOutCount == 2ULL, "4b fadeIn=2 fadeOut=2");
        CHECK(fin.VerifyInvariants(true), "4b invariants");
        CHECK(sink.LeftAt(3360ULL) < sink.LeftAt(3599ULL) && sink.LeftAt(3600ULL) == 12000,
              "4b resume fades in to bit-exact audio");
    }

    // 4c. DATA_DISCONTINUITY | TIMESTAMP_ERROR & out-of-window QPC fallback re-anchor
    {
        CaptureMemorySink sink;
        staged::StageDTimelineBridge bridge(sink);
        bridge.Reset(kRealOrigin100ns);
        bridge.OnStreamStarted(48000u, 0u);

        const std::vector<int16_t> loud(480u * 2u, 5000);
        for (uint64_t p = 0ULL; p < 2ULL; ++p) {
            staged::TimelinePacketInput pkt;
            pkt.sourceFrames = 480u;
            pkt.stereo48kLR = loud.data();
            pkt.outFrames48k = 480u;
            pkt.qpcPos100ns = kRealOrigin100ns + p * 100000ULL;
            pkt.nowQpc100ns = pkt.qpcPos100ns + 100000ULL;
            CHECK(bridge.IngestPacket(pkt), "4c initial packets");
        }

        // Inject DATA_DISCONTINUITY | TIMESTAMP_ERROR at now = 80ms (wall=3840 -> fallback anchor = 3360)
        staged::TimelinePacketInput tsErrPkt;
        tsErrPkt.sourceFrames = 480u;
        tsErrPkt.stereo48kLR = loud.data();
        tsErrPkt.outFrames48k = 480u;
        tsErrPkt.qpcPos100ns = 0ULL;
        tsErrPkt.nowQpc100ns = kRealOrigin100ns + 800000ULL;
        tsErrPkt.discontinuityFlag = true;
        tsErrPkt.timestampErrorFlag = true;
        CHECK(bridge.IngestPacket(tsErrPkt), "4c discont + ts error");

        const auto afterTsErr = bridge.Snapshot();
        CHECK(afterTsErr.discontinuityCount == 1ULL, "4c discont count");
        CHECK(afterTsErr.timestampErrorCount == 1ULL, "4c ts err count");
        CHECK(afterTsErr.timestampFallbackAnchors == 1ULL, "4c fallback anchor 1");
        CHECK(afterTsErr.sourceGapZeroFrames == 2400ULL, "4c fallback gap 2400 frames");
        CHECK(afterTsErr.timelineQueuedFrames == 3840ULL, "4c Q == 3840");

        // Inject out-of-window future QPC (+50s) when now = 90ms (should NOT emit 50s zeros!)
        staged::TimelinePacketInput oowPkt;
        oowPkt.sourceFrames = 480u;
        oowPkt.stereo48kLR = loud.data();
        oowPkt.outFrames48k = 480u;
        oowPkt.qpcPos100ns = kRealOrigin100ns + 500000000ULL;
        oowPkt.nowQpc100ns = kRealOrigin100ns + 900000ULL;
        CHECK(bridge.IngestPacket(oowPkt), "4c out-of-window QPC packet");

        const auto afterOow = bridge.Snapshot();
        CHECK(afterOow.timestampFallbackAnchors == 2ULL, "4c fallback anchor 2");
        CHECK(afterOow.sourceGapZeroFrames == 2400ULL, "4c rejected bogus 50s QPC gap");
        CHECK(afterOow.timelineQueuedFrames == 4320ULL, "4c Q == 4320");
        CHECK(bridge.FlushHoldbackOnStop(), "4c flush");
        CHECK(bridge.Snapshot().VerifyInvariants(true), "4c invariants");
    }

    // 4d. Cold-start convDelay clamp + non-zero initial devPos + 44.1kHz fractional gap + client restart + devPos==0 gap
    {
        CaptureMemorySink sink;
        staged::StageDTimelineBridge bridge(sink);
        bridge.Reset(kRealOrigin100ns);
        bridge.OnStreamStarted(44100u, 240u);

        const std::vector<int16_t> sig(480u * 2u, 4000);

        // Cold-start with qpcPos == origin and convDelay == 240 -> clamped to 0, ZERO cold-start frame drop!
        staged::TimelinePacketInput p0;
        p0.sourceFrames = 441u;
        p0.stereo48kLR = sig.data();
        p0.outFrames48k = 480u;
        p0.devPos = 100000ULL;
        p0.qpcPos100ns = kRealOrigin100ns;
        p0.nowQpc100ns = kRealOrigin100ns + 100000ULL;
        CHECK(bridge.IngestPacket(p0), "4d p0 nonzero initial devPos + convDelay clamp");
        CHECK(bridge.Snapshot().lateOverlapDroppedFrames == 0ULL, "4d cold-start convDelay zero drop");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 0ULL, "4d nonzero initial devPos zero false gap");
        CHECK(bridge.Snapshot().devPosTrackingActive, "4d devPosTrackingActive");

        // Jump devPos at 44100 Hz by +1, +100, +46 source frames (total 147 src -> 160 exact 48k frames)
        uint64_t nextDev = 100000ULL + 441ULL;
        const uint64_t jumps[3] = { 1ULL, 100ULL, 46ULL };
        for (int j = 0; j < 3; ++j) {
            nextDev += jumps[j];
            staged::TimelinePacketInput pj;
            pj.sourceFrames = 441u;
            pj.stereo48kLR = sig.data();
            pj.outFrames48k = 480u;
            pj.devPos = nextDev;
            pj.qpcPos100ns = kRealOrigin100ns + static_cast<uint64_t>(j + 2) * 100000ULL;
            pj.nowQpc100ns = pj.qpcPos100ns + 100000ULL;
            CHECK(bridge.IngestPacket(pj), "4d 44.1k jump");
            nextDev += 441ULL;
        }
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 160ULL, "4d 44.1k fractional jump sums to exact 160");

        // Restart client after OnStreamEnded() with different non-zero devPos and 48kHz loopback.
        // At Q = 4*480 + 160 = 2080, add +480 frames (10ms) gap -> frame 2560 (ceil ticks = 533334).
        bridge.OnStreamEnded();
        bridge.OnStreamStarted(48000u, 0u);
        CHECK(bridge.Snapshot().streamRestartCount == 1ULL, "4d streamRestartCount == 1");

        const uint64_t exact2560Ticks = (2560ULL * 10000000ULL + 47999ULL) / 48000ULL;
        const uint64_t exact2560Qpc = kRealOrigin100ns + exact2560Ticks;
        staged::TimelinePacketInput pr;
        pr.sourceFrames = 480u;
        pr.stereo48kLR = sig.data();
        pr.outFrames48k = 480u;
        pr.devPos = 500000ULL;
        pr.qpcPos100ns = exact2560Qpc;
        pr.nowQpc100ns = exact2560Qpc + 100000ULL;
        CHECK(bridge.IngestPacket(pr), "4d client restart");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 160ULL + 480ULL, "4d clean restart gap");

        // Also test devPos == 0 dropped-packet QPC gap (10ms dropped = +480 zeros)
        bridge.OnStreamEnded();
        bridge.OnStreamStarted(48000u, 0u);
        const uint64_t qpcA = exact2560Qpc + 100000ULL;
        staged::TimelinePacketInput ql0;
        ql0.sourceFrames = 480u;
        ql0.stereo48kLR = sig.data();
        ql0.outFrames48k = 480u;
        ql0.devPos = 0ULL;
        ql0.qpcPos100ns = qpcA;
        ql0.nowQpc100ns = qpcA + 100000ULL;
        CHECK(bridge.IngestPacket(ql0), "4d devPos==0 first pkt");

        staged::TimelinePacketInput ql1;
        ql1.sourceFrames = 480u;
        ql1.stereo48kLR = sig.data();
        ql1.outFrames48k = 480u;
        ql1.devPos = 0ULL;
        ql1.qpcPos100ns = qpcA + 200000ULL; // +20ms instead of +10ms (10ms missing!)
        ql1.nowQpc100ns = ql1.qpcPos100ns + 100000ULL;
        CHECK(bridge.IngestPacket(ql1), "4d devPos==0 dropped packet QPC gap");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 160ULL + 480ULL + 480ULL,
              "4d devPos==0 QPC gap == 480");

        CHECK(bridge.FlushHoldbackOnStop(), "4d flush");
        CHECK(bridge.Snapshot().VerifyInvariants(true), "4d invariants");
    }

    // 4e. StageCIpcSharedBlock ring wrap-around (> 1,048,576 frames with FrameCode readback)
    {
        auto block = std::make_unique<stagec::StageCIpcSharedBlock>();
        constexpr uint64_t kGen = 4ULL;
        InitSharedHeaderForTest(block.get(), kGen);

        SimulatedIpcSharedSink ipcSink(block.get());
        staged::StageDTimelineBridge bridge(ipcSink);
        bridge.Reset(kRealOrigin100ns);
        bridge.OnStreamStarted(48000u, 0u);

        std::vector<int16_t> fcBuf(480u * 2u);
        constexpr uint64_t kWrapPackets = 2200ULL; // 2200 * 480 = 1,056,000 frames > 1,048,576
        for (uint64_t p = 0ULL; p < kWrapPackets; ++p) {
            const uint64_t baseFrame = p * 480ULL;
            for (uint32_t j = 0u; j < 480u; ++j) {
                stagec::EncodeFrameCode(baseFrame + j, kGen, fcBuf[j * 2u], fcBuf[j * 2u + 1u]);
            }
            staged::TimelinePacketInput pkt;
            pkt.sourceFrames = 480u;
            pkt.stereo48kLR = fcBuf.data();
            pkt.outFrames48k = 480u;
            pkt.devPos = 0ULL;
            pkt.qpcPos100ns = kRealOrigin100ns + p * 100000ULL;
            pkt.nowQpc100ns = pkt.qpcPos100ns + 100000ULL;
            CHECK(bridge.IngestPacket(pkt), "4e wrap packet");
        }
        CHECK(bridge.FlushHoldbackOnStop(), "4e wrap flush");

        const auto snap = bridge.Snapshot();
        CHECK(snap.VerifyInvariants(true), "4e invariants after wrap");
        CHECK(snap.committedRingFrames == 1056000ULL, "4e C == 1,056,000");
        CHECK(stagec::AtomicLoadU64(&block->header.write_commit_frame) == 1056000ULL, "4e hdr commit");
        CHECK(stagec::AtomicLoadU64(&block->header.write_reserve_frame) == 1056000ULL, "4e hdr reserve");

        // Decode every frame across the 1,048,576 wrap boundary [1,040,000 .. 1,055,759]
        // (excluding only the final 240 fade-out holdback frames [1,055,760 .. 1,055,999])
        for (uint64_t f = 1040000ULL; f < 1055760ULL; ++f) {
            const uint32_t slot = static_cast<uint32_t>(f & stagec::kRingFrameMask);
            const int16_t l = block->pcm_samples[static_cast<size_t>(slot) * 2u];
            const int16_t r = block->pcm_samples[static_cast<size_t>(slot) * 2u + 1u];
            uint64_t decodedMod = 0ULL;
            if (!stagec::DecodeFrameCode(l, r, kGen, decodedMod) ||
                decodedMod != (f % stagec::kFrameCodeMod)) {
                CHECK(false, "4e FrameCode mismatch across 2^20 ring wrap");
                break;
            }
        }
    }

    // 4f. devPosTrackingActive_ mid-stream devPos==0 / timestampErrorFlag resilience (48kHz & 44.1kHz)
    {
        CaptureMemorySink sink;
        staged::StageDTimelineBridge bridge(sink);
        bridge.Reset(kRealOrigin100ns);
        bridge.OnStreamStarted(48000u, 0u);

        const std::vector<int16_t> sig(480u * 2u, 3000);
        for (uint64_t i = 0ULL; i < 2ULL; ++i) {
            staged::TimelinePacketInput pkt;
            pkt.sourceFrames = 480u;
            pkt.stereo48kLR = sig.data();
            pkt.outFrames48k = 480u;
            pkt.devPos = 200000ULL + i * 480ULL;
            pkt.qpcPos100ns = kRealOrigin100ns + i * 100000ULL;
            pkt.nowQpc100ns = pkt.qpcPos100ns + 100000ULL;
            CHECK(bridge.IngestPacket(pkt), "4f 48k initial devPos");
        }

        // Mid-stream SILENT packet with dummy devPos == 0 must not zero out expectedNextDevPos_
        staged::TimelinePacketInput silentZeroDev;
        silentZeroDev.sourceFrames = 480u;
        silentZeroDev.stereo48kLR = nullptr;
        silentZeroDev.outFrames48k = 480u;
        silentZeroDev.devPos = 0ULL;
        silentZeroDev.qpcPos100ns = kRealOrigin100ns + 200000ULL;
        silentZeroDev.nowQpc100ns = kRealOrigin100ns + 300000ULL;
        silentZeroDev.silentFlag = true;
        CHECK(bridge.IngestPacket(silentZeroDev), "4f mid-stream silent devPos==0");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 0ULL, "4f zero false gap on silent devPos==0");

        // Mid-stream TIMESTAMP_ERROR packet with devPos == 0 & qpcPos == 0
        staged::TimelinePacketInput tsErrZeroDev;
        tsErrZeroDev.sourceFrames = 480u;
        tsErrZeroDev.stereo48kLR = sig.data();
        tsErrZeroDev.outFrames48k = 480u;
        tsErrZeroDev.devPos = 0ULL;
        tsErrZeroDev.qpcPos100ns = 0ULL;
        tsErrZeroDev.nowQpc100ns = kRealOrigin100ns + 400000ULL;
        tsErrZeroDev.timestampErrorFlag = true;
        CHECK(bridge.IngestPacket(tsErrZeroDev), "4f mid-stream tsError devPos==0");
        CHECK(bridge.Snapshot().timestampFallbackAnchors == 1ULL, "4f fallback anchor == 1");

        // Following packet resumes normal devPos = 200000 + 4 * 480 = 201920
        staged::TimelinePacketInput resumeDev;
        resumeDev.sourceFrames = 480u;
        resumeDev.stereo48kLR = sig.data();
        resumeDev.outFrames48k = 480u;
        resumeDev.devPos = 201920ULL;
        resumeDev.qpcPos100ns = kRealOrigin100ns + 400000ULL;
        resumeDev.nowQpc100ns = kRealOrigin100ns + 500000ULL;
        CHECK(bridge.IngestPacket(resumeDev), "4f resume normal devPos");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 0ULL, "4f no bogus 200k gap after devPos recovery");
        CHECK(bridge.Snapshot().lateOverlapDroppedFrames == 0ULL, "4f zero drop on recovery");
        CHECK(bridge.Snapshot().timelineQueuedFrames == 2400ULL, "4f Q == 2400");
        CHECK(bridge.FlushHoldbackOnStop(), "4f 48k flush");
        CHECK(bridge.Snapshot().VerifyInvariants(true), "4f 48k invariants");

        // Verify devPosGapFracRem_ is also preserved across mid-stream devPos==0 at 44100 Hz
        CaptureMemorySink sink44;
        staged::StageDTimelineBridge bridge44(sink44);
        bridge44.Reset(kRealOrigin100ns);
        bridge44.OnStreamStarted(44100u, 0u);

        staged::TimelinePacketInput q0;
        q0.sourceFrames = 441u;
        q0.stereo48kLR = sig.data();
        q0.outFrames48k = 480u;
        q0.devPos = 100000ULL;
        q0.qpcPos100ns = kRealOrigin100ns;
        q0.nowQpc100ns = kRealOrigin100ns + 100000ULL;
        CHECK(bridge44.IngestPacket(q0), "4f 44.1k q0");

        // Jump +1 source frame -> gap=1, rem=3900
        staged::TimelinePacketInput q1 = q0;
        q1.devPos = 100442ULL;
        q1.qpcPos100ns = kRealOrigin100ns + 100000ULL;
        q1.nowQpc100ns = kRealOrigin100ns + 200000ULL;
        CHECK(bridge44.IngestPacket(q1), "4f 44.1k jump +1");
        CHECK(bridge44.Snapshot().sourceGapZeroFrames == 1ULL, "4f 44.1k gap == 1");

        // Intermediate devPos == 0 silent packet preserves rem == 3900 and advances expectedNextDevPos_ by 441
        staged::TimelinePacketInput q2;
        q2.sourceFrames = 441u;
        q2.stereo48kLR = nullptr;
        q2.outFrames48k = 480u;
        q2.devPos = 0ULL;
        q2.qpcPos100ns = kRealOrigin100ns + 200000ULL;
        q2.nowQpc100ns = kRealOrigin100ns + 300000ULL;
        q2.silentFlag = true;
        CHECK(bridge44.IngestPacket(q2), "4f 44.1k mid silent devPos==0");
        CHECK(bridge44.Snapshot().sourceGapZeroFrames == 1ULL, "4f 44.1k silent devPos==0 zero gap");

        // Remaining jumps +100 and +46 -> exact cumulative 160 frames
        staged::TimelinePacketInput q3 = q0;
        q3.devPos = 101424ULL; // 100883 + 441 + 100
        q3.qpcPos100ns = kRealOrigin100ns + 300000ULL;
        q3.nowQpc100ns = kRealOrigin100ns + 400000ULL;
        CHECK(bridge44.IngestPacket(q3), "4f 44.1k jump +100");

        staged::TimelinePacketInput q4 = q0;
        q4.devPos = 101911ULL; // 101424 + 441 + 46
        q4.qpcPos100ns = kRealOrigin100ns + 400000ULL;
        q4.nowQpc100ns = kRealOrigin100ns + 500000ULL;
        CHECK(bridge44.IngestPacket(q4), "4f 44.1k jump +46");
        CHECK(bridge44.Snapshot().sourceGapZeroFrames == 160ULL, "4f 44.1k fractional rem preserved to exact 160");
        CHECK(bridge44.FlushHoldbackOnStop(), "4f 44.1k flush");
        CHECK(bridge44.Snapshot().VerifyInvariants(true), "4f 44.1k invariants");
    }

    // 4g. Zero-frame packets (sourceFrames == 0 && outFrames48k == 0) & sink failure rollback hygiene
    {
        CaptureMemorySink sink;
        staged::StageDTimelineBridge bridge(sink);
        bridge.Reset(kRealOrigin100ns);
        bridge.OnStreamStarted(48000u, 0u);

        const std::vector<int16_t> sig(480u * 2u, 2500);

        // Cold-start 0-frame packet with qpcPos==0 must NOT set streamHasFirstPacket_ or timestampFallbackAnchors
        staged::TimelinePacketInput e0;
        e0.sourceFrames = 0u;
        e0.outFrames48k = 0u;
        e0.devPos = 0ULL;
        e0.qpcPos100ns = 0ULL;
        e0.nowQpc100ns = kRealOrigin100ns + 100000ULL;
        e0.silentFlag = true;
        CHECK(bridge.IngestPacket(e0), "4g cold-start 0-frame packet");
        CHECK(bridge.Snapshot().timestampFallbackAnchors == 0ULL, "4g zero bogus fallback on 0-frame pkt");
        CHECK(bridge.Snapshot().convertedInFrames48k == 0ULL && bridge.Snapshot().timelineQueuedFrames == 0ULL,
              "4g 0-frame queued == 0");

        // First real packet at t = 20ms still hits !streamHasFirstPacket_ and anchors cleanly to 960
        staged::TimelinePacketInput r0;
        r0.sourceFrames = 480u;
        r0.stereo48kLR = sig.data();
        r0.outFrames48k = 480u;
        r0.qpcPos100ns = kRealOrigin100ns + 200000ULL;
        r0.nowQpc100ns = kRealOrigin100ns + 300000ULL;
        CHECK(bridge.IngestPacket(r0), "4g first real packet at 20ms");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 960ULL, "4g initial 20ms gap == 960");
        CHECK(bridge.Snapshot().timestampFallbackAnchors == 0ULL, "4g still 0 fallback anchors");

        // Mid-stream 0-frame timestampError packet must NOT corrupt expectedNextQpc100ns_
        staged::TimelinePacketInput e1;
        e1.sourceFrames = 0u;
        e1.outFrames48k = 0u;
        e1.qpcPos100ns = 0ULL;
        e1.nowQpc100ns = kRealOrigin100ns + 310000ULL;
        e1.timestampErrorFlag = true;
        CHECK(bridge.IngestPacket(e1), "4g mid-stream 0-frame tsErr packet");
        CHECK(bridge.Snapshot().timestampFallbackAnchors == 0ULL, "4g 0-frame tsErr zero fallback");

        staged::TimelinePacketInput r1;
        r1.sourceFrames = 480u;
        r1.stereo48kLR = sig.data();
        r1.outFrames48k = 480u;
        r1.qpcPos100ns = kRealOrigin100ns + 300000ULL;
        r1.nowQpc100ns = kRealOrigin100ns + 400000ULL;
        CHECK(bridge.IngestPacket(r1), "4g contiguous packet after 0-frame pkt");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 960ULL, "4g zero false gap after 0-frame pkt");

        // 0-frame discontinuity resets streamHasFirstPacket_ so next packet re-anchors via QPC (+10ms gap = 480)
        staged::TimelinePacketInput e2;
        e2.sourceFrames = 0u;
        e2.outFrames48k = 0u;
        e2.nowQpc100ns = kRealOrigin100ns + 410000ULL;
        e2.discontinuityFlag = true;
        CHECK(bridge.IngestPacket(e2), "4g 0-frame discontinuity packet");

        staged::TimelinePacketInput r2;
        r2.sourceFrames = 480u;
        r2.stereo48kLR = sig.data();
        r2.outFrames48k = 480u;
        r2.qpcPos100ns = kRealOrigin100ns + 500000ULL;
        r2.nowQpc100ns = kRealOrigin100ns + 600000ULL;
        CHECK(bridge.IngestPacket(r2), "4g re-anchor packet after 0-frame discont");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 960ULL + 480ULL, "4g 0-frame discont re-anchors");
        CHECK(bridge.FlushHoldbackOnStop(), "4g flush");
        CHECK(bridge.Snapshot().VerifyInvariants(true), "4g invariants");

        // Sink failure rollback accounting check
        class FailingSink final : public staged::IRingFrameSink {
        public:
            bool WriteCommittedFrames(uint64_t, const int16_t*, uint32_t) override {
                return false;
            }
        } failSink;

        staged::StageDTimelineBridge failBridge(failSink);
        failBridge.Reset(kRealOrigin100ns);
        failBridge.OnStreamStarted(48000u, 0u);
        staged::TimelinePacketInput failPkt;
        failPkt.sourceFrames = 480u;
        failPkt.stereo48kLR = sig.data();
        failPkt.outFrames48k = 480u;
        failPkt.qpcPos100ns = kRealOrigin100ns + 200000ULL; // triggers gap write > 240
        failPkt.nowQpc100ns = kRealOrigin100ns + 300000ULL;
        CHECK(!failBridge.IngestPacket(failPkt), "4g failing sink returns false");
        const auto failSnap = failBridge.Snapshot();
        CHECK(failSnap.convertedInFrames48k == 0ULL && failSnap.sourceGapZeroFrames == 0ULL &&
              failSnap.lateOverlapDroppedFrames == 0ULL,
              "4g failing sink keeps accounting consistent");
        CHECK(failSnap.VerifyInvariants(false), "4g failing sink invariants");
    }

    // 4h. 0-frame discontinuity followed by devPos==0 packet & multi-chunk sink mid-failure accounting
    {
        // (4h-a) 44.1kHz devPosTrackingActive_ with non-zero devPosGapFracRem_ -> 0-frame discont
        // followed by valid devPos==0 packet -> devPos recovery preserves cursor & rem.
        CaptureMemorySink sink;
        staged::StageDTimelineBridge bridge(sink);
        bridge.Reset(kRealOrigin100ns);
        bridge.OnStreamStarted(44100u, 0u);

        const std::vector<int16_t> sig(580u * 2u, 2000);
        staged::TimelinePacketInput p0;
        p0.sourceFrames = 441u;
        p0.stereo48kLR = sig.data();
        p0.outFrames48k = 480u;
        p0.devPos = 100000ULL;
        p0.qpcPos100ns = kRealOrigin100ns;
        p0.nowQpc100ns = kRealOrigin100ns + 100000ULL;
        CHECK(bridge.IngestPacket(p0), "4h-a p0");

        staged::TimelinePacketInput p1 = p0;
        p1.devPos = 100442ULL; // +1 source frame -> gap=1, devPosGapFracRem_=3900
        p1.qpcPos100ns = kRealOrigin100ns + 100000ULL;
        p1.nowQpc100ns = kRealOrigin100ns + 200000ULL;
        CHECK(bridge.IngestPacket(p1), "4h-a p1 +1 jump");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 1ULL, "4h-a gap==1");

        staged::TimelinePacketInput zeroDiscont;
        zeroDiscont.sourceFrames = 0u;
        zeroDiscont.outFrames48k = 0u;
        zeroDiscont.discontinuityFlag = true;
        zeroDiscont.nowQpc100ns = kRealOrigin100ns + 200100ULL;
        CHECK(bridge.IngestPacket(zeroDiscont), "4h-a 0-frame discontinuity");

        // Next valid packet arrives with devPos==0 and discontinuityFlag==false (at Q == 961).
        staged::TimelinePacketInput p2;
        p2.sourceFrames = 441u;
        p2.stereo48kLR = sig.data();
        p2.outFrames48k = 480u;
        p2.devPos = 0ULL;
        p2.qpcPos100ns = kRealOrigin100ns + 200209ULL; // floor(200209 * 48000 / 10000000) == 961
        p2.nowQpc100ns = kRealOrigin100ns + 300209ULL;
        CHECK(bridge.IngestPacket(p2), "4h-a devPos==0 after 0-frame discont");
        CHECK(bridge.Snapshot().sourceGapZeroFrames == 1ULL, "4h-a devPos==0 zero false gap");

        // Recover devPos with +100 and +46 jumps -> exact cumulative 160 frames (rem 3900 preserved)
        staged::TimelinePacketInput p3 = p0;
        p3.devPos = 101424ULL; // expected 100883 + 441 (p2) + 100
        p3.qpcPos100ns = kRealOrigin100ns + 300209ULL;
        p3.nowQpc100ns = kRealOrigin100ns + 400209ULL;
        CHECK(bridge.IngestPacket(p3), "4h-a recover +100 jump");

        staged::TimelinePacketInput p4 = p0;
        p4.devPos = 101911ULL; // 101424 + 441 + 46
        p4.qpcPos100ns = kRealOrigin100ns + 400209ULL;
        p4.nowQpc100ns = kRealOrigin100ns + 500209ULL;
        CHECK(bridge.IngestPacket(p4), "4h-a recover +46 jump");

        CHECK(bridge.Snapshot().sourceGapZeroFrames == 160ULL, "4h-a fractional rem preserved to exact 160");
        CHECK(bridge.FlushHoldbackOnStop(), "4h-a flush");
        CHECK(bridge.Snapshot().VerifyInvariants(true), "4h-a invariants");

        // (4h-b) Mid-failure sink preserving 3-tier accounting invariants across multi-chunk emissions
        class MidFailureSink final : public staged::IRingFrameSink {
        public:
            explicit MidFailureSink(uint32_t successCalls) noexcept
                : remainingSuccessCalls_(successCalls) {}

            bool WriteCommittedFrames(uint64_t startFrame,
                                      const int16_t* stereoLR,
                                      uint32_t frameCount) override {
                if (frameCount == 0u) {
                    return true;
                }
                if (startFrame != committedFrames_ || !stereoLR) {
                    return false;
                }
                if (remainingSuccessCalls_ == 0u) {
                    return false;
                }
                --remainingSuccessCalls_;
                committedFrames_ += static_cast<uint64_t>(frameCount);
                return true;
            }

            uint64_t CommittedFrames() const noexcept { return committedFrames_; }

        private:
            uint32_t remainingSuccessCalls_ = 0u;
            uint64_t committedFrames_ = 0ULL;
        };

        // Case 1: Multi-chunk EmitZeroFrames(5000) via Tick() fails on chunk 2 (904) after chunk 1 (4096 -> C=3856, H=240).
        // wall = 3840 + 5000 = 8840 -> exact ceil ticks = (8840 * 10000000 + 47999) / 48000 = 1841667.
        {
            MidFailureSink midSink(1u);
            staged::StageDTimelineBridge failTickBridge(midSink);
            failTickBridge.Reset(kRealOrigin100ns);
            failTickBridge.OnStreamStarted(48000u, 0u);
            CHECK(!failTickBridge.Tick(kRealOrigin100ns + 1841667ULL), "4h-b Tick chunk-2 fails");
            const auto snap = failTickBridge.Snapshot();
            CHECK(snap.timelineQueuedFrames == 4096ULL && snap.starvationZeroFrames == 4096ULL,
                  "4h-b Tick partial starvation counted");
            CHECK(snap.committedRingFrames == 3856ULL && snap.holdbackFrames == 240u,
                  "4h-b Tick partial commit + holdback consistent");
            CHECK(midSink.CommittedFrames() == 3856ULL, "4h-b Tick sink committed 3856");
            CHECK(snap.VerifyInvariants(false), "4h-b Tick invariants hold on partial fail");
        }

        // Case 2: Overlap + fade-boundary EmitActiveFrames(480) fails on post-fade chunk 2 after fade chunk 1 succeeds.
        // Setup: Tick(820834) -> wall=3940, starv=100 held in holdback (Q=100, C=0, H=100, starv=100).
        // Ingest packet at qpc=origin with outFrames48k=580 -> trim=100, remain=480:
        // Call #1 flushes 100 holdback & stages 240 fade-in frames (succeeds); Call #2 fails flushing 240 holdback.
        {
            MidFailureSink midSink(1u);
            staged::StageDTimelineBridge failFadeBridge(midSink);
            failFadeBridge.Reset(kRealOrigin100ns);
            failFadeBridge.OnStreamStarted(48000u, 0u);

            CHECK(failFadeBridge.Tick(kRealOrigin100ns + 820834ULL), "4h-b fade setup tick starv=100");
            CHECK(failFadeBridge.Snapshot().starvationZeroFrames == 100ULL, "4h-b setup starv==100");

            staged::TimelinePacketInput overlapPkt;
            overlapPkt.sourceFrames = 580u;
            overlapPkt.stereo48kLR = sig.data();
            overlapPkt.outFrames48k = 580u;
            overlapPkt.qpcPos100ns = kRealOrigin100ns;
            overlapPkt.nowQpc100ns = kRealOrigin100ns + 820834ULL;
            CHECK(!failFadeBridge.IngestPacket(overlapPkt), "4h-b fade chunk-2 fails");

            const auto snap = failFadeBridge.Snapshot();
            CHECK(snap.convertedInFrames48k == 340ULL, "4h-b convertedIn == trim(100) + fade(240)");
            CHECK(snap.lateOverlapDroppedFrames == 100ULL, "4h-b lateDrop == 100");
            CHECK(snap.starvationZeroFrames == 100ULL, "4h-b starv == 100");
            CHECK(snap.timelineQueuedFrames == 340ULL, "4h-b Q == 340");
            CHECK(snap.committedRingFrames == 100ULL && snap.holdbackFrames == 240u,
                  "4h-b C == 100, H == 240");
            CHECK(midSink.CommittedFrames() == 100ULL, "4h-b sink committed 100");
            CHECK(snap.VerifyInvariants(false), "4h-b fade partial invariants hold");
        }

        // Case 3: isGap path where gap succeeds (300 frames -> C=60, H=240 on Call #1) and EmitActiveFrames(480)
        // succeeds on fade-in chunk 1 (Call #2 flushes H=240 -> C=300, Q=540) then fails on Call #3.
        // At Q=0, wall = gap 300 -> exact ceil ticks = (300 * 10000000 + 47999) / 48000 = 62500.
        {
            MidFailureSink midSink(2u);
            staged::StageDTimelineBridge failGapPktBridge(midSink);
            failGapPktBridge.Reset(kRealOrigin100ns);
            failGapPktBridge.OnStreamStarted(48000u, 0u);

            staged::TimelinePacketInput gapPkt;
            gapPkt.sourceFrames = 480u;
            gapPkt.stereo48kLR = sig.data();
            gapPkt.outFrames48k = 480u;
            gapPkt.qpcPos100ns = kRealOrigin100ns + 62500ULL;
            gapPkt.nowQpc100ns = kRealOrigin100ns + 162500ULL;
            CHECK(!failGapPktBridge.IngestPacket(gapPkt), "4h-b isGap pkt chunk-2 fails");

            const auto snap = failGapPktBridge.Snapshot();
            CHECK(snap.sourceGapZeroFrames == 300ULL && snap.convertedInFrames48k == 240ULL,
                  "4h-b isGap gap=300 convertedIn=240");
            CHECK(snap.timelineQueuedFrames == 540ULL && snap.committedRingFrames == 300ULL &&
                  snap.holdbackFrames == 240u,
                  "4h-b isGap Q=540 C=300 H=240");
            CHECK(midSink.CommittedFrames() == 300ULL, "4h-b isGap sink committed 300");
            CHECK(snap.VerifyInvariants(false), "4h-b isGap partial invariants hold");
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring sampleTelemetryPath;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--write-sample-telemetry") == 0 && i + 1 < argc) {
            sampleTelemetryPath = argv[++i];
        } else {
            std::fprintf(stderr, "Usage: stage_d_bridge_selftest.exe [--write-sample-telemetry <path>]\n");
            return 1;
        }
    }

    RunTest1_MathAndHoldbackFlush();
    RunTest2_ZeroPpmStallAndDelayedBurstRecovery();
    RunTest3_ClockDriftAndTelemetryEndToEnd(sampleTelemetryPath);
    RunTest4_BoundaryAndErrorPaths();

    if (g_hasFailures) {
        std::fprintf(stderr, "\n[RESULT] stage_d_bridge_selftest FAILED\n");
        return 1;
    }
    std::printf("\n[RESULT] stage_d_bridge_selftest PASSED (all 4 deterministic suites green)\n");
    return 0;
}
