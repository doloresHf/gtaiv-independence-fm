#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#include "ipc_ring_protocol.h"

namespace probe {

struct StageCIpcConfig {
    std::wstring mappingName = stagec::kDefaultMappingName;
    std::vector<std::wstring> targetFiles; // Lowercased or case-insensitive leaf names
    uint32_t steadyLeadMs = 3500u;         // Default 3,500 ms (168,000 frames) post-preload cushion
    uint32_t heartbeatTimeoutMs = 500u;    // Producer considered disconnected if heartbeat older than 500ms
    uint32_t fadeMs = 10u;                 // Cosine fade-in / fade-out duration (0 = raw bit-exact mode for tests)
    bool allowHotReattach = true;          // Re-bind when a new even session_generation appears
};

struct StageCPullResult {
    bool filled = false;                   // True when outInterleavedLR[0..frameCount*2) was populated (audio or safe 0)
    bool undecidedPreloadSilence = false;  // True during seq <= 221 undecided preload window
    bool producerOnline = false;           // True when attached to a live even session_generation
    uint64_t sessionGeneration = 0;
    uint64_t liveReadFrameStart = 0;       // Valid when live frames were read
    uint32_t liveFramesDelivered = 0;      // Live ring frames delivered in this call (0..frameCount)
    uint32_t silenceFramesDelivered = 0;   // Zero/faded silence frames delivered in this call
    uint64_t watermarkFrames = 0;          // Observed (commit - liveReadFrame) after pull
    uint64_t frameAgeUs = 0;               // Estimated age of delivered audio relative to producer clock
};

// Process-wide Stage C consumer coordinator inside IVRadioProbe.asi (or direct unit test harness).
// Thread-safe via internal CRITICAL_SECTION; NEVER acquires cross-process mutexes or blocks unboundedly.
class StageCIpcManager {
public:
    static StageCIpcManager& Instance();

    void Configure(const StageCIpcConfig& cfg);
    void Shutdown();

    const StageCIpcConfig& GetConfig() const;

    // Lifecycle notifications from WrappedSyncReader (for active reader count & immediate primary lease release)
    void NotifyReaderOpened(int32_t readerId);
    void NotifyReaderEofOrClosed(int32_t readerId);

    // Core pull entry point called on GetNextSample (sampleSeq is 1-based per reader: 1..221 = undecided preload).
    // Guaranteed bounded execution (<1ms, zero unbounded spin).
    StageCPullResult PullFramesForReader(int32_t readerId,
                                         uint64_t sampleSeq,
                                         uint32_t frameCount,
                                         int16_t* outInterleavedLR);

    // Read-only telemetry snapshot for tests and diagnostics
    bool ReadSharedHeaderSnapshot(stagec::StageCIpcRingHeader& outHeader) const;

private:
    StageCIpcManager();
    ~StageCIpcManager();

    StageCIpcManager(const StageCIpcManager&) = delete;
    StageCIpcManager& operator=(const StageCIpcManager&) = delete;

    bool TryEnsureMappingLocked(int64_t nowUs);
    void CloseMappingLocked();
    bool ValidateHeaderStaticFieldsLocked(const stagec::StageCIpcRingHeader* hdr) const;
    bool IsProducerLiveLocked(const stagec::StageCIpcRingHeader* hdr, int64_t nowQpc, uint64_t& outGen) const;
    void ApplyFadeOutTail(int16_t* buf, uint32_t totalFrames);
    void EmitDisconnectedOrStartupSilenceLocked(int16_t* outLR, uint32_t frameCount, bool countAsDisconnect);

    mutable CRITICAL_SECTION cs_{};
    StageCIpcConfig cfg_{};

    HANDLE hMapping_ = nullptr;
    stagec::StageCIpcSharedBlock* shared_ = nullptr;
    size_t mappedBytes_ = 0;
    int64_t lastOpenAttemptUs_ = 0;

    // Session & single-primary stream state
    uint64_t attachedGeneration_ = 0;
    bool sessionEverAttached_ = false;
    bool disconnectedLatched_ = false;     // Used when allowHotReattach == false

    int32_t primaryReaderId_ = 0;
    int64_t primaryLastPullUs_ = 0;
    bool hasPriorPrimaryEnd_ = false;
    uint64_t lastPrimaryEndFrame_ = 0;

    bool cursorAnchored_ = false;
    uint64_t liveReadFrame_ = 0;
    uint64_t startupSilenceDebtFrames_ = 0;

    // Smooth cosine envelope state
    uint32_t fadeTotalFrames_ = 480u;
    uint32_t fadeInRemaining_ = 0u;
    int16_t lastOutputL_ = 0;
    int16_t lastOutputR_ = 0;
    bool hadNonZeroAudio_ = false;

    std::vector<int32_t> activeReaders_;
    LARGE_INTEGER localQpcFreq_{};
};

} // namespace probe
