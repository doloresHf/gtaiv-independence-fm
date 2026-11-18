#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <sdkddkver.h>
#if defined(NTDDI_VERSION) && (NTDDI_VERSION < NTDDI_WIN10_FE)
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_FE
#endif

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmreg.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace staged {

// Documented on Microsoft Learn for IAudioSessionControl2::GetProcessId when a session spans
// multiple processes (slot 0x00d in FACILITY_AUDCLNT, omitted from Windows SDK 10.0.22621.0 headers).
inline constexpr HRESULT kAudclntSNoSingleProcess = MAKE_SCODE(SEVERITY_SUCCESS, FACILITY_AUDCLNT, 0x00d);
inline constexpr HRESULT kAudclntENoSingleProcess = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_AUDCLNT, 0x00d);

// ============================================================================
// Section A: Capture Format Validation & Sample Conversion
// ============================================================================

enum class CaptureSampleType : uint8_t {
    Unknown = 0,
    Pcm16   = 1,
    Float32 = 2,
};

struct ValidatedCaptureFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    CaptureSampleType sampleType = CaptureSampleType::Unknown;
    bool isExtensible = false;
    uint32_t channelMask = 0;
};

// Validates an incoming WAVEFORMATEX / WAVEFORMATEXTENSIBLE for Stage D capture:
//  - Channels: ONLY 1 or 2
//  - Sample rate: 8000..384000 Hz
//  - WAVE_FORMAT_PCM: 16-bit, nBlockAlign == nChannels * 2
//  - WAVE_FORMAT_IEEE_FLOAT: 32-bit, nBlockAlign == nChannels * 4
//  - WAVE_FORMAT_EXTENSIBLE: cbSize >= 22, wValidBitsPerSample == wBitsPerSample (no padded 20/24-bit),
//    SubFormat == KSDATAFORMAT_SUBTYPE_PCM (16-bit) or KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (32-bit),
//    and dwChannelMask (if non-zero) must be 0x1/0x4 for 1ch or 0x3 for 2ch.
bool ValidateCaptureWaveFormat(const WAVEFORMATEX* wfx,
                               ValidatedCaptureFormat& out,
                               std::string& outError);

// Human-readable summary for logging any WAVEFORMATEX (including unsupported engine mix formats).
std::string FormatWaveFormatSummary(const WAVEFORMATEX* wfx);
std::string DescribeValidatedFormat(const ValidatedCaptureFormat& fmt);

// Converts a validated 1ch/2ch PCM16 or IEEE_FLOAT32 chunk into interleaved stereo int16_t (L, R):
//  - If silentFlag or !inBytes: emits frameCount * 2 exact 0s.
//  - PCM16 2ch: bit-exact memcpy preserving [-32768, 32767].
//  - PCM16 1ch: bit-exact sample duplication (s, s) preserving [-32768, 32767].
//  - IEEE_FLOAT32 (1ch or 2ch):
//      * Non-finite (!std::isfinite(v), i.e. NaN, +Inf, -Inf) -> 0.0f -> 0 (safe silence).
//      * Finite values are clamped to [-1.0f, 1.0f] BEFORE std::llround (zero overflow UB on +/-1e30f).
//      * Asymmetric linear scaling: negative samples scale by 32768.0 (-1.0f -> -32768),
//        non-negative samples scale by 32767.0 (+1.0f -> +32767, +/-0.0f -> 0), then clamped to [-32768, 32767].
void ConvertCaptureChunkToStereoInt16(const ValidatedCaptureFormat& fmt,
                                      const void* inBytes,
                                      uint32_t frameCount,
                                      bool silentFlag,
                                      std::vector<int16_t>& outStereoLR);

// ============================================================================
// Section B: Audio Topology, Process Tree & Mute Isolation Journal
// ============================================================================

struct ProcessEntry {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    std::wstring exeName;
    std::wstring exeFullPath;
};

struct AudioSessionRecord {
    std::wstring endpointId;
    std::wstring endpointFriendlyName;
    bool isDefaultConsoleEndpoint = false;
    bool isDefaultMultimediaEndpoint = false;

    std::wstring sessionId;
    std::wstring sessionInstanceId;
    std::wstring displayName;
    uint32_t processId = 0;
    uint32_t parentProcessId = 0;
    std::wstring exeFullPath;
    std::wstring exeName;
    HRESULT getProcessIdHr = S_OK;
    bool isMultiProcessSession = false;
    bool isSystemSoundsSession = false;
    uint32_t sessionState = 0; // AudioSessionState
    bool muted = false;
    float masterVolume = 1.0f;
    bool volumeValid = false;
};

struct AudioEndpointRecord {
    std::wstring endpointId;
    std::wstring friendlyName;
    bool isDefaultConsole = false;
    bool isDefaultMultimedia = false;
    bool isDefaultCommunications = false;
    HRESULT mixFormatHr = E_FAIL;
    std::string mixFormatDescription;
    ValidatedCaptureFormat validatedMixFormat{};
    bool mixFormatValid = false;
    HRESULT sessionEnumHr = E_FAIL;
    std::vector<AudioSessionRecord> sessions;
};

