#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace staged {

inline constexpr uint32_t kOutputSampleRate = 48000u;
inline constexpr uint32_t kMaxHoldbackFrames = 240u;           // 5.0 ms at 48 kHz
inline constexpr uint32_t kFadeFrames = 240u;                  // 5.0 ms cosine fade-in / fade-out
inline constexpr uint32_t kStarvationHorizonFrames = 3840u;    // 80.0 ms at 48 kHz
inline constexpr uint64_t kDefaultQpcGapThreshold100ns = 50000ULL;    // 5.0 ms
inline constexpr uint64_t kDefaultMaxTimestampSkew100ns = 100000000ULL; // 10.0 s

// ============================================================================
// Pure Portable C++17 128-bit Overflow-Safe Integer Math & Strict FloorDiv
// ============================================================================

// Computes floor((a * num + addRem) / den) with full 128-bit unsigned intermediate
// precision (returns saturated UINT64_MAX if result exceeds 64 bits or den == 0).
uint64_t MulDivFloorAddRemU64(uint64_t a,
                               uint64_t num,
                               uint64_t addRem,
                               uint64_t den,
                               uint64_t* outRem = nullptr) noexcept;

uint64_t MulDivFloorU64(uint64_t a,
                         uint64_t num,
                         uint64_t den,
                         uint64_t* outRem = nullptr) noexcept;

// Computes strict mathematical floor(a * num / den) for signed 64-bit 'a'
// without signed 64-bit multiplication overflow (saturates to INT64_MIN/INT64_MAX).
int64_t FloorMulDivI64(int64_t a, uint64_t num, uint64_t den) noexcept;

// Converts signed QPC tick delta/value into 100-nanosecond units via 128-bit FloorMulDivI64.
int64_t QpcTicksTo100ns(int64_t ticks, uint64_t qpcFreq) noexcept;

// ============================================================================
// Ring Frame Sink Abstraction & Timeline Bridge Types
// ============================================================================

class IRingFrameSink {
public:
    virtual ~IRingFrameSink() = default;

    // Appends 'frameCount' contiguous stereo frames starting at 'startFrame'
    // to the output ring/sink. Must never modify previously committed frames.
    virtual bool WriteCommittedFrames(uint64_t startFrame,
                                      const int16_t* stereoLR,
                                      uint32_t frameCount) = 0;
};

struct TimelineConfig {
    uint64_t qpcGapThreshold100ns = kDefaultQpcGapThreshold100ns;
    uint64_t maxTimestampSkew100ns = kDefaultMaxTimestampSkew100ns;
};

struct TimelinePacketInput {
    uint32_t sourceFrames = 0u;
    const int16_t* stereo48kLR = nullptr;
    uint32_t outFrames48k = 0u;
    uint64_t devPos = 0ULL;
    uint64_t qpcPos100ns = 0ULL;
    uint64_t nowQpc100ns = 0ULL;
    bool silentFlag = false;
    bool discontinuityFlag = false;
    bool timestampErrorFlag = false;
};

struct TimelineStatsSnapshot {
    uint64_t originQpc100ns = 0ULL;
    uint64_t lastWallQpc100ns = 0ULL;
    uint64_t wallElapsedFrames48k = 0ULL;

    uint64_t convertedInFrames48k = 0ULL;      // I
    uint64_t timelineQueuedFrames = 0ULL;      // Q
    uint64_t committedRingFrames = 0ULL;       // C
    uint32_t holdbackFrames = 0u;              // H in [0, 240]

    uint64_t lateOverlapDroppedFrames = 0ULL;
    uint64_t sourceGapZeroFrames = 0ULL;
    uint64_t starvationZeroFrames = 0ULL;
    uint64_t timestampFallbackAnchors = 0ULL;

    uint64_t packetCount = 0ULL;
    uint64_t silentPacketCount = 0ULL;
    uint64_t discontinuityCount = 0ULL;
    uint64_t timestampErrorCount = 0ULL;
    uint64_t streamRestartCount = 0ULL;
    uint64_t fadeInCount = 0ULL;
    uint64_t fadeOutCount = 0ULL;

    uint64_t lastPacketQpc100ns = 0ULL;
    uint64_t lastPacketArrivalQpc100ns = 0ULL;
    uint32_t lastPacketFrames48k = 0u;
    uint64_t lastPacketEndQpc100ns = 0ULL;

    uint64_t streamCursor48k = 0ULL;
    uint32_t sourceSampleRate = kOutputSampleRate;
    uint32_t convDelayFrames48k = 0u;
    bool streamActive = false;
    bool audioActive = false;
    bool devPosTrackingActive = false;

    bool VerifyInvariants(bool requireFlushed = false) const noexcept;
};

class StageDTimelineBridge {
public:
    explicit StageDTimelineBridge(IRingFrameSink& sink,
                                  const TimelineConfig& cfg = {}) noexcept;

    void Reset(uint64_t originQpc100ns) noexcept;

    // Optional startup prefill zeros synchronized with timeline counters.
    // Should only be invoked before any packets are ingested.
    bool PrefillZeroFrames(uint64_t zeroFrames, bool commitImmediately = true);

