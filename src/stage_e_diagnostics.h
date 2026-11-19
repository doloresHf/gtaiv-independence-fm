#pragma once

#include "stage_d_capture.h"
#include "stage_d_timeline.h"
#include "stage_e_routing_isolator.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace stagee {

// ============================================================================
// Section A0: UI Language, Audio Source Config ("Stream Anything") & Settings
// ============================================================================

enum class UiLanguage : uint8_t {
    Auto = 0, // Detect from Windows GetUserDefaultUILanguage() (Chinese -> ZhCn, else EnUs)
    ZhCn = 1, // Simplified Chinese (简体中文)
    EnUs = 2, // English
};

const char* UiLanguageToCode(UiLanguage lang) noexcept;
bool ParseUiLanguageCode(const std::string& codeUtf8, UiLanguage& outLang) noexcept;
UiLanguage ResolveEffectiveLanguage(UiLanguage preference) noexcept;

enum class AudioSourceMode : uint8_t {
    AppleMusicPreset = 0, // Store-prioritized AMPLibraryAgent.exe + Dolby Atmos diagnostics
    CustomProcess    = 1, // Generic app: same-image root PID + process-tree loopback ("Stream Anything")
};

const char* AudioSourceModeToCode(AudioSourceMode mode) noexcept;
bool ParseAudioSourceModeCode(const std::string& codeUtf8, AudioSourceMode& outMode) noexcept;

struct AudioSourceConfig {
    AudioSourceMode mode = AudioSourceMode::AppleMusicPreset;
    std::wstring customExeName; // Leaf filename e.g. L"Spotify.exe"
    std::wstring customExePath; // Optional full path for same-name disambiguation
};

// Returns human-friendly UTF-8 display name ("Apple Music" for preset, or leaf filename e.g. "Spotify.exe").
std::string FormatAudioSourceDisplayNameUtf8(const AudioSourceConfig& src);

// Blocks GTAIV.exe, bridge/producer binaries, and system host processes (audiodg/svchost/explorer)
// to prevent radio feedback howling loops or accidental system-wide audio capture.
bool IsForbiddenCaptureTargetExe(const std::wstring& exeNameOrPath) noexcept;

// Returns true for AppleMusic.exe or AMPLibraryAgent.exe so UI can guide to AppleMusicPreset.
bool IsAppleMusicShellOrAgentExe(const std::wstring& exeNameOrPath) noexcept;

// Walks parentPid upward while parent shares the exact same leaf .exe name (cycle-safe).
// Solves Chromium / Edge / Firefox / Electron child Audio Utility PID recycling & multi-tab capture.
uint32_t ResolveSameImageRootPid(const std::vector<staged::ProcessEntry>& processes,
                                 uint32_t candidatePid);

struct SelectableAudioApp {
    uint32_t rootPid = 0u;
    std::wstring exeName;
    std::wstring exeFullPath;
    std::wstring sessionDisplayName;
    std::wstring endpointName;
    bool hasActiveSession = false; // true = Playing (AudioSessionStateActive), false = Idle
    bool isAppleMusicFamily = false;
};

// Lists only apps that currently hold Active or Inactive WASAPI render sessions (sorted Playing -> Idle),
// aggregated to their same-image root PID and excluding forbidden targets / system sounds.
std::vector<SelectableAudioApp> EnumerateSelectableAudioApps(const staged::AudioTopologyReport& topo);

struct LauncherSettings {
    uint32_t version = 3u;
    UiLanguage language = UiLanguage::Auto;
    AudioSourceConfig source{};
    AutoRouteMode autoRouteMode = AutoRouteMode::Auto;
};

// Returns '<exeDir>\iv_apple_music_radio.settings.json' (or relative fallback when exeDir empty).
std::wstring ResolveDefaultSettingsFilePath(const std::wstring& exePathOverride = L"");

// Missing file is NOT an error: populates defaults and returns true.
bool LoadLauncherSettings(const std::wstring& filePath,
                          LauncherSettings& outSettings,
                          std::string& outError);

// Atomically persists settings JSON next to executable (or custom test path).
bool SaveLauncherSettings(const std::wstring& filePath,
                          const LauncherSettings& settings,
                          std::string& outError);

// ============================================================================
// Section A: Rolling Wall-Clock + Packet PCM Level Meter (1.0s & 5.0s Windows)
// ============================================================================