struct AudioTopologyReport {
    std::wstring defaultConsoleEndpointId;
    std::wstring defaultMultimediaEndpointId;
    std::wstring defaultCommunicationsEndpointId;
    std::vector<AudioEndpointRecord> endpoints;
    std::vector<ProcessEntry> processes;
    std::vector<std::string> warnings;
};

// Enumerates active eRender endpoints, mix formats, audio sessions, and process tree snapshot.
bool DiscoverAudioTopology(AudioTopologyReport& outReport, std::string& outError);

// Pure helpers on a ProcessEntry snapshot.
bool SnapshotProcesses(std::vector<ProcessEntry>& outProcesses, std::string& outError);
std::vector<uint32_t> FindPidsByExeName(const std::vector<ProcessEntry>& processes,
                                        const std::wstring& exeName);
std::vector<uint32_t> CollectProcessTreePids(const std::vector<ProcessEntry>& processes,
                                             uint32_t rootPid);

struct SessionVolumeIdentity {
    std::wstring endpointId;
    std::wstring sessionInstanceId;
    std::wstring exeFullPath;
    uint32_t processId = 0; // Informational only; NEVER used as match key during restore
    bool isMultiProcessSession = false;
    bool isSystemSoundsSession = false;
};

struct SessionVolumeState {
    bool mute = false;
    float masterVolume = 1.0f;
};

struct AudioSessionHandleInfo {
    SessionVolumeIdentity identity;
    SessionVolumeState state;
    uint32_t sessionState = 0;
    std::wstring sessionId;
    std::wstring displayName;
};

class IAudioSessionAccess {
public:
    virtual ~IAudioSessionAccess() = default;

    virtual bool EnumerateSessions(std::vector<AudioSessionHandleInfo>& outSessions,
                                   std::string& outError) = 0;

    virtual bool SetSessionMuteAndVolume(const SessionVolumeIdentity& target,
                                         bool mute,
                                         float masterVolume,
                                         std::string& outError) = 0;
};

// Production Win32 Core Audio session accessor (uses IMMDeviceEnumerator / IAudioSessionManager2).
class Win32AudioSessionAccess final : public IAudioSessionAccess {
public:
    bool EnumerateSessions(std::vector<AudioSessionHandleInfo>& outSessions,
                           std::string& outError) override;

    bool SetSessionMuteAndVolume(const SessionVolumeIdentity& target,
                                 bool mute,
                                 float masterVolume,
                                 std::string& outError) override;
};

struct JournalSessionEntry {
    std::wstring endpointId;
    std::wstring sessionInstanceId;
    std::wstring exeFullPath;
    uint32_t recordedPid = 0; // Informational only
    bool origMute = false;
    float origVolume = 1.0f;
    bool appliedMute = false;
    float appliedVolume = 1.0f;
};

struct JournalDocument {
    uint32_t version = 1;
    std::string createdUtcIso;
    std::vector<JournalSessionEntry> entries;
};

enum class RestoreStatus : uint8_t {
    Restored            = 0,
    SkippedSessionGone  = 1,
    SkippedUserModified = 2,
    FailedWrite         = 3,
};

const char* RestoreStatusToString(RestoreStatus status) noexcept;

struct JournalRestoreItemResult {
    JournalSessionEntry entry;
    RestoreStatus status = RestoreStatus::SkippedSessionGone;
    std::string detail;
};

struct JournalRestoreSummary {
    bool journalExisted = false;
    bool journalParsed = false;
    bool journalRemovedOrArchived = false;
    std::vector<JournalRestoreItemResult> items;
    std::string error;
};

class SessionMuteJournal {
public:
    static constexpr float kVolumeCompareTolerance = 1e-3f;

    // JSON serialization / deserialization (proper backslash/quote/control/UTF-8 escaping).
    static std::string SerializeJournalJson(const JournalDocument& doc);
    static bool DeserializeJournalJson(const std::string& jsonUtf8,
                                       JournalDocument& outDoc,
                                       std::string& outError);

    // Reads original session state(s), atomically writes journal via temp file +
    // MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), and ONLY THEN
    // applies SetSessionMuteAndVolume.
    static bool BeginApplyMute(IAudioSessionAccess& access,
                               const std::wstring& journalPath,
                               const std::vector<SessionVolumeIdentity>& targets,
                               bool appliedMute,
                               const float* optionalOverrideVolume,
                               JournalDocument& outDoc,
                               std::string& outError);

    static bool BeginApplyMute(IAudioSessionAccess& access,
                               const std::wstring& journalPath,
                               const SessionVolumeIdentity& target,
                               bool appliedMute,
                               float appliedVolume,
                               JournalDocument& outDoc,
                               std::string& outError);

    // Strictly matches endpointId + sessionInstanceId + case-insensitive exeFullPath.
    // Preserves user changes if current (mute, volume) differs from (appliedMute, appliedVolume) > 1e-3f.
    // Removes journal file after terminal decision for all entries.
    static bool RestoreFromJournal(IAudioSessionAccess& access,
                                   const std::wstring& journalPath,
                                   JournalRestoreSummary& outSummary);
};

