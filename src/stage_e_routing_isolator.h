#pragma once

#include "stage_d_capture.h"

#include <mmdeviceapi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace stagee {

// ============================================================================
// Section 1: Auto-Route Mode, SWD Endpoint ID Helpers & Silent Sink Detector
// ============================================================================

enum class AutoRouteMode : uint8_t {
    Auto   = 0, // Automatically route target app to virtual silent sink on start & restore on stop/exit
    Manual = 1, // Manual split-endpoint routing via Windows Volume Mixer
};

const char* AutoRouteModeToCode(AutoRouteMode mode) noexcept;
bool ParseAutoRouteModeCode(const std::string& codeUtf8, AutoRouteMode& outMode) noexcept;

enum class VirtualSinkPresence : uint8_t {
    NotInstalled               = 0, // No virtual silent sink endpoint detected
    ActiveReady                = 1, // Virtual silent sink active and distinct from default & GTA IV
    VirtualSinkIsSystemDefault = 2, // Hazard: virtual sink is currently set as system default output
};

const char* VirtualSinkPresenceName(VirtualSinkPresence presence) noexcept;

enum class AutoRouteState : uint8_t {
    DisabledManual            = 0,
    UnsupportedOsPolicy       = 1,
    NoVirtualSinkInstalled    = 2,
    VirtualSinkIsSystemDefault = 3,
    WaitingForTargetProcess   = 4,
    AutoIsolatedOk            = 5,
    PartiallyIsolated         = 6,
    PolicyWriteFailed         = 7,
};

const char* AutoRouteStateName(AutoRouteState state) noexcept;

struct SilentSinkCandidate {
    std::wstring endpointId;
    std::wstring friendlyName;
    int32_t priority = 0; // 100 = Steam Streaming Speakers, 80..70 = VB-Cable / Virtual Audio Cable
    bool isVirtual = false;
    bool isConflictEndpoint = false;
};

struct SilentSinkReport {
    VirtualSinkPresence presence = VirtualSinkPresence::NotInstalled;
    bool hasSelectableSink = false;
    std::wstring selectedEndpointId;
    std::wstring selectedFriendlyName;
    int32_t selectedPriority = 0;
    uint32_t virtualEndpointCount = 0u;
    std::vector<SilentSinkCandidate> candidates;
};

// Known virtual render sink classification & SWD interface formatting helpers.
bool IsKnownVirtualSilentSinkFriendlyName(const std::wstring& friendlyName, int32_t& outPriority) noexcept;

// Converts CoreAudio endpoint ID "{0.0.0.00000000}.{guid}" into Windows AudioPolicyConfig SWD string
// "\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{e6327cad-dcec-4949-ae8a-991e976a79d2}" (idempotent).
std::wstring FormatSwdAudioRenderInterfaceId(const std::wstring& endpointId);

// Extracts CoreAudio endpoint ID "{0.0.0.00000000}.{guid}" from a policy SWD interface string.
std::wstring ExtractEndpointIdFromPolicyString(const std::wstring& policyId);

bool ArePolicyEndpointStringsEqual(const std::wstring& a, const std::wstring& b) noexcept;

// Inspects active CoreAudio topology and classifies available render-only virtual silent sinks.
// Rejects system default endpoints and any endpoint hosting GTAIV.exe.
SilentSinkReport EvaluateSilentSinks(const staged::AudioTopologyReport& topo);

// ============================================================================
// Section 2: Per-Process Endpoint Policy Access (AudioPolicyConfig COM)
// ============================================================================

struct ProcessPolicyIdentity {
    uint32_t pid = 0u;
    std::wstring identityKey; // "aumid:<lower_aumid>" for MSIX Store apps, or "exe:<lower_full_path>"
    std::wstring exeLeaf;
    std::wstring exeFullPath;
    std::wstring aumid;
    bool hasActiveSession = false;
    bool hasAnySession = false;
};

// Constructs canonical persisted policy identity key for deduplicating multi-process apps
// while separating distinct MSIX AUMIDs (e.g., AppleMusic!App vs AMPLibraryAgent!LibraryServer).
std::wstring ComputeProcessPolicyIdentityKey(const std::wstring& exeFullPath,
                                             const std::wstring& exeLeaf,
                                             const std::wstring& aumid);

class IAudioEndpointPolicyAccess {
public:
    virtual ~IAudioEndpointPolicyAccess() = default;

    // Returns false when AudioPolicyConfig activation factory / IID is unavailable on the OS.
    virtual bool IsSupported(std::string& outReason) = 0;

    // Returns current process policy identities matching candidate PIDs (or all relevant target processes).
    // Empty filter means enumerate all alive non-forbidden processes/sessions where policy lookup succeeds.
    virtual bool EnumeratePolicyIdentities(const staged::AudioTopologyReport& topo,
                                           std::vector<ProcessPolicyIdentity>& outIdentities,
                                           std::string& outError) = 0;

