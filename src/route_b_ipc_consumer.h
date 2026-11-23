#pragma once

#include <windows.h>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "ipc_ring_protocol.h"

namespace routeb {

inline constexpr uint64_t kSkipPostPullReserveFrames = 84000ULL; // 1.75s post-skip cushion @ 48 kHz
inline constexpr uint64_t kSavedTailTriggerLagFrames = 480000ULL; // 10.0s writer lag triggers unread tail snapshot
inline constexpr uint64_t kMaxSavedTailFrames        = 576000ULL; // 12.0s max RAM tail snapshot (~2.3 MB)

struct RouteBConsumerConfig {
    std::wstring mappingName   = stagec::kDefaultMappingName;
    uint32_t steadyLeadMs      = 5500u;
    uint32_t heartbeatTimeoutMs = 500u;
    uint32_t fadeMs            = 10u;
    bool allowHotReattach      = true;
};

struct RouteBPullStatus {
    bool ok                    = false;
    bool producerOnline        = false;
    bool usedPrerollAnchor     = false;
    uint64_t sessionGeneration = 0ULL;
    uint64_t readStartFrame    = 0ULL;
    uint64_t readEndFrame      = 0ULL;
    uint32_t framesPerBlock    = 0u;
    uint32_t liveFramesDelivered    = 0u;
    uint32_t silenceFramesDelivered = 0u;
    uint64_t watermarkFrames   = 0ULL;
};

// Matches GTAIV.exe 0x00883050 / 0x00883166 planar block framing.
constexpr uint32_t ComputeBlockFrameCount(uint32_t blockBytes) noexcept {
    return (blockBytes & ~7u) / stagec::kBytesPerFrame;
}

// Writes digital silence to the entire wave block.
void ZeroPlanarBlock(void* blockPtr, uint32_t blockBytes) noexcept;

// Splits interleaved [L0, R0, L1, R1, ...] stereo PCM into planar [L0..Ln-1], [R0..Rn-1].
void DeinterleaveStereoToPlanar(const int16_t* interleavedLR,
                                uint32_t frames,
                                int16_t* outL,
                                int16_t* outR) noexcept;

// Writes interleaved stereo PCM directly into a GTA IV planar wave block,
// zero-filling any non-multiple-of-8 trailing bytes safely.
bool WriteInterleavedToPlanarBlock(void* blockPtr,
                                   uint32_t blockBytes,
                                   const int16_t* interleavedLR,
                                   uint32_t frames) noexcept;

// Lock-free cross-bitness planar ring consumer tailored for audUserTrackDecoder::FillBlocks
// and audRadioSlot::Play zero-gap handoff.
// Never acquires cross-process producer mutexes or blocks unboundedly.
class RouteBIpcConsumer {
public:
    RouteBIpcConsumer();
    ~RouteBIpcConsumer();

    RouteBIpcConsumer(const RouteBIpcConsumer&) = delete;
    RouteBIpcConsumer& operator=(const RouteBIpcConsumer&) = delete;

    void Configure(const RouteBConsumerConfig& cfg);
    void Shutdown();

    // Test hook: injects an in-memory shared block without CreateFileMapping/MapViewOfFile.
    // Caller retains ownership; Shutdown() detaches without unmapping.
    void AttachExternalBlockForTest(stagec::StageCIpcSharedBlock* externalBlock,
                                    size_t mappedBytes = stagec::kTotalSharedBytes);

    void ResetCursor();
    void JumpReadCursorToFrame(uint64_t targetFrame);
    void QueueSkipGap(uint64_t gapStartFrame, uint64_t gapEndFrame);
    void SetActiveReaderCount(uint32_t count);
    bool PreserveUnreadTailIfApproachingLap();
    void SetSongEndBarrier(uint64_t barrierFrame);
    void ClearSongEndBarrier();

    uint64_t GetLiveReadFrame() const;
    bool ReadSharedHeaderSnapshot(stagec::StageCIpcRingHeader& outHeader) const;

    // Steady-state refill path (block1 == nullptr in FillBlocks).
    RouteBPullStatus FillSteadyPlanarBlock(void* blockPtr,
                                           uint32_t blockBytes,
                                           uint64_t* outEndFrame = nullptr);