// ============================================================================
// Section C: Process Loopback Activation, Format Probe & Capture Engine
// ============================================================================

struct LoopbackActivationResult {
    HRESULT asyncCallHr = E_FAIL;
    HRESULT activateResultHr = E_FAIL;
    bool timedOut = false;
    IAudioClient* audioClient = nullptr; // Caller owns one COM reference on success
};

// Activates a fresh IAudioClient for VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK using an agile
// IActivateAudioInterfaceCompletionHandler + IAgileObject. Ensures MTA COM on activation thread.
LoopbackActivationResult ActivateProcessLoopbackClient(DWORD targetPid,
                                                       PROCESS_LOOPBACK_MODE mode,
                                                       DWORD timeoutMs = 5000u);

struct FormatCandidateResult {
    uint32_t priorityIndex = 0;
    std::string label;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    CaptureSampleType sampleType = CaptureSampleType::Unknown;
    bool useAutoConvert = false;
    bool isExtensibleMixFormat = false;

    HRESULT activateHr = E_FAIL;
    HRESULT initializeHr = E_FAIL;
    uint32_t bufferFrames = 0;
    bool formatValidated = false;
    bool succeeded = false;
    std::string error;
    ValidatedCaptureFormat validatedFormat{};
    std::vector<uint8_t> rawWaveFormatBytes;
};

struct ProcessLoopbackProbeReport {
    DWORD targetPid = 0;
    PROCESS_LOOPBACK_MODE mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    HRESULT mixFormatActivateHr = E_FAIL;
    HRESULT getMixFormatHr = E_FAIL;
    std::string mixFormatDescription;
    bool mixFormatValidated = false;
    std::vector<FormatCandidateResult> candidates;

    bool hasSelectedCandidate = false;
    size_t selectedCandidateIndex = 0;
};

// Probes GetMixFormat (without assuming E_NOTIMPL) and activates a fresh IAudioClient
// for each candidate format so earlier Initialize attempts never poison later ones.
bool ProbeProcessLoopbackFormats(DWORD targetPid,
                                 PROCESS_LOOPBACK_MODE mode,
                                 ProcessLoopbackProbeReport& outReport,
                                 std::string& outError);

struct CaptureSessionOptions {
    DWORD targetPid = 0;
    PROCESS_LOOPBACK_MODE mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    double durationSec = 5.0;
    std::vector<uint8_t> waveFormatBytes; // WAVEFORMATEX (+ optional trailing cbSize bytes)
    bool useAutoConvert = true;
    std::wstring dumpWavPath;             // Optional stereo int16_t WAV output path
    HANDLE externalStopEvent = nullptr;   // Optional Win32 event to stop early
};

struct CaptureSessionStats {
    bool initialized = false;
    bool completed = false;
    bool mmcssRegistered = false;
    HRESULT activateHr = E_FAIL;
    HRESULT initializeHr = E_FAIL;
    HRESULT lastCaptureHr = S_OK;
    uint32_t bufferFrames = 0;
    ValidatedCaptureFormat validatedFormat{};

    double requestedDurationSec = 0.0;
    double elapsedSec = 0.0;

    uint64_t packetCount = 0;
    uint64_t totalFrames = 0;
    uint64_t nonZeroFrames = 0;
    uint64_t silentPacketCount = 0;
    uint64_t silentFrames = 0;
    uint64_t dataDiscontinuityCount = 0;
    uint64_t timestampErrorCount = 0;

    int32_t peakAbsInt16 = 0;
    double peakDbfs = -144.0;
    double rmsDbfs = -144.0;

    bool hasDevicePosition = false;
    uint64_t firstDevicePosition = 0;
    uint64_t lastDevicePosition = 0;

    bool hasQpcPosition = false;
    uint64_t firstQpcPosition = 0;
    uint64_t lastQpcPosition = 0;

    bool wavWritten = false;
    uint64_t wavPcmBytes = 0;
    std::string error;
};

bool RunCaptureSession(const CaptureSessionOptions& options,
                       CaptureSessionStats& outStats);

// ============================================================================
// Shared JSON & Utility Helpers (Exposed for CLI & Pure-Offline Self-Tests)
// ============================================================================

std::string WideToUtf8(const std::wstring& wstr);
std::wstring Utf8ToWide(const std::string& utf8);
std::string EscapeJsonString(const std::string& utf8);
bool UnescapeJsonString(const std::string& escaped, std::string& outUtf8, std::string& outError);
bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b);
std::wstring ExtractLeafFilename(const std::wstring& path);
std::string FormatHResultHex(HRESULT hr);
std::string GetCurrentUtcIsoString();
bool WriteTextFileAtomic(const std::wstring& path, const std::string& contentUtf8, std::string& outError);
bool ReadTextFileUtf8(const std::wstring& path, std::string& outContentUtf8, std::string& outError);

} // namespace staged