    void OnStreamStarted(uint32_t sourceSampleRate,
                         uint32_t convDelayFrames48k) noexcept;
    void OnStreamEnded() noexcept;

    bool IngestPacket(const TimelinePacketInput& pkt);
    bool Tick(uint64_t nowQpc100ns);
    bool FlushHoldbackOnStop();

    TimelineStatsSnapshot Snapshot() const noexcept;

private:
    void UpdateWallClock(uint64_t nowQpc100ns) noexcept;
    void EndActiveAudioTail() noexcept;
    bool PushSamplesToHoldbackAndSink(const int16_t* stereoLR, uint32_t frameCount);
    bool EmitZeroFrames(uint64_t zeroFrames, uint64_t* outEmittedFrames = nullptr);
    bool EmitActiveFrames(const int16_t* stereoLR,
                          uint32_t frameCount,
                          uint32_t* outEmittedFrames = nullptr);
    uint64_t AdvanceExpectedQpc(uint64_t baseQpc100ns,
                                uint32_t srcFrames,
                                uint32_t srcRate) noexcept;
    bool IsQpcTimestampValid(const TimelinePacketInput& pkt,
                             uint64_t refNowQpc100ns) const noexcept;
    int64_t ComputeFallbackAnchor48k(uint64_t refNowQpc100ns,
                                     uint32_t outFrames48k,
                                     bool isColdStartCandidate) const noexcept;

    IRingFrameSink& sink_;
    TimelineConfig cfg_{};

    uint64_t originQpc100ns_ = 0ULL;
    uint64_t lastWallQpc100ns_ = 0ULL;
    uint64_t wallElapsedFrames48k_ = 0ULL;

    uint64_t convertedInFrames48k_ = 0ULL;
    uint64_t timelineQueuedFrames_ = 0ULL;
    uint64_t committedRingFrames_ = 0ULL;
    std::array<int16_t, kMaxHoldbackFrames * 2u> holdbackBuf_{};
    uint32_t holdbackFrames_ = 0u;

    uint64_t lateOverlapDroppedFrames_ = 0ULL;
    uint64_t sourceGapZeroFrames_ = 0ULL;
    uint64_t starvationZeroFrames_ = 0ULL;
    uint64_t timestampFallbackAnchors_ = 0ULL;

    uint64_t packetCount_ = 0ULL;
    uint64_t silentPacketCount_ = 0ULL;
    uint64_t discontinuityCount_ = 0ULL;
    uint64_t timestampErrorCount_ = 0ULL;
    uint64_t streamRestartCount_ = 0ULL;
    uint64_t fadeInCount_ = 0ULL;
    uint64_t fadeOutCount_ = 0ULL;

    uint64_t lastPacketQpc100ns_ = 0ULL;
    uint64_t lastPacketArrivalQpc100ns_ = 0ULL;
    uint32_t lastPacketFrames48k_ = 0u;
    uint64_t lastPacketEndQpc100ns_ = 0ULL;

    // Stream state
    bool hasStartedAnyStream_ = false;
    bool streamActive_ = false;
    bool streamHasFirstPacket_ = false;
    bool devPosTrackingActive_ = false;
    uint32_t sourceSampleRate_ = kOutputSampleRate;
    uint32_t convDelayFrames48k_ = 0u;
    uint64_t streamCursor48k_ = 0ULL;
    uint64_t expectedNextDevPos_ = 0ULL;
    uint64_t devPosGapFracRem_ = 0ULL;
    uint64_t expectedNextQpc100ns_ = 0ULL;
    uint64_t qpcPacketFracRem_ = 0ULL;

    // Envelope state
    bool audioActive_ = false;
    uint32_t fadeInRemaining_ = 0u;
};

// ============================================================================
// Telemetry JSONL Formatting Shared Across Producer & Self-Tests
// ============================================================================

struct IpcTelemetryCounters {
    uint64_t sessionGeneration = 0ULL;
    uint32_t producerState = 0u;
    uint64_t writeReserveFrame = 0ULL;
    uint64_t writeCommitFrame = 0ULL;
    uint64_t consumerReadFrame = 0ULL;
    uint64_t watermarkFrames = 0ULL;
    uint64_t underrunEvents = 0ULL;
    uint64_t underrunFrames = 0ULL;
    uint64_t overrunEvents = 0ULL;
    uint64_t overrunSkippedFrames = 0ULL;
    uint64_t overwriteConflictEvents = 0ULL;
    uint64_t lastFrameAgeUs = 0ULL;
    uint32_t consumerActiveReaders = 0u;
};

struct TelemetryLineContext {
    const char* kind = "periodic";
    uint64_t nowQpc100ns = 0ULL;
    uint32_t targetPid = 0u;
    std::string targetExeUtf8;
    std::string formatLabelUtf8;
    IpcTelemetryCounters ipc{};
};

std::string FormatTelemetryJsonLine(const TimelineStatsSnapshot& snap,
                                    const TelemetryLineContext& ctx);

} // namespace staged