inline constexpr uint64_t kLevelBucketDuration100ns = 500000ULL; // 50 ms per bucket
inline constexpr size_t kLevelBuckets1s = 20u;                   // 20 * 50 ms = 1.0 s
inline constexpr size_t kLevelBuckets5s = 100u;                  // 100 * 50 ms = 5.0 s
inline constexpr int32_t kNearZeroPeakThreshold = 2;             // <= 2 LSB (~ -84 dBFS) treated as digital silence
inline constexpr uint64_t kRecentPacketWindow100ns = 3500000ULL; // 350 ms
inline constexpr uint64_t kSustainedSilenceWarnMs = 5000ULL;     // 5.0 s continuous zero-audio threshold
inline constexpr uint64_t kStreamingHoldoverMs = 1500ULL;        // 1.5 s anti-flicker holdover for musical pauses

struct PcmLevelSnapshot {
    uint64_t nowQpc100ns = 0ULL;

    uint64_t frames1s = 0ULL;
    uint64_t nonZeroFrames1s = 0ULL;
    uint64_t audibleFrames1s = 0ULL;
    uint64_t silentFlagFrames1s = 0ULL;
    int32_t peakAbs1s = 0;
    double peakDbfs1s = -144.0;
    double rmsDbfs1s = -144.0;

    uint64_t frames5s = 0ULL;
    uint64_t nonZeroFrames5s = 0ULL;
    uint64_t audibleFrames5s = 0ULL;
    int32_t peakAbs5s = 0;
    double peakDbfs5s = -144.0;
    double rmsDbfs5s = -144.0;

    // Latest 100 ms (2 buckets) instantaneous activity for responsive silence tracking
    uint64_t frames100ms = 0ULL;
    uint64_t audibleFrames100ms = 0ULL;
    int32_t peakAbs100ms = 0;

    bool hasAnyPacket = false;
    bool hasRecentPackets = false;
    uint64_t msSinceLastPacket = UINT64_MAX;
    uint64_t msSinceLastAudible = UINT64_MAX;
};

class RollingLevelMeter {
public:
    RollingLevelMeter() noexcept;

    void Reset(uint64_t originQpc100ns) noexcept;

    // Advances the sliding buckets up to wall-clock 'nowQpc100ns' even when no packets arrive.
    void AdvanceTo(uint64_t nowQpc100ns) noexcept;

    // Ingests a converted 48 kHz interleaved stereo int16_t chunk at 'nowQpc100ns'.
    void IngestStereoInt16(uint64_t nowQpc100ns,
                           const int16_t* stereo48kLR,
                           uint32_t frameCount,
                           bool silentFlag) noexcept;

    PcmLevelSnapshot Snapshot(uint64_t nowQpc100ns) noexcept;

private:
    struct Bucket {
        uint64_t frames = 0ULL;
        uint64_t nonZeroFrames = 0ULL;
        uint64_t audibleFrames = 0ULL;
        uint64_t silentFlagFrames = 0ULL;
        int32_t peakAbs = 0;
        uint64_t sumSquares = 0ULL;
    };

    bool initialized_ = false;
    uint64_t originQpc100ns_ = 0ULL;
    uint64_t currentWallQpc100ns_ = 0ULL;
    uint64_t currentSlot_ = 0ULL;

    bool hasAnyPacket_ = false;
    uint64_t lastPacketWallQpc100ns_ = 0ULL;
    bool hasAnyAudible_ = false;
    uint64_t lastAudibleWallQpc100ns_ = 0ULL;

    std::array<Bucket, kLevelBuckets5s> buckets_{};
};

// ============================================================================
// Section B: Split-Endpoint Audio Routing & Session Isolation Classifier
// ============================================================================

enum class RoutingIsolationState : uint8_t {
    Unknown = 0,
    SingleEndpointOnly = 1,            // <= 1 active render endpoint and target has no active/conflict session yet
    WaitingForTargetSession = 2,       // >= 2 render endpoints ready; target has not opened a render session yet
    IsolatedOk = 3,                    // Target session exclusively on non-default / non-GTAIV secondary endpoint
    ConflictOnDefaultOutput = 4,       // Target session resides on default output (or same endpoint as GTAIV.exe)
    SecondaryEndpointLostFallback = 5, // Previously IsolatedOk on secondary endpoint, which vanished & fell back to main
};

const char* RoutingIsolationStateName(RoutingIsolationState state) noexcept;

