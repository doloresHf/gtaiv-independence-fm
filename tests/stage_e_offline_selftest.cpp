#include "stage_e_diagnostics.h"
#include "stage_e_engine.h"
#include "stage_e_routing_isolator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int g_totalChecks = 0;
int g_failedChecks = 0;

void CheckImpl(bool cond, const char* expr, const char* file, int line) {
    ++g_totalChecks;
    if (!cond) {
        ++g_failedChecks;
        std::fprintf(stderr, "[FAIL] %s:%d: %s\n", file, line, expr);
    }
}

#define CHECK(expr) CheckImpl(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

bool FileExistsW(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0u);
}

void DeleteDirectoryTreeTestOnly(const std::wstring& dirPath) {
    if (dirPath.empty()) return;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dirPath + L"\\*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name(fd.cFileName);
            if (name == L"." || name == L"..") continue;
            const std::wstring child = dirPath + L"\\" + name;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
                DeleteDirectoryTreeTestOnly(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(hFind, &fd) != FALSE);
        FindClose(hFind);
    }
    RemoveDirectoryW(dirPath.c_str());
}

std::wstring MakeUniqueTempDir(const wchar_t* tag) {
    wchar_t tmpBase[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(tmpBase)), tmpBase);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    wchar_t dirBuf[MAX_PATH * 2]{};
    std::swprintf(
        dirBuf,
        std::size(dirBuf),
        L"%lsgtaiv_stage_e1_%ls_%lu_%llu",
        tmpBase,
        tag,
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(qpc.QuadPart));
    CreateDirectoryW(dirBuf, nullptr);
    return dirBuf;
}

// ============================================================================
// Suite 1: RollingLevelMeter (1s / 5s Windows, INT16_MIN, Wall-Clock Decay)
// ============================================================================

void TestRollingLevelMeter() {
    std::printf("[Suite 1] RollingLevelMeter (1s/5s windows, INT16_MIN, wall-clock decay)...\n");

    stagee::RollingLevelMeter meter;
    const uint64_t t0 = 100000000ULL; // 10.0 s
    meter.Reset(t0);

    // 1. INT16_MIN (-32768) full-scale square wave for 200 ms (20 * 10 ms packets of 480 stereo frames)
    std::vector<int16_t> fullScale(480u * 2u, static_cast<int16_t>(-32768));
    uint64_t t = t0;
    for (int i = 0; i < 20; ++i) {
        t += 100000ULL; // +10 ms
        meter.IngestStereoInt16(t, fullScale.data(), 480u, false);
    }
    const stagee::PcmLevelSnapshot sFull = meter.Snapshot(t);
    CHECK(sFull.hasAnyPacket);
    CHECK(sFull.hasRecentPackets);
    CHECK(sFull.frames1s == 9600ULL);
    CHECK(sFull.audibleFrames1s == 9600ULL);
    CHECK(sFull.peakAbs1s == 32768);
    CHECK(std::abs(sFull.peakDbfs1s - 0.0) < 0.01);
    CHECK(std::abs(sFull.rmsDbfs1s - 0.0) < 0.01);

    // 2. Advance wall-clock +1.2 s with ZERO packets -> 1s window decays to -144 dBFS while 5s retains history
    const uint64_t tAfterPause = t + 12000000ULL; // +1.2 s
    const stagee::PcmLevelSnapshot sPaused1s = meter.Snapshot(tAfterPause);
    CHECK(!sPaused1s.hasRecentPackets);
    CHECK(sPaused1s.msSinceLastPacket == 1200ULL);
    CHECK(sPaused1s.frames1s == 0ULL);
    CHECK(sPaused1s.peakAbs1s == 0);
    CHECK(sPaused1s.peakDbfs1s <= -143.9);
    CHECK(sPaused1s.frames5s == 9600ULL);
    CHECK(sPaused1s.peakAbs5s == 32768);

    // 3. Advance wall-clock +5.5 s total -> 5s window also fully cleared
    const stagee::PcmLevelSnapshot sCleared5s = meter.Snapshot(t + 55000000ULL);
    CHECK(sCleared5s.frames5s == 0ULL);
    CHECK(sCleared5s.peakAbs5s == 0);

    // 4. Half-scale sine wave (peak = 16384 -> -6.02 dBFS, RMS -> -9.03 dBFS) + silentFlag check
    meter.Reset(t0);
    std::vector<int16_t> sineWave(4800u * 2u, 0);
    constexpr double kTwoPi = 6.28318530717958647692;
    for (uint32_t i = 0u; i < 4800u; ++i) {
        const int16_t sample = static_cast<int16_t>(std::llround(16384.0 * std::sin(kTwoPi * 1000.0 * i / 48000.0)));
        sineWave[i * 2u] = sample;
        sineWave[i * 2u + 1u] = sample;
    }
    meter.IngestStereoInt16(t0 + 1000000ULL, sineWave.data(), 4800u, false);
    const stagee::PcmLevelSnapshot sSine = meter.Snapshot(t0 + 1000000ULL);
    CHECK(std::abs(sSine.peakDbfs1s - (-6.0206)) < 0.05);
    CHECK(std::abs(sSine.rmsDbfs1s - (-9.0309)) < 0.08);

    meter.Reset(t0);
    meter.IngestStereoInt16(t0 + 100000ULL, fullScale.data(), 480u, true /* silentFlag */);
    const stagee::PcmLevelSnapshot sSilentFlag = meter.Snapshot(t0 + 100000ULL);
    CHECK(sSilentFlag.frames1s == 480ULL);
    CHECK(sSilentFlag.silentFlagFrames1s == 480ULL);
    CHECK(sSilentFlag.nonZeroFrames1s == 0ULL);
    CHECK(sSilentFlag.audibleFrames1s == 0ULL);
}

// ============================================================================
// Suite 2: Split-Endpoint Routing Drift, Unplug Fallback & Tombstone Disambiguation
// ============================================================================

staged::AudioEndpointRecord MakeMockEndpoint(const wchar_t* id,
                                             const wchar_t* name,
                                             bool isDefault) {
    staged::AudioEndpointRecord ep;
    ep.endpointId = id;
    ep.friendlyName = name;
    ep.isDefaultConsole = isDefault;
    ep.isDefaultMultimedia = isDefault;
    return ep;
}

staged::AudioSessionRecord MakeMockSession(const staged::AudioEndpointRecord& ep,
                                           uint32_t pid,
                                           const wchar_t* exeLeaf,
                                           const wchar_t* fullPath,
                                           uint32_t state,
                                           bool muted = false,
                                           float vol = 1.0f) {
    staged::AudioSessionRecord s;
    s.endpointId = ep.endpointId;
    s.endpointFriendlyName = ep.friendlyName;
    s.isDefaultConsoleEndpoint = ep.isDefaultConsole;
    s.isDefaultMultimediaEndpoint = ep.isDefaultMultimedia;
    s.processId = pid;
    s.exeName = exeLeaf;
    s.exeFullPath = fullPath;
    s.sessionState = state;
    s.muted = muted;
    s.masterVolume = vol;
    s.volumeValid = true;
    return s;
}

void TestRoutingIsolationClassifier() {
    std::printf("[Suite 2] Split-Endpoint Routing Isolation & Unplug Fallback Matrix...\n");

    const wchar_t* kStoreAmpPath =
        L"C:\\Program Files\\WindowsApps\\AppleInc.AppleMusicWin_1.1284.20263.0_x64__nzyj5cx40ttqa\\AMPLibraryAgent.exe";

    // 2a. Normal split-endpoint isolation + co-existing VLC & Edge active sessions
    stagee::RoutingIsolatorTracker tracker;
    {
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main_bt";
        topo.defaultMultimediaEndpointId = L"ep_main_bt";

        auto epMain = MakeMockEndpoint(L"ep_main_bt", L"耳机 (Test Headphones)", true);
        epMain.sessions.push_back(MakeMockSession(epMain, 5000u, L"GTAIV.exe", L"E:\\GTAIV\\GTAIV.exe", AudioSessionStateActive));
        epMain.sessions.push_back(MakeMockSession(epMain, 6100u, L"vlc.exe", L"C:\\VLC\\vlc.exe", AudioSessionStateActive));
        epMain.sessions.push_back(MakeMockSession(epMain, 6200u, L"msedge.exe", L"C:\\Edge\\msedge.exe", AudioSessionStateActive));

        auto epSub = MakeMockEndpoint(L"ep_sub_dac", L"扬声器 (2- Test USB Audio)", false);
        epSub.sessions.push_back(MakeMockSession(epSub, 4242u, L"AMPLibraryAgent.exe", kStoreAmpPath, AudioSessionStateActive));

        topo.endpoints = { epMain, epSub };
        topo.processes = {
            { 4240u, 1u, L"AppleMusic.exe", L"C:\\WindowsApps\\AppleMusic.exe" },
            { 4242u, 4240u, L"AMPLibraryAgent.exe", kStoreAmpPath },
            { 5000u, 1u, L"GTAIV.exe", L"E:\\GTAIV\\GTAIV.exe" },
        };

        const stagee::RoutingDiagnostic d = stagee::EvaluateRoutingIsolation(topo, 4242u, L"AMPLibraryAgent.exe", tracker);
        CHECK(d.state == stagee::RoutingIsolationState::IsolatedOk);
        CHECK(d.targetHasSession && d.targetSessionActive && !d.targetSessionMuted);
        CHECK(d.otherActiveMediaAppsUtf8.size() == 2u);
        CHECK(d.otherActiveMediaAppsUtf8[0] == "msedge.exe");
        CHECK(d.otherActiveMediaAppsUtf8[1] == "vlc.exe");
    }

    // 2b. Unplug secondary USB DAC -> endpoint disappears & AMPLibraryAgent falls back to default output
    {
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main_bt";
        topo.defaultMultimediaEndpointId = L"ep_main_bt";

        auto epMain = MakeMockEndpoint(L"ep_main_bt", L"耳机 (Test Headphones)", true);
        epMain.sessions.push_back(MakeMockSession(epMain, 5000u, L"GTAIV.exe", L"E:\\GTAIV\\GTAIV.exe", AudioSessionStateActive));
        epMain.sessions.push_back(MakeMockSession(epMain, 4242u, L"AMPLibraryAgent.exe", kStoreAmpPath, AudioSessionStateActive));
        topo.endpoints = { epMain };

        const stagee::RoutingDiagnostic d = stagee::EvaluateRoutingIsolation(topo, 4242u, L"AMPLibraryAgent.exe", tracker);
        CHECK(d.state == stagee::RoutingIsolationState::SecondaryEndpointLostFallback);
        CHECK(d.lostEndpointNameUtf8 == "扬声器 (2- Test USB Audio)");
        CHECK(d.targetEndpointNameUtf8 == "耳机 (Test Headphones)");
    }

    // 2c. Re-plug secondary DAC & switch in Windows Volume Mixer (leaving stale Inactive tombstone on ep_main_bt)
    {
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main_bt";
        topo.defaultMultimediaEndpointId = L"ep_main_bt";

        auto epMain = MakeMockEndpoint(L"ep_main_bt", L"耳机 (Test Headphones)", true);
        epMain.sessions.push_back(MakeMockSession(epMain, 4242u, L"AMPLibraryAgent.exe", kStoreAmpPath, AudioSessionStateInactive));

        auto epSub = MakeMockEndpoint(L"ep_sub_dac", L"扬声器 (2- Test USB Audio)", false);
        epSub.sessions.push_back(MakeMockSession(epSub, 4242u, L"AMPLibraryAgent.exe", kStoreAmpPath, AudioSessionStateActive));
        topo.endpoints = { epMain, epSub };

        const stagee::RoutingDiagnostic d = stagee::EvaluateRoutingIsolation(topo, 4242u, L"AMPLibraryAgent.exe", tracker);
        CHECK(d.state == stagee::RoutingIsolationState::IsolatedOk);
        CHECK(d.targetEndpointNameUtf8 == "扬声器 (2- Test USB Audio)");

        // 2d. Pause Apple Music while stale tombstone still exists on ep_main_bt -> stays IsolatedOk via lastActiveEndpointId
        topo.endpoints[1].sessions[0].sessionState = AudioSessionStateInactive;
        const stagee::RoutingDiagnostic dPaused = stagee::EvaluateRoutingIsolation(topo, 4242u, L"AMPLibraryAgent.exe", tracker);
        CHECK(dPaused.state == stagee::RoutingIsolationState::IsolatedOk);
        CHECK(!dPaused.targetSessionActive);
    }

    // 2e. Fresh start with conflict on default output + Volume Mixer Mute detection
    {
        stagee::RoutingIsolatorTracker freshTracker;
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main_bt";
        topo.defaultMultimediaEndpointId = L"ep_main_bt";

        auto epMain = MakeMockEndpoint(L"ep_main_bt", L"耳机 (Test Headphones)", true);
        epMain.sessions.push_back(MakeMockSession(epMain, 4242u, L"AMPLibraryAgent.exe", kStoreAmpPath, AudioSessionStateActive, true /* muted */, 0.8f));
        auto epSub = MakeMockEndpoint(L"ep_sub_dac", L"扬声器 (2- Test USB Audio)", false);
        topo.endpoints = { epMain, epSub };

        const stagee::RoutingDiagnostic d = stagee::EvaluateRoutingIsolation(topo, 4242u, L"AMPLibraryAgent.exe", freshTracker);
        CHECK(d.state == stagee::RoutingIsolationState::ConflictOnDefaultOutput);
        CHECK(d.targetSessionMuted);
    }

    // 2f. Single endpoint only before target opens any session
    {
        stagee::RoutingIsolatorTracker freshTracker;
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main_bt";
        topo.defaultMultimediaEndpointId = L"ep_main_bt";
        topo.endpoints = { MakeMockEndpoint(L"ep_main_bt", L"耳机 (Test Headphones)", true) };

        const stagee::RoutingDiagnostic d = stagee::EvaluateRoutingIsolation(topo, 0u, L"AMPLibraryAgent.exe", freshTracker);
        CHECK(d.state == stagee::RoutingIsolationState::SingleEndpointOnly);
    }
}

