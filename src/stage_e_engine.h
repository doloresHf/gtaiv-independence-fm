#pragma once

#include "ipc_ring_protocol.h"
#include "route_b_coordinator.h"
#include "route_b_ctrl_protocol.h"
#include "route_b_smtc.h"
#include "stage_d_capture.h"
#include "stage_d_timeline.h"
#include "stage_e_diagnostics.h"
#include "stage_e_routing_isolator.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace stagee {

struct StageEEngineOptions {
    std::wstring mappingName = stagec::kDefaultMappingName;
    uint32_t targetPid = 0u; // 0 = auto-discover configured source
    std::wstring targetExe;  // Optional CLI override; empty = use 'source' config
    AudioSourceConfig source{};
    PROCESS_LOOPBACK_MODE mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    uint32_t prefillMs = 150u;
    uint32_t telemetryIntervalMs = 1000u;
    uint32_t topologyPollIntervalMs = 1500u;
    UiLanguage language = UiLanguage::Auto;
    AutoRouteMode autoRouteMode = AutoRouteMode::Auto;
    std::wstring routeJournalPath; // Empty = sibling of executable
    bool enableAutoRouting = true; // False disables AudioPolicyConfig mutations (for headless flags/tests)
    std::wstring ctrlMappingName = routeb::kDefaultCtrlMappingName;
    bool enableRadioModeBridge = true;
    RotatingLoggerOptions logger{};
};

struct StageELiveSnapshot {
    bool running = false;
    bool producerMutexBusy = false;
    std::string errorDetailUtf8;

    AudioSourceConfig activeSource{};
    std::string targetDisplayNameUtf8 = "Apple Music";

    uint64_t sessionGeneration = 0ULL;
    double uptimeSec = 0.0;

    bool captureStreamActive = false;
    uint32_t captureTargetPid = 0u;
    std::string captureTargetExeUtf8;
    std::string captureFormatUtf8;
    std::string lastStreamEndReasonUtf8;

    std::wstring activeLogPath;
    std::wstring logDirectory;
    uint32_t activeLogPartIndex = 1u;
    uint64_t activeLogPartBytes = 0ULL;
    uint32_t retainedLogSessions = 0u;

    bool lastShutdownVerified = false;
    bool lastShutdownClean = false;
    uint64_t lastShutdownQueuedFrames = 0ULL;
    uint64_t lastShutdownCommittedFrames = 0ULL;
    uint32_t lastShutdownHoldbackFrames = 0u;

    staged::TimelineStatsSnapshot timeline{};
    staged::IpcTelemetryCounters ipc{};
    PcmLevelSnapshot level{};
    RoutingDiagnostic routing{};
    routeb::RouteBCoordinatorStatus routeB{};
    BridgeDiagnosticReport report{};
};

class StageEBridgeEngine {
public:
    StageEBridgeEngine();
    ~StageEBridgeEngine();

    StageEBridgeEngine(const StageEBridgeEngine&) = delete;
    StageEBridgeEngine& operator=(const StageEBridgeEngine&) = delete;

    // Starts the IPC producer ring, MMCSS capture worker, and topology monitor.
    // Returns false (and populates outError + GetSnapshot()) if the producer mutex is busy
    // or shared memory / log initialization fails.
    bool Start(const StageEEngineOptions& opts, std::string& outError);

    // Cleanly stops capture, drains residual packets, executes FlushHoldbackOnStop()
    // (verifying H == 0 && C == Q), writes shutdown_flush JSONL, and releases the IPC mutex.
    void Stop() noexcept;

    bool IsRunning() const noexcept;
    StageELiveSnapshot GetSnapshot() const;

    // Hot-switches diagnostic copy language immediately (0 ms audio interruption).
    void SetLanguage(UiLanguage lang) noexcept;
    UiLanguage GetLanguage() const noexcept;
    UiLanguage GetEffectiveLanguage() const noexcept;

    // Hot-switches audio capture target in-place without tearing down the IPC ring or session_generation
    // (smooth cosine fade-out -> rebind -> fade-in, zero 5.3s carrier reset in GTA IV).
    bool SetAudioSource(const AudioSourceConfig& source);
    AudioSourceConfig GetAudioSource() const;

    // Hot-switches zero-hardware automatic endpoint isolation (Auto <-> Manual, restores routes on Manual).
    void SetAutoRouteMode(AutoRouteMode mode) noexcept;
    AutoRouteMode GetAutoRouteMode() const noexcept;

    // Enables or disables Route B Independence FM Radio Mode synchronization without stopping capture.
    void SetRadioModeBridgeEnabled(bool enabled) noexcept;
    bool GetRadioModeBridgeEnabled() const noexcept;

    // Restores any isolated/pending app routes back to their pre-bridge original endpoints immediately.
    static RouteRestoreSummary RestoreAllAutoRoutesNow(const std::wstring& journalPathOverride = L"");

private:
    void TimelineCommitThreadMain(StageEEngineOptions opts, HANDLE startupDoneEvent);

    std::atomic<uint8_t> uiLanguage_{ static_cast<uint8_t>(UiLanguage::Auto) };
    std::atomic<uint8_t> autoRouteMode_{ static_cast<uint8_t>(AutoRouteMode::Auto) };
    std::atomic<bool> radioModeBridgeEnabled_{ true };

    mutable std::mutex sourceMu_;
    AudioSourceConfig sourceConfig_{};
    std::atomic<uint64_t> sourceConfigSeq_{ 1ULL };
    HANDLE captureReconfigEvent_ = nullptr;
    HANDLE topoReconfigEvent_ = nullptr;

    mutable std::mutex stateMu_;
    StageELiveSnapshot snapshot_{};
    BridgeDiagnosticInput lastDiagInput_{};
    BridgeStatusTracker lastDiagTracker_{};
    std::string lastBusyMutexNameUtf8_;

    std::mutex lifecycleMu_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    HANDLE stopEvent_ = nullptr;
    bool startupSucceeded_ = false;
    std::string startupError_;
    std::thread commitThread_;
};

} // namespace stagee