    // Queries persisted per-process default endpoint ID ("{0.0.0.00000000}.{...}" or "" for system default).
    virtual bool GetPersistedDefaultEndpoint(uint32_t pid,
                                             EDataFlow flow,
                                             ERole role,
                                             std::wstring& outEndpointId,
                                             HRESULT& outHr,
                                             std::string& outError) = 0;

    // Sets persisted per-process default endpoint ID (pass "" to restore system default output).
    virtual bool SetPersistedDefaultEndpoint(uint32_t pid,
                                             EDataFlow flow,
                                             ERole role,
                                             const std::wstring& endpointId,
                                             HRESULT& outHr,
                                             std::string& outError) = 0;
};

// Production WinRT/COM Windows.Media.Internal.AudioPolicyConfig accessor (verified on Windows 11 24H2).
class Win32AudioPolicyConfigAccess final : public IAudioEndpointPolicyAccess {
public:
    Win32AudioPolicyConfigAccess() = default;
    ~Win32AudioPolicyConfigAccess() override;

    Win32AudioPolicyConfigAccess(const Win32AudioPolicyConfigAccess&) = delete;
    Win32AudioPolicyConfigAccess& operator=(const Win32AudioPolicyConfigAccess&) = delete;

    bool IsSupported(std::string& outReason) override;

    bool EnumeratePolicyIdentities(const staged::AudioTopologyReport& topo,
                                   std::vector<ProcessPolicyIdentity>& outIdentities,
                                   std::string& outError) override;

    bool GetPersistedDefaultEndpoint(uint32_t pid,
                                     EDataFlow flow,
                                     ERole role,
                                     std::wstring& outEndpointId,
                                     HRESULT& outHr,
                                     std::string& outError) override;

    bool SetPersistedDefaultEndpoint(uint32_t pid,
                                     EDataFlow flow,
                                     ERole role,
                                     const std::wstring& endpointId,
                                     HRESULT& outHr,
                                     std::string& outError) override;

private:
    bool EnsureFactory(std::string& outReason) noexcept;
    void ReleaseFactory() noexcept;

    bool probed_ = false;
    bool supported_ = false;
    bool comInitialized_ = false;
    DWORD comInitThreadId_ = 0u;
    std::string cachedError_;
    void* factory_ = nullptr; // IAudioPolicyConfigFactory* for {ab3d4648-e242-459f-b02f-541c70306324}
};

// ============================================================================
// Section 3: Crash-Safe Route Journal & Automatic Isolation Controller
// ============================================================================

struct RouteJournalEntry {
    std::wstring identityKey;
    std::wstring exeLeaf;
    std::wstring exeFullPath;
    std::wstring aumid;
    std::wstring origConsoleEndpointId;     // Empty string = System Default
    std::wstring origMultimediaEndpointId;  // Empty string = System Default
    std::wstring appliedSinkEndpointId;
    std::wstring appliedSinkName;
    uint32_t lastAppliedPid = 0u;
    bool pendingRestore = false;
};

struct RouteJournalDocument {
    uint32_t version = 1u;
    std::string createdUtcIso;
    std::vector<RouteJournalEntry> entries;
};

enum class RouteRestoreItemStatus : uint8_t {
    Restored            = 0,
    SkippedUserModified = 1,
    DeferredProcessGone = 2,
    FailedWrite         = 3,
};

const char* RouteRestoreItemStatusName(RouteRestoreItemStatus st) noexcept;

struct RouteRestoreItemResult {
    std::wstring identityKey;
    std::wstring exeLeaf;
    RouteRestoreItemStatus status = RouteRestoreItemStatus::DeferredProcessGone;
    std::string detail;
};

struct RouteRestoreSummary {
    bool journalExisted = false;
    bool journalParsed = false;
    bool journalEmptyAfterRestore = true;
    uint32_t restoredCount = 0u;
    uint32_t skippedUserModifiedCount = 0u;
    uint32_t deferredCount = 0u;
    uint32_t failedCount = 0u;
    std::vector<RouteRestoreItemResult> items;
    std::string error;
};

// Resolves '<settingsDir>\iv_apple_music_radio.route_journal.json' (or next to executable when empty).
std::wstring ResolveRouteJournalPathForSettings(const std::wstring& settingsFilePath,
                                                const std::wstring& exePathOverride = L"");

class EndpointRouteJournal {
public:
    static std::string SerializeJson(const RouteJournalDocument& doc);
    static bool DeserializeJson(const std::string& jsonUtf8,
                                RouteJournalDocument& outDoc,
                                std::string& outError);