// ============================================================================
// Suite 3: Silence Reason Hysteresis (Pause vs Brief Gap vs >=5s Atmos vs Mute)
// ============================================================================

void TestSilenceAndPipelineClassifier() {
    std::printf("[Suite 3] Silence & Pipeline Diagnostics State Machine...\n");

    stagee::BridgeStatusTracker tracker;
    stagee::RollingLevelMeter meter;
    const uint64_t origin = 10000000ULL;
    meter.Reset(origin);

    // 3a. BridgeStopped with verified clean shutdown drain
    {
        stagee::BridgeDiagnosticInput in{};
        in.engineRunning = false;
        in.lastShutdownVerified = true;
        in.lastShutdownClean = true;
        in.lastShutdownQueuedFrames = 48000ULL;
        in.lastShutdownCommittedFrames = 48000ULL;
        in.lastShutdownHoldbackFrames = 0u;

        const stagee::BridgeDiagnosticReport r = stagee::EvaluateBridgeDiagnostics(in, tracker);
        CHECK(r.headline == stagee::PipelineHeadlineState::BridgeStopped);
        CHECK(r.badgeColor == stagee::TrayBadgeColor::Gray);
        CHECK(r.headlineDetailUtf8.find("H=0, C=Q") != std::string::npos);
    }

    // 3b. WaitingForAppleMusic vs ReconnectingTarget
    {
        stagee::BridgeDiagnosticInput in{};
        in.engineRunning = true;
        in.captureStreamActive = false;
        in.streamRestartCount = 1ULL;
        in.msSinceLastStreamEnd = 400ULL;
        CHECK(stagee::EvaluateBridgeDiagnostics(in, tracker).headline == stagee::PipelineHeadlineState::ReconnectingTarget);

        in.msSinceLastStreamEnd = 4500ULL;
        CHECK(stagee::EvaluateBridgeDiagnostics(in, tracker).headline == stagee::PipelineHeadlineState::WaitingForAppleMusic);
    }

    // 3c. Audible music -> StreamingWaitingForGame -> StreamingInGame -> Holdover -> BriefQuiet -> >=5s SustainedZeroAudioWarning
    std::vector<int16_t> audibleChunk(480u * 2u, 4096);
    std::vector<int16_t> zeroChunk(480u * 2u, 0);

    uint64_t now = origin + 1000000ULL;
    for (int i = 0; i < 10; ++i) {
        now += 100000ULL; // +10 ms
        meter.IngestStereoInt16(now, audibleChunk.data(), 480u, false);
    }

    stagee::BridgeDiagnosticInput baseIn{};
    baseIn.engineRunning = true;
    baseIn.captureStreamActive = true;
    baseIn.captureTargetPid = 4242u;
    baseIn.captureTargetExeUtf8 = "AMPLibraryAgent.exe";
    baseIn.routing.state = stagee::RoutingIsolationState::IsolatedOk;
    baseIn.routing.targetHasSession = true;
    baseIn.routing.targetSessionActive = true;

    {
        baseIn.nowQpc100ns = now;
        baseIn.level = meter.Snapshot(now);
        baseIn.ipc.consumerActiveReaders = 0u;
        const auto rWaitGame = stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
        CHECK(rWaitGame.headline == stagee::PipelineHeadlineState::StreamingWaitingForGame);
        CHECK(rWaitGame.badgeColor == stagee::TrayBadgeColor::Green);

        baseIn.ipc.consumerActiveReaders = 1u;
        const auto rInGame = stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
        CHECK(rInGame.headline == stagee::PipelineHeadlineState::StreamingInGame);
        CHECK(rInGame.badgeColor == stagee::TrayBadgeColor::Green);

        // Routing conflict flips tray badge to Red while preserving StreamingInGame headline
        baseIn.routing.state = stagee::RoutingIsolationState::ConflictOnDefaultOutput;
        const auto rConflict = stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
        CHECK(rConflict.headline == stagee::PipelineHeadlineState::StreamingInGame);
        CHECK(rConflict.hasRoutingConflict);
        CHECK(rConflict.badgeColor == stagee::TrayBadgeColor::Red);
        baseIn.routing.state = stagee::RoutingIsolationState::IsolatedOk;
    }

    // Feed continuous zero-amplitude packets while session remains Active
    // After 800 ms (< 1.5 s holdover) -> still StreamingInGame (anti-flicker)
    for (int i = 0; i < 80; ++i) {
        now += 100000ULL;
        meter.IngestStereoInt16(now, zeroChunk.data(), 480u, false);
    }
    baseIn.nowQpc100ns = now;
    baseIn.level = meter.Snapshot(now);
    CHECK(stagee::EvaluateBridgeDiagnostics(baseIn, tracker).headline == stagee::PipelineHeadlineState::StreamingInGame);

    // Advance another 1.5 s of silence (total ~2.3 s silent, < 5.0 s) -> BriefQuietPassage (NOT error!)
    for (int i = 0; i < 150; ++i) {
        now += 100000ULL;
        meter.IngestStereoInt16(now, zeroChunk.data(), 480u, false);
    }
    baseIn.nowQpc100ns = now;
    baseIn.level = meter.Snapshot(now);
    {
        const auto rQuiet = stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
        CHECK(rQuiet.headline == stagee::PipelineHeadlineState::BriefQuietPassage);
        CHECK(rQuiet.badgeColor == stagee::TrayBadgeColor::Green);
    }

    // Advance another 3.0 s of silence (total ~5.3 s >= 5.0 s) -> SustainedZeroAudioWarning (Dolby Atmos / 0-vol hint!)
    for (int i = 0; i < 300; ++i) {
        now += 100000ULL;
        meter.IngestStereoInt16(now, zeroChunk.data(), 480u, false);
    }
    baseIn.nowQpc100ns = now;
    baseIn.level = meter.Snapshot(now);
    {
        const auto rWarn = stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
        CHECK(rWarn.headline == stagee::PipelineHeadlineState::SustainedZeroAudioWarning);
        CHECK(rWarn.badgeColor == stagee::TrayBadgeColor::Red);
        CHECK(rWarn.headlineDetailUtf8.find("Dolby Atmos") != std::string::npos);
    }

    // 1 audible packet immediately clears SustainedZeroAudioWarning
    now += 100000ULL;
    meter.IngestStereoInt16(now, audibleChunk.data(), 480u, false);
    baseIn.nowQpc100ns = now;
    baseIn.level = meter.Snapshot(now);
    {
        const auto rRecovered = stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
        CHECK(rRecovered.headline == stagee::PipelineHeadlineState::StreamingInGame);
        CHECK(rRecovered.continuousActiveSilentMs == 0ULL);
    }

    // 3d. Normal Pause (targetSessionActive == false and packets stop for 45 seconds) -> AppleMusicPausedOrIdle
    now += 450000000ULL; // +45.0 s
    baseIn.nowQpc100ns = now;
    baseIn.routing.targetSessionActive = false;
    baseIn.level = meter.Snapshot(now);
    {
        const auto rPaused = stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
        CHECK(rPaused.headline == stagee::PipelineHeadlineState::AppleMusicPausedOrIdle);
        CHECK(rPaused.badgeColor == stagee::TrayBadgeColor::Blue);
    }

    // 3d2. Live Windows 11 Process Loopback contract (Gate E1a regression):
    // Even while paused/idle (targetSessionActive == false), Process Loopback pumps 100 pkt/s
    // (48,000 frames/s) of unflagged zero-amplitude PCM. Must remain AppleMusicPausedOrIdle (Blue)
    // across 30s and NEVER trigger false BriefQuietPassage or SustainedZeroAudioWarning.
    for (int sec = 0; sec < 30; ++sec) {
        for (int p = 0; p < 100; ++p) {
            now += 100000ULL; // +10 ms
            meter.IngestStereoInt16(now, zeroChunk.data(), 480u, false);
        }
        baseIn.nowQpc100ns = now;
        baseIn.routing.state = (sec < 10)
            ? stagee::RoutingIsolationState::WaitingForTargetSession
            : stagee::RoutingIsolationState::IsolatedOk;
        baseIn.routing.targetSessionActive = false;
        baseIn.level = meter.Snapshot(now);
        const auto rPausedZeroStream = stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
        CHECK(rPausedZeroStream.headline == stagee::PipelineHeadlineState::AppleMusicPausedOrIdle);
        CHECK(rPausedZeroStream.badgeColor == stagee::TrayBadgeColor::Blue);
        CHECK(rPausedZeroStream.continuousActiveSilentMs == 0ULL);
    }

    // When user clicks Play on Stereo, first audible packet arrives BEFORE 1Hz topology flips Active -> immediate StreamingInGame
    now += 100000ULL;
    meter.IngestStereoInt16(now, audibleChunk.data(), 480u, false);
    baseIn.nowQpc100ns = now;
    baseIn.routing.targetSessionActive = false;
    baseIn.level = meter.Snapshot(now);
    CHECK(stagee::EvaluateBridgeDiagnostics(baseIn, tracker).headline ==
          stagee::PipelineHeadlineState::StreamingInGame);

    // Pause again with 10s of continuous zero packets -> AppleMusicPausedOrIdle, then start Dolby Atmos (session flips Active):
    // timer must anchor to session activation (not inherit the 10s pause!).
    for (int i = 0; i < 1000; ++i) {
        now += 100000ULL;
        meter.IngestStereoInt16(now, zeroChunk.data(), 480u, false);
    }
    baseIn.nowQpc100ns = now;
    baseIn.routing.targetSessionActive = false;
    baseIn.level = meter.Snapshot(now);
    CHECK(stagee::EvaluateBridgeDiagnostics(baseIn, tracker).headline ==
          stagee::PipelineHeadlineState::AppleMusicPausedOrIdle);

    // Dolby Atmos starts (session becomes Active while packets remain zero): at +2.0s -> BriefQuietPassage, at +5.2s -> SustainedZeroAudioWarning
    baseIn.routing.targetSessionActive = true;
    for (int i = 0; i < 200; ++i) {
        now += 100000ULL;
        meter.IngestStereoInt16(now, zeroChunk.data(), 480u, false);
        baseIn.nowQpc100ns = now;
        baseIn.level = meter.Snapshot(now);
        (void)stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
    }
    CHECK(stagee::EvaluateBridgeDiagnostics(baseIn, tracker).headline ==
          stagee::PipelineHeadlineState::BriefQuietPassage);

    for (int i = 0; i < 320; ++i) {
        now += 100000ULL;
        meter.IngestStereoInt16(now, zeroChunk.data(), 480u, false);
        baseIn.nowQpc100ns = now;
        baseIn.level = meter.Snapshot(now);
        (void)stagee::EvaluateBridgeDiagnostics(baseIn, tracker);
    }
    CHECK(stagee::EvaluateBridgeDiagnostics(baseIn, tracker).headline ==
          stagee::PipelineHeadlineState::SustainedZeroAudioWarning);

    // Reset back to paused clean state for 3e
    baseIn.routing.targetSessionActive = false;
    (void)stagee::EvaluateBridgeDiagnostics(baseIn, tracker);

    // 3e. Volume Mixer Mute -> TargetMutedInMixerWarning
    baseIn.routing.targetSessionMuted = true;
    CHECK(stagee::EvaluateBridgeDiagnostics(baseIn, tracker).headline ==
          stagee::PipelineHeadlineState::TargetMutedInMixerWarning);
}

// ============================================================================
// Suite 4: Repo-Aware Log Dir, 10-Session Retention, Part Split & Sample JSONL
// ============================================================================

class NullRingSink final : public staged::IRingFrameSink {
public:
    bool WriteCommittedFrames(uint64_t, const int16_t*, uint32_t) override { return true; }
};