struct RoutingIsolatorTracker {
    uint32_t lastObservedTargetPid = 0u;
    std::wstring lastActiveEndpointId;
    std::wstring lastActiveEndpointName;
    std::wstring lastIsolatedEndpointId;
    std::wstring lastIsolatedEndpointName;

    void Reset() noexcept {
        lastObservedTargetPid = 0u;
        lastActiveEndpointId.clear();
        lastActiveEndpointName.clear();
        lastIsolatedEndpointId.clear();
        lastIsolatedEndpointName.clear();
    }
};

struct RoutingDiagnostic {
    RoutingIsolationState state = RoutingIsolationState::Unknown;
    uint32_t activeRenderEndpointCount = 0u;

    bool isAppleMusicMode = true;
    std::string targetDisplayNameUtf8 = "Apple Music";

    std::string defaultEndpointIdUtf8;
    std::string defaultEndpointNameUtf8;
    std::string targetEndpointIdUtf8;
    std::string targetEndpointNameUtf8;
    std::string lostEndpointNameUtf8;
    std::string gtaEndpointNameUtf8;

    bool targetHasSession = false;
    bool targetSessionActive = false;
    bool targetSessionMuted = false;
    float targetSessionVolume = 1.0f;

    bool appleMusicUiProcessAlive = false;
    bool ampAgentProcessAlive = false;
    bool gtaProcessAlive = false;

    std::vector<std::string> otherActiveMediaAppsUtf8;

    // Stage E2 zero-hardware auto-isolation & virtual silent sink status
    bool hasAutoRouteStatus = false;
    AutoRouteStatus autoRoute{};

    std::string bannerTitleUtf8;
    std::string bannerDetailUtf8;
};

void FormatRoutingDiagnosticText(RoutingDiagnostic& diag, UiLanguage lang);

RoutingDiagnostic EvaluateRoutingIsolation(const staged::AudioTopologyReport& topo,
                                           uint32_t currentTargetPid,
                                           const std::wstring& targetExeLeaf,
                                           RoutingIsolatorTracker& tracker,
                                           UiLanguage lang = UiLanguage::ZhCn,
                                           const AutoRouteStatus* optAutoRoute = nullptr);

// ============================================================================
// Section C: Unified Human-Readable Pipeline & Silence State Machine
// ============================================================================

enum class PipelineHeadlineState : uint8_t {
    BridgeStopped = 0,
    ProducerMutexBusy = 1,
    WaitingForAppleMusic = 2,
    ReconnectingTarget = 3,
    TargetMutedInMixerWarning = 4,
    AppleMusicPausedOrIdle = 5,
    BriefQuietPassage = 6,
    SustainedZeroAudioWarning = 7,
    StreamingWaitingForGame = 8,
    StreamingInGame = 9,
};

enum class TrayBadgeColor : uint8_t {
    Gray = 0,
    Blue = 1,
    Green = 2,
    Amber = 3,
    Red = 4,
};

const char* PipelineHeadlineStateName(PipelineHeadlineState state) noexcept;

struct BridgeStatusTracker {
    bool activeSilentTracking = false;
    uint64_t activeSilentStartQpc100ns = 0ULL;
    uint64_t lastNonActiveQpc100ns = 0ULL;
    uint32_t trackedPid = 0u;

    void Reset() noexcept {
        activeSilentTracking = false;
        activeSilentStartQpc100ns = 0ULL;
        lastNonActiveQpc100ns = 0ULL;
        trackedPid = 0u;
    }
};

struct BridgeDiagnosticInput {
    uint64_t nowQpc100ns = 0ULL;
    bool engineRunning = false;
    bool producerMutexBusy = false;
    std::string mutexBusyDetailUtf8;

    AudioSourceMode sourceMode = AudioSourceMode::AppleMusicPreset;
    std::string targetDisplayNameUtf8; // Empty defaults to "Apple Music" (or routing.targetDisplayNameUtf8)

    bool captureStreamActive = false;
    uint32_t captureTargetPid = 0u;
    std::string captureTargetExeUtf8;
    std::string captureFormatUtf8;
    uint64_t streamRestartCount = 0ULL;
    uint64_t msSinceLastStreamEnd = UINT64_MAX;

    bool lastShutdownVerified = false;
    bool lastShutdownClean = false;
    uint64_t lastShutdownQueuedFrames = 0ULL;
    uint64_t lastShutdownCommittedFrames = 0ULL;
    uint32_t lastShutdownHoldbackFrames = 0u;

