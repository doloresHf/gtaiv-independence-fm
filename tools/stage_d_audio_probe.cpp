#include "stage_d_capture.h"

#include <mmdeviceapi.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace staged;

constexpr double kTwoPi = 6.283185307179586476925286766559;

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* Get() const noexcept { return ptr_; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    void Reset() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

template <typename T>
class CoTaskMemPtr {
public:
    CoTaskMemPtr() = default;
    ~CoTaskMemPtr() { Reset(); }
    CoTaskMemPtr(const CoTaskMemPtr&) = delete;
    CoTaskMemPtr& operator=(const CoTaskMemPtr&) = delete;

    T* Get() const noexcept { return ptr_; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    void Reset() noexcept {
        if (ptr_) {
            CoTaskMemFree(ptr_);
            ptr_ = nullptr;
        }
    }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : handle_(h) {}
    ~ScopedHandle() { Reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE Get() const noexcept { return handle_; }
    bool IsValid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE h = nullptr) noexcept {
        if (IsValid()) {
            CloseHandle(handle_);
        }
        handle_ = h;
    }

private:
    HANDLE handle_ = nullptr;
};

struct ComMtaInit {
    HRESULT hr = E_FAIL;
    bool shouldUninit = false;
    ComMtaInit() noexcept {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        shouldUninit = (hr == S_OK || hr == S_FALSE);
    }
    ~ComMtaInit() {
        if (shouldUninit) {
            CoUninitialize();
        }
    }
    bool Usable() const noexcept { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

std::wstring GetSelfExecutablePath() {
    DWORD cap = MAX_PATH;
    for (int i = 0; i < 4; ++i) {
        std::wstring buf(cap, L'\0');
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), cap);
        if (len == 0) {
            return {};
        }
        if (len < cap - 1) {
            buf.resize(len);
            return buf;
        }
        cap *= 2;
    }
    return {};
}

std::wstring GetDefaultJournalPath() {
    const std::wstring self = GetSelfExecutablePath();
    if (!self.empty()) {
        std::filesystem::path p(self);
        return (p.parent_path() / L"stage_d_mute_journal.json").wstring();
    }
    return L"stage_d_mute_journal.json";
}

std::wstring GetTempKillRecoveryJournalPath() {
    wchar_t tmpDir[MAX_PATH]{};
    const DWORD len = GetTempPathW(MAX_PATH, tmpDir);
    std::filesystem::path base = (len > 0 && len < MAX_PATH)
        ? std::filesystem::path(tmpDir)
        : std::filesystem::current_path();
    const std::wstring name = L"stage_d_selftest_kill_journal_" +
                              std::to_wstring(GetCurrentProcessId()) + L".json";
    return (base / name).wstring();
}

const char* SessionStateName(uint32_t st) noexcept {
    switch (st) {
    case AudioSessionStateInactive: return "Inactive";
    case AudioSessionStateActive:   return "Active";
    case AudioSessionStateExpired:  return "Expired";
    default:                        return "Unknown";
    }
}

const char* LoopbackModeName(PROCESS_LOOPBACK_MODE mode) noexcept {
    return (mode == PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE)
        ? "EXCLUDE_TARGET_PROCESS_TREE"
        : "INCLUDE_TARGET_PROCESS_TREE";
}

bool ParseUint32Arg(const wchar_t* s, uint32_t& out) {
    if (!s || !*s || s[0] == L'-' || s[0] < L'0' || s[0] > L'9') {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long v = std::wcstoull(s, &end, 10);
    if (errno == ERANGE || end == s || *end != L'\0' ||
        v > static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool ParseDoubleArg(const wchar_t* s, double& out) {
    if (!s || !*s || s[0] == L' ' || s[0] == L'\t' || s[0] == L'\r' || s[0] == L'\n') {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const double v = std::wcstod(s, &end);
    if (errno == ERANGE || end == s || *end != L'\0' || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

std::string JsonBool(bool v) {
    return v ? "true" : "false";
}

std::string JsonString(const std::string& utf8) {
    return "\"" + EscapeJsonString(utf8) + "\"";
}

std::string JsonWString(const std::wstring& w) {
    return JsonString(WideToUtf8(w));
}

std::string JsonDouble(double v) {
    if (!std::isfinite(v)) {
        return "0.0";
    }
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

std::string SerializeTopologyJson(const AudioTopologyReport& report) {
    std::string out;
    out.reserve(4096);
    out += "{\n";
    out += "  \"generatedUtcIso\": " + JsonString(GetCurrentUtcIsoString()) + ",\n";
    out += "  \"defaultConsoleEndpointId\": " + JsonWString(report.defaultConsoleEndpointId) + ",\n";
    out += "  \"defaultMultimediaEndpointId\": " + JsonWString(report.defaultMultimediaEndpointId) + ",\n";
    out += "  \"defaultCommunicationsEndpointId\": " + JsonWString(report.defaultCommunicationsEndpointId) + ",\n";

    out += "  \"warnings\": [";
    for (size_t i = 0; i < report.warnings.size(); ++i) {
        if (i > 0) out += ", ";
        out += JsonString(report.warnings[i]);
    }
    out += "],\n";

    out += "  \"endpoints\": [\n";
    for (size_t i = 0; i < report.endpoints.size(); ++i) {
        const auto& ep = report.endpoints[i];
        out += "    {\n";
        out += "      \"endpointId\": " + JsonWString(ep.endpointId) + ",\n";
        out += "      \"friendlyName\": " + JsonWString(ep.friendlyName) + ",\n";
        out += "      \"isDefaultConsole\": " + JsonBool(ep.isDefaultConsole) + ",\n";
        out += "      \"isDefaultMultimedia\": " + JsonBool(ep.isDefaultMultimedia) + ",\n";
        out += "      \"isDefaultCommunications\": " + JsonBool(ep.isDefaultCommunications) + ",\n";
        out += "      \"mixFormatHr\": " + JsonString(FormatHResultHex(ep.mixFormatHr)) + ",\n";
        out += "      \"mixFormatDescription\": " + JsonString(ep.mixFormatDescription) + ",\n";
        out += "      \"mixFormatValid\": " + JsonBool(ep.mixFormatValid) + ",\n";
        out += "      \"sessionEnumHr\": " + JsonString(FormatHResultHex(ep.sessionEnumHr)) + ",\n";
        out += "      \"sessions\": [\n";
        for (size_t s = 0; s < ep.sessions.size(); ++s) {
            const auto& sess = ep.sessions[s];
            out += "        {\n";
            out += "          \"processId\": " + std::to_string(sess.processId) + ",\n";
            out += "          \"parentProcessId\": " + std::to_string(sess.parentProcessId) + ",\n";
            out += "          \"exeName\": " + JsonWString(sess.exeName) + ",\n";
            out += "          \"exeFullPath\": " + JsonWString(sess.exeFullPath) + ",\n";
            out += "          \"sessionState\": " + JsonString(SessionStateName(sess.sessionState)) + ",\n";
            out += "          \"getProcessIdHr\": " + JsonString(FormatHResultHex(sess.getProcessIdHr)) + ",\n";
            out += "          \"isMultiProcessSession\": " + JsonBool(sess.isMultiProcessSession) + ",\n";
            out += "          \"isSystemSoundsSession\": " + JsonBool(sess.isSystemSoundsSession) + ",\n";
            out += "          \"muted\": " + JsonBool(sess.muted) + ",\n";
            out += "          \"masterVolume\": " + JsonDouble(sess.masterVolume) + ",\n";
            out += "          \"volumeValid\": " + JsonBool(sess.volumeValid) + ",\n";
            out += "          \"displayName\": " + JsonWString(sess.displayName) + ",\n";
            out += "          \"sessionId\": " + JsonWString(sess.sessionId) + ",\n";
            out += "          \"sessionInstanceId\": " + JsonWString(sess.sessionInstanceId) + "\n";
            out += (s + 1 < ep.sessions.size()) ? "        },\n" : "        }\n";
        }
        out += "      ]\n";
        out += (i + 1 < report.endpoints.size()) ? "    },\n" : "    }\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

std::string SerializeProbeReportBlock(const ProcessLoopbackProbeReport& r) {
    std::string out;
    out += "{\n";
    out += "    \"targetPid\": " + std::to_string(r.targetPid) + ",\n";
    out += "    \"mode\": " + JsonString(LoopbackModeName(r.mode)) + ",\n";
    out += "    \"mixFormatActivateHr\": " + JsonString(FormatHResultHex(r.mixFormatActivateHr)) + ",\n";
    out += "    \"getMixFormatHr\": " + JsonString(FormatHResultHex(r.getMixFormatHr)) + ",\n";
    out += "    \"mixFormatDescription\": " + JsonString(r.mixFormatDescription) + ",\n";
    out += "    \"mixFormatValidated\": " + JsonBool(r.mixFormatValidated) + ",\n";
    out += "    \"hasSelectedCandidate\": " + JsonBool(r.hasSelectedCandidate) + ",\n";
    out += "    \"selectedCandidateIndex\": " + std::to_string(r.selectedCandidateIndex) + ",\n";
    out += "    \"candidates\": [\n";
    for (size_t i = 0; i < r.candidates.size(); ++i) {
        const auto& c = r.candidates[i];
        out += "      {\n";
        out += "        \"priorityIndex\": " + std::to_string(c.priorityIndex) + ",\n";
        out += "        \"label\": " + JsonString(c.label) + ",\n";
        out += "        \"sampleRate\": " + std::to_string(c.sampleRate) + ",\n";
        out += "        \"channels\": " + std::to_string(c.channels) + ",\n";
        out += "        \"useAutoConvert\": " + JsonBool(c.useAutoConvert) + ",\n";
        out += "        \"activateHr\": " + JsonString(FormatHResultHex(c.activateHr)) + ",\n";
        out += "        \"initializeHr\": " + JsonString(FormatHResultHex(c.initializeHr)) + ",\n";
        out += "        \"bufferFrames\": " + std::to_string(c.bufferFrames) + ",\n";
        out += "        \"succeeded\": " + JsonBool(c.succeeded) + ",\n";
        out += "        \"error\": " + JsonString(c.error) + "\n";
        out += (i + 1 < r.candidates.size()) ? "      },\n" : "      }\n";
    }
    out += "    ]\n";
    out += "  }";
    return out;
}

std::string SerializeCaptureStatsBlock(const CaptureSessionStats& s) {
    std::string out;
    out += "{\n";
    out += "    \"initialized\": " + JsonBool(s.initialized) + ",\n";
    out += "    \"completed\": " + JsonBool(s.completed) + ",\n";
    out += "    \"mmcssRegistered\": " + JsonBool(s.mmcssRegistered) + ",\n";
    out += "    \"activateHr\": " + JsonString(FormatHResultHex(s.activateHr)) + ",\n";
    out += "    \"initializeHr\": " + JsonString(FormatHResultHex(s.initializeHr)) + ",\n";
    out += "    \"lastCaptureHr\": " + JsonString(FormatHResultHex(s.lastCaptureHr)) + ",\n";
    out += "    \"bufferFrames\": " + std::to_string(s.bufferFrames) + ",\n";
    out += "    \"format\": " + JsonString(DescribeValidatedFormat(s.validatedFormat)) + ",\n";
    out += "    \"requestedDurationSec\": " + JsonDouble(s.requestedDurationSec) + ",\n";
    out += "    \"elapsedSec\": " + JsonDouble(s.elapsedSec) + ",\n";
    out += "    \"packetCount\": " + std::to_string(s.packetCount) + ",\n";
    out += "    \"totalFrames\": " + std::to_string(s.totalFrames) + ",\n";
    out += "    \"nonZeroFrames\": " + std::to_string(s.nonZeroFrames) + ",\n";
    out += "    \"silentPacketCount\": " + std::to_string(s.silentPacketCount) + ",\n";
    out += "    \"silentFrames\": " + std::to_string(s.silentFrames) + ",\n";
    out += "    \"dataDiscontinuityCount\": " + std::to_string(s.dataDiscontinuityCount) + ",\n";
    out += "    \"timestampErrorCount\": " + std::to_string(s.timestampErrorCount) + ",\n";
    out += "    \"peakAbsInt16\": " + std::to_string(s.peakAbsInt16) + ",\n";
    out += "    \"peakDbfs\": " + JsonDouble(s.peakDbfs) + ",\n";
    out += "    \"rmsDbfs\": " + JsonDouble(s.rmsDbfs) + ",\n";
    out += "    \"hasDevicePosition\": " + JsonBool(s.hasDevicePosition) + ",\n";
    out += "    \"firstDevicePosition\": " + std::to_string(s.firstDevicePosition) + ",\n";
    out += "    \"lastDevicePosition\": " + std::to_string(s.lastDevicePosition) + ",\n";
    out += "    \"hasQpcPosition\": " + JsonBool(s.hasQpcPosition) + ",\n";
    out += "    \"firstQpcPosition\": " + std::to_string(s.firstQpcPosition) + ",\n";
    out += "    \"lastQpcPosition\": " + std::to_string(s.lastQpcPosition) + ",\n";
    out += "    \"wavWritten\": " + JsonBool(s.wavWritten) + ",\n";
    out += "    \"wavPcmBytes\": " + std::to_string(s.wavPcmBytes) + ",\n";
    out += "    \"error\": " + JsonString(s.error) + "\n";
    out += "  }";
    return out;
}

std::string SerializeRestoreSummaryBlock(const JournalRestoreSummary& r) {
    std::string out;
    out += "{\n";
    out += "    \"journalExisted\": " + JsonBool(r.journalExisted) + ",\n";
    out += "    \"journalParsed\": " + JsonBool(r.journalParsed) + ",\n";
    out += "    \"journalRemovedOrArchived\": " + JsonBool(r.journalRemovedOrArchived) + ",\n";
    out += "    \"error\": " + JsonString(r.error) + ",\n";
    out += "    \"items\": [\n";
    for (size_t i = 0; i < r.items.size(); ++i) {
        const auto& item = r.items[i];
        out += "      {\n";
        out += "        \"endpointId\": " + JsonWString(item.entry.endpointId) + ",\n";
        out += "        \"sessionInstanceId\": " + JsonWString(item.entry.sessionInstanceId) + ",\n";
        out += "        \"exeFullPath\": " + JsonWString(item.entry.exeFullPath) + ",\n";
        out += "        \"recordedPid\": " + std::to_string(item.entry.recordedPid) + ",\n";
        out += "        \"origMute\": " + JsonBool(item.entry.origMute) + ",\n";
        out += "        \"origVolume\": " + JsonDouble(item.entry.origVolume) + ",\n";
        out += "        \"appliedMute\": " + JsonBool(item.entry.appliedMute) + ",\n";
        out += "        \"appliedVolume\": " + JsonDouble(item.entry.appliedVolume) + ",\n";
        out += "        \"status\": " + JsonString(RestoreStatusToString(item.status)) + ",\n";
        out += "        \"detail\": " + JsonString(item.detail) + "\n";
        out += (i + 1 < r.items.size()) ? "      },\n" : "      }\n";
    }
    out += "    ]\n";
    out += "  }";
    return out;
}

void PrintProbeReport(const ProcessLoopbackProbeReport& r) {
    std::printf("=== Process Loopback Format Probe (PID=%lu, mode=%s) ===\n",
                static_cast<unsigned long>(r.targetPid),
                LoopbackModeName(r.mode));
    std::printf("  GetMixFormat probe: activateHr=%s, getMixFormatHr=%s, validated=%s\n",
                FormatHResultHex(r.mixFormatActivateHr).c_str(),
                FormatHResultHex(r.getMixFormatHr).c_str(),
                r.mixFormatValidated ? "true" : "false");
    std::printf("  Mix format detail : %s\n", r.mixFormatDescription.c_str());
    for (size_t i = 0; i < r.candidates.size(); ++i) {
        const auto& c = r.candidates[i];
        const bool isWinner = (r.hasSelectedCandidate && i == r.selectedCandidateIndex);
        std::printf("  [%u]%s %-34s activate=%s init=%s bufFrames=%u ok=%s%s\n",
                    c.priorityIndex,
                    isWinner ? "*" : " ",
                    c.label.c_str(),
                    FormatHResultHex(c.activateHr).c_str(),
                    FormatHResultHex(c.initializeHr).c_str(),
                    c.bufferFrames,
                    c.succeeded ? "YES" : "NO",
                    c.error.empty() ? "" : (" (" + c.error + ")").c_str());
    }
}

void PrintCaptureStats(const char* title, const CaptureSessionStats& s) {
    std::printf("=== %s ===\n", title);
    std::printf("  format=%s, bufferFrames=%u, mmcss=%s, completed=%s\n",
                DescribeValidatedFormat(s.validatedFormat).c_str(),
                s.bufferFrames,
                s.mmcssRegistered ? "true" : "false",
                s.completed ? "true" : "false");
    std::printf("  elapsed=%.3fs (req=%.3fs), packets=%llu, frames=%llu, nonZeroFrames=%llu\n",
                s.elapsedSec,
                s.requestedDurationSec,
                static_cast<unsigned long long>(s.packetCount),
                static_cast<unsigned long long>(s.totalFrames),
                static_cast<unsigned long long>(s.nonZeroFrames));
    std::printf("  silentPackets=%llu, silentFrames=%llu, discontinuity=%llu, tsErrors=%llu\n",
                static_cast<unsigned long long>(s.silentPacketCount),
                static_cast<unsigned long long>(s.silentFrames),
                static_cast<unsigned long long>(s.dataDiscontinuityCount),
                static_cast<unsigned long long>(s.timestampErrorCount));
    std::printf("  peakAbsInt16=%d, peakDbfs=%.2f, rmsDbfs=%.2f\n",
                s.peakAbsInt16, s.peakDbfs, s.rmsDbfs);
    std::printf("  devPos=[%llu..%llu] (valid=%s), qpcPos=[%llu..%llu] (valid=%s)\n",
                static_cast<unsigned long long>(s.firstDevicePosition),
                static_cast<unsigned long long>(s.lastDevicePosition),
                s.hasDevicePosition ? "true" : "false",
                static_cast<unsigned long long>(s.firstQpcPosition),
                static_cast<unsigned long long>(s.lastQpcPosition),
                s.hasQpcPosition ? "true" : "false");
    if (s.wavWritten) {
        std::printf("  wavPcmBytes=%llu\n", static_cast<unsigned long long>(s.wavPcmBytes));
    }
    if (!s.error.empty()) {
        std::printf("  error=%s\n", s.error.c_str());
    }
}

void WriteOptionalReportJson(const std::wstring& path, const std::string& jsonContent) {
    if (path.empty()) {
        return;
    }
    std::string writeErr;
    if (WriteTextFileAtomic(path, jsonContent, writeErr)) {
        std::printf("Wrote JSON report: %s\n", WideToUtf8(path).c_str());
    } else {
        std::fprintf(stderr, "Failed writing JSON report '%s': %s\n",
                     WideToUtf8(path).c_str(), writeErr.c_str());
    }
}

struct TargetResolutionResult {
    DWORD selectedPid = 0;
    std::vector<uint32_t> matchedPids;
    std::vector<uint32_t> treePids;
    std::string note;
};

bool ResolveTargetFromTopoOrSnapshot(const std::wstring& targetExe,
                                     uint32_t explicitPid,
                                     const AudioTopologyReport& topo,
                                     TargetResolutionResult& outRes,
                                     std::string& outError) {
    outRes = TargetResolutionResult{};
    outError.clear();

    if (explicitPid != 0) {
        outRes.selectedPid = explicitPid;
        outRes.matchedPids.push_back(explicitPid);
        outRes.treePids = CollectProcessTreePids(topo.processes, explicitPid);
        outRes.note = "Explicit target PID";
        return true;
    }

    if (targetExe.empty()) {
        outError = "Either --target-pid or --target-exe must be provided";
        return false;
    }

    outRes.matchedPids = FindPidsByExeName(topo.processes, targetExe);
    if (outRes.matchedPids.empty()) {
        outError = "No running process matched exe name: " + WideToUtf8(targetExe);
        return false;
    }

    std::unordered_set<uint32_t> matchedSet(outRes.matchedPids.begin(), outRes.matchedPids.end());

    struct CandidateScore {
        uint32_t pid = 0;
        bool isTopLevel = false;
        size_t activeSessions = 0;
        size_t totalSessions = 0;
        std::vector<uint32_t> tree;
    };

    std::vector<CandidateScore> scored;
    for (uint32_t pid : outRes.matchedPids) {
        CandidateScore cs;
        cs.pid = pid;
        cs.isTopLevel = true;
        for (const auto& proc : topo.processes) {
            if (proc.pid == pid && matchedSet.count(proc.parentPid) != 0) {
                cs.isTopLevel = false;
                break;
            }
        }
        cs.tree = CollectProcessTreePids(topo.processes, pid);
        std::unordered_set<uint32_t> treeSet(cs.tree.begin(), cs.tree.end());

        for (const auto& ep : topo.endpoints) {
            for (const auto& sess : ep.sessions) {
                if (sess.processId != 0 && treeSet.count(sess.processId) != 0) {
                    ++cs.totalSessions;
                    if (sess.sessionState == AudioSessionStateActive) {
                        ++cs.activeSessions;
                    }
                }
            }
        }
        scored.push_back(std::move(cs));
    }

    std::sort(scored.begin(), scored.end(), [](const CandidateScore& a, const CandidateScore& b) {
        const bool aHasAct = a.activeSessions > 0;
        const bool bHasAct = b.activeSessions > 0;
        if (aHasAct != bHasAct) return aHasAct > bHasAct;

        const bool aHasSess = a.totalSessions > 0;
        const bool bHasSess = b.totalSessions > 0;
        if (aHasSess != bHasSess) return aHasSess > bHasSess;

        if (a.isTopLevel != b.isTopLevel) return a.isTopLevel > b.isTopLevel;
        if (a.tree.size() != b.tree.size()) return a.tree.size() > b.tree.size();
        return a.pid < b.pid;
    });

    outRes.selectedPid = scored.front().pid;
    outRes.treePids = scored.front().tree;
    outRes.note = "Resolved from exe '" + WideToUtf8(targetExe) + "' (matched=" +
                  std::to_string(outRes.matchedPids.size()) + ", activeSessionsInTree=" +
                  std::to_string(scored.front().activeSessions) + ", totalSessionsInTree=" +
                  std::to_string(scored.front().totalSessions) + ")";
    return true;
}

// ============================================================================
// Global Ctrl-C Recovery Safety Hook for --test-mute-isolation
// ============================================================================

struct ConsoleRestoreContext {
    std::mutex mu;
    bool armed = false;
    std::wstring journalPath;
};

ConsoleRestoreContext g_consoleRestoreCtx;

BOOL WINAPI IsolationConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: {
        std::lock_guard<std::mutex> lock(g_consoleRestoreCtx.mu);
        if (g_consoleRestoreCtx.armed && !g_consoleRestoreCtx.journalPath.empty()) {
            Win32AudioSessionAccess access;
            JournalRestoreSummary summary;
            SessionMuteJournal::RestoreFromJournal(access, g_consoleRestoreCtx.journalPath, summary);
            g_consoleRestoreCtx.armed = false;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

class MuteIsolationGuard {
public:
    explicit MuteIsolationGuard(std::wstring journalPath)
        : journalPath_(std::move(journalPath)) {
        std::lock_guard<std::mutex> lock(g_consoleRestoreCtx.mu);
        g_consoleRestoreCtx.journalPath = journalPath_;
        g_consoleRestoreCtx.armed = false;
    }

    MuteIsolationGuard(const MuteIsolationGuard&) = delete;
    MuteIsolationGuard& operator=(const MuteIsolationGuard&) = delete;

    ~MuteIsolationGuard() {
        Win32AudioSessionAccess access;
        JournalRestoreSummary summary;
        RestoreNow(access, summary);
        if (handlerInstalled_) {
            SetConsoleCtrlHandler(IsolationConsoleCtrlHandler, FALSE);
            handlerInstalled_ = false;
        }
        std::lock_guard<std::mutex> lock(g_consoleRestoreCtx.mu);
        g_consoleRestoreCtx.armed = false;
        g_consoleRestoreCtx.journalPath.clear();
    }

    bool BeginApply(IAudioSessionAccess& access,
                    const std::vector<SessionVolumeIdentity>& targets,
                    bool appliedMute,
                    const float* optionalOverrideVolume,
                    JournalDocument& outDoc,
                    std::string& outError) {
        if (!handlerInstalled_) {
            handlerInstalled_ = (SetConsoleCtrlHandler(IsolationConsoleCtrlHandler, TRUE) != FALSE);
        }
        std::lock_guard<std::mutex> lock(g_consoleRestoreCtx.mu);
        g_consoleRestoreCtx.journalPath = journalPath_;
        g_consoleRestoreCtx.armed = false;
        const bool ok = SessionMuteJournal::BeginApplyMute(
            access, journalPath_, targets, appliedMute, optionalOverrideVolume, outDoc, outError);
        if (ok) {
            g_consoleRestoreCtx.armed = true;
        }
        return ok;
    }

    bool RestoreNow(IAudioSessionAccess& access, JournalRestoreSummary& outSummary) {
        std::lock_guard<std::mutex> lock(g_consoleRestoreCtx.mu);
        if (!g_consoleRestoreCtx.armed) {
            outSummary = JournalRestoreSummary{};
            outSummary.journalExisted = false;
            return true;
        }
        const bool ok = SessionMuteJournal::RestoreFromJournal(access, journalPath_, outSummary);
        g_consoleRestoreCtx.armed = false;
        return ok;
    }

private:
    std::wstring journalPath_;
    bool handlerInstalled_ = false;
};

// ============================================================================
// Child Process Management & WASAPI Render Helpers (for D0b Local Selftests)
// ============================================================================

struct ChildProcessGuard {
    PROCESS_INFORMATION pi{};
    ScopedHandle stopEvent;
    bool active = false;

    ~ChildProcessGuard() { Cleanup(); }

    void Cleanup() noexcept {
        if (!active) return;
        if (stopEvent.IsValid()) {
            SetEvent(stopEvent.Get());
        }
        if (pi.hProcess) {
            if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 98);
                WaitForSingleObject(pi.hProcess, 2000);
            }
            CloseHandle(pi.hProcess);
            pi.hProcess = nullptr;
        }
        if (pi.hThread) {
            CloseHandle(pi.hThread);
            pi.hThread = nullptr;
        }
        stopEvent.Reset();
        active = false;
    }
};

std::wstring QuoteCmdArg(const std::wstring& s) {
    std::wstring q = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') q += L"\\\"";
        else q += c;
    }
    q += L"\"";
    return q;
}

bool SpawnHiddenSelfHelper(const std::wstring& args, ChildProcessGuard& outChild, std::string& outError) {
    const std::wstring selfExe = GetSelfExecutablePath();
    if (selfExe.empty()) {
        outError = "GetModuleFileNameW failed";
        return false;
    }
    std::wstring cmdLine = QuoteCmdArg(selfExe) + L" " + args;
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(
            selfExe.c_str(),
            mutableCmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        outError = "CreateProcessW failed: err=" + std::to_string(GetLastError());
        return false;
    }

    outChild.pi = pi;
    outChild.active = true;
    return true;
}

bool WaitReadyOrChildExit(HANDLE readyEvent, HANDLE childProc, DWORD timeoutMs, std::string& outError) {
    HANDLE handles[2] = { readyEvent, childProc };
    const DWORD w = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
    if (w == WAIT_OBJECT_0) {
        return true;
    }
    if (w == WAIT_OBJECT_0 + 1) {
        DWORD exitCode = 0;
        GetExitCodeProcess(childProc, &exitCode);
        outError = "Helper child exited before ready event (exitCode=" + std::to_string(exitCode) + ")";
        return false;
    }
    outError = "Timed out waiting for helper child ready event";
    return false;
}

class WasapiSineRenderHelper {
public:
    bool Start(double freqHz, double amplitude, std::string& outError) {
        outError.clear();
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(enumerator.Put()));
        if (FAILED(hr) || !enumerator) {
            outError = "Helper CoCreateInstance(MMDeviceEnumerator) failed: " + FormatHResultHex(hr);
            return false;
        }

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.Put());
        if (FAILED(hr) || !device) {
            outError = "Helper GetDefaultAudioEndpoint failed: " + FormatHResultHex(hr);
            return false;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(audioClient_.Put()));
        if (FAILED(hr) || !audioClient_) {
            outError = "Helper Activate(IAudioClient) failed: " + FormatHResultHex(hr);
            return false;
        }

        hr = audioClient_->GetMixFormat(mixFormat_.Put());
        if (FAILED(hr) || !mixFormat_) {
            outError = "Helper GetMixFormat failed: " + FormatHResultHex(hr);
            return false;
        }

        const WAVEFORMATEX* targetWfx = mixFormat_.Get();
        WAVEFORMATEX fallbackWfx{};
        DWORD initFlags = AUDCLNT_STREAMFLAGS_NOPERSIST; // Keep helper volume/mute transient across test invocations

        const bool mixSupported = DetermineRenderSampleFormat(targetWfx, isFloat32_, channels_, sampleRate_);
        if (!mixSupported) {
            fallbackWfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            fallbackWfx.nChannels = 2;
            fallbackWfx.nSamplesPerSec = 48000;
            fallbackWfx.wBitsPerSample = 32;
            fallbackWfx.nBlockAlign = 8;
            fallbackWfx.nAvgBytesPerSec = 48000u * 8u;
            fallbackWfx.cbSize = 0;
            targetWfx = &fallbackWfx;
            isFloat32_ = true;
            channels_ = 2;
            sampleRate_ = 48000;
            initFlags |= (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
        }

        // 100ms shared buffer
        hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, initFlags, 1000000, 0, targetWfx, nullptr);
        if (FAILED(hr)) {
            outError = "Helper IAudioClient::Initialize failed: " + FormatHResultHex(hr);
            return false;
        }

        hr = audioClient_->GetBufferSize(&bufferFrames_);
        if (FAILED(hr) || bufferFrames_ == 0) {
            outError = "Helper GetBufferSize failed: " + FormatHResultHex(hr);
            return false;
        }

        hr = audioClient_->GetService(__uuidof(IAudioRenderClient),
                                      reinterpret_cast<void**>(renderClient_.Put()));
        if (FAILED(hr) || !renderClient_) {
            outError = "Helper GetService(IAudioRenderClient) failed: " + FormatHResultHex(hr);
            return false;
        }

        freqHz_ = freqHz;
        amplitude_ = amplitude;

        if (!FillFrames(bufferFrames_, outError)) {
            return false;
        }

        hr = audioClient_->Start();
        if (FAILED(hr)) {
            outError = "Helper IAudioClient::Start failed: " + FormatHResultHex(hr);
            return false;
        }
        started_ = true;
        return true;
    }

    bool PumpOnce(std::string& outError) {
        if (!started_ || !audioClient_ || !renderClient_) {
            return false;
        }
        UINT32 padding = 0;
        const HRESULT hr = audioClient_->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            outError = "Helper GetCurrentPadding failed: " + FormatHResultHex(hr);
            return false;
        }
        if (padding >= bufferFrames_) {
            return true;
        }
        const UINT32 avail = bufferFrames_ - padding;
        return FillFrames(avail, outError);
    }

    void Stop() noexcept {
        if (started_ && audioClient_) {
            audioClient_->Stop();
            started_ = false;
        }
    }

    ~WasapiSineRenderHelper() { Stop(); }

private:
    static bool DetermineRenderSampleFormat(const WAVEFORMATEX* wfx,
                                            bool& outIsFloat32,
                                            uint16_t& outChannels,
                                            uint32_t& outRate) {
        if (!wfx || wfx->nChannels == 0 || wfx->nSamplesPerSec == 0) {
            return false;
        }
        outChannels = wfx->nChannels;
        outRate = wfx->nSamplesPerSec;
        if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32) {
            outIsFloat32 = true;
            return true;
        }
        if (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16) {
            outIsFloat32 = false;
            return true;
        }
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
            const auto* wfxe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
            if (IsEqualGUID(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && wfx->wBitsPerSample == 32) {
                outIsFloat32 = true;
                return true;
            }
            if (IsEqualGUID(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && wfx->wBitsPerSample == 16) {
                outIsFloat32 = false;
                return true;
            }
        }
        return false;
    }

    bool FillFrames(UINT32 frames, std::string& outError) {
        if (frames == 0) {
            return true;
        }
        BYTE* data = nullptr;
        const HRESULT hr = renderClient_->GetBuffer(frames, &data);
        if (FAILED(hr) || !data) {
            outError = "Helper IAudioRenderClient::GetBuffer failed: " + FormatHResultHex(hr);
            return false;
        }
        const double step = freqHz_ / static_cast<double>(sampleRate_);
        if (isFloat32_) {
            auto* out = reinterpret_cast<float*>(data);
            for (UINT32 f = 0; f < frames; ++f) {
                const float val = static_cast<float>(amplitude_ * std::sin(kTwoPi * phase_));
                phase_ += step;
                if (phase_ >= 1.0) phase_ -= std::floor(phase_);
                for (uint16_t c = 0; c < channels_; ++c) {
                    out[static_cast<size_t>(f) * channels_ + c] = val;
                }
            }
        } else {
            auto* out = reinterpret_cast<int16_t*>(data);
            for (UINT32 f = 0; f < frames; ++f) {
                const double s = amplitude_ * std::sin(kTwoPi * phase_);
                phase_ += step;
                if (phase_ >= 1.0) phase_ -= std::floor(phase_);
                const int16_t q = static_cast<int16_t>(std::clamp(std::llround(s * 32767.0), -32768LL, 32767LL));
                for (uint16_t c = 0; c < channels_; ++c) {
                    out[static_cast<size_t>(f) * channels_ + c] = q;
                }
            }
        }
        const HRESULT relHr = renderClient_->ReleaseBuffer(frames, 0);
        if (FAILED(relHr)) {
            outError = "Helper ReleaseBuffer failed: " + FormatHResultHex(relHr);
            return false;
        }
        return true;
    }

    ComPtr<IAudioClient> audioClient_;
    ComPtr<IAudioRenderClient> renderClient_;
    CoTaskMemPtr<WAVEFORMATEX> mixFormat_;
    UINT32 bufferFrames_ = 0;
    bool isFloat32_ = true;
    uint16_t channels_ = 2;
    uint32_t sampleRate_ = 48000;
    double freqHz_ = 440.0;
    double amplitude_ = 0.025;
    double phase_ = 0.0;
    bool started_ = false;
};

// ============================================================================
// Subcommand Implementations
// ============================================================================

int CmdListTopology(const std::wstring& reportJsonPath) {
    AudioTopologyReport report;
    std::string err;
    if (!DiscoverAudioTopology(report, err)) {
        std::fprintf(stderr, "DiscoverAudioTopology failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("=== Stage D Audio Topology (eRender active=%zu, processes=%zu) ===\n",
                report.endpoints.size(), report.processes.size());
    std::printf("  Default Console       : %s\n", WideToUtf8(report.defaultConsoleEndpointId).c_str());
    std::printf("  Default Multimedia    : %s\n", WideToUtf8(report.defaultMultimediaEndpointId).c_str());
    std::printf("  Default Communications: %s\n", WideToUtf8(report.defaultCommunicationsEndpointId).c_str());

    for (size_t i = 0; i < report.endpoints.size(); ++i) {
        const auto& ep = report.endpoints[i];
        std::printf("\n[Endpoint %zu] %s\n", i, WideToUtf8(ep.friendlyName).c_str());
        std::printf("  Id         : %s\n", WideToUtf8(ep.endpointId).c_str());
        std::printf("  Defaults   : Console=%s, Multimedia=%s, Comm=%s\n",
                    ep.isDefaultConsole ? "true" : "false",
                    ep.isDefaultMultimedia ? "true" : "false",
                    ep.isDefaultCommunications ? "true" : "false");
        std::printf("  MixFormat  : hr=%s, valid=%s | %s\n",
                    FormatHResultHex(ep.mixFormatHr).c_str(),
                    ep.mixFormatValid ? "true" : "false",
                    ep.mixFormatDescription.c_str());
        std::printf("  Sessions (%zu):\n", ep.sessions.size());
        for (size_t s = 0; s < ep.sessions.size(); ++s) {
            const auto& sess = ep.sessions[s];
            std::printf("    #%zu PID=%-6u PPID=%-6u state=%-8s exe=%-28s multiProc=%s sys=%s mute=%s vol=%.3f getPidHr=%s\n",
                        s,
                        sess.processId,
                        sess.parentProcessId,
                        SessionStateName(sess.sessionState),
                        WideToUtf8(sess.exeName).c_str(),
                        sess.isMultiProcessSession ? "true" : "false",
                        sess.isSystemSoundsSession ? "true" : "false",
                        sess.muted ? "true" : "false",
                        sess.masterVolume,
                        FormatHResultHex(sess.getProcessIdHr).c_str());
            if (!sess.exeFullPath.empty()) {
                std::printf("       exeFullPath       : %s\n", WideToUtf8(sess.exeFullPath).c_str());
            }
            if (!sess.sessionInstanceId.empty()) {
                std::printf("       sessionInstanceId : %s\n", WideToUtf8(sess.sessionInstanceId).c_str());
            }
        }
    }

    const auto applePids = FindPidsByExeName(report.processes, L"AppleMusic.exe");
    if (!applePids.empty()) {
        std::printf("\nDetected AppleMusic.exe PIDs:");
        for (uint32_t pid : applePids) {
            std::printf(" %u", pid);
        }
        std::printf("\n");
    }

    if (!report.warnings.empty()) {
        std::printf("\nWarnings:\n");
        for (const auto& w : report.warnings) {
            std::printf("  - %s\n", w.c_str());
        }
    }

    WriteOptionalReportJson(reportJsonPath, SerializeTopologyJson(report));
    return 0;
}

int CmdProbeCapture(const std::wstring& targetExe,
                    uint32_t targetPidArg,
                    PROCESS_LOOPBACK_MODE mode,
                    double durationSec,
                    const std::wstring& reportJsonPath,
                    const std::wstring& dumpWavPath) {
    AudioTopologyReport topo;
    std::string err;
    if (!DiscoverAudioTopology(topo, err)) {
        std::fprintf(stderr, "DiscoverAudioTopology failed: %s\n", err.c_str());
        return 1;
    }

    TargetResolutionResult target;
    if (!ResolveTargetFromTopoOrSnapshot(targetExe, targetPidArg, topo, target, err)) {
        std::fprintf(stderr, "Target resolution failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("Resolved target PID=%u (%s)\n", target.selectedPid, target.note.c_str());
    std::printf("Process tree PIDs (%zu):", target.treePids.size());
    for (uint32_t p : target.treePids) {
        std::printf(" %u", p);
    }
    std::printf("\n");

    ProcessLoopbackProbeReport probeReport;
    std::string probeErr;
    const bool probeOk = ProbeProcessLoopbackFormats(target.selectedPid, mode, probeReport, probeErr);
    PrintProbeReport(probeReport);

    CaptureSessionStats capStats;
    bool capRan = false;
    bool capOk = false;
    if (probeOk && probeReport.hasSelectedCandidate && durationSec > 0.0) {
        const auto& winner = probeReport.candidates[probeReport.selectedCandidateIndex];
        CaptureSessionOptions opts;
        opts.targetPid = target.selectedPid;
        opts.mode = mode;
        opts.durationSec = durationSec;
        opts.waveFormatBytes = winner.rawWaveFormatBytes;
        opts.useAutoConvert = winner.useAutoConvert;
        opts.dumpWavPath = dumpWavPath;
        capRan = true;
        capOk = RunCaptureSession(opts, capStats);
        PrintCaptureStats("Process Loopback Capture", capStats);
    } else if (!probeOk) {
        std::fprintf(stderr, "ProbeProcessLoopbackFormats failed: %s\n", probeErr.c_str());
    }

    if (!reportJsonPath.empty()) {
        std::string json;
        json += "{\n";
        json += "  \"generatedUtcIso\": " + JsonString(GetCurrentUtcIsoString()) + ",\n";
        json += "  \"targetResolution\": {\n";
        json += "    \"selectedPid\": " + std::to_string(target.selectedPid) + ",\n";
        json += "    \"note\": " + JsonString(target.note) + ",\n";
        json += "    \"treePids\": [";
        for (size_t i = 0; i < target.treePids.size(); ++i) {
            if (i > 0) json += ", ";
            json += std::to_string(target.treePids[i]);
        }
        json += "]\n  },\n";
        json += "  \"formatProbe\": " + SerializeProbeReportBlock(probeReport) + ",\n";
        json += "  \"captureExecuted\": " + JsonBool(capRan) + ",\n";
        json += "  \"captureOk\": " + JsonBool(capOk) + ",\n";
        json += "  \"captureStats\": " + SerializeCaptureStatsBlock(capStats) + "\n";
        json += "}\n";
        WriteOptionalReportJson(reportJsonPath, json);
    }

    if (!probeOk) return 2;
    if (capRan && !capOk) return 3;
    return 0;
}

int CmdRestoreAudio(const std::wstring& journalPathArg, const std::wstring& reportJsonPath) {
    const std::wstring journalPath = journalPathArg.empty() ? GetDefaultJournalPath() : journalPathArg;
    Win32AudioSessionAccess access;
    JournalRestoreSummary summary;
    const bool ok = SessionMuteJournal::RestoreFromJournal(access, journalPath, summary);

    std::printf("=== SessionMuteJournal Restore ===\n");
    std::printf("  journalPath            : %s\n", WideToUtf8(journalPath).c_str());
    std::printf("  journalExisted         : %s\n", summary.journalExisted ? "true" : "false");
    std::printf("  journalParsed          : %s\n", summary.journalParsed ? "true" : "false");
    std::printf("  journalRemovedArchived : %s\n", summary.journalRemovedOrArchived ? "true" : "false");
    if (!summary.error.empty()) {
        std::printf("  error                  : %s\n", summary.error.c_str());
    }
    for (size_t i = 0; i < summary.items.size(); ++i) {
        const auto& item = summary.items[i];
        std::printf("  [%zu] status=%-19s exe=%s detail=%s\n",
                    i,
                    RestoreStatusToString(item.status),
                    WideToUtf8(item.entry.exeFullPath).c_str(),
                    item.detail.c_str());
    }

    if (!reportJsonPath.empty()) {
        std::string json;
        json += "{\n";
        json += "  \"generatedUtcIso\": " + JsonString(GetCurrentUtcIsoString()) + ",\n";
        json += "  \"journalPath\": " + JsonWString(journalPath) + ",\n";
        json += "  \"ok\": " + JsonBool(ok) + ",\n";
        json += "  \"restoreSummary\": " + SerializeRestoreSummaryBlock(summary) + "\n";
        json += "}\n";
        WriteOptionalReportJson(reportJsonPath, json);
    }

    return ok ? 0 : 1;
}

int CmdTestMuteIsolation(const std::wstring& targetExe,
                         uint32_t targetPidArg,
                         double phaseDurationSec,
                         const std::wstring& journalPathArg,
                         const std::wstring& reportJsonPath) {
    const std::wstring journalPath = journalPathArg.empty() ? GetDefaultJournalPath() : journalPathArg;
    const double dur = (phaseDurationSec > 0.0) ? phaseDurationSec : 3.0;

    AudioTopologyReport topo;
    std::string err;
    if (!DiscoverAudioTopology(topo, err)) {
        std::fprintf(stderr, "DiscoverAudioTopology failed: %s\n", err.c_str());
        return 1;
    }

    TargetResolutionResult target;
    if (!ResolveTargetFromTopoOrSnapshot(targetExe, targetPidArg, topo, target, err)) {
        std::fprintf(stderr, "Target resolution failed: %s\n", err.c_str());
        return 1;
    }

    std::unordered_set<uint32_t> treeSet(target.treePids.begin(), target.treePids.end());

    Win32AudioSessionAccess access;
    std::vector<AudioSessionHandleInfo> allSessions;
    if (!access.EnumerateSessions(allSessions, err)) {
        std::fprintf(stderr, "EnumerateSessions failed: %s\n", err.c_str());
        return 1;
    }

    std::vector<SessionVolumeIdentity> muteTargets;
    for (const auto& s : allSessions) {
        if (s.identity.processId == 0 || treeSet.count(s.identity.processId) == 0) {
            continue;
        }
        if (s.identity.isMultiProcessSession || s.identity.isSystemSoundsSession) {
            std::printf("Skipping non-isolatable session PID=%u (multiProc=%s, sys=%s)\n",
                        s.identity.processId,
                        s.identity.isMultiProcessSession ? "true" : "false",
                        s.identity.isSystemSoundsSession ? "true" : "false");
            continue;
        }
        if (s.identity.endpointId.empty() || s.identity.sessionInstanceId.empty() ||
            s.identity.exeFullPath.empty()) {
            std::printf("Skipping session PID=%u with incomplete identity\n", s.identity.processId);
            continue;
        }
        muteTargets.push_back(s.identity);
    }

    if (muteTargets.empty()) {
        std::fprintf(stderr, "No single-process non-system audio sessions found in PID %u process tree\n",
                     target.selectedPid);
        return 1;
    }

    ProcessLoopbackProbeReport probeReport;
    if (!ProbeProcessLoopbackFormats(
            target.selectedPid, PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE, probeReport, err) ||
        !probeReport.hasSelectedCandidate) {
        std::fprintf(stderr, "ProbeProcessLoopbackFormats failed: %s\n", err.c_str());
        return 2;
    }
    PrintProbeReport(probeReport);

    const auto& winner = probeReport.candidates[probeReport.selectedCandidateIndex];
    CaptureSessionOptions opts;
    opts.targetPid = target.selectedPid;
    opts.mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    opts.durationSec = dur;
    opts.waveFormatBytes = winner.rawWaveFormatBytes;
    opts.useAutoConvert = winner.useAutoConvert;

    CaptureSessionStats baselineStats;
    const bool baselineOk = RunCaptureSession(opts, baselineStats);
    PrintCaptureStats("Phase 1: Baseline (Unmuted)", baselineStats);
    if (!baselineOk) {
        return 3;
    }

    JournalDocument journalDoc;
    MuteIsolationGuard guard(journalPath);
    if (!guard.BeginApply(access, muteTargets, true, nullptr, journalDoc, err)) {
        std::fprintf(stderr, "SessionMuteJournal::BeginApplyMute failed: %s\n", err.c_str());
        return 4;
    }
    std::printf("Applied mute via journal '%s' (%zu session entries); settling 150ms...\n",
                WideToUtf8(journalPath).c_str(), journalDoc.entries.size());
    Sleep(150);

    CaptureSessionStats mutedStats;
    const bool mutedOk = RunCaptureSession(opts, mutedStats);
    PrintCaptureStats("Phase 2: Endpoint Session Muted", mutedStats);

    JournalRestoreSummary restoreSummary;
    const bool restoreOk = guard.RestoreNow(access, restoreSummary);
    std::printf("RestoreFromJournal ok=%s, existed=%s, parsed=%s, cleaned=%s\n",
                restoreOk ? "true" : "false",
                restoreSummary.journalExisted ? "true" : "false",
                restoreSummary.journalParsed ? "true" : "false",
                restoreSummary.journalRemovedOrArchived ? "true" : "false");
    Sleep(150);

    CaptureSessionStats postRestoreStats;
    const bool postOk = RunCaptureSession(opts, postRestoreStats);
    PrintCaptureStats("Phase 3: Post-Restore", postRestoreStats);

    const bool isolationPreservedSignal = (mutedStats.nonZeroFrames > 0 && mutedStats.peakDbfs > -60.0);
    std::printf("\nMute Isolation Verdict: mutedCaptureSignalPresent=%s (mutedPeakDbfs=%.2f, baselinePeakDbfs=%.2f)\n",
                isolationPreservedSignal ? "YES" : "NO",
                mutedStats.peakDbfs,
                baselineStats.peakDbfs);

    if (!reportJsonPath.empty()) {
        std::string json;
        json += "{\n";
        json += "  \"generatedUtcIso\": " + JsonString(GetCurrentUtcIsoString()) + ",\n";
        json += "  \"targetPid\": " + std::to_string(target.selectedPid) + ",\n";
        json += "  \"journalPath\": " + JsonWString(journalPath) + ",\n";
        json += "  \"isolatedSessionsCount\": " + std::to_string(journalDoc.entries.size()) + ",\n";
        json += "  \"isolationPreservedSignal\": " + JsonBool(isolationPreservedSignal) + ",\n";
        json += "  \"formatProbe\": " + SerializeProbeReportBlock(probeReport) + ",\n";
        json += "  \"baselineStats\": " + SerializeCaptureStatsBlock(baselineStats) + ",\n";
        json += "  \"mutedStats\": " + SerializeCaptureStatsBlock(mutedStats) + ",\n";
        json += "  \"postRestoreStats\": " + SerializeCaptureStatsBlock(postRestoreStats) + ",\n";
        json += "  \"restoreOk\": " + JsonBool(restoreOk) + ",\n";
        json += "  \"restoreSummary\": " + SerializeRestoreSummaryBlock(restoreSummary) + "\n";
        json += "}\n";
        WriteOptionalReportJson(reportJsonPath, json);
    }

    return (baselineOk && mutedOk && restoreOk && postOk) ? 0 : 5;
}

int CmdSelfTestLocalLoopback(double durationSec, const std::wstring& reportJsonPath) {
    const double dur = (durationSec > 0.0) ? durationSec : 2.0;
    const DWORD pid = GetCurrentProcessId();
    const DWORD tick = GetTickCount();

    const std::wstring toneReadyName = L"Local\\StageD_LocalLoopback_ToneReady_" +
                                       std::to_wstring(pid) + L"_" + std::to_wstring(tick);
    const std::wstring toneStopName = L"Local\\StageD_LocalLoopback_ToneStop_" +
                                      std::to_wstring(pid) + L"_" + std::to_wstring(tick);
    const std::wstring idleReadyName = L"Local\\StageD_LocalLoopback_IdleReady_" +
                                       std::to_wstring(pid) + L"_" + std::to_wstring(tick);
    const std::wstring idleStopName = L"Local\\StageD_LocalLoopback_IdleStop_" +
                                      std::to_wstring(pid) + L"_" + std::to_wstring(tick);

    ScopedHandle toneReady(CreateEventW(nullptr, TRUE, FALSE, toneReadyName.c_str()));
    ScopedHandle idleReady(CreateEventW(nullptr, TRUE, FALSE, idleReadyName.c_str()));
    ChildProcessGuard toneChild;
    ChildProcessGuard idleChild;
    toneChild.stopEvent.Reset(CreateEventW(nullptr, TRUE, FALSE, toneStopName.c_str()));
    idleChild.stopEvent.Reset(CreateEventW(nullptr, TRUE, FALSE, idleStopName.c_str()));

    if (!toneReady.IsValid() || !idleReady.IsValid() ||
        !toneChild.stopEvent.IsValid() || !idleChild.stopEvent.IsValid()) {
        std::fprintf(stderr, "Failed creating selftest synchronization events\n");
        return 1;
    }

    std::string spawnErr;
    const std::wstring toneArgs = L"--helper-tone --freq 440 --amp 0.025 --max-sec 30 --ready-event " +
                                  QuoteCmdArg(toneReadyName) + L" --stop-event " + QuoteCmdArg(toneStopName);
    const std::wstring idleArgs = L"--helper-idle --max-sec 30 --ready-event " +
                                  QuoteCmdArg(idleReadyName) + L" --stop-event " + QuoteCmdArg(idleStopName);

    if (!SpawnHiddenSelfHelper(toneArgs, toneChild, spawnErr) ||
        !SpawnHiddenSelfHelper(idleArgs, idleChild, spawnErr)) {
        std::fprintf(stderr, "Failed spawning helper child: %s\n", spawnErr.c_str());
        return 1;
    }

    if (!WaitReadyOrChildExit(toneReady.Get(), toneChild.pi.hProcess, 10000, spawnErr) ||
        !WaitReadyOrChildExit(idleReady.Get(), idleChild.pi.hProcess, 10000, spawnErr)) {
        std::fprintf(stderr, "Helper readiness failed: %s\n", spawnErr.c_str());
        return 1;
    }

    Sleep(250);

    ProcessLoopbackProbeReport toneProbe;
    std::string err;
    const bool toneProbeOk = ProbeProcessLoopbackFormats(
        toneChild.pi.dwProcessId,
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE,
        toneProbe,
        err);
    PrintProbeReport(toneProbe);

    CaptureSessionStats toneStats;
    bool toneCapOk = false;
    std::string toneCandidateLabel;
    if (toneProbeOk && toneProbe.hasSelectedCandidate) {
        const auto& toneWinner = toneProbe.candidates[toneProbe.selectedCandidateIndex];
        toneCandidateLabel = toneWinner.label;
        std::printf("Selected tone loopback candidate: [%u] %s (autoConvert=%s)\n",
                    toneWinner.priorityIndex,
                    toneWinner.label.c_str(),
                    toneWinner.useAutoConvert ? "true" : "false");
        CaptureSessionOptions opts;
        opts.targetPid = toneChild.pi.dwProcessId;
        opts.mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        opts.durationSec = dur;
        opts.waveFormatBytes = toneWinner.rawWaveFormatBytes;
        opts.useAutoConvert = toneWinner.useAutoConvert;
        toneCapOk = RunCaptureSession(opts, toneStats);
        PrintCaptureStats("Selftest Target Tone Child Capture", toneStats);
    }

    ProcessLoopbackProbeReport idleProbe;
    std::string idleErr;
    const bool idleProbeOk = ProbeProcessLoopbackFormats(
        idleChild.pi.dwProcessId,
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE,
        idleProbe,
        idleErr);

    CaptureSessionStats idleStats;
    bool idleCapOk = false;
    bool idleExactZeroPath = false;
    std::string idleCandidateLabel;
    const char* isolationThresholdMode = "none";
    if (idleProbeOk && idleProbe.hasSelectedCandidate) {
        const auto& idleWinner = idleProbe.candidates[idleProbe.selectedCandidateIndex];
        idleCandidateLabel = idleWinner.label;
        idleExactZeroPath =
            (idleWinner.sampleType == CaptureSampleType::Float32 && !idleWinner.useAutoConvert);
        isolationThresholdMode =
            idleExactZeroPath ? "exact_zero_float32_native" : "dither_bounded_pcm16";
        std::printf("Selected idle loopback candidate: [%u] %s (autoConvert=%s, thresholdMode=%s)\n",
                    idleWinner.priorityIndex,
                    idleWinner.label.c_str(),
                    idleWinner.useAutoConvert ? "true" : "false",
                    isolationThresholdMode);
        CaptureSessionOptions opts;
        opts.targetPid = idleChild.pi.dwProcessId;
        opts.mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        opts.durationSec = std::min(dur, 1.5);
        opts.waveFormatBytes = idleWinner.rawWaveFormatBytes;
        opts.useAutoConvert = idleWinner.useAutoConvert;
        idleCapOk = RunCaptureSession(opts, idleStats);
        PrintCaptureStats("Selftest Silent Sibling Isolation Capture", idleStats);
    }

    toneChild.Cleanup();
    idleChild.Cleanup();

    const bool positivePass = toneProbeOk && toneCapOk &&
                              toneStats.totalFrames > 0 &&
                              toneStats.nonZeroFrames > 0 &&
                              toneStats.peakDbfs > -60.0;

    bool isolationPass = false;
    if (idleProbeOk && idleCapOk) {
        if (idleExactZeroPath) {
            isolationPass = (idleStats.nonZeroFrames == 0 &&
                             idleStats.peakAbsInt16 == 0);
        } else {
            isolationPass = positivePass &&
                            (idleStats.peakAbsInt16 <= 2 &&
                             idleStats.peakDbfs < -84.0 &&
                             (toneStats.peakDbfs - idleStats.peakDbfs) >= 40.0);
        }
    }
    const bool overallPass = positivePass && isolationPass;

    std::printf("\nSelftest Local Loopback Verdict: positivePass=%s, isolationPass=%s, "
                "toneCandidate=%s, idleCandidate=%s, isolationThresholdMode=%s -> %s\n",
                positivePass ? "PASS" : "FAIL",
                isolationPass ? "PASS" : "FAIL",
                toneCandidateLabel.c_str(),
                idleCandidateLabel.c_str(),
                isolationThresholdMode,
                overallPass ? "PASS" : "FAIL");

    if (!reportJsonPath.empty()) {
        std::string json;
        json += "{\n";
        json += "  \"generatedUtcIso\": " + JsonString(GetCurrentUtcIsoString()) + ",\n";
        json += "  \"positivePass\": " + JsonBool(positivePass) + ",\n";
        json += "  \"isolationPass\": " + JsonBool(isolationPass) + ",\n";
        json += "  \"overallPass\": " + JsonBool(overallPass) + ",\n";
        json += "  \"toneCandidateLabel\": " + JsonString(toneCandidateLabel) + ",\n";
        json += "  \"idleCandidateLabel\": " + JsonString(idleCandidateLabel) + ",\n";
        json += "  \"isolationThresholdMode\": " + JsonString(isolationThresholdMode) + ",\n";
        json += "  \"toneProbe\": " + SerializeProbeReportBlock(toneProbe) + ",\n";
        json += "  \"toneCapture\": " + SerializeCaptureStatsBlock(toneStats) + ",\n";
        json += "  \"idleProbe\": " + SerializeProbeReportBlock(idleProbe) + ",\n";
        json += "  \"idleCapture\": " + SerializeCaptureStatsBlock(idleStats) + "\n";
        json += "}\n";
        WriteOptionalReportJson(reportJsonPath, json);
    }

    return overallPass ? 0 : 1;
}

int CmdSelfTestMuteKillRecovery(const std::wstring& journalPathArg, const std::wstring& reportJsonPath) {
    const std::wstring journalPath = journalPathArg.empty() ? GetTempKillRecoveryJournalPath() : journalPathArg;
    DeleteFileW(journalPath.c_str());

    struct ScopedJournalCleanup {
        std::wstring path;
        ~ScopedJournalCleanup() {
            if (!path.empty()) {
                DeleteFileW(path.c_str());
            }
        }
    };

    const DWORD pid = GetCurrentProcessId();
    const DWORD tick = GetTickCount();
    const std::wstring toneReadyName = L"Local\\StageD_KillRec_ToneReady_" +
                                       std::to_wstring(pid) + L"_" + std::to_wstring(tick);
    const std::wstring toneStopName = L"Local\\StageD_KillRec_ToneStop_" +
                                      std::to_wstring(pid) + L"_" + std::to_wstring(tick);
    const std::wstring muteReadyName = L"Local\\StageD_KillRec_MuteReady_" +
                                       std::to_wstring(pid) + L"_" + std::to_wstring(tick);
    const std::wstring muteStopName = L"Local\\StageD_KillRec_MuteStop_" +
                                      std::to_wstring(pid) + L"_" + std::to_wstring(tick);

    ScopedJournalCleanup journalCleanup{ journalPath };
    ScopedHandle toneReady(CreateEventW(nullptr, TRUE, FALSE, toneReadyName.c_str()));
    ScopedHandle muteReady(CreateEventW(nullptr, TRUE, FALSE, muteReadyName.c_str()));

    ChildProcessGuard toneChild;
    toneChild.stopEvent.Reset(CreateEventW(nullptr, TRUE, FALSE, toneStopName.c_str()));
    ChildProcessGuard muteChild;
    muteChild.stopEvent.Reset(CreateEventW(nullptr, TRUE, FALSE, muteStopName.c_str()));

    if (!toneReady.IsValid() || !muteReady.IsValid() ||
        !toneChild.stopEvent.IsValid() || !muteChild.stopEvent.IsValid()) {
        std::fprintf(stderr, "Failed creating kill-recovery selftest events\n");
        return 1;
    }

    // Helper 1: Keep a live WASAPI audio player session alive throughout the crash & recovery cycle.
    std::string spawnErr;
    const std::wstring toneArgs =
        L"--helper-tone --freq 440 --amp 0.005 --max-sec 30 --ready-event " + QuoteCmdArg(toneReadyName) +
        L" --stop-event " + QuoteCmdArg(toneStopName);
    if (!SpawnHiddenSelfHelper(toneArgs, toneChild, spawnErr)) {
        std::fprintf(stderr, "Failed spawning Helper 1 (--helper-tone): %s\n", spawnErr.c_str());
        return 1;
    }

    if (!WaitReadyOrChildExit(toneReady.Get(), toneChild.pi.hProcess, 10000, spawnErr)) {
        std::fprintf(stderr, "Helper 1 readiness failed: %s\n", spawnErr.c_str());
        return 1;
    }

    const DWORD tonePid = toneChild.pi.dwProcessId;
    Win32AudioSessionAccess access;
    bool helper1SessionVisible = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::vector<AudioSessionHandleInfo> sessions;
        std::string enumErr;
        if (access.EnumerateSessions(sessions, enumErr)) {
            for (const auto& s : sessions) {
                if (s.identity.processId == tonePid &&
                    !s.identity.isMultiProcessSession &&
                    !s.identity.isSystemSoundsSession &&
                    !s.identity.endpointId.empty() &&
                    !s.identity.sessionInstanceId.empty() &&
                    !s.identity.exeFullPath.empty()) {
                    helper1SessionVisible = true;
                    break;
                }
            }
        }
        if (helper1SessionVisible) {
            break;
        }
        if (WaitForSingleObject(toneChild.pi.hProcess, 50) != WAIT_TIMEOUT) {
            std::fprintf(stderr, "Helper 1 exited before its WASAPI session became visible\n");
            return 1;
        }
    }
    if (!helper1SessionVisible) {
        std::fprintf(stderr, "Helper 1 WASAPI session did not become visible within 2s\n");
        return 1;
    }

    // Helper 2: Mutator/producer process that writes the journal and mutes Helper 1's live session.
    const std::wstring muteArgs =
        L"--helper-mute-target --target-pid " + std::to_wstring(tonePid) +
        L" --max-sec 15 --journal " + QuoteCmdArg(journalPath) +
        L" --ready-event " + QuoteCmdArg(muteReadyName) +
        L" --stop-event " + QuoteCmdArg(muteStopName);
    if (!SpawnHiddenSelfHelper(muteArgs, muteChild, spawnErr)) {
        std::fprintf(stderr, "Failed spawning Helper 2 (--helper-mute-target): %s\n", spawnErr.c_str());
        return 1;
    }

    if (!WaitReadyOrChildExit(muteReady.Get(), muteChild.pi.hProcess, 10000, spawnErr)) {
        std::fprintf(stderr, "Helper 2 readiness failed: %s\n", spawnErr.c_str());
        return 1;
    }

    const bool journalExistedBeforeKill = (GetFileAttributesW(journalPath.c_str()) != INVALID_FILE_ATTRIBUTES);

    bool helper1MutedBeforeKill = false;
    {
        std::vector<AudioSessionHandleInfo> sessions;
        std::string enumErr;
        if (access.EnumerateSessions(sessions, enumErr)) {
            for (const auto& s : sessions) {
                if (s.identity.processId == tonePid &&
                    !s.identity.isMultiProcessSession &&
                    !s.identity.isSystemSoundsSession) {
                    helper1MutedBeforeKill = s.state.mute;
                    break;
                }
            }
        }
    }

    // Intentionally terminate Helper 2 (mutator/producer) hard while Helper 1 (player) stays alive.
    const BOOL killedMutator = TerminateProcess(muteChild.pi.hProcess, 99u);
    WaitForSingleObject(muteChild.pi.hProcess, 5000);
    CloseHandle(muteChild.pi.hProcess);
    CloseHandle(muteChild.pi.hThread);
    muteChild.pi.hProcess = nullptr;
    muteChild.pi.hThread = nullptr;
    muteChild.active = false;

    const bool journalPersistedAfterKill = (GetFileAttributesW(journalPath.c_str()) != INVALID_FILE_ATTRIBUTES);

    Sleep(100);

    JournalRestoreSummary firstRestore;
    const bool firstOk = SessionMuteJournal::RestoreFromJournal(access, journalPath, firstRestore);
    const bool journalRemovedAfterRestore = (GetFileAttributesW(journalPath.c_str()) == INVALID_FILE_ATTRIBUTES);

    bool restoredStatusExact =
        (firstRestore.items.size() == 1) &&
        (firstRestore.items[0].status == RestoreStatus::Restored);

    bool restoredSessionFound = false;
    bool restoredValuesMatch = false;
    if (restoredStatusExact) {
        const auto& entry = firstRestore.items[0].entry;
        std::vector<AudioSessionHandleInfo> postSessions;
        std::string enumErr;
        if (access.EnumerateSessions(postSessions, enumErr)) {
            for (const auto& s : postSessions) {
                if (s.identity.endpointId == entry.endpointId &&
                    s.identity.sessionInstanceId == entry.sessionInstanceId &&
                    EqualsIgnoreCase(s.identity.exeFullPath, entry.exeFullPath)) {
                    restoredSessionFound = true;
                    restoredValuesMatch =
                        (s.state.mute == entry.origMute) &&
                        (std::fabs(s.state.masterVolume - entry.origVolume) <=
                            SessionMuteJournal::kVolumeCompareTolerance);
                    break;
                }
            }
        }
    }

    JournalRestoreSummary secondRestore;
    const bool secondIdempotentOk = SessionMuteJournal::RestoreFromJournal(access, journalPath, secondRestore) &&
                                    !secondRestore.journalExisted;

    toneChild.Cleanup();

    const bool overallPass =
        helper1SessionVisible &&
        journalExistedBeforeKill &&
        helper1MutedBeforeKill &&
        (killedMutator != FALSE) &&
        journalPersistedAfterKill &&
        firstOk &&
        firstRestore.journalExisted &&
        firstRestore.journalParsed &&
        firstRestore.journalRemovedOrArchived &&
        journalRemovedAfterRestore &&
        restoredStatusExact &&
        restoredSessionFound &&
        restoredValuesMatch &&
        secondIdempotentOk;

    std::printf("=== Selftest Mute-Kill Recovery (Helper1 player stays alive, Helper2 mutator killed) ===\n");
    std::printf("  journalPath              : %s\n", WideToUtf8(journalPath).c_str());
    std::printf("  helper1Pid               : %lu\n", static_cast<unsigned long>(tonePid));
    std::printf("  helper1MutedBeforeKill   : %s\n", helper1MutedBeforeKill ? "true" : "false");
    std::printf("  existedBeforeKill        : %s\n", journalExistedBeforeKill ? "true" : "false");
    std::printf("  terminatedMutator        : %s\n", killedMutator ? "true" : "false");
    std::printf("  persistedAfterKill       : %s\n", journalPersistedAfterKill ? "true" : "false");
    std::printf("  firstRestoreOk           : %s (items=%zu, removed=%s)\n",
                firstOk ? "true" : "false",
                firstRestore.items.size(),
                journalRemovedAfterRestore ? "true" : "false");
    if (!firstRestore.items.empty()) {
        std::printf("  item[0] status           : %s (%s)\n",
                    RestoreStatusToString(firstRestore.items[0].status),
                    firstRestore.items[0].detail.c_str());
    }
    std::printf("  restoredSessionFound     : %s\n", restoredSessionFound ? "true" : "false");
    std::printf("  restoredValuesMatch      : %s\n", restoredValuesMatch ? "true" : "false");
    std::printf("  secondRestoreIdempotent  : %s\n", secondIdempotentOk ? "true" : "false");
    std::printf("  verdict                  : %s\n", overallPass ? "PASS" : "FAIL");

    if (!reportJsonPath.empty()) {
        std::string json;
        json += "{\n";
        json += "  \"generatedUtcIso\": " + JsonString(GetCurrentUtcIsoString()) + ",\n";
        json += "  \"journalPath\": " + JsonWString(journalPath) + ",\n";
        json += "  \"helper1Pid\": " + std::to_string(tonePid) + ",\n";
        json += "  \"helper1MutedBeforeKill\": " + JsonBool(helper1MutedBeforeKill) + ",\n";
        json += "  \"existedBeforeKill\": " + JsonBool(journalExistedBeforeKill) + ",\n";
        json += "  \"terminatedMutator\": " + JsonBool(killedMutator != FALSE) + ",\n";
        json += "  \"persistedAfterKill\": " + JsonBool(journalPersistedAfterKill) + ",\n";
        json += "  \"journalRemovedAfterRestore\": " + JsonBool(journalRemovedAfterRestore) + ",\n";
        json += "  \"restoredStatusExact\": " + JsonBool(restoredStatusExact) + ",\n";
        json += "  \"restoredSessionFound\": " + JsonBool(restoredSessionFound) + ",\n";
        json += "  \"restoredValuesMatch\": " + JsonBool(restoredValuesMatch) + ",\n";
        json += "  \"secondRestoreIdempotent\": " + JsonBool(secondIdempotentOk) + ",\n";
        json += "  \"overallPass\": " + JsonBool(overallPass) + ",\n";
        json += "  \"restoreSummary\": " + SerializeRestoreSummaryBlock(firstRestore) + "\n";
        json += "}\n";
        WriteOptionalReportJson(reportJsonPath, json);
    }

    return overallPass ? 0 : 1;
}

// ============================================================================
// Hidden Child Helper Processes
// ============================================================================

int RunHelperIdle(const std::wstring& readyEventName, const std::wstring& stopEventName, double maxSec) {
    ScopedHandle readyEv(!readyEventName.empty() ? OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName.c_str()) : nullptr);
    ScopedHandle stopEv(!stopEventName.empty() ? OpenEventW(SYNCHRONIZE, FALSE, stopEventName.c_str()) : nullptr);
    if ((!readyEventName.empty() && !readyEv.IsValid()) ||
        (!stopEventName.empty() && !stopEv.IsValid())) {
        return 10;
    }

    if (readyEv.IsValid()) {
        SetEvent(readyEv.Get());
    }
    const DWORD timeoutMs = static_cast<DWORD>(std::clamp(maxSec, 0.5, 60.0) * 1000.0);
    if (stopEv.IsValid()) {
        WaitForSingleObject(stopEv.Get(), timeoutMs);
    } else {
        Sleep(timeoutMs);
    }
    return 0;
}

int RunHelperTone(double freqHz,
                  double amp,
                  const std::wstring& readyEventName,
                  const std::wstring& stopEventName,
                  double maxSec) {
    ComMtaInit com;
    if (!com.Usable()) {
        return 10;
    }

    ScopedHandle readyEv(!readyEventName.empty() ? OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName.c_str()) : nullptr);
    ScopedHandle stopEv(!stopEventName.empty() ? OpenEventW(SYNCHRONIZE, FALSE, stopEventName.c_str()) : nullptr);
    if ((!readyEventName.empty() && !readyEv.IsValid()) ||
        (!stopEventName.empty() && !stopEv.IsValid())) {
        return 10;
    }

    WasapiSineRenderHelper renderer;
    std::string err;
    if (!renderer.Start(freqHz, amp, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 11;
    }

    if (readyEv.IsValid()) {
        SetEvent(readyEv.Get());
    }

    const double clampedMax = std::clamp(maxSec, 0.5, 60.0);
    const auto startTp = std::chrono::steady_clock::now();

    while (true) {
        const auto nowTp = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(nowTp - startTp).count();
        if (elapsed >= clampedMax) {
            break;
        }
        if (stopEv.IsValid()) {
            if (WaitForSingleObject(stopEv.Get(), 15) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            Sleep(15);
        }

        if (!renderer.PumpOnce(err)) {
            return 12;
        }
    }

    renderer.Stop();
    return 0;
}

int RunHelperMuteTarget(uint32_t targetPid,
                        const std::wstring& journalPath,
                        const std::wstring& readyEventName,
                        const std::wstring& stopEventName,
                        double maxSec) {
    if (targetPid == 0 || journalPath.empty()) {
        return 10;
    }
    ComMtaInit com;
    if (!com.Usable()) {
        return 10;
    }

    ScopedHandle readyEv(!readyEventName.empty() ? OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName.c_str()) : nullptr);
    ScopedHandle stopEv(!stopEventName.empty() ? OpenEventW(SYNCHRONIZE, FALSE, stopEventName.c_str()) : nullptr);
    if ((!readyEventName.empty() && !readyEv.IsValid()) ||
        (!stopEventName.empty() && !stopEv.IsValid())) {
        return 10;
    }

    Win32AudioSessionAccess access;
    SessionVolumeIdentity targetId{};
    float origVol = 1.0f;
    bool found = false;
    std::string err;

    for (int attempt = 0; attempt < 40; ++attempt) {
        if (stopEv.IsValid() && WaitForSingleObject(stopEv.Get(), 0) == WAIT_OBJECT_0) {
            return 13;
        }
        std::vector<AudioSessionHandleInfo> sessions;
        if (access.EnumerateSessions(sessions, err)) {
            for (const auto& s : sessions) {
                if (s.identity.processId == targetPid &&
                    !s.identity.isMultiProcessSession &&
                    !s.identity.isSystemSoundsSession &&
                    !s.identity.endpointId.empty() &&
                    !s.identity.sessionInstanceId.empty() &&
                    !s.identity.exeFullPath.empty()) {
                    targetId = s.identity;
                    origVol = s.state.masterVolume;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            break;
        }
        if (stopEv.IsValid()) {
            if (WaitForSingleObject(stopEv.Get(), 50) == WAIT_OBJECT_0) {
                return 13;
            }
        } else {
            Sleep(50);
        }
    }

    if (!found) {
        std::fprintf(stderr, "RunHelperMuteTarget: target PID %u single-process session not found\n", targetPid);
        return 13;
    }

    JournalDocument doc;
    if (!SessionMuteJournal::BeginApplyMute(access, journalPath, targetId, true, origVol, doc, err)) {
        std::fprintf(stderr, "RunHelperMuteTarget: BeginApplyMute failed: %s\n", err.c_str());
        return 14;
    }

    if (readyEv.IsValid()) {
        SetEvent(readyEv.Get());
    }

    const DWORD timeoutMs = static_cast<DWORD>(std::clamp(maxSec, 0.5, 30.0) * 1000.0);
    if (stopEv.IsValid()) {
        WaitForSingleObject(stopEv.Get(), timeoutMs);
    } else {
        Sleep(timeoutMs);
    }
    return 0;
}

void PrintUsage() {
    std::printf(
        "stage_d_audio_probe — Stage D Phase D0 Audio Topology & Process Loopback Probe\n\n"
        "D0a Read-Only Subcommands (zero mute/volume changes):\n"
        "  --list-topology [--report-json <path>]\n"
        "  --probe-capture (--target-exe <name> | --target-pid <pid>) [--mode include|exclude]\n"
        "                  [--duration-sec <N>] [--report-json <path>] [--dump-wav <path>]\n\n"
        "D0b State-Modifying / Host Integration Subcommands:\n"
        "  --test-mute-isolation (--target-pid <pid> | --target-exe <name>)\n"
        "                        [--duration-sec <N>] [--journal <path>] [--report-json <path>]\n"
        "  --restore-audio [--journal <path>] [--report-json <path>]\n"
        "  --selftest-local-loopback [--duration-sec <N>] [--report-json <path>]\n"
        "  --selftest-mute-kill-recovery [--journal <path>] [--report-json <path>]\n");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    ComMtaInit com;
    if (!com.Usable()) {
        std::fprintf(stderr, "CoInitializeEx(COINIT_MULTITHREADED) failed: %s\n",
                     FormatHResultHex(com.hr).c_str());
        return 1;
    }

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    enum class Command {
        None,
        Help,
        ListTopology,
        ProbeCapture,
        TestMuteIsolation,
        RestoreAudio,
        SelfTestLocalLoopback,
        SelfTestMuteKillRecovery,
        HelperTone,
        HelperIdle,
        HelperMuteTarget,
    };

    Command cmd = Command::None;
    std::wstring targetExe;
    uint32_t targetPid = 0;
    PROCESS_LOOPBACK_MODE mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    double durationSec = -1.0;
    double freqHz = 440.0;
    double amp = 0.025;
    double maxSec = 30.0;
    std::wstring reportJsonPath;
    std::wstring dumpWavPath;
    std::wstring journalPath;
    std::wstring readyEventName;
    std::wstring stopEventName;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto needNext = [&](std::wstring& dst) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for argument: %s\n", WideToUtf8(arg).c_str());
                return false;
            }
            dst = argv[++i];
            return true;
        };

        if (arg == L"--help" || arg == L"-h") cmd = Command::Help;
        else if (arg == L"--list-topology") cmd = Command::ListTopology;
        else if (arg == L"--probe-capture") cmd = Command::ProbeCapture;
        else if (arg == L"--test-mute-isolation") cmd = Command::TestMuteIsolation;
        else if (arg == L"--restore-audio") cmd = Command::RestoreAudio;
        else if (arg == L"--selftest-local-loopback") cmd = Command::SelfTestLocalLoopback;
        else if (arg == L"--selftest-mute-kill-recovery") cmd = Command::SelfTestMuteKillRecovery;
        else if (arg == L"--helper-tone") cmd = Command::HelperTone;
        else if (arg == L"--helper-idle") cmd = Command::HelperIdle;
        else if (arg == L"--helper-mute-target") cmd = Command::HelperMuteTarget;
        else if (arg == L"--target-exe") {
            if (!needNext(targetExe)) return 1;
        } else if (arg == L"--target-pid") {
            std::wstring val;
            if (!needNext(val) || !ParseUint32Arg(val.c_str(), targetPid) || targetPid == 0) {
                std::fprintf(stderr, "Invalid --target-pid\n");
                return 1;
            }
        } else if (arg == L"--mode") {
            std::wstring val;
            if (!needNext(val)) return 1;
            if (EqualsIgnoreCase(val, L"include")) {
                mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
            } else if (EqualsIgnoreCase(val, L"exclude")) {
                mode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
            } else {
                std::fprintf(stderr, "Invalid --mode '%s' (expected include|exclude)\n",
                             WideToUtf8(val).c_str());
                return 1;
            }
        } else if (arg == L"--duration-sec") {
            std::wstring val;
            if (!needNext(val) || !ParseDoubleArg(val.c_str(), durationSec) || durationSec < 0.0) {
                std::fprintf(stderr, "Invalid --duration-sec\n");
                return 1;
            }
        } else if (arg == L"--freq") {
            std::wstring val;
            if (!needNext(val) || !ParseDoubleArg(val.c_str(), freqHz) || freqHz <= 0.0) {
                std::fprintf(stderr, "Invalid --freq\n");
                return 1;
            }
        } else if (arg == L"--amp") {
            std::wstring val;
            if (!needNext(val) || !ParseDoubleArg(val.c_str(), amp) || amp < 0.0 || amp > 1.0) {
                std::fprintf(stderr, "Invalid --amp\n");
                return 1;
            }
        } else if (arg == L"--max-sec") {
            std::wstring val;
            if (!needNext(val) || !ParseDoubleArg(val.c_str(), maxSec) || maxSec <= 0.0) {
                std::fprintf(stderr, "Invalid --max-sec\n");
                return 1;
            }
        } else if (arg == L"--report-json") {
            if (!needNext(reportJsonPath)) return 1;
        } else if (arg == L"--dump-wav") {
            if (!needNext(dumpWavPath)) return 1;
        } else if (arg == L"--journal") {
            if (!needNext(journalPath)) return 1;
        } else if (arg == L"--ready-event") {
            if (!needNext(readyEventName)) return 1;
        } else if (arg == L"--stop-event") {
            if (!needNext(stopEventName)) return 1;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", WideToUtf8(arg).c_str());
            PrintUsage();
            return 1;
        }
    }

    switch (cmd) {
    case Command::Help:
        PrintUsage();
        return 0;
    case Command::ListTopology:
        return CmdListTopology(reportJsonPath);
    case Command::ProbeCapture:
        return CmdProbeCapture(targetExe, targetPid, mode,
                               (durationSec >= 0.0) ? durationSec : 5.0,
                               reportJsonPath, dumpWavPath);
    case Command::TestMuteIsolation:
        return CmdTestMuteIsolation(targetExe, targetPid,
                                    (durationSec > 0.0) ? durationSec : 3.0,
                                    journalPath, reportJsonPath);
    case Command::RestoreAudio:
        return CmdRestoreAudio(journalPath, reportJsonPath);
    case Command::SelfTestLocalLoopback:
        return CmdSelfTestLocalLoopback((durationSec > 0.0) ? durationSec : 2.0, reportJsonPath);
    case Command::SelfTestMuteKillRecovery:
        return CmdSelfTestMuteKillRecovery(journalPath, reportJsonPath);
    case Command::HelperTone:
        return RunHelperTone(freqHz, amp, readyEventName, stopEventName, maxSec);
    case Command::HelperIdle:
        return RunHelperIdle(readyEventName, stopEventName, maxSec);
    case Command::HelperMuteTarget:
        return RunHelperMuteTarget(targetPid, journalPath, readyEventName, stopEventName, maxSec);
    default:
        PrintUsage();
        return 1;
    }
}