void TestLogDirectoryAndRotation(const std::wstring& sampleTelemetryPath) {
    std::printf("[Suite 4] Default Log Directory Resolution & 10-Session Rotating Logger...\n");

    const std::wstring tempRoot = MakeUniqueTempDir(L"logs");

    // 4a. Repo-root walk (4 levels deep: build\x64_main\bin\Release\iv_apple_music_radio.exe)
    {
        const std::wstring mockRepo = tempRoot + L"\\mock_repo";
        const std::wstring d1 = mockRepo + L"\\build";
        const std::wstring d2 = d1 + L"\\x64_main";
        const std::wstring d3 = d2 + L"\\bin";
        const std::wstring d4 = d3 + L"\\Release";
        CreateDirectoryW(mockRepo.c_str(), nullptr);
        CreateDirectoryW(d1.c_str(), nullptr);
        CreateDirectoryW(d2.c_str(), nullptr);
        CreateDirectoryW(d3.c_str(), nullptr);
        CreateDirectoryW(d4.c_str(), nullptr);

        std::string err;
        staged::WriteTextFileAtomic(mockRepo + L"\\CMakePresets.json", "{}", err);
        staged::WriteTextFileAtomic(mockRepo + L"\\AGENTS.md", "# AGENTS", err);

        const std::wstring resolvedRepoLogs =
            stagee::ResolveDefaultLogDirectory(d4 + L"\\iv_apple_music_radio.exe");
        CHECK(staged::EqualsIgnoreCase(resolvedRepoLogs, mockRepo + L"\\logs"));

        // Standalone directory without repo markers -> <exeDir>\logs
        const std::wstring standaloneDir = tempRoot + L"\\standalone_app";
        CreateDirectoryW(standaloneDir.c_str(), nullptr);
        const std::wstring resolvedStandaloneLogs =
            stagee::ResolveDefaultLogDirectory(standaloneDir + L"\\iv_apple_music_radio.exe");
        CHECK(staged::EqualsIgnoreCase(resolvedStandaloneLogs, standaloneDir + L"\\logs"));
    }

    // 4b. Strict filename parser + 14 sessions -> retain latest 10 + protect Stage D/E0/Probe logs + part splitting
    {
        std::wstring parsedKey;
        CHECK(stagee::ParseStageESessionLogFilename(L"stage_e_session_20261116_200000_pid1234.jsonl", parsedKey));
        CHECK(parsedKey == L"stage_e_session_20261116_200000_pid1234");
        CHECK(stagee::ParseStageESessionLogFilename(L"stage_e_session_20261116_200000_pid1234_s02_part03.jsonl", parsedKey));
        CHECK(parsedKey == L"stage_e_session_20261116_200000_pid1234_s02");
        CHECK(!stagee::ParseStageESessionLogFilename(L"stage_e0_r1_lead3500_telemetry.jsonl", parsedKey));
        CHECK(!stagee::ParseStageESessionLogFilename(L"stage_d_bridge_sample_telemetry.jsonl", parsedKey));
        CHECK(!stagee::ParseStageESessionLogFilename(L"IVRadioProbe.jsonl", parsedKey));

        const std::wstring rotDir = tempRoot + L"\\rot_dir";
        CreateDirectoryW(rotDir.c_str(), nullptr);

        std::string writeErr;
        const std::wstring protectedD  = rotDir + L"\\stage_d_r1_telemetry.jsonl";
        const std::wstring protectedE0 = rotDir + L"\\stage_e0_r1_lead3500_telemetry.jsonl";
        const std::wstring protectedPr = rotDir + L"\\IVRadioProbe.jsonl";
        staged::WriteTextFileAtomic(protectedD,  "{\"protected\":true}\n", writeErr);
        staged::WriteTextFileAtomic(protectedE0, "{\"protected\":true}\n", writeErr);
        staged::WriteTextFileAtomic(protectedPr, "{\"protected\":true}\n", writeErr);

        for (int idx = 1; idx <= 14; ++idx) {
            wchar_t keyBuf[96]{};
            std::swprintf(keyBuf, std::size(keyBuf), L"stage_e_session_20261116_20%02d00_pid9000_s01", idx);

            stagee::RotatingLoggerOptions lopts;
            lopts.logDirectory = rotDir;
            lopts.maxRetainedSessions = 10u;
            lopts.maxBytesPerPart = 900ULL; // Force multi-part splitting
            lopts.sessionKeyOverride = keyBuf;

            stagee::RotatingTelemetryLogger logger;
            std::string openErr;
            CHECK(logger.Open(lopts, openErr));

            for (int line = 0; line < 8; ++line) {
                const std::string payload(220u, 'x');
                CHECK(logger.WriteLine("{\"msg\":\"" + payload + "\"}", true));
            }
            CHECK(logger.CurrentPartIndex() >= 2u);
            const uint32_t partBeforeFlush = logger.CurrentPartIndex();
            // shutdown_flush with allowPartSplit=false must remain in the current active part
            CHECK(logger.WriteLine("{\"kind\":\"shutdown_flush\"}", false));
            CHECK(logger.CurrentPartIndex() == partBeforeFlush);
            logger.Close();
        }

        // Sessions 01..04 must be pruned (including their _part02 files); Sessions 05..14 must exist
        for (int idx = 1; idx <= 14; ++idx) {
            wchar_t p1[160]{};
            wchar_t p2[160]{};
            std::swprintf(p1, std::size(p1), L"%ls\\stage_e_session_20261116_20%02d00_pid9000_s01.jsonl", rotDir.c_str(), idx);
            std::swprintf(p2, std::size(p2), L"%ls\\stage_e_session_20261116_20%02d00_pid9000_s01_part02.jsonl", rotDir.c_str(), idx);
            if (idx <= 4) {
                CHECK(!FileExistsW(p1));
                CHECK(!FileExistsW(p2));
            } else {
                CHECK(FileExistsW(p1));
                CHECK(FileExistsW(p2));
            }
        }

        // Protected non-E1 logs must remain untouched
        CHECK(FileExistsW(protectedD));
        CHECK(FileExistsW(protectedE0));
        CHECK(FileExistsW(protectedPr));
    }

    // 4c. Generate Stage E1 sample telemetry JSONL for analyze_stage_d_telemetry.py compatibility audit
    if (!sampleTelemetryPath.empty()) {
        NullRingSink sink;
        staged::StageDTimelineBridge bridge(sink);
        stagee::RollingLevelMeter meter;
        stagee::BridgeStatusTracker statusTracker;

        const uint64_t origin = 100000000ULL;
        bridge.Reset(origin);
        meter.Reset(origin);
        bridge.PrefillZeroFrames(7200ULL, true);

        stagee::RotatingLoggerOptions sopts;
        sopts.explicitFilePath = sampleTelemetryPath;
        stagee::RotatingTelemetryLogger slog;
        std::string openErr;
        CHECK(slog.Open(sopts, openErr));

        stagee::RoutingDiagnostic route{};
        route.state = stagee::RoutingIsolationState::IsolatedOk;
        route.targetHasSession = true;
        route.targetSessionActive = true;
        route.targetEndpointNameUtf8 = "扬声器 (Test USB Audio)";
        route.defaultEndpointNameUtf8 = "耳机 (Test Headphones)";

        staged::TelemetryLineContext ctx{};
        ctx.targetPid = 4242u;
        ctx.targetExeUtf8 = "AMPLibraryAgent.exe";
        ctx.formatLabelUtf8 = "48000Hz_2ch_Float32";
        ctx.ipc.sessionGeneration = 2ULL;
        ctx.ipc.producerState = static_cast<uint32_t>(stagec::ProducerState::Streaming);
        ctx.ipc.consumerActiveReaders = 1u;

        auto emitLine = [&](const char* kind, uint64_t now100ns, bool running, bool allowSplit) {
            ctx.kind = kind;
            ctx.nowQpc100ns = now100ns;
            const auto tlSnap = bridge.Snapshot();
            ctx.ipc.writeReserveFrame = tlSnap.committedRingFrames;
            ctx.ipc.writeCommitFrame = tlSnap.committedRingFrames;
            const auto lvlSnap = meter.Snapshot(now100ns);

            stagee::BridgeDiagnosticInput din{};
            din.nowQpc100ns = now100ns;
            din.engineRunning = running;
            din.captureStreamActive = running;
            din.captureTargetPid = 4242u;
            din.captureTargetExeUtf8 = ctx.targetExeUtf8;
            din.captureFormatUtf8 = ctx.formatLabelUtf8;
            din.level = lvlSnap;
            din.routing = route;
            din.ipc = ctx.ipc;
            if (!running) {
                din.lastShutdownVerified = true;
                din.lastShutdownClean = tlSnap.VerifyInvariants(true);
                din.lastShutdownQueuedFrames = tlSnap.timelineQueuedFrames;
                din.lastShutdownCommittedFrames = tlSnap.committedRingFrames;
                din.lastShutdownHoldbackFrames = tlSnap.holdbackFrames;
            }
            const auto rep = stagee::EvaluateBridgeDiagnostics(din, statusTracker);
            CHECK(slog.WriteLine(stagee::FormatStageETelemetryJsonLine(tlSnap, ctx, lvlSnap, route, rep), allowSplit));
        };

        uint64_t now = origin + 1500000ULL;
        emitLine("startup", now, true, true);

        bridge.OnStreamStarted(48000u, 0u);
        emitLine("stream_started", now, true, true);

        std::vector<int16_t> pcm(480u * 2u, 2048);
        uint64_t devPos = 0ULL;
        for (int p = 0; p < 120; ++p) {
            const uint64_t pktQpc = now;
            now += 100000ULL; // +10 ms exact 0 ppm
            meter.IngestStereoInt16(now, pcm.data(), 480u, false);

            staged::TimelinePacketInput pkt{};
            pkt.sourceFrames = 480u;
            pkt.stereo48kLR = pcm.data();
            pkt.outFrames48k = 480u;
            pkt.devPos = devPos;
            pkt.qpcPos100ns = pktQpc;
            pkt.nowQpc100ns = now;
            bridge.IngestPacket(pkt);
            bridge.Tick(now);
            devPos += 480ULL;

            if ((p % 40) == 39) {
                emitLine("periodic", now, true, true);
            }
        }

        bridge.OnStreamEnded();
        bridge.FlushHoldbackOnStop();
        ctx.ipc.producerState = static_cast<uint32_t>(stagec::ProducerState::Stopped);
        emitLine("shutdown_flush", now + 50000ULL, false, false);
        slog.Close();
    }

    DeleteDirectoryTreeTestOnly(tempRoot);
}

// ============================================================================
// Suite 5: StageEBridgeEngine Mutex Conflict & iv_apple_music_radio.exe Process Gate
// ============================================================================