    static bool Load(const std::wstring& journalPath,
                     RouteJournalDocument& outDoc,
                     bool& outExisted,
                     std::string& outError);

    static bool SaveAtomicOrRemoveIfEmpty(const std::wstring& journalPath,
                                          const RouteJournalDocument& doc,
                                          std::string& outError);
};

struct AutoRouteTargetSpec {
    bool isAppleMusicPreset = true;
    std::wstring customExeName;
    std::wstring customExePath;
    uint32_t captureTargetPid = 0u;
};

struct AutoRouteStatus {
    AutoRouteMode mode = AutoRouteMode::Auto;
    AutoRouteState state = AutoRouteState::DisabledManual;
    VirtualSinkPresence virtualSinkPresence = VirtualSinkPresence::NotInstalled;
    std::string sinkEndpointIdUtf8;
    std::string sinkFriendlyNameUtf8;
    uint32_t isolatedIdentityCount = 0u;
    uint32_t pendingRestoreCount = 0u;
    bool policyApiSupported = true;
    std::string lastRestoreSummaryUtf8;
    std::string lastErrorUtf8;
};

// Stateful controller managing pre-isolation journaling, hot source-switch restoration,
// exit/stop restoration, and crash-safe startup recovery.
class AutoRouteController {
public:
    // Replays any orphaned route journal left by a previous crash before starting capture.
    RouteRestoreSummary RecoverOnStartup(IAudioEndpointPolicyAccess& policy,
                                         const std::wstring& journalPath,
                                         const staged::AudioTopologyReport* optTopo = nullptr);

    // Reconciles current target process routing with AutoRouteMode + detected virtual silent sinks.
    AutoRouteStatus ReconcileDuringBridge(IAudioEndpointPolicyAccess& policy,
                                          const std::wstring& journalPath,
                                          const staged::AudioTopologyReport& topo,
                                          const AutoRouteTargetSpec& target,
                                          AutoRouteMode mode);

    // Restores all active / pending entries back to their recorded original endpoints.
    // Keeps entries whose target process has already exited marked pendingRestore = true if deferIfProcessGone.
    RouteRestoreSummary RestoreAllRoutes(IAudioEndpointPolicyAccess& policy,
                                         const std::wstring& journalPath,
                                         const staged::AudioTopologyReport* optTopo = nullptr,
                                         bool deferIfProcessGone = true);

    // Clears dead pending entries from disk only on explicit user reset (returns count cleared).
    static uint32_t CountPendingJournalEntries(const std::wstring& journalPath);
};

// ============================================================================
// Section 4: Bundled WHQL SteamStreamingSpeakers Installer & Anti-Hijack Guard
// ============================================================================

struct VirtualSinkDriverPackage {
    bool found = false;
    bool integrityValid = false;
    std::wstring directory;
    std::wstring infPath;
    std::wstring sysPath;
    std::wstring catPath;
    std::string errorDetail;
};

// Resolves assets/drivers/SteamStreamingSpeakers relative to exeDir or repo root and verifies
// all three pinned SHA-256 digests before any PnP operation is allowed.
VirtualSinkDriverPackage LocateAndVerifyVirtualSinkPackage(const std::wstring& explicitDriverDir = L"",
                                                           const std::wstring& exePathOverride = L"");

struct VirtualSinkInstallResult {
    bool succeeded = false;
    bool requiresReboot = false;
    bool alreadyInstalled = false;
    bool elevated = false;
    bool defaultsPreserved = false;
    std::string action; // "install" | "uninstall"
    std::string defaultRenderBeforeUtf8;
    std::string defaultRenderAfterUtf8;
    std::string defaultCaptureBeforeUtf8;
    std::string defaultCaptureAfterUtf8;
    std::string virtualSinkEndpointIdUtf8;
    std::string virtualSinkFriendlyNameUtf8;
    std::string detailUtf8;
};

// Checks whether the Steam Streaming Speakers ROOT devnode or driver files are installed.
bool IsVirtualAudioSinkDevicePresent();

// Installs (or repairs) ROOT\SteamStreamingSpeakers from the verified WHQL driver package while
// preserving existing system default playback/capture endpoints. Requires Administrator privileges.
VirtualSinkInstallResult InstallVirtualAudioSink(const std::wstring& explicitDriverDir = L"",
                                                 const std::wstring& reportJsonPath = L"");

// Uninstalls ROOT\SteamStreamingSpeakers PnP devnodes and published OEM INF packages cleanly.
// Requires Administrator privileges.
VirtualSinkInstallResult UninstallVirtualAudioSink(const std::wstring& reportJsonPath = L"");

std::string SerializeVirtualSinkInstallResultJson(const VirtualSinkInstallResult& res);

} // namespace stagee