    // Pre-roll handoff path at audRadioSlot::Play time:
    // populates Block 0 + Block 1 cleanly without losing intro frames or colliding with mixer.
    RouteBPullStatus FillPreloadedBlocksAtPlay(void* block0,
                                               void* block1,
                                               uint32_t blockBytes,
                                               bool hasPrerollAnchor,
                                               uint64_t prerollStartFrame,
                                               uint32_t minPostPlayReserveFrames,
                                               uint64_t* outEndFrame = nullptr,
                                               uint32_t minHeadPadFrames = 0u);

private:
    struct SkipGapRange {
        uint64_t startFrame = 0ULL;
        uint64_t endFrame   = 0ULL;
    };

    bool TryEnsureMappingLocked(int64_t nowUs);
    void CloseMappingLocked();
    bool ValidateHeaderStaticFieldsLocked(const stagec::StageCIpcRingHeader* hdr) const;
    bool IsProducerLiveLocked(const stagec::StageCIpcRingHeader* hdr,
                              int64_t nowQpc,
                              uint64_t& outGen) const;
    bool RefreshProducerAttachmentLocked(int64_t nowQpc, int64_t nowUs, uint64_t& outGen);
    void CopyRingRangeInterleavedLocked(uint64_t startFrame, uint32_t frameCount, int16_t* outLR) const;
    void SetFadeInLocked(uint32_t spanFrames) noexcept;
    void ApplyCosineFadeInLocked(int16_t* interleavedLR, uint32_t frameCount);
    void ApplyCosineFadeWindowInLocked(int16_t* interleavedLR,
                                       uint32_t frameCount,
                                       uint32_t alreadyDone,
                                       uint32_t spanFrames);
    void ApplyCosineFadeOutTailLocked(int16_t* interleavedLR, uint32_t frameCount);
    void ApplyCosineFadeOutTailExactLocked(int16_t* interleavedLR, uint32_t totalFrames, uint32_t fadeLen);
    void EmitSilencePlanarLocked(void* blockPtr, uint32_t blockBytes, bool countAsDisconnect);
    void UpdateConsumerTelemetryLocked(int64_t nowQpc, uint64_t commitFrame) noexcept;
    void ClearSavedTailLocked() noexcept;
    void ClearSongEndBarrierLocked() noexcept;
    bool IsFrameInSavedTailLocked(uint64_t frame, uint64_t gen) const noexcept;
    bool IsRangeCoveredBySavedTailLocked(uint64_t startFrame, uint64_t endFrame, uint64_t gen) const noexcept;
    uint64_t CountEffectiveFramesExcludingGapsLocked(uint64_t startFrame,
                                                     uint64_t commitFrame,
                                                     size_t firstGapIdx) const noexcept;
    bool FillPrimedSkipTargetLocked(void* blockPtr,
                                    uint32_t blockBytes,
                                    int64_t nowQpc,
                                    uint64_t gen1,
                                    uint64_t steadyLeadFrames,
                                    RouteBPullStatus& outRes,
                                    uint64_t* outEndFrame);

    mutable CRITICAL_SECTION cs_{};
    RouteBConsumerConfig cfg_{};

    HANDLE hMapping_ = nullptr;
    stagec::StageCIpcSharedBlock* shared_ = nullptr;
    size_t mappedBytes_ = 0u;
    bool externalBlock_ = false;
    int64_t lastOpenAttemptUs_ = 0;

    uint64_t attachedGeneration_ = 0ULL;
    bool disconnectedLatched_ = false;
    bool cursorAnchored_ = false;
    uint64_t liveReadFrame_ = 0ULL;
    bool hasPendingSkip_ = false;
    uint64_t pendingSkipTargetFrame_ = 0ULL;
    std::deque<SkipGapRange> pendingGaps_;

    std::vector<int16_t> savedTailInterleaved_;
    uint64_t savedTailStartFrame_ = 0ULL;
    uint64_t savedTailEndFrame_   = 0ULL;
    uint64_t savedTailGeneration_ = 0ULL;
    bool     hasSongEndBarrier_   = false;
    uint64_t songEndBarrierFrame_ = 0ULL;

    uint32_t fadeTotalFrames_ = 480u;
    uint32_t fadeInSpanFrames_ = 480u;
    uint32_t fadeInRemaining_ = 0u;
    int16_t lastOutputL_ = 0;
    int16_t lastOutputR_ = 0;
    bool hadNonZeroAudio_ = false;
    uint32_t activeReaderCount_ = 0u;

    std::vector<int16_t> scratchInterleaved_;
    LARGE_INTEGER localQpcFreq_{};
};

} // namespace routeb