void TestEngineAndLauncherProcess(const std::wstring& launcherExePath) {
    std::printf("[Suite 5] StageEBridgeEngine Mutex Conflict & Headless Single-Instance/Shutdown Gate...\n");

    const std::wstring tempRoot = MakeUniqueTempDir(L"proc");
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const std::wstring uniqueSuffix =
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(qpc.QuadPart);

    // 5a. In-process ProducerMutexBusy fast detection (< 1s, non-deadlocking)
    {
        const std::wstring busyMap = L"Local\\GTAIV_StageE1_BusyTest_" + uniqueSuffix;
        const std::wstring busyMutexName = busyMap + stagec::kProducerMutexSuffix;
        HANDLE hHeldMutex = CreateMutexW(nullptr, TRUE /* bInitialOwner */, busyMutexName.c_str());
        CHECK(hHeldMutex != nullptr);

        // Run conflict attempt from a separate worker thread because Win32 mutexes are recursive on the same thread!
        bool startReturned = true;
        stagee::StageELiveSnapshot snapAfterFail{};
        std::thread conflictThread([&]() {
            stagee::StageEBridgeEngine eng;
            stagee::StageEEngineOptions eopts;
            eopts.mappingName = busyMap;
            eopts.targetPid = 999999u;
            eopts.logger.logDirectory = tempRoot + L"\\busy_logs";
            std::string err;
            startReturned = eng.Start(eopts, err);
            snapAfterFail = eng.GetSnapshot();
        });
        conflictThread.join();

        CHECK(!startReturned);
        CHECK(snapAfterFail.producerMutexBusy);
        CHECK(snapAfterFail.report.headline == stagee::PipelineHeadlineState::ProducerMutexBusy);

        if (hHeldMutex) {
            ReleaseMutex(hHeldMutex);
            CloseHandle(hHeldMutex);
        }
    }

    // 5b. Spawn iv_apple_music_radio.exe --headless -> verify IPC takeover -> verify duplicate instance exits 0 -> verify clean shutdown_flush
    if (!launcherExePath.empty()) {
        CHECK(FileExistsW(launcherExePath));

        const std::wstring instName  = L"Local\\GTAIV_StageE1_Inst_" + uniqueSuffix;
        const std::wstring mapName   = L"Local\\GTAIV_StageE1_Ring_" + uniqueSuffix;
        const std::wstring readyEv   = L"Local\\GTAIV_StageE1_Ready_" + uniqueSuffix;
        const std::wstring stopEv    = L"Local\\GTAIV_StageE1_Stop_" + uniqueSuffix;
        const std::wstring logDir    = tempRoot + L"\\launcher_logs";
        const std::wstring status1   = tempRoot + L"\\status_primary.json";
        const std::wstring status2   = tempRoot + L"\\status_duplicate.json";

        HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());
        HANDLE hStop  = CreateEventW(nullptr, TRUE, FALSE, stopEv.c_str());
        CHECK(hReady != nullptr && hStop != nullptr);

        auto spawnLauncher = [&](const std::wstring& extraArgs, PROCESS_INFORMATION& outPi) -> bool {
            std::wstring cmd = L"\"" + launcherExePath + L"\" " + extraArgs;
            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            return CreateProcessW(
                launcherExePath.c_str(),
                cmdBuf.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                nullptr,
                &si,
                &outPi) != FALSE;
        };

        PROCESS_INFORMATION pi1{};
        const std::wstring args1 =
            L"--headless --instance-name \"" + instName +
            L"\" --mapping-name \"" + mapName +
            L"\" --log-dir \"" + logDir +
            L"\" --ready-event \"" + readyEv +
            L"\" --stop-event \"" + stopEv +
            L"\" --status-file \"" + status1 +
            L"\" --target-pid 999999 --prefill-ms 100 --duration-sec 15";
        CHECK(spawnLauncher(args1, pi1));

        CHECK(WaitForSingleObject(hReady, 8000u) == WAIT_OBJECT_0);

        // Inspect live shared memory header initialized by iv_apple_music_radio.exe
        // Note: stagec::AtomicLoadU64/AtomicLoad32 use InterlockedCompareExchange64/32, which require RW view permissions.
        HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapName.c_str());
        CHECK(hMap != nullptr);
        void* view = hMap ? MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(stagec::StageCIpcRingHeader)) : nullptr;
        CHECK(view != nullptr);
        if (view) {
            const auto* hdr = static_cast<const stagec::StageCIpcRingHeader*>(view);
            const uint64_t gen = stagec::AtomicLoadU64(&hdr->session_generation);
            CHECK(stagec::AtomicLoad32(&hdr->magic) == stagec::kIpcMagic);
            CHECK(gen >= 2ULL && (gen % 2ULL) == 0ULL);
            CHECK(stagec::AtomicLoad32(&hdr->producer_state) == static_cast<uint32_t>(stagec::ProducerState::Streaming));
            CHECK(stagec::AtomicLoadU64(&hdr->write_commit_frame) >= 4800ULL); // 100 ms prefill = 4,800 frames
        }

        // Launch second duplicate instance with the same --instance-name -> must exit 0 immediately!
        PROCESS_INFORMATION pi2{};
        const std::wstring args2 =
            L"--headless --instance-name \"" + instName +
            L"\" --status-file \"" + status2 + L"\"";
        CHECK(spawnLauncher(args2, pi2));
        CHECK(WaitForSingleObject(pi2.hProcess, 5000u) == WAIT_OBJECT_0);
        DWORD exit2 = 99u;
        GetExitCodeProcess(pi2.hProcess, &exit2);
        CHECK(exit2 == 0u);
        CloseHandle(pi2.hThread);
        CloseHandle(pi2.hProcess);

        std::string dupJson;
        std::string readErr;
        CHECK(staged::ReadTextFileUtf8(status2, dupJson, readErr));
        CHECK(dupJson.find("\"duplicate_instance\": true") != std::string::npos);

        // Signal clean stop to primary instance -> must flush holdback (H=0, C=Q) and set ProducerState::Stopped
        SetEvent(hStop);
        CHECK(WaitForSingleObject(pi1.hProcess, 8000u) == WAIT_OBJECT_0);
        DWORD exit1 = 99u;
        GetExitCodeProcess(pi1.hProcess, &exit1);
        CHECK(exit1 == 0u);
        CloseHandle(pi1.hThread);
        CloseHandle(pi1.hProcess);

        if (view) {
            const auto* hdr = static_cast<const stagec::StageCIpcRingHeader*>(view);
            CHECK(stagec::AtomicLoad32(&hdr->producer_state) == static_cast<uint32_t>(stagec::ProducerState::Stopped));
            UnmapViewOfFile(view);
        }
        if (hMap) {
            CloseHandle(hMap);
        }

        std::string primJson;
        CHECK(staged::ReadTextFileUtf8(status1, primJson, readErr));
        CHECK(primJson.find("\"started_ok\": true") != std::string::npos);
        CHECK(primJson.find("\"clean_shutdown_verified\": true") != std::string::npos);
        CHECK(primJson.find("\"clean_shutdown_ok\": true") != std::string::npos);
        CHECK(primJson.find("\"shutdown_holdback_frames\": 0") != std::string::npos);

        if (hReady) CloseHandle(hReady);
        if (hStop)  CloseHandle(hStop);
    }

    DeleteDirectoryTreeTestOnly(tempRoot);
}

// ============================================================================
// Suite 6: English Localization, Persistent Settings & Live Language Hot-Switch
// ============================================================================

bool ContainsNoCjkCharacters(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    const std::wstring w = staged::Utf8ToWide(s);
    if (w.empty()) {
        return false;
    }
    for (wchar_t ch : w) {
        // CJK Symbols & Punctuation (U+3000..U+303F), CJK Unified Ideographs (U+3400..U+9FFF),
        // Fullwidth Forms (U+FF00..U+FFEF)
        if ((ch >= 0x3000 && ch <= 0x9FFF) || (ch >= 0xFF00 && ch <= 0xFFEF)) {
            return false;
        }
    }
    return true;
}

void TestLocalizationAndSettings(const std::wstring& launcherExePath) {
    std::printf("[Suite 6] English Localization, Settings Persistence & Live Language Hot-Switch...\n");

    const std::wstring tempRoot = MakeUniqueTempDir(L"i18n");

    // 6a. Language code parser & resolver
    {
        stagee::UiLanguage parsed = stagee::UiLanguage::ZhCn;
        CHECK(stagee::ParseUiLanguageCode("auto", parsed) && parsed == stagee::UiLanguage::Auto);
        CHECK(stagee::ParseUiLanguageCode("AUTO", parsed) && parsed == stagee::UiLanguage::Auto);
        CHECK(stagee::ParseUiLanguageCode("zh", parsed) && parsed == stagee::UiLanguage::ZhCn);
        CHECK(stagee::ParseUiLanguageCode("zh-CN", parsed) && parsed == stagee::UiLanguage::ZhCn);
        CHECK(stagee::ParseUiLanguageCode("en", parsed) && parsed == stagee::UiLanguage::EnUs);
        CHECK(stagee::ParseUiLanguageCode("en-US", parsed) && parsed == stagee::UiLanguage::EnUs);
        CHECK(!stagee::ParseUiLanguageCode("fr-FR", parsed));

        CHECK(std::strcmp(stagee::UiLanguageToCode(stagee::UiLanguage::Auto), "auto") == 0);
        CHECK(std::strcmp(stagee::UiLanguageToCode(stagee::UiLanguage::ZhCn), "zh") == 0);
        CHECK(std::strcmp(stagee::UiLanguageToCode(stagee::UiLanguage::EnUs), "en") == 0);

        CHECK(stagee::ResolveEffectiveLanguage(stagee::UiLanguage::ZhCn) == stagee::UiLanguage::ZhCn);
        CHECK(stagee::ResolveEffectiveLanguage(stagee::UiLanguage::EnUs) == stagee::UiLanguage::EnUs);
        const stagee::UiLanguage autoResolved = stagee::ResolveEffectiveLanguage(stagee::UiLanguage::Auto);
        CHECK(autoResolved == stagee::UiLanguage::ZhCn || autoResolved == stagee::UiLanguage::EnUs);
    }

    // 6b. LauncherSettings JSON load/save round-trip & corrupt rejection
    {
        const std::wstring cfgPath = tempRoot + L"\\iv_apple_music_radio.settings.json";
        stagee::LauncherSettings loaded{};
        std::string err;

        // Missing file -> defaults, returns true
        CHECK(stagee::LoadLauncherSettings(cfgPath, loaded, err));
        CHECK(loaded.language == stagee::UiLanguage::Auto);

        // Save EnUs -> Load EnUs
        stagee::LauncherSettings toSave{};
        toSave.language = stagee::UiLanguage::EnUs;
        CHECK(stagee::SaveLauncherSettings(cfgPath, toSave, err));
        CHECK(FileExistsW(cfgPath));
        CHECK(stagee::LoadLauncherSettings(cfgPath, loaded, err));
        CHECK(loaded.language == stagee::UiLanguage::EnUs);

        // Save ZhCn -> Load ZhCn
        toSave.language = stagee::UiLanguage::ZhCn;
        CHECK(stagee::SaveLauncherSettings(cfgPath, toSave, err));
        CHECK(stagee::LoadLauncherSettings(cfgPath, loaded, err));
        CHECK(loaded.language == stagee::UiLanguage::ZhCn);

        // Corrupt JSON -> returns false cleanly
        const std::wstring badPath = tempRoot + L"\\corrupt.settings.json";
        CHECK(staged::WriteTextFileAtomic(badPath, "{ \"language\": \"invalid-lang\" }", err));
        CHECK(!stagee::LoadLauncherSettings(badPath, loaded, err));
    }

    // 6c. Full English Routing & Pipeline Diagnostic Matrix (Pure ASCII verification + hot-reformat)
    {
        const wchar_t* kStoreAmpPath =
            L"C:\\Program Files\\WindowsApps\\AppleInc.AppleMusicWin_1.1284.20263.0_x64__nzyj5cx40ttqa\\AMPLibraryAgent.exe";

        stagee::RoutingIsolatorTracker rTracker;
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main";
        topo.defaultMultimediaEndpointId = L"ep_main";

        auto epMain = MakeMockEndpoint(L"ep_main", L"Headphones (Main DAC)", true);
        epMain.sessions.push_back(MakeMockSession(epMain, 5000u, L"GTAIV.exe", L"E:\\GTAIV\\GTAIV.exe", AudioSessionStateActive));
        auto epSub = MakeMockEndpoint(L"ep_sub", L"Speakers (Secondary HDMI)", false);
        epSub.sessions.push_back(MakeMockSession(epSub, 4242u, L"AMPLibraryAgent.exe", kStoreAmpPath, AudioSessionStateActive));
        topo.endpoints = { epMain, epSub };
        topo.processes = {
            { 4240u, 1u, L"AppleMusic.exe", L"C:\\WindowsApps\\AppleMusic.exe" },
            { 4242u, 4240u, L"AMPLibraryAgent.exe", kStoreAmpPath },
            { 5000u, 1u, L"GTAIV.exe", L"E:\\GTAIV\\GTAIV.exe" },
        };

        stagee::RoutingDiagnostic dIso = stagee::EvaluateRoutingIsolation(
            topo, 4242u, L"AMPLibraryAgent.exe", rTracker, stagee::UiLanguage::EnUs);
        CHECK(dIso.state == stagee::RoutingIsolationState::IsolatedOk);
        CHECK(ContainsNoCjkCharacters(dIso.bannerTitleUtf8));
        CHECK(ContainsNoCjkCharacters(dIso.bannerDetailUtf8));
        CHECK(dIso.bannerTitleUtf8.find("Split-Endpoint Isolation") != std::string::npos);

        // Hot-reformat same RoutingDiagnostic ZhCn <-> EnUs
        stagee::FormatRoutingDiagnosticText(dIso, stagee::UiLanguage::ZhCn);
        CHECK(!ContainsNoCjkCharacters(dIso.bannerTitleUtf8));
        stagee::FormatRoutingDiagnosticText(dIso, stagee::UiLanguage::EnUs);
        CHECK(ContainsNoCjkCharacters(dIso.bannerTitleUtf8));

        // Unplug fallback in English
        epSub.sessions.clear();
        epMain.sessions.push_back(MakeMockSession(epMain, 4242u, L"AMPLibraryAgent.exe", kStoreAmpPath, AudioSessionStateActive));
        topo.endpoints = { epMain };
        const stagee::RoutingDiagnostic dLost = stagee::EvaluateRoutingIsolation(
            topo, 4242u, L"AMPLibraryAgent.exe", rTracker, stagee::UiLanguage::EnUs);
        CHECK(dLost.state == stagee::RoutingIsolationState::SecondaryEndpointLostFallback);
        CHECK(ContainsNoCjkCharacters(dLost.bannerTitleUtf8));
        CHECK(ContainsNoCjkCharacters(dLost.bannerDetailUtf8));

        // Verify all 10 PipelineHeadlineState branches produce clean English strings
        stagee::BridgeStatusTracker bTracker;
        stagee::BridgeDiagnosticInput in{};
        in.routing = dIso;

        // 1. BridgeStopped
        in.engineRunning = false;
        in.lastShutdownVerified = true;
        in.lastShutdownClean = true;
        auto rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::BridgeStopped);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));
        CHECK(ContainsNoCjkCharacters(rep.headlineDetailUtf8));
        CHECK(ContainsNoCjkCharacters(rep.trayTooltipUtf8));

        // 2. ProducerMutexBusy
        in.producerMutexBusy = true;
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::ProducerMutexBusy);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));
        CHECK(ContainsNoCjkCharacters(rep.headlineDetailUtf8));
        in.producerMutexBusy = false;

        // 3. WaitingForAppleMusic
        in.engineRunning = true;
        in.captureStreamActive = false;
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::WaitingForAppleMusic);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));
        CHECK(ContainsNoCjkCharacters(rep.headlineDetailUtf8));

        // 4. ReconnectingTarget
        in.streamRestartCount = 1ULL;
        in.msSinceLastStreamEnd = 500ULL;
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::ReconnectingTarget);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));

        // 5. TargetMutedInMixerWarning
        in.captureStreamActive = true;
        in.captureTargetPid = 4242u;
        in.captureTargetExeUtf8 = "AMPLibraryAgent.exe";
        in.captureFormatUtf8 = "48000Hz_2ch_Float32_Native";
        in.routing.targetHasSession = true;
        in.routing.targetSessionMuted = true;
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::TargetMutedInMixerWarning);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));
        in.routing.targetSessionMuted = false;

        // 6. AppleMusicPausedOrIdle
        in.routing.targetSessionActive = false;
        in.level.hasAnyPacket = true;
        in.level.hasRecentPackets = true;
        in.level.frames1s = 48000ULL;
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::AppleMusicPausedOrIdle);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));

        // 7 & 8. BriefQuietPassage -> SustainedZeroAudioWarning
        in.routing.targetSessionActive = true;
        in.nowQpc100ns = 100000000ULL;
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::BriefQuietPassage);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));

        in.nowQpc100ns += 55000000ULL; // +5.5 s
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::SustainedZeroAudioWarning);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));
        CHECK(rep.headlineDetailUtf8.find("Dolby Atmos") != std::string::npos);

        // 9 & 10. StreamingWaitingForGame -> StreamingInGame + JSONL telemetry invariance
        in.level.audibleFrames100ms = 4800ULL;
        in.level.audibleFrames1s = 48000ULL;
        in.level.msSinceLastAudible = 0ULL;
        in.level.peakDbfs1s = -6.0;
        in.level.rmsDbfs1s = -9.0;
        in.ipc.consumerActiveReaders = 0u;
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::StreamingWaitingForGame);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));

        in.ipc.consumerActiveReaders = 1u;
        in.ipc.watermarkFrames = 168000ULL;
        rep = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(rep.headline == stagee::PipelineHeadlineState::StreamingInGame);
        CHECK(ContainsNoCjkCharacters(rep.headlineTitleUtf8));

        staged::TimelineStatsSnapshot tl{};
        staged::TelemetryLineContext tctx{};
        tctx.kind = "periodic";
        tctx.targetPid = 4242u;
        tctx.targetExeUtf8 = "AMPLibraryAgent.exe";
        tctx.formatLabelUtf8 = "48000Hz_2ch_Float32_Native";
        tctx.ipc = in.ipc;
        const std::string jsonLine = stagee::FormatStageETelemetryJsonLine(tl, tctx, in.level, in.routing, rep);
        CHECK(jsonLine.find("\"e1_headline\":\"streaming_in_game\"") != std::string::npos);
        CHECK(jsonLine.find("\"e1_routing\":\"isolated_ok\"") != std::string::npos);

        stagee::RouteBTelemetrySummary rbSum{};
        rbSum.valid              = true;
        rbSum.state              = 5u;
        rbSum.gameOnline         = true;
        rbSum.escPaused          = true;
        rbSum.audioGateMuted     = false;
        rbSum.prerollActive      = false;
        rbSum.smtcPlaybackStatus = 5u;
        rbSum.trackTitleUtf8     = "Track Title Example";
        const std::string rbJsonLine = stagee::FormatStageETelemetryJsonLine(
            tl, tctx, in.level, in.routing, rep, &rbSum);
        CHECK(rbJsonLine.find("\"rb_state\":\"paused_for_esc\"") != std::string::npos);
        CHECK(rbJsonLine.find("\"rb_online\":true") != std::string::npos);
        CHECK(rbJsonLine.find("\"rb_smtc\":5") != std::string::npos);
        CHECK(rbJsonLine.find("\"rb_esc\":true") != std::string::npos);
        CHECK(rbJsonLine.find("\"rb_track\":\"Track Title Example\"") != std::string::npos);
    }

    // 6d. Live StageEBridgeEngine::SetLanguage hot-switch & headless --config-path / --lang verification
    {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(qpc.QuadPart);
        const std::wstring mapName = L"Local\\GTAIV_StageE1_LangLive_" + suffix;

        stagee::StageEBridgeEngine eng;
        stagee::StageEEngineOptions eopts;
        eopts.mappingName = mapName;
        eopts.targetPid = 999999u;
        eopts.prefillMs = 50u;
        eopts.language = stagee::UiLanguage::EnUs;
        eopts.logger.logDirectory = tempRoot + L"\\live_logs";

        std::string startErr;
        CHECK(eng.Start(eopts, startErr));
        const auto snapEn = eng.GetSnapshot();
        CHECK(ContainsNoCjkCharacters(snapEn.report.headlineTitleUtf8));
        CHECK(snapEn.report.headlineTitleUtf8.find("Waiting for Apple Music") != std::string::npos);

        // Hot-switch to ZhCn while running -> immediate snapshot update with 0 ms audio stop
        eng.SetLanguage(stagee::UiLanguage::ZhCn);
        const auto snapZh = eng.GetSnapshot();
        CHECK(snapZh.running);
        CHECK(!ContainsNoCjkCharacters(snapZh.report.headlineTitleUtf8));

        // Hot-switch back to EnUs -> immediate English snapshot
        eng.SetLanguage(stagee::UiLanguage::EnUs);
        const auto snapEn2 = eng.GetSnapshot();
        CHECK(ContainsNoCjkCharacters(snapEn2.report.headlineTitleUtf8));
        eng.Stop();
        const auto snapStoppedEn = eng.GetSnapshot();
        CHECK(snapStoppedEn.report.headlineTitleUtf8 == "Bridge Stopped");

        // Subprocess check: persisted EnUs config loaded by iv_apple_music_radio.exe --headless --config-path
        if (!launcherExePath.empty() && FileExistsW(launcherExePath)) {
            const std::wstring cfgPath = tempRoot + L"\\persisted_en.settings.json";
            stagee::LauncherSettings s{};
            s.language = stagee::UiLanguage::EnUs;
            std::string saveErr;
            CHECK(stagee::SaveLauncherSettings(cfgPath, s, saveErr));

            const std::wstring statusEn = tempRoot + L"\\status_en.json";
            const std::wstring instEn = L"Local\\GTAIV_StageE1_InstEn_" + suffix;
            const std::wstring ringEn = L"Local\\GTAIV_StageE1_RingEn_" + suffix;

            std::wstring cmd =
                L"\"" + launcherExePath + L"\" --headless"
                L" --config-path \"" + cfgPath + L"\""
                L" --instance-name \"" + instEn + L"\""
                L" --mapping-name \"" + ringEn + L"\""
                L" --log-dir \"" + tempRoot + L"\\sub_logs\""
                L" --status-file \"" + statusEn + L"\""
                L" --target-pid 999999 --prefill-ms 50 --duration-sec 0.25";
            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(L'\0');

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            CHECK(CreateProcessW(launcherExePath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != FALSE);
            if (pi.hProcess) {
                CHECK(WaitForSingleObject(pi.hProcess, 8000u) == WAIT_OBJECT_0);
                DWORD exitCode = 99u;
                GetExitCodeProcess(pi.hProcess, &exitCode);
                CHECK(exitCode == 0u);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);

                std::string statusJson;
                std::string readErr;
                CHECK(staged::ReadTextFileUtf8(statusEn, statusJson, readErr));
                CHECK(statusJson.find("\"configured_language\": \"en\"") != std::string::npos);
                CHECK(statusJson.find("\"effective_language\": \"en\"") != std::string::npos);
                CHECK(statusJson.find("\"headline_title\": \"Bridge Stopped\"") != std::string::npos);
            }
        }
    }

    DeleteDirectoryTreeTestOnly(tempRoot);
}