    PcmLevelSnapshot level{};
    RoutingDiagnostic routing{};
    staged::IpcTelemetryCounters ipc{};
};

struct BridgeDiagnosticReport {
    PipelineHeadlineState headline = PipelineHeadlineState::BridgeStopped;
    TrayBadgeColor badgeColor = TrayBadgeColor::Gray;
    bool hasRoutingConflict = false;
    uint64_t continuousActiveSilentMs = 0ULL;

    std::string headlineTitleUtf8;
    std::string headlineDetailUtf8;
    std::string trayTooltipUtf8;
};

BridgeDiagnosticReport EvaluateBridgeDiagnostics(const BridgeDiagnosticInput& input,
                                                 BridgeStatusTracker& tracker,
                                                 UiLanguage lang = UiLanguage::ZhCn);

// ============================================================================
// Section D: Repo-Aware Default Log Directory & Rotating Session Logger
// ============================================================================

// Walks 0..4 parent directories from executable path (or 'exePathOverride' for offline tests).
// If a directory contains both CMakePresets.json and AGENTS.md, returns '<repoRoot>\logs';
// otherwise returns '<exeDir>\logs'.
std::wstring ResolveDefaultLogDirectory(const std::wstring& exePathOverride = L"");

// Strictly parses Stage E session filenames of the form:
//   stage_e_session_YYYYMMDD_HHMMSS_pid<digits>[_s<digits>][_part<digits>].jsonl
// Returns false for any Stage D, Stage E0, or IVRadioProbe log file so pruning never touches them.
bool ParseStageESessionLogFilename(const std::wstring& leafFilename,
                                   std::wstring& outSessionKey);

struct RotatingLoggerOptions {
    std::wstring explicitFilePath; // If non-empty (--telemetry-jsonl), single-file mode without pruning/splitting
    std::wstring logDirectory;     // Target directory when explicitFilePath is empty
    uint32_t maxRetainedSessions = 10u;
    uint64_t maxBytesPerPart = 20ULL * 1024ULL * 1024ULL; // 20 MiB default
    std::wstring sessionKeyOverride; // Optional deterministic key for unit tests
};

class RotatingTelemetryLogger {
public:
    RotatingTelemetryLogger() = default;
    ~RotatingTelemetryLogger();

    RotatingTelemetryLogger(const RotatingTelemetryLogger&) = delete;
    RotatingTelemetryLogger& operator=(const RotatingTelemetryLogger&) = delete;

    bool Open(const RotatingLoggerOptions& opts, std::string& outError);
    bool WriteLine(const std::string& jsonLine, bool allowPartSplit = true);
    void Close() noexcept;

    bool IsOpen() const noexcept { return fp_ != nullptr; }
    const std::wstring& ActiveFilePath() const noexcept { return activeFilePath_; }
    const std::wstring& LogDirectory() const noexcept { return activeDirectory_; }
    const std::wstring& SessionKey() const noexcept { return sessionKey_; }
    uint32_t CurrentPartIndex() const noexcept { return partIndex_; }
    uint64_t CurrentPartBytes() const noexcept { return currentPartBytes_; }
    uint32_t RetainedSessionCountAfterPrune() const noexcept { return retainedSessionsCount_; }

private:
    bool OpenPartFile(uint32_t partIndex, std::string& outError);
    void PruneOldSessionLogs() noexcept;

    FILE* fp_ = nullptr;
    bool isExplicitFile_ = false;
    RotatingLoggerOptions opts_{};
    std::wstring activeDirectory_;
    std::wstring sessionKey_;
    std::wstring activeFilePath_;
    uint32_t partIndex_ = 1u;
    uint64_t currentPartBytes_ = 0ULL;
    uint32_t retainedSessionsCount_ = 0u;
};

// Formats a single JSONL line preserving 100% of Stage D's FormatTelemetryJsonLine fields
// while appending Stage E1 level, silence, and split-endpoint routing diagnostics.
std::string FormatStageETelemetryJsonLine(const staged::TimelineStatsSnapshot& snap,
                                          const staged::TelemetryLineContext& ctx,
                                          const PcmLevelSnapshot& level,
                                          const RoutingDiagnostic& routing,
                                          const BridgeDiagnosticReport& report);

} // namespace stagee