// ============================================================================
// Suite 7: Stream Anything (Audio Session Picker, Root-PID Promotion, Guardrails & Live Hot-Swap)
// ============================================================================

void TestStreamAnythingAndHotSwap(const std::wstring& launcherExePath) {
    std::printf("[Suite 7] Stream Anything (Audio-Session Picker, Root-PID Tree, Guardrails & Live Hot-Swap)...\n");

    const std::wstring tempRoot = MakeUniqueTempDir(L"stream_any");

    // 7a. Forbidden Target Guardrails & Apple Music Family Detection
    {
        CHECK(stagee::IsForbiddenCaptureTargetExe(L""));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"GTAIV.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"gtaiv.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"D:\\GameLibrary\\steamapps\\common\\Grand Theft Auto IV\\GTAIV\\GTAIV.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"EFLC.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"LaunchGTAIV.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"iv_apple_music_radio.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"iv_stage_d_producer.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"audiodg.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"C:\\Windows\\System32\\svchost.exe"));
        CHECK(stagee::IsForbiddenCaptureTargetExe(L"C:\\Windows\\explorer.exe"));

        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"Spotify.exe"));
        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"C:\\Users\\Test\\AppData\\Roaming\\Spotify\\Spotify.exe"));
        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"msedge.exe"));
        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"chrome.exe"));
        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"firefox.exe"));
        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"foobar2000.exe"));
        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"vlc.exe"));
        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"cloudmusic.exe"));
        CHECK(!stagee::IsForbiddenCaptureTargetExe(L"QQMusic.exe"));

        CHECK(stagee::IsAppleMusicShellOrAgentExe(L"AppleMusic.exe"));
        CHECK(stagee::IsAppleMusicShellOrAgentExe(L"C:\\Program Files\\WindowsApps\\AppleInc.AppleMusicWin\\AMPLibraryAgent.exe"));
        CHECK(!stagee::IsAppleMusicShellOrAgentExe(L"Spotify.exe"));

        stagee::AudioSourceMode parsedMode = stagee::AudioSourceMode::CustomProcess;
        CHECK(stagee::ParseAudioSourceModeCode("apple_music", parsedMode) &&
              parsedMode == stagee::AudioSourceMode::AppleMusicPreset);
        CHECK(stagee::ParseAudioSourceModeCode("custom_exe", parsedMode) &&
              parsedMode == stagee::AudioSourceMode::CustomProcess);
        CHECK(!stagee::ParseAudioSourceModeCode("unknown_mode", parsedMode));
    }

    // 7b. ResolveSameImageRootPid: Multi-level Browser/Electron Child Tree & PID-Reuse Cycle Safety
    std::vector<staged::ProcessEntry> mockProcs = {
        { 500u,  100u,  L"explorer.exe",   L"C:\\Windows\\explorer.exe" },
        { 1000u, 500u,  L"msedge.exe",     L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe" },
        { 1020u, 1000u, L"msedge.exe",     L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe" },
        { 1048u, 1020u, L"msedge.exe",     L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe" },
        { 3100u, 500u,  L"Spotify.exe",    L"C:\\Users\\TestPlayer\\AppData\\Roaming\\Spotify\\Spotify.exe" },
        { 3125u, 3100u, L"Spotify.exe",    L"C:\\Users\\TestPlayer\\AppData\\Roaming\\Spotify\\Spotify.exe" },
        { 4200u, 500u,  L"foobar2000.exe", L"C:\\Program Files\\foobar2000\\foobar2000.exe" },
        { 4240u, 500u,  L"AppleMusic.exe", L"C:\\Program Files\\WindowsApps\\AppleMusic.exe" },
        { 4242u, 4240u, L"AMPLibraryAgent.exe", L"C:\\Program Files\\WindowsApps\\AMPLibraryAgent.exe" },
        { 5000u, 500u,  L"GTAIV.exe",      L"E:\\GTAIV\\GTAIV.exe" },
        { 6000u, 500u,  L"notepad.exe",    L"C:\\Windows\\System32\\notepad.exe" },
        // Pathological PID-reuse cycle: 9001 <-> 9002
        { 9001u, 9002u, L"vlc.exe",        L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe" },
        { 9002u, 9001u, L"vlc.exe",        L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe" },
    };
    {
        // Browser Audio Utility child 1048 -> Broker 1020 -> Root 1000 (stops before explorer.exe [500]!)
        CHECK(stagee::ResolveSameImageRootPid(mockProcs, 1048u) == 1000u);
        CHECK(stagee::ResolveSameImageRootPid(mockProcs, 1000u) == 1000u);
        // Electron Spotify child 3125 -> Root 3100
        CHECK(stagee::ResolveSameImageRootPid(mockProcs, 3125u) == 3100u);
        // Single-process native player untouched
        CHECK(stagee::ResolveSameImageRootPid(mockProcs, 4200u) == 4200u);
        // Cycle terminates safely
        const uint32_t cycRoot = stagee::ResolveSameImageRootPid(mockProcs, 9001u);
        CHECK(cycRoot == 9001u || cycRoot == 9002u);
    }

    // 7c. EnumerateSelectableAudioApps: Strictly Audio-Sessions-Only, Aggregated by Root PID & Sorted Playing -> Idle
    {
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main";
        topo.defaultMultimediaEndpointId = L"ep_main";
        topo.processes = mockProcs;

        auto epMain = MakeMockEndpoint(L"ep_main", L"Headphones (Main DAC)", true);
        // Forbidden game session -> MUST be excluded
        epMain.sessions.push_back(MakeMockSession(epMain, 5000u, L"GTAIV.exe", L"E:\\GTAIV\\GTAIV.exe", AudioSessionStateActive));
        // System sounds session -> MUST be excluded
        auto sysSess = MakeMockSession(epMain, 0u, L"", L"", AudioSessionStateActive);
        sysSess.isSystemSoundsSession = true;
        epMain.sessions.push_back(sysSess);
        // Expired VLC session -> MUST be excluded
        epMain.sessions.push_back(MakeMockSession(epMain, 9001u, L"vlc.exe", L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe", AudioSessionStateExpired));
        // Inactive foobar2000 session with raw @%SystemRoot% resource displayName -> Idle, stripped displayName
        auto fbSess = MakeMockSession(epMain, 4200u, L"foobar2000.exe", L"C:\\Program Files\\foobar2000\\foobar2000.exe", AudioSessionStateInactive);
        fbSess.displayName = L"@%SystemRoot%\\System32\\AudioSrv.dll,-202";
        epMain.sessions.push_back(fbSess);

        auto epSub = MakeMockEndpoint(L"ep_sub", L"Speakers (Secondary HDMI)", false);
        // Two child sessions of msedge.exe (1020 Inactive + 1048 Active) -> aggregated to 1 entry at rootPid=1000 (Playing)
        epSub.sessions.push_back(MakeMockSession(epSub, 1020u, L"msedge.exe", L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", AudioSessionStateInactive));
        auto edgeAct = MakeMockSession(epSub, 1048u, L"msedge.exe", L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", AudioSessionStateActive);
        edgeAct.displayName = L"YouTube Music";
        epSub.sessions.push_back(edgeAct);
        // Electron Spotify child 3125 Active -> promoted to rootPid=3100 (Playing)
        auto spotAct = MakeMockSession(epSub, 3125u, L"Spotify.exe", L"C:\\Users\\TestPlayer\\AppData\\Roaming\\Spotify\\Spotify.exe", AudioSessionStateActive);
        spotAct.displayName = L"Spotify";
        epSub.sessions.push_back(spotAct);

        topo.endpoints = { epMain, epSub };

        const auto selectable = stagee::EnumerateSelectableAudioApps(topo);
        // Expected ONLY 3 entries: [0] msedge.exe (Playing, rootPid 1000), [1] Spotify.exe (Playing, rootPid 3100),
        // [2] foobar2000.exe (Idle, rootPid 4200). Non-audio notepad.exe, expired vlc.exe, and forbidden GTAIV.exe absent!
        CHECK(selectable.size() == 3u);
        if (selectable.size() == 3u) {
            CHECK(selectable[0].hasActiveSession);
            CHECK(selectable[0].exeName == L"msedge.exe");
            CHECK(selectable[0].rootPid == 1000u);
            CHECK(selectable[0].sessionDisplayName == L"YouTube Music");

            CHECK(selectable[1].hasActiveSession);
            CHECK(selectable[1].exeName == L"Spotify.exe");
            CHECK(selectable[1].rootPid == 3100u);

            CHECK(!selectable[2].hasActiveSession);
            CHECK(selectable[2].exeName == L"foobar2000.exe");
            CHECK(selectable[2].rootPid == 4200u);
            CHECK(selectable[2].sessionDisplayName.empty());
        }
    }

    // 7d. Custom Target Routing Isolation (Child Session Matched via Root Process Tree) & Generic Silence Diagnostics
    {
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main";
        topo.defaultMultimediaEndpointId = L"ep_main";
        topo.processes = mockProcs;

        auto epMain = MakeMockEndpoint(L"ep_main", L"Headphones (Main DAC)", true);
        epMain.sessions.push_back(MakeMockSession(epMain, 5000u, L"GTAIV.exe", L"E:\\GTAIV\\GTAIV.exe", AudioSessionStateActive));
        // Apple Music active on main speakers while user is streaming msedge.exe!
        epMain.sessions.push_back(MakeMockSession(epMain, 4242u, L"AMPLibraryAgent.exe", L"C:\\Program Files\\WindowsApps\\AMPLibraryAgent.exe", AudioSessionStateActive));

        auto epSub = MakeMockEndpoint(L"ep_sub", L"Speakers (Secondary HDMI)", false);
        // Audio Utility child 1048 of msedge.exe (root 1000) is active on secondary endpoint
        epSub.sessions.push_back(MakeMockSession(epSub, 1048u, L"msedge.exe", L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", AudioSessionStateActive));

        topo.endpoints = { epMain, epSub };

        stagee::RoutingIsolatorTracker rTracker;
        const stagee::RoutingDiagnostic dEdge = stagee::EvaluateRoutingIsolation(
            topo, 1000u /* bound root PID */, L"msedge.exe", rTracker, stagee::UiLanguage::EnUs);
        CHECK(!dEdge.isAppleMusicMode);
        CHECK(dEdge.targetDisplayNameUtf8 == "msedge.exe");
        CHECK(dEdge.state == stagee::RoutingIsolationState::IsolatedOk);
        CHECK(dEdge.targetHasSession && dEdge.targetSessionActive);
        CHECK(dEdge.bannerDetailUtf8.find("msedge.exe") != std::string::npos);
        // AMPLibraryAgent.exe must be treated as an external active app (excluded cleanly), while msedge.exe is NOT in otherActiveMediaAppsUtf8!
        CHECK(dEdge.otherActiveMediaAppsUtf8.size() == 1u);
        if (!dEdge.otherActiveMediaAppsUtf8.empty()) {
            CHECK(dEdge.otherActiveMediaAppsUtf8[0] == "AMPLibraryAgent.exe");
        }

        // Verify SustainedZeroAudioWarning for CustomProcess gives Exclusive Mode / in-app mute hint instead of Dolby Atmos
        stagee::BridgeStatusTracker bTracker;
        stagee::BridgeDiagnosticInput in{};
        in.engineRunning = true;
        in.sourceMode = stagee::AudioSourceMode::CustomProcess;
        in.targetDisplayNameUtf8 = "msedge.exe";
        in.captureStreamActive = true;
        in.captureTargetPid = 1000u;
        in.captureTargetExeUtf8 = "msedge.exe";
        in.captureFormatUtf8 = "48000Hz_2ch_Float32_Native";
        in.routing = dEdge;
        in.level.hasAnyPacket = true;
        in.level.hasRecentPackets = true;
        in.level.frames1s = 48000ULL;
        in.nowQpc100ns = 100000000ULL;
        (void)stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);

        in.nowQpc100ns += 56000000ULL; // +5.6 s sustained zero PCM
        const auto repEn = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::EnUs);
        CHECK(repEn.headline == stagee::PipelineHeadlineState::SustainedZeroAudioWarning);
        CHECK(ContainsNoCjkCharacters(repEn.headlineTitleUtf8));
        CHECK(ContainsNoCjkCharacters(repEn.headlineDetailUtf8));
        CHECK(repEn.headlineDetailUtf8.find("msedge.exe") != std::string::npos);
        CHECK(repEn.headlineDetailUtf8.find("WASAPI Exclusive") != std::string::npos);
        CHECK(repEn.headlineDetailUtf8.find("Dolby Atmos") == std::string::npos);

        const auto repZh = stagee::EvaluateBridgeDiagnostics(in, bTracker, stagee::UiLanguage::ZhCn);
        CHECK(repZh.headline == stagee::PipelineHeadlineState::SustainedZeroAudioWarning);
        CHECK(repZh.headlineDetailUtf8.find("msedge.exe") != std::string::npos);
        CHECK(repZh.headlineDetailUtf8.find("WASAPI") != std::string::npos);
        CHECK(repZh.headlineDetailUtf8.find("Dolby Atmos") == std::string::npos);
    }

    // 7e. LauncherSettings v1 Backward Compatibility & v2 Custom Source Validation
    {
        std::string err;
        stagee::LauncherSettings loaded{};

        // Legacy v1 JSON (language only) -> loads cleanly as AppleMusicPreset
        const std::wstring v1Path = tempRoot + L"\\legacy_v1.settings.json";
        CHECK(staged::WriteTextFileAtomic(v1Path, "{\n  \"version\": 1,\n  \"language\": \"en\"\n}\n", err));
        CHECK(stagee::LoadLauncherSettings(v1Path, loaded, err));
        CHECK(loaded.language == stagee::UiLanguage::EnUs);
        CHECK(loaded.source.mode == stagee::AudioSourceMode::AppleMusicPreset);
        CHECK(loaded.source.customExeName.empty());

        // v2 CustomProcess round-trip with full path
        const std::wstring v2Path = tempRoot + L"\\custom_v2.settings.json";
        stagee::LauncherSettings toSave{};
        toSave.language = stagee::UiLanguage::ZhCn;
        toSave.source.mode = stagee::AudioSourceMode::CustomProcess;
        toSave.source.customExeName = L"Spotify.exe";
        toSave.source.customExePath = L"C:\\Users\\TestPlayer\\AppData\\Roaming\\Spotify\\Spotify.exe";
        CHECK(stagee::SaveLauncherSettings(v2Path, toSave, err));
        CHECK(stagee::LoadLauncherSettings(v2Path, loaded, err));
        CHECK(loaded.language == stagee::UiLanguage::ZhCn);
        CHECK(loaded.source.mode == stagee::AudioSourceMode::CustomProcess);
        CHECK(loaded.source.customExeName == L"Spotify.exe");
        CHECK(loaded.source.customExePath == L"C:\\Users\\TestPlayer\\AppData\\Roaming\\Spotify\\Spotify.exe");

        // Forbidden target in JSON -> rejected cleanly
        const std::wstring badForbiddenPath = tempRoot + L"\\forbidden.settings.json";
        CHECK(staged::WriteTextFileAtomic(
            badForbiddenPath,
            "{\n  \"version\": 2,\n  \"language\": \"en\",\n  \"source_mode\": \"custom_exe\",\n  \"custom_exe_name\": \"GTAIV.exe\"\n}\n",
            err));
        CHECK(!stagee::LoadLauncherSettings(badForbiddenPath, loaded, err));

        // AppleMusic.exe in custom_exe JSON -> auto-normalized to AppleMusicPreset
        const std::wstring appleShellPath = tempRoot + L"\\apple_shell.settings.json";
        CHECK(staged::WriteTextFileAtomic(
            appleShellPath,
            "{\n  \"version\": 2,\n  \"language\": \"en\",\n  \"source_mode\": \"custom_exe\",\n  \"custom_exe_name\": \"AppleMusic.exe\"\n}\n",
            err));
        CHECK(stagee::LoadLauncherSettings(appleShellPath, loaded, err));
        CHECK(loaded.source.mode == stagee::AudioSourceMode::AppleMusicPreset);
        CHECK(loaded.source.customExeName.empty());
    }

    // 7f. Live StageEBridgeEngine::SetAudioSource Hot-Swap (Zero session_generation Reset) & Subprocess Checks
    {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(qpc.QuadPart);
        const std::wstring mapName = L"Local\\GTAIV_StageE1_HotSwap_" + suffix;

        stagee::StageEBridgeEngine eng;
        stagee::StageEEngineOptions eopts;
        eopts.mappingName = mapName;
        eopts.prefillMs = 50u;
        eopts.language = stagee::UiLanguage::EnUs;
        eopts.source.mode = stagee::AudioSourceMode::AppleMusicPreset;
        eopts.logger.logDirectory = tempRoot + L"\\hotswap_logs";

        std::string startErr;
        CHECK(eng.Start(eopts, startErr));
        const auto snap0 = eng.GetSnapshot();
        const uint64_t gen0 = snap0.sessionGeneration;
        CHECK(snap0.running);
        CHECK(gen0 >= 2ULL && (gen0 & 1ULL) == 0ULL);
        CHECK(snap0.activeSource.mode == stagee::AudioSourceMode::AppleMusicPreset);
        CHECK(snap0.targetDisplayNameUtf8 == "Apple Music");

        // Reject forbidden targets during live hot-swap
        CHECK(!eng.SetAudioSource({ stagee::AudioSourceMode::CustomProcess, L"GTAIV.exe", L"" }));
        CHECK(!eng.SetAudioSource({ stagee::AudioSourceMode::CustomProcess, L"explorer.exe", L"" }));

        // Live hot-swap to non-existent custom target (__NonExistentPlayer_StageE7__.exe)
        // Verifies session_generation remains 100% unchanged (no 5.3s GTA IV carrier reset!)
        stagee::AudioSourceConfig customCfg{};
        customCfg.mode = stagee::AudioSourceMode::CustomProcess;
        customCfg.customExeName = L"__NonExistentPlayer_StageE7__.exe";
        customCfg.customExePath = L"C:\\MockApps\\__NonExistentPlayer_StageE7__.exe";
        CHECK(eng.SetAudioSource(customCfg));

        Sleep(80u); // Allow worker threads to wake on captureReconfigEvent_ / topoReconfigEvent_
        const auto snap1 = eng.GetSnapshot();
        CHECK(snap1.running);
        CHECK(snap1.sessionGeneration == gen0);
        CHECK(snap1.activeSource.mode == stagee::AudioSourceMode::CustomProcess);
        CHECK(snap1.targetDisplayNameUtf8 == "__NonExistentPlayer_StageE7__.exe");
        CHECK(snap1.report.headlineTitleUtf8.find("__NonExistentPlayer_StageE7__.exe") != std::string::npos);

        // Passing AppleMusic.exe auto-normalizes back to AppleMusicPreset without ring reset
        CHECK(eng.SetAudioSource({ stagee::AudioSourceMode::CustomProcess, L"AppleMusic.exe", L"" }));
        const auto snap2 = eng.GetSnapshot();
        CHECK(snap2.sessionGeneration == gen0);
        CHECK(snap2.activeSource.mode == stagee::AudioSourceMode::AppleMusicPreset);
        CHECK(snap2.targetDisplayNameUtf8 == "Apple Music");

        eng.Stop();
        const auto snapStopped = eng.GetSnapshot();
        CHECK(snapStopped.lastShutdownVerified && snapStopped.lastShutdownClean);

        // Subprocess verification: persisted custom_exe loaded by iv_apple_music_radio.exe + forbidden CLI rejection
        if (!launcherExePath.empty() && FileExistsW(launcherExePath)) {
            const std::wstring cfgCustom = tempRoot + L"\\persisted_custom.settings.json";
            stagee::LauncherSettings sCustom{};
            sCustom.language = stagee::UiLanguage::EnUs;
            sCustom.source.mode = stagee::AudioSourceMode::CustomProcess;
            sCustom.source.customExeName = L"__NonExistentSpotify_StageE7__.exe";
            std::string saveErr;
            CHECK(stagee::SaveLauncherSettings(cfgCustom, sCustom, saveErr));

            const std::wstring statusCustom = tempRoot + L"\\status_custom.json";
            const std::wstring instCustom = L"Local\\GTAIV_StageE1_InstCust_" + suffix;
            const std::wstring ringCustom = L"Local\\GTAIV_StageE1_RingCust_" + suffix;

            std::wstring cmd1 =
                L"\"" + launcherExePath + L"\" --headless"
                L" --config-path \"" + cfgCustom + L"\""
                L" --instance-name \"" + instCustom + L"\""
                L" --mapping-name \"" + ringCustom + L"\""
                L" --log-dir \"" + tempRoot + L"\\sub_cust_logs\""
                L" --status-file \"" + statusCustom + L"\""
                L" --prefill-ms 50 --duration-sec 0.25";
            std::vector<wchar_t> cmdBuf1(cmd1.begin(), cmd1.end());
            cmdBuf1.push_back(L'\0');

            STARTUPINFOW si1{};
            si1.cb = sizeof(si1);
            PROCESS_INFORMATION pi1{};
            CHECK(CreateProcessW(launcherExePath.c_str(), cmdBuf1.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si1, &pi1) != FALSE);
            if (pi1.hProcess) {
                CHECK(WaitForSingleObject(pi1.hProcess, 8000u) == WAIT_OBJECT_0);
                DWORD exitCode1 = 99u;
                GetExitCodeProcess(pi1.hProcess, &exitCode1);
                CHECK(exitCode1 == 0u);
                CloseHandle(pi1.hThread);
                CloseHandle(pi1.hProcess);

                std::string statusJson;
                std::string readErr;
                CHECK(staged::ReadTextFileUtf8(statusCustom, statusJson, readErr));
                CHECK(statusJson.find("\"source_mode\": \"custom_exe\"") != std::string::npos);
                CHECK(statusJson.find("\"target_display_name\": \"__NonExistentSpotify_StageE7__.exe\"") != std::string::npos);
                CHECK(statusJson.find("\"clean_shutdown_ok\": true") != std::string::npos);
            }

            // Forbidden CLI --source-exe GTAIV.exe must exit immediately with code 2
            std::wstring cmd2 =
                L"\"" + launcherExePath + L"\" --headless --source-exe GTAIV.exe";
            std::vector<wchar_t> cmdBuf2(cmd2.begin(), cmd2.end());
            cmdBuf2.push_back(L'\0');

            STARTUPINFOW si2{};
            si2.cb = sizeof(si2);
            PROCESS_INFORMATION pi2{};
            CHECK(CreateProcessW(launcherExePath.c_str(), cmdBuf2.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si2, &pi2) != FALSE);
            if (pi2.hProcess) {
                CHECK(WaitForSingleObject(pi2.hProcess, 5000u) == WAIT_OBJECT_0);
                DWORD exitCode2 = 0u;
                GetExitCodeProcess(pi2.hProcess, &exitCode2);
                CHECK(exitCode2 == 2u);
                CloseHandle(pi2.hThread);
                CloseHandle(pi2.hProcess);
            }
        }
    }

    DeleteDirectoryTreeTestOnly(tempRoot);
}

// ============================================================================
// Suite 8: Stage E2 Zero-Hardware Virtual Sink & AutoRouteController Lifecycle
// ============================================================================

class FakeAudioEndpointPolicyAccess final : public stagee::IAudioEndpointPolicyAccess {
public:
    bool supported = true;
    bool failWrites = false;
    std::vector<stagee::ProcessPolicyIdentity> liveIdentities;
    std::map<std::wstring, std::map<ERole, std::wstring>> persistedByKey;

    bool IsSupported(std::string& outReason) override {
        if (!supported) {
            outReason = "Mock unsupported OS build";
            return false;
        }
        outReason.clear();
        return true;
    }

    bool EnumeratePolicyIdentities(const staged::AudioTopologyReport&,
                                   std::vector<stagee::ProcessPolicyIdentity>& outIdentities,
                                   std::string& outError) override {
        outError.clear();
        outIdentities = liveIdentities;
        return true;
    }

    bool GetPersistedDefaultEndpoint(uint32_t pid,
                                     EDataFlow flow,
                                     ERole role,
                                     std::wstring& outEndpointId,
                                     HRESULT& outHr,
                                     std::string& outError) override {
        outEndpointId.clear();
        outError.clear();
        if (!supported) {
            outHr = E_NOINTERFACE;
            return false;
        }
        if (flow != eRender) {
            outHr = E_INVALIDARG;
            return false;
        }
        const std::wstring key = FindKeyByPid(pid);
        if (key.empty()) {
            outHr = E_INVALIDARG;
            outError = "Unknown PID";
            return false;
        }
        auto itKey = persistedByKey.find(key);
        if (itKey != persistedByKey.end()) {
            auto itRole = itKey->second.find(role);
            if (itRole != itKey->second.end()) {
                outEndpointId = itRole->second;
            }
        }
        outHr = S_OK;
        return true;
    }

    bool SetPersistedDefaultEndpoint(uint32_t pid,
                                     EDataFlow flow,
                                     ERole role,
                                     const std::wstring& endpointId,
                                     HRESULT& outHr,
                                     std::string& outError) override {
        outError.clear();
        if (!supported) {
            outHr = E_NOINTERFACE;
            return false;
        }
        if (failWrites || flow != eRender) {
            outHr = E_ACCESSDENIED;
            outError = "Mock policy write failure";
            return false;
        }
        const std::wstring key = FindKeyByPid(pid);
        if (key.empty()) {
            outHr = E_INVALIDARG;
            outError = "Unknown PID";
            return false;
        }
        persistedByKey[key][role] = stagee::ExtractEndpointIdFromPolicyString(endpointId);
        outHr = S_OK;
        return true;
    }

private:
    std::wstring FindKeyByPid(uint32_t pid) const {
        for (const auto& id : liveIdentities) {
            if (id.pid == pid) {
                return id.identityKey;
            }
        }
        return {};
    }
};

void TestStageE2ZeroHardwareIsolation(const std::wstring& launcherExePath) {
    std::printf("[Suite 8] Stage E2 Zero-Hardware Virtual Sink & AutoRouteController...\n");
    const std::wstring tempRoot = MakeUniqueTempDir(L"stage_e2");

    // 8a. SWD policy string helpers & identity key formatting
    {
        const std::wstring epGuid = L"{0.0.0.00000000}.{6aa649d5-6312-43ef-af85-e8e6b4f7db4d}";
        const std::wstring swd = stagee::FormatSwdAudioRenderInterfaceId(epGuid);
        CHECK(swd == L"\\\\?\\SWD#MMDEVAPI#{0.0.0.00000000}.{6aa649d5-6312-43ef-af85-e8e6b4f7db4d}#{e6327cad-dcec-4949-ae8a-991e976a79d2}");
        CHECK(stagee::FormatSwdAudioRenderInterfaceId(swd) == swd);
        CHECK(stagee::ExtractEndpointIdFromPolicyString(swd) == epGuid);
        CHECK(stagee::ArePolicyEndpointStringsEqual(swd, L"{0.0.0.00000000}.{6AA649D5-6312-43EF-AF85-E8E6B4F7DB4D}"));
        CHECK(stagee::FormatSwdAudioRenderInterfaceId(L"").empty());
        CHECK(stagee::ExtractEndpointIdFromPolicyString(L"").empty());

        const std::wstring kApp = stagee::ComputeProcessPolicyIdentityKey(
            L"C:\\Apps\\AppleMusic.exe", L"AppleMusic.exe", L"AppleInc.AppleMusicWin_abc!App");
        const std::wstring kLib = stagee::ComputeProcessPolicyIdentityKey(
            L"C:\\Apps\\AMPLibraryAgent.exe", L"AMPLibraryAgent.exe", L"AppleInc.AppleMusicWin_abc!LibraryServer");
        const std::wstring kEdge = stagee::ComputeProcessPolicyIdentityKey(
            L"C:\\Program Files\\Edge\\msedge.exe", L"msedge.exe", L"");
        CHECK(kApp == L"aumid:appleinc.applemusicwin_abc!app");
        CHECK(kLib == L"aumid:appleinc.applemusicwin_abc!libraryserver");
        CHECK(kApp != kLib);
        CHECK(kEdge == L"exe:c:\\program files\\edge\\msedge.exe");
    }

    // 8b. SilentSinkDetector priority ranking & hazard rejection
    {
        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main";
        topo.defaultMultimediaEndpointId = L"ep_main";
        auto epMain = MakeMockEndpoint(L"ep_main", L"Headphones (Test Headphones)", true);
        auto epHdmi = MakeMockEndpoint(L"ep_hdmi", L"Monitor (NVIDIA High Definition Audio)", false);
        auto epVb   = MakeMockEndpoint(L"ep_vb",   L"CABLE Input (VB-Audio Virtual Cable)", false);
        auto epStm  = MakeMockEndpoint(L"ep_stm",  L"扬声器 (Steam Streaming Speakers)", false);

        // Only physical endpoints -> NotInstalled (never hijacks HDMI monitor!)
        topo.endpoints = { epMain, epHdmi };
        auto rNone = stagee::EvaluateSilentSinks(topo);
        CHECK(rNone.presence == stagee::VirtualSinkPresence::NotInstalled);
        CHECK(!rNone.hasSelectableSink);

        // Both VB-Cable and Steam Streaming Speakers -> picks Steam Streaming Speakers (priority 100 > 80)
        topo.endpoints = { epMain, epVb, epStm };
        auto rBoth = stagee::EvaluateSilentSinks(topo);
        CHECK(rBoth.presence == stagee::VirtualSinkPresence::ActiveReady);
        CHECK(rBoth.hasSelectableSink);
        CHECK(rBoth.selectedEndpointId == L"ep_stm");
        CHECK(rBoth.selectedPriority == 100);

        // Steam Streaming Speakers set as Windows Default -> hazard flagged & refused
        topo.defaultMultimediaEndpointId = L"ep_stm";
        auto epStmDefault = MakeMockEndpoint(L"ep_stm", L"Speakers (Steam Streaming Speakers)", true);
        topo.endpoints = { epStmDefault };
        auto rHazard = stagee::EvaluateSilentSinks(topo);
        CHECK(rHazard.presence == stagee::VirtualSinkPresence::VirtualSinkIsSystemDefault);
        CHECK(!rHazard.hasSelectableSink);
    }

    // 8c & 8d. Full AutoRouteController lifecycle (AppleMusic -> Edge -> UserOverride -> Exit -> Crash/Deferred Restore)
    {
        const std::wstring journalPath = tempRoot + L"\\iv_apple_music_radio.route_journal.json";

        staged::AudioTopologyReport topo;
        topo.defaultConsoleEndpointId = L"ep_main";
        topo.defaultMultimediaEndpointId = L"ep_main";
        auto epMain = MakeMockEndpoint(L"ep_main", L"耳机 (Test Headphones)", true);
        auto epStm  = MakeMockEndpoint(L"ep_stm",  L"扬声器 (Steam Streaming Speakers)", false);
        topo.endpoints = { epMain, epStm };

        FakeAudioEndpointPolicyAccess fakePolicy;
        const std::wstring kKeyApp  = L"aumid:appleinc.applemusicwin_nzyj5cx40ttqa!app";
        const std::wstring kKeyLib  = L"aumid:appleinc.applemusicwin_nzyj5cx40ttqa!libraryserver";
        const std::wstring kKeyEdge = L"exe:c:\\edge\\msedge.exe";
        const std::wstring kKeySpot = L"exe:c:\\spotify\\spotify.exe";

        fakePolicy.liveIdentities = {
            { 101u, kKeyApp,  L"AppleMusic.exe",      L"C:\\WindowsApps\\AppleMusic.exe",      L"AppleInc.AppleMusicWin_nzyj5cx40ttqa!App",           false, false },
            { 102u, kKeyLib,  L"AMPLibraryAgent.exe", L"C:\\WindowsApps\\AMPLibraryAgent.exe", L"AppleInc.AppleMusicWin_nzyj5cx40ttqa!LibraryServer", true,  true },
            { 204u, kKeyEdge, L"msedge.exe",          L"C:\\Edge\\msedge.exe",                 L"",                                                   true,  true },
        };
        // AppleMusic!App started previously on ep_old_dac; LibraryServer was System Default (""); Edge was System Default ("")
        fakePolicy.persistedByKey[kKeyApp][eConsole]    = L"ep_old_dac";
        fakePolicy.persistedByKey[kKeyApp][eMultimedia] = L"ep_old_dac";

        stagee::AutoRouteController ctrl;
        stagee::AutoRouteTargetSpec appleTarget{};
        appleTarget.isAppleMusicPreset = true;
        appleTarget.captureTargetPid = 102u;

        // 1. Reconcile in Auto mode -> isolates BOTH AppleMusic!App and !LibraryServer to ep_stm; leaves msedge untouched!
        const auto st1 = ctrl.ReconcileDuringBridge(
            fakePolicy, journalPath, topo, appleTarget, stagee::AutoRouteMode::Auto);
        CHECK(st1.state == stagee::AutoRouteState::AutoIsolatedOk);
        CHECK(st1.isolatedIdentityCount == 2u);
        CHECK(fakePolicy.persistedByKey[kKeyApp][eConsole] == L"ep_stm");
        CHECK(fakePolicy.persistedByKey[kKeyApp][eMultimedia] == L"ep_stm");
        CHECK(fakePolicy.persistedByKey[kKeyLib][eConsole] == L"ep_stm");
        CHECK(fakePolicy.persistedByKey[kKeyLib][eMultimedia] == L"ep_stm");
        CHECK(fakePolicy.persistedByKey[kKeyEdge].empty());
        CHECK(FileExistsW(journalPath));

        // 2. EvaluateRoutingIsolation formats Stage E2 auto-isolation text cleanly
        topo.endpoints[1].sessions.push_back(
            MakeMockSession(epStm, 102u, L"AMPLibraryAgent.exe", L"C:\\WindowsApps\\AMPLibraryAgent.exe", AudioSessionStateActive));
        stagee::RoutingIsolatorTracker rTrk;
        const auto diagAutoEn = stagee::EvaluateRoutingIsolation(
            topo, 102u, L"AMPLibraryAgent.exe", rTrk, stagee::UiLanguage::EnUs, &st1);
        CHECK(diagAutoEn.state == stagee::RoutingIsolationState::IsolatedOk);
        CHECK(diagAutoEn.bannerTitleUtf8.find("Auto Silent Isolation Active") != std::string::npos);

        // 3. Hot switch source from Apple Music -> msedge.exe: restores AppleMusic (!App->ep_old_dac, !Lib->"") & isolates Edge -> ep_stm!
        stagee::AutoRouteTargetSpec edgeTarget{};
        edgeTarget.isAppleMusicPreset = false;
        edgeTarget.customExeName = L"msedge.exe";
        edgeTarget.customExePath = L"C:\\Edge\\msedge.exe";
        edgeTarget.captureTargetPid = 204u;

        const auto st2 = ctrl.ReconcileDuringBridge(
            fakePolicy, journalPath, topo, edgeTarget, stagee::AutoRouteMode::Auto);
        CHECK(st2.state == stagee::AutoRouteState::AutoIsolatedOk);
        CHECK(st2.isolatedIdentityCount == 1u);
        CHECK(fakePolicy.persistedByKey[kKeyApp][eConsole] == L"ep_old_dac");
        CHECK(fakePolicy.persistedByKey[kKeyApp][eMultimedia] == L"ep_old_dac");
        CHECK(fakePolicy.persistedByKey[kKeyLib][eConsole].empty());
        CHECK(fakePolicy.persistedByKey[kKeyLib][eMultimedia].empty());
        CHECK(fakePolicy.persistedByKey[kKeyEdge][eConsole] == L"ep_stm");
        CHECK(fakePolicy.persistedByKey[kKeyEdge][eMultimedia] == L"ep_stm");

        // 4. User manually edits Edge in Volume Mixer -> ep_user_bt: RestoreAllRoutes MUST preserve user choice!
        fakePolicy.persistedByKey[kKeyEdge][eConsole]    = L"ep_user_bt";
        fakePolicy.persistedByKey[kKeyEdge][eMultimedia] = L"ep_user_bt";
        const auto sumUserMod = ctrl.RestoreAllRoutes(fakePolicy, journalPath, &topo, true);
        CHECK(sumUserMod.skippedUserModifiedCount == 1u);
        CHECK(sumUserMod.restoredCount == 0u);
        CHECK(fakePolicy.persistedByKey[kKeyEdge][eMultimedia] == L"ep_user_bt");
        CHECK(sumUserMod.journalEmptyAfterRestore);
        CHECK(!FileExistsW(journalPath));

        // 5. Deferred restore when target exits BEFORE Stop, then heals on RecoverOnStartup when Spotify relaunches!
        fakePolicy.liveIdentities.push_back(
            { 305u, kKeySpot, L"Spotify.exe", L"C:\\Spotify\\Spotify.exe", L"", true, true });
        stagee::AutoRouteTargetSpec spotTarget{};
        spotTarget.isAppleMusicPreset = false;
        spotTarget.customExeName = L"Spotify.exe";
        spotTarget.customExePath = L"C:\\Spotify\\Spotify.exe";
        spotTarget.captureTargetPid = 305u;

        const auto stSpot = ctrl.ReconcileDuringBridge(
            fakePolicy, journalPath, topo, spotTarget, stagee::AutoRouteMode::Auto);
        CHECK(stSpot.state == stagee::AutoRouteState::AutoIsolatedOk);
        CHECK(fakePolicy.persistedByKey[kKeySpot][eMultimedia] == L"ep_stm");

        // Spotify closes BEFORE user stops bridge -> RestoreAllRoutes marks it deferred in journal
        fakePolicy.liveIdentities.pop_back();
        const auto sumDeferred = ctrl.RestoreAllRoutes(fakePolicy, journalPath, &topo, true);
        CHECK(sumDeferred.deferredCount == 1u);
        CHECK(!sumDeferred.journalEmptyAfterRestore);
        CHECK(FileExistsW(journalPath));

        // Later, user opens Spotify and restarts Bridge -> RecoverOnStartup restores Spotify back to System Default & cleans journal!
        fakePolicy.liveIdentities.push_back(
            { 999u, kKeySpot, L"Spotify.exe", L"C:\\Spotify\\Spotify.exe", L"", false, false });
        const auto sumRecovered = ctrl.RecoverOnStartup(fakePolicy, journalPath, &topo);
        CHECK(sumRecovered.restoredCount == 1u);
        CHECK(sumRecovered.journalEmptyAfterRestore);
        CHECK(fakePolicy.persistedByKey[kKeySpot][eConsole].empty());
        CHECK(fakePolicy.persistedByKey[kKeySpot][eMultimedia].empty());
        CHECK(!FileExistsW(journalPath));

        // 6. Self-healing: if process already points to ep_stm with NO journal (e.g. journal deleted), orig falls back to "" Default
        fakePolicy.persistedByKey[kKeySpot][eConsole] = L"ep_stm";
        fakePolicy.persistedByKey[kKeySpot][eMultimedia] = L"ep_stm";
        (void)ctrl.ReconcileDuringBridge(fakePolicy, journalPath, topo, spotTarget, stagee::AutoRouteMode::Auto);
        const auto sumHealed = ctrl.RestoreAllRoutes(fakePolicy, journalPath, &topo, true);
        CHECK(sumHealed.restoredCount == 1u);
        CHECK(fakePolicy.persistedByKey[kKeySpot][eMultimedia].empty());
    }

    // 8e. LauncherSettings v2 -> v3 upgrade & AutoRouteMode persistence
    {
        std::string err;
        stagee::LauncherSettings loaded{};
        const std::wstring v2Legacy = tempRoot + L"\\v2_upgrade.settings.json";
        CHECK(staged::WriteTextFileAtomic(
            v2Legacy,
            "{\n  \"version\": 2,\n  \"language\": \"zh\",\n  \"source_mode\": \"apple_music\"\n}\n",
            err));
        CHECK(stagee::LoadLauncherSettings(v2Legacy, loaded, err));
        CHECK(loaded.autoRouteMode == stagee::AutoRouteMode::Auto); // Default Auto!
        CHECK(loaded.enableRadioModeBridge == true); // Default true when omitted!

        const std::wstring v3Path = tempRoot + L"\\v3_manual.settings.json";
        loaded.autoRouteMode = stagee::AutoRouteMode::Manual;
        loaded.enableRadioModeBridge = false;
        CHECK(stagee::SaveLauncherSettings(v3Path, loaded, err));
        stagee::LauncherSettings reloaded{};
        CHECK(stagee::LoadLauncherSettings(v3Path, reloaded, err));
        CHECK(reloaded.autoRouteMode == stagee::AutoRouteMode::Manual);
        CHECK(reloaded.enableRadioModeBridge == false);

        const std::wstring badBoolPath = tempRoot + L"\\v3_bad_bool.settings.json";
        CHECK(staged::WriteTextFileAtomic(
            badBoolPath,
            "{\n  \"version\": 3,\n  \"enable_radio_mode_bridge\": \"yes\"\n}\n",
            err));
        CHECK(!stagee::LoadLauncherSettings(badBoolPath, reloaded, err));
    }

    // 8f. Route A WHQL driver package verification (missing/tamper-reject + host probe) & Win32 AudioPolicyConfig live COM probe
    {
        const auto missingPkg = stagee::LocateAndVerifyVirtualSinkPackage(tempRoot + L"\\no_such_driver_dir");
        CHECK(!missingPkg.found);
        CHECK(!missingPkg.integrityValid);

        const std::wstring fakeDrvDir = tempRoot + L"\\tampered_driver";
        CHECK(CreateDirectoryW(fakeDrvDir.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS);
        std::string err;
        CHECK(staged::WriteTextFileAtomic(fakeDrvDir + L"\\SteamStreamingSpeakers.inf", "tampered inf", err));
        CHECK(staged::WriteTextFileAtomic(fakeDrvDir + L"\\SteamStreamingSpeakers.sys", "tampered sys", err));
        CHECK(staged::WriteTextFileAtomic(fakeDrvDir + L"\\steamstreamingspeakers.cat", "tampered cat", err));
        const auto tamperedPkg = stagee::LocateAndVerifyVirtualSinkPackage(fakeDrvDir);
        CHECK(tamperedPkg.found);
        CHECK(!tamperedPkg.integrityValid);
        CHECK(tamperedPkg.errorDetail.find("SHA-256 mismatch") != std::string::npos);

        const auto autoPkg = stagee::LocateAndVerifyVirtualSinkPackage();
        if (autoPkg.found) {
            CHECK(autoPkg.integrityValid);
        }

        stagee::Win32AudioPolicyConfigAccess livePolicy;
        std::string reason;
        const bool policySupported = livePolicy.IsSupported(reason);
        if (!policySupported) {
            std::fprintf(stderr, "  livePolicy.IsSupported failed reason: %s\n", reason.c_str());
        }
        CHECK(policySupported);

        if (!launcherExePath.empty() && FileExistsW(launcherExePath)) {
            std::wstring cmd = L"\"" + launcherExePath + L"\" --restore-routes --quiet --config-path \"" +
                               tempRoot + L"\\unused.settings.json\"";
            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            CHECK(CreateProcessW(launcherExePath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != FALSE);
            if (pi.hProcess) {
                CHECK(WaitForSingleObject(pi.hProcess, 5000u) == WAIT_OBJECT_0);
                DWORD exitCode = 99u;
                GetExitCodeProcess(pi.hProcess, &exitCode);
                CHECK(exitCode == 0u);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }

    DeleteDirectoryTreeTestOnly(tempRoot);
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::wstring launcherExePath;
    std::wstring sampleTelemetryPath;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--launcher-exe" && i + 1 < argc) {
            launcherExePath = argv[++i];
        } else if (arg == L"--write-sample-telemetry" && i + 1 < argc) {
            sampleTelemetryPath = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", staged::WideToUtf8(arg).c_str());
            return 2;
        }
    }

    std::printf("=== Stage E1/E2 Offline Self-Test Suite ===\n");
    TestRollingLevelMeter();
    TestRoutingIsolationClassifier();
    TestSilenceAndPipelineClassifier();
    TestLogDirectoryAndRotation(sampleTelemetryPath);
    TestEngineAndLauncherProcess(launcherExePath);
    TestLocalizationAndSettings(launcherExePath);
    TestStreamAnythingAndHotSwap(launcherExePath);
    TestStageE2ZeroHardwareIsolation(launcherExePath);

    std::printf("=== Stage E1 Summary: %d/%d checks passed (%d failed) ===\n",
                g_totalChecks - g_failedChecks,
                g_totalChecks,
                g_failedChecks);
    return (g_failedChecks == 0) ? 0 : 1;
}
