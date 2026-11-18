#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "ipc_ring_protocol.h"
#include "stage_d_capture.h"
#include "stage_d_timeline.h"

#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")
#endif

namespace {

volatile LONG g_ctrlStopRequested = 0;

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedExchange(&g_ctrlStopRequested, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

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
    void Attach(T* p) noexcept {
        Reset();
        ptr_ = p;
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

struct ProducerOptions {
    std::wstring mappingName = stagec::kDefaultMappingName;
    double durationSec = 0.0;
    std::wstring stopEventName;
    std::wstring readyEventName;
    uint32_t targetPid = 0u;
    std::wstring targetExe;
    uint32_t prefillMs = 0u;
    std::wstring telemetryJsonlPath;
    uint32_t telemetryIntervalMs = 1000u;
    PROCESS_LOOPBACK_MODE mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
};

bool ParseDoubleArg(const wchar_t* s, double& outVal) {
    if (!s || !*s || s[0] == L' ' || s[0] == L'\t') {
        return false;
    }
    errno = 0;
    wchar_t* endPtr = nullptr;
    const double v = std::wcstod(s, &endPtr);
    if (errno == ERANGE || endPtr == s || *endPtr != L'\0' || !std::isfinite(v)) {
        return false;
    }
    outVal = v;
    return true;
}

bool ParseU32Arg(const wchar_t* s, uint32_t& outVal) {
    if (!s || !*s || s[0] < L'0' || s[0] > L'9') {
        return false;
    }
    errno = 0;
    wchar_t* endPtr = nullptr;
    const unsigned long long v = std::wcstoull(s, &endPtr, 10);
    if (errno == ERANGE || endPtr == s || *endPtr != L'\0' ||
        v > static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
        return false;
    }
    outVal = static_cast<uint32_t>(v);
    return true;
}

void PrintUsage() {
    std::printf(
        "Usage: iv_stage_d_producer.exe [options]\n"
        "  --mapping-name <wstr>          Shared memory mapping name (default Local\\GTAIV_StageC_IpcRing_v1)\n"
        "  --duration-sec <double>        Run duration in seconds (default 0 = until Ctrl+C/stop event)\n"
        "  --stop-event-name <wstr>       Optional named Win32 stop event\n"
        "  --ready-event-name <wstr>      Optional named Win32 ready event\n"
        "  --target-pid <uint32>          Explicit target process ID (overrides auto-discovery)\n"
        "  --target-exe <wstr>            Explicit target executable name (overrides default AMPLibraryAgent.exe)\n"
        "  --prefill-ms <uint32>          Optional startup zero prefill milliseconds (default 0)\n"
        "  --telemetry-jsonl <wstr>       Optional JSONL telemetry output path\n"
        "  --telemetry-interval-ms <u32>  Telemetry snapshot period in ms (default 1000)\n"
        "  --mode <include|exclude>       Process loopback tree mode (default include)\n"
        "  --help                         Show this message\n");
}

bool ParseCommandLine(int argc, wchar_t** argv, ProducerOptions& opts, bool& outShowHelp) {
    outShowHelp = false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* arg = argv[i];
        if (_wcsicmp(arg, L"--help") == 0 || _wcsicmp(arg, L"-h") == 0) {
            PrintUsage();
            outShowHelp = true;
            return false;
        } else if (_wcsicmp(arg, L"--mapping-name") == 0 && i + 1 < argc) {
            opts.mappingName = argv[++i];
        } else if (_wcsicmp(arg, L"--duration-sec") == 0 && i + 1 < argc) {
            if (!ParseDoubleArg(argv[++i], opts.durationSec) || opts.durationSec < 0.0) {
                std::fprintf(stderr, "Invalid --duration-sec value\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--stop-event-name") == 0 && i + 1 < argc) {
            opts.stopEventName = argv[++i];
        } else if (_wcsicmp(arg, L"--ready-event-name") == 0 && i + 1 < argc) {
            opts.readyEventName = argv[++i];
        } else if (_wcsicmp(arg, L"--target-pid") == 0 && i + 1 < argc) {
            if (!ParseU32Arg(argv[++i], opts.targetPid) || opts.targetPid == 0u) {
                std::fprintf(stderr, "Invalid --target-pid value (must be > 0)\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--target-exe") == 0 && i + 1 < argc) {
            opts.targetExe = argv[++i];
        } else if (_wcsicmp(arg, L"--prefill-ms") == 0 && i + 1 < argc) {
            if (!ParseU32Arg(argv[++i], opts.prefillMs)) {
                std::fprintf(stderr, "Invalid --prefill-ms value\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--telemetry-jsonl") == 0 && i + 1 < argc) {
            opts.telemetryJsonlPath = argv[++i];
        } else if (_wcsicmp(arg, L"--telemetry-interval-ms") == 0 && i + 1 < argc) {
            if (!ParseU32Arg(argv[++i], opts.telemetryIntervalMs) || opts.telemetryIntervalMs == 0u) {
                std::fprintf(stderr, "Invalid --telemetry-interval-ms value\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--mode") == 0 && i + 1 < argc) {
            const wchar_t* m = argv[++i];
            if (_wcsicmp(m, L"include") == 0) {
                opts.mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
            } else if (_wcsicmp(m, L"exclude") == 0) {
                opts.mode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
            } else {
                std::fprintf(stderr, "Invalid --mode value: %ls\n", m);
                return false;
            }
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument: %ls\n", arg);
            return false;
        }
    }
    return true;
}

void SignalNamedEvent(const std::wstring& eventName) {
    if (eventName.empty()) {
        return;
    }
    HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (!hEvt) {
        hEvt = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    }
    if (hEvt) {
        SetEvent(hEvt);
        CloseHandle(hEvt);
    }
}

HANDLE OpenOrCreateNamedEvent(const std::wstring& eventName, bool manualReset) {
    if (eventName.empty()) {
        return nullptr;
    }
    HANDLE hEvt = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (!hEvt) {
        hEvt = CreateEventW(nullptr, manualReset ? TRUE : FALSE, FALSE, eventName.c_str());
    }
    return hEvt;
}

bool ShouldStopNow(HANDLE hExternalStopEvent) {
    if (InterlockedCompareExchange(&g_ctrlStopRequested, 0, 0) != 0) {
        return true;
    }
    if (hExternalStopEvent && WaitForSingleObject(hExternalStopEvent, 0) == WAIT_OBJECT_0) {
        return true;
    }
    return false;
}

bool ContainsIgnoreCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    auto it = std::search(
        haystack.begin(),
        haystack.end(),
        needle.begin(),
        needle.end(),
        [](wchar_t a, wchar_t b) {
            return std::towlower(a) == std::towlower(b);
        });
    return it != haystack.end();
}

bool IsAppleMusicStorePath(const std::wstring& fullPath) {
    return ContainsIgnoreCase(fullPath, L"AppleInc.AppleMusicWin");
}

bool IsProcessAlivePid(DWORD pid) {
    if (pid == 0u) {
        return false;
    }
    ScopedHandle hProc(OpenProcess(SYNCHRONIZE, FALSE, pid));
    if (hProc.IsValid()) {
        return WaitForSingleObject(hProc.Get(), 0) == WAIT_TIMEOUT;
    }
    std::vector<staged::ProcessEntry> procs;
    std::string err;
    if (!staged::SnapshotProcesses(procs, err)) {
        return false;
    }
    for (const auto& p : procs) {
        if (p.pid == pid) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// IPC Ring Frame Sink
// ============================================================================

class RingSharedMemorySink final : public staged::IRingFrameSink {
public:
    explicit RingSharedMemorySink(stagec::StageCIpcSharedBlock* shared) noexcept
        : shared_(shared) {}

    bool WriteCommittedFrames(uint64_t startFrame,
                              const int16_t* stereoLR,
                              uint32_t frameCount) override {
        if (frameCount == 0u) {
            return true;
        }
        if (!shared_ || !stereoLR) {
            return false;
        }

        constexpr uint32_t kMaxChunk = stagec::kRingCapacityFrames / 4u;
        uint64_t curFrame = startFrame;
        const int16_t* src = stereoLR;
        uint32_t remaining = frameCount;

        while (remaining > 0u) {
            const uint32_t chunk = std::min<uint32_t>(remaining, kMaxChunk);
            const uint64_t newReserve = curFrame + static_cast<uint64_t>(chunk);

            stagec::AtomicStoreU64(&shared_->header.write_reserve_frame, newReserve);

            const uint32_t startSlot = static_cast<uint32_t>(curFrame & stagec::kRingFrameMask);
            const uint32_t firstPart = std::min<uint32_t>(chunk, stagec::kRingCapacityFrames - startSlot);
            const uint32_t secondPart = chunk - firstPart;

            std::memcpy(&shared_->pcm_samples[static_cast<size_t>(startSlot) * 2u],
                        src,
                        static_cast<size_t>(firstPart) * 2u * sizeof(int16_t));
            if (secondPart > 0u) {
                std::memcpy(&shared_->pcm_samples[0],
                            src + static_cast<size_t>(firstPart) * 2u,
                            static_cast<size_t>(secondPart) * 2u * sizeof(int16_t));
            }

            stagec::AtomicStoreU64(&shared_->header.write_commit_frame, newReserve);

            curFrame = newReserve;
            src += static_cast<size_t>(chunk) * 2u;
            remaining -= chunk;
        }
        return true;
    }

private:
    stagec::StageCIpcSharedBlock* shared_ = nullptr;
};

// ============================================================================
// Target Resolution & 48kHz Format Selection (D0 Contracts)
// ============================================================================

struct ResolvedCaptureTarget {
    uint32_t pid = 0u;
    std::wstring exeName;
    std::wstring exeFullPath;
    std::string resolutionTier;
};

bool ResolveTargetSessionMatches(const staged::AudioSessionRecord& sess,
                                 const std::wstring& desiredExeLeaf) {
    if (sess.isMultiProcessSession || sess.isSystemSoundsSession || sess.processId == 0u) {
        return false;
    }
    const std::wstring sessLeaf = !sess.exeFullPath.empty()
        ? staged::ExtractLeafFilename(sess.exeFullPath)
        : sess.exeName;
    if (staged::EqualsIgnoreCase(sessLeaf, desiredExeLeaf)) {
        return true;
    }
    if (!sess.exeName.empty() && staged::EqualsIgnoreCase(sess.exeName, desiredExeLeaf)) {
        return true;
    }
    return false;
}

bool ResolveCaptureTarget(const ProducerOptions& opts,
                          ResolvedCaptureTarget& outTarget,
                          std::string& outDetail) {
    outTarget = ResolvedCaptureTarget{};
    outDetail.clear();

    // Explicit --target-pid override
    if (opts.targetPid != 0u) {
        std::vector<staged::ProcessEntry> procs;
        std::string snapErr;
        staged::SnapshotProcesses(procs, snapErr);
        outTarget.pid = opts.targetPid;
        outTarget.exeName = !opts.targetExe.empty() ? opts.targetExe : L"explicit_pid";
        for (const auto& p : procs) {
            if (p.pid == opts.targetPid) {
                outTarget.exeName = p.exeName;
                outTarget.exeFullPath = p.exeFullPath;
                break;
            }
        }
        if (!IsProcessAlivePid(opts.targetPid)) {
            outDetail = "Explicit target PID " + std::to_string(opts.targetPid) + " is not currently alive";
            return false;
        }
        outTarget.resolutionTier = "explicit_pid";
        outDetail = "Explicit --target-pid=" + std::to_string(outTarget.pid) +
                    " (" + staged::WideToUtf8(outTarget.exeName) + ")";
        return true;
    }

    const bool defaultAppleMusicMode = opts.targetExe.empty();
    const std::wstring targetLeaf = defaultAppleMusicMode
        ? std::wstring(L"AMPLibraryAgent.exe")
        : staged::ExtractLeafFilename(opts.targetExe);

    staged::AudioTopologyReport topo;
    std::string topoErr;
    const bool hasTopo = staged::DiscoverAudioTopology(topo, topoErr);

    if (hasTopo) {
        // Priority (1): Active single-process non-system session (preferring AppleInc.AppleMusicWin path)
        for (int pass = 0; pass < 2; ++pass) {
            const bool requireStorePath = (pass == 0) && defaultAppleMusicMode;
            for (const auto& ep : topo.endpoints) {
                for (const auto& sess : ep.sessions) {
                    if (!ResolveTargetSessionMatches(sess, targetLeaf)) {
                        continue;
                    }
                    if (sess.sessionState != AudioSessionStateActive) {
                        continue;
                    }
                    if (requireStorePath && !IsAppleMusicStorePath(sess.exeFullPath)) {
                        continue;
                    }
                    if (!IsProcessAlivePid(sess.processId)) {
                        continue;
                    }
                    outTarget.pid = sess.processId;
                    outTarget.exeName = !sess.exeName.empty() ? sess.exeName : targetLeaf;
                    outTarget.exeFullPath = sess.exeFullPath;
                    outTarget.resolutionTier = requireStorePath
                        ? "apple_store_active_session"
                        : "active_session";
                    outDetail = "Resolved active session PID=" + std::to_string(outTarget.pid) +
                                " exe=" + staged::WideToUtf8(outTarget.exeName) +
                                " tier=" + outTarget.resolutionTier;
                    return true;
                }
            }
        }

        // Priority (2): Inactive single-process non-system session (preferring AppleInc.AppleMusicWin path)
        for (int pass = 0; pass < 2; ++pass) {
            const bool requireStorePath = (pass == 0) && defaultAppleMusicMode;
            for (const auto& ep : topo.endpoints) {
                for (const auto& sess : ep.sessions) {
                    if (!ResolveTargetSessionMatches(sess, targetLeaf)) {
                        continue;
                    }
                    if (sess.sessionState != AudioSessionStateInactive) {
                        continue;
                    }
                    if (requireStorePath && !IsAppleMusicStorePath(sess.exeFullPath)) {
                        continue;
                    }
                    if (!IsProcessAlivePid(sess.processId)) {
                        continue;
                    }
                    outTarget.pid = sess.processId;
                    outTarget.exeName = !sess.exeName.empty() ? sess.exeName : targetLeaf;
                    outTarget.exeFullPath = sess.exeFullPath;
                    outTarget.resolutionTier = requireStorePath
                        ? "apple_store_inactive_session"
                        : "inactive_session";
                    outDetail = "Resolved inactive session PID=" + std::to_string(outTarget.pid) +
                                " exe=" + staged::WideToUtf8(outTarget.exeName) +
                                " tier=" + outTarget.resolutionTier;
                    return true;
                }
            }
        }
    }

    // Priority (3): Running process snapshot lookup (preferring AppleInc.AppleMusicWin path)
    std::vector<staged::ProcessEntry> procs;
    std::string procErr;
    const std::vector<staged::ProcessEntry>* procList = nullptr;
    if (hasTopo && !topo.processes.empty()) {
        procList = &topo.processes;
    } else if (staged::SnapshotProcesses(procs, procErr)) {
        procList = &procs;
    }

    if (procList) {
        for (int pass = 0; pass < 2; ++pass) {
            const bool requireStorePath = (pass == 0) && defaultAppleMusicMode;
            for (const auto& p : *procList) {
                if (p.pid == 0u) {
                    continue;
                }
                const std::wstring leaf = !p.exeName.empty()
                    ? p.exeName
                    : staged::ExtractLeafFilename(p.exeFullPath);
                if (!staged::EqualsIgnoreCase(leaf, targetLeaf)) {
                    continue;
                }
                if (requireStorePath && !IsAppleMusicStorePath(p.exeFullPath)) {
                    continue;
                }
                outTarget.pid = p.pid;
                outTarget.exeName = !p.exeName.empty() ? p.exeName : targetLeaf;
                outTarget.exeFullPath = p.exeFullPath;
                outTarget.resolutionTier = requireStorePath
                    ? "apple_store_process_snapshot"
                    : "process_snapshot";
                outDetail = "Resolved process snapshot PID=" + std::to_string(outTarget.pid) +
                            " exe=" + staged::WideToUtf8(outTarget.exeName) +
                            " tier=" + outTarget.resolutionTier;
                return true;
            }
        }
    }

    outDetail = "Target process '" + staged::WideToUtf8(targetLeaf) + "' not found in audio sessions or process snapshot";
    return false;
}

bool SelectBest48kLoopbackFormat(DWORD targetPid,
                                 PROCESS_LOOPBACK_MODE mode,
                                 staged::FormatCandidateResult& outSelected,
                                 std::string& outError) {
    outSelected = staged::FormatCandidateResult{};
    outError.clear();

    staged::ProcessLoopbackProbeReport probeReport;
    std::string probeErr;
    if (!staged::ProbeProcessLoopbackFormats(targetPid, mode, probeReport, probeErr)) {
        outError = probeErr.empty() ? "ProbeProcessLoopbackFormats failed" : probeErr;
        return false;
    }

    // Enforce D0 Branch A 48kHz contract:
    // Priority: 48000Hz_2ch_Float32_Native -> 48000Hz_2ch_PCM16_Native -> any 48kHz native/auto-convert.
    auto findCandidate = [&](const auto& pred) -> const staged::FormatCandidateResult* {
        for (const auto& c : probeReport.candidates) {
            if (c.succeeded && pred(c)) {
                return &c;
            }
        }
        return nullptr;
    };

    const staged::FormatCandidateResult* chosen = findCandidate([](const staged::FormatCandidateResult& c) {
        return c.label == "48000Hz_2ch_Float32_Native";
    });
    if (!chosen) {
        chosen = findCandidate([](const staged::FormatCandidateResult& c) {
            return c.label == "48000Hz_2ch_PCM16_Native";
        });
    }
    if (!chosen) {
        chosen = findCandidate([](const staged::FormatCandidateResult& c) {
            return c.sampleRate == 48000u && c.channels == 2u && !c.useAutoConvert;
        });
    }
    if (!chosen) {
        chosen = findCandidate([](const staged::FormatCandidateResult& c) {
            return c.sampleRate == 48000u && c.channels == 2u;
        });
    }
    if (!chosen) {
        chosen = findCandidate([](const staged::FormatCandidateResult& c) {
            return c.sampleRate == 48000u;
        });
    }

    if (!chosen) {
        outError = "No 48000 Hz loopback format succeeded (non-48kHz-only loopback is rejected under D0/D1 Branch A contract)";
        return false;
    }

    outSelected = *chosen;
    return true;
}

// ============================================================================
// Bounded Cross-Thread Capture Event Queue
// ============================================================================

struct CaptureQueueItem {
    enum class Type : uint8_t {
        StreamStarted = 0,
        Packet = 1,
        StreamEnded = 2,
    };

    Type type = Type::Packet;
    uint32_t sampleRate = staged::kOutputSampleRate;
    uint32_t convDelayFrames48k = 0u;
    uint32_t targetPid = 0u;
    std::string targetExeUtf8;
    std::string formatLabelUtf8;
    std::string reasonUtf8;

    uint32_t sourceFrames = 0u;
    uint32_t outFrames48k = 0u;
    uint64_t devPos = 0ULL;
    uint64_t qpcPos100ns = 0ULL;
    uint64_t nowQpc100ns = 0ULL;
    bool silentFlag = false;
    bool discontinuityFlag = false;
    bool timestampErrorFlag = false;
    std::vector<int16_t> stereo48kLR;
};

class BoundedCaptureQueue {
public:
    static constexpr size_t kMaxPackets = 1024u;

    void Push(CaptureQueueItem&& item) {
        std::lock_guard<std::mutex> lock(mu_);
        if (item.type == CaptureQueueItem::Type::StreamStarted) {
            markNextDiscontinuity_ = false;
        } else if (item.type == CaptureQueueItem::Type::Packet) {
            if (items_.size() >= kMaxPackets) {
                // Drop oldest packet and attach discontinuity to the next surviving packet
                // in the same stream; only defer to markNextDiscontinuity_ if none remains.
                for (auto it = items_.begin(); it != items_.end(); ++it) {
                    if (it->type == CaptureQueueItem::Type::Packet) {
                        auto after = items_.erase(it);
                        bool markedSurvivor = false;
                        bool crossedStreamStart = false;
                        for (auto nextIt = after; nextIt != items_.end(); ++nextIt) {
                            if (nextIt->type == CaptureQueueItem::Type::StreamStarted) {
                                crossedStreamStart = true;
                                break;
                            }
                            if (nextIt->type == CaptureQueueItem::Type::Packet) {
                                nextIt->discontinuityFlag = true;
                                markedSurvivor = true;
                                break;
                            }
                        }
                        if (!markedSurvivor && !crossedStreamStart) {
                            markNextDiscontinuity_ = true;
                        }
                        break;
                    }
                }
            }
            if (markNextDiscontinuity_) {
                item.discontinuityFlag = true;
                markNextDiscontinuity_ = false;
            }
        }
        items_.push_back(std::move(item));
    }

    void DrainAll(std::vector<CaptureQueueItem>& outItems) {
        outItems.clear();
        std::lock_guard<std::mutex> lock(mu_);
        if (items_.empty()) {
            return;
        }
        outItems.reserve(items_.size());
        while (!items_.empty()) {
            outItems.push_back(std::move(items_.front()));
            items_.pop_front();
        }
    }

private:
    std::mutex mu_;
    std::deque<CaptureQueueItem> items_;
    bool markNextDiscontinuity_ = false;
};

struct CaptureWorkerContext {
    ProducerOptions opts;
    uint64_t qpcFreq = 10000000ULL;
    HANDLE stopEvent = nullptr;
    std::atomic<bool> stopRequested{ false };
    BoundedCaptureQueue* queue = nullptr;
};

bool WaitInterruptibleMs(HANDLE stopEvent, const std::atomic<bool>& stopFlag, DWORD waitMs) {
    if (stopFlag.load(std::memory_order_acquire)) {
        return false;
    }
    if (!stopEvent) {
        Sleep(waitMs);
        return !stopFlag.load(std::memory_order_acquire);
    }
    const DWORD res = WaitForSingleObject(stopEvent, waitMs);
    return (res != WAIT_OBJECT_0) && !stopFlag.load(std::memory_order_acquire);
}

void RunSingleSessionCapture(CaptureWorkerContext& ctx,
                             const ResolvedCaptureTarget& target,
                             const staged::FormatCandidateResult& selectedFormat) {
    constexpr REFERENCE_TIME kLoopbackBufferDuration100ns = 200000; // 20 ms

    staged::LoopbackActivationResult act = staged::ActivateProcessLoopbackClient(
        target.pid, ctx.opts.mode, 5000u);
    ComPtr<IAudioClient> client;
    client.Attach(act.audioClient);
    const HRESULT actHr = SUCCEEDED(act.asyncCallHr) ? act.activateResultHr : act.asyncCallHr;
    if (FAILED(actHr) || !client) {
        std::fprintf(stderr, "[capture] ActivateProcessLoopbackClient(pid=%u) failed: %s\n",
                     target.pid, staged::FormatHResultHex(actHr).c_str());
        return;
    }

    if (selectedFormat.rawWaveFormatBytes.size() < sizeof(WAVEFORMATEX)) {
        std::fprintf(stderr, "[capture] Selected candidate missing WAVEFORMATEX bytes\n");
        return;
    }
    const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(selectedFormat.rawWaveFormatBytes.data());
    if (selectedFormat.rawWaveFormatBytes.size() < sizeof(WAVEFORMATEX) + wfx->cbSize) {
        std::fprintf(stderr, "[capture] Selected WAVEFORMATEX truncated relative to cbSize\n");
        return;
    }

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (selectedFormat.useAutoConvert) {
        streamFlags |= (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
    }

    HRESULT hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        kLoopbackBufferDuration100ns,
        0,
        wfx,
        nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[capture] IAudioClient::Initialize(%s) failed: %s\n",
                     selectedFormat.label.c_str(), staged::FormatHResultHex(hr).c_str());
        return;
    }

    ScopedHandle sampleEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!sampleEvent.IsValid()) {
        std::fprintf(stderr, "[capture] CreateEventW failed: err=%lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return;
    }

    hr = client->SetEventHandle(sampleEvent.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "[capture] SetEventHandle failed: %s\n", staged::FormatHResultHex(hr).c_str());
        return;
    }

    ComPtr<IAudioCaptureClient> captureClient;
    hr = client->GetService(__uuidof(IAudioCaptureClient),
                            reinterpret_cast<void**>(captureClient.Put()));
    if (FAILED(hr) || !captureClient) {
        std::fprintf(stderr, "[capture] GetService(IAudioCaptureClient) failed: %s\n",
                     staged::FormatHResultHex(hr).c_str());
        return;
    }

    ScopedHandle targetProcHandle(OpenProcess(SYNCHRONIZE, FALSE, target.pid));

    hr = client->Start();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[capture] IAudioClient::Start failed: %s\n",
                     staged::FormatHResultHex(hr).c_str());
        return;
    }

    {
        CaptureQueueItem startItem;
        startItem.type = CaptureQueueItem::Type::StreamStarted;
        startItem.sampleRate = selectedFormat.validatedFormat.sampleRate;
        startItem.convDelayFrames48k = 0u;
        startItem.targetPid = target.pid;
        startItem.targetExeUtf8 = staged::WideToUtf8(target.exeName);
        startItem.formatLabelUtf8 = selectedFormat.label;
        ctx.queue->Push(std::move(startItem));
    }

    std::vector<int16_t> scratchStereo;
    std::string endReason = "stopped";
    LARGE_INTEGER lastAliveCheckQpc{};
    QueryPerformanceCounter(&lastAliveCheckQpc);

    while (!ctx.stopRequested.load(std::memory_order_acquire)) {
        HANDLE waitHandles[3]{};
        DWORD handleCount = 0u;
        waitHandles[handleCount++] = sampleEvent.Get();
        DWORD stopSlot = static_cast<DWORD>(-1);
        if (ctx.stopEvent) {
            stopSlot = handleCount;
            waitHandles[handleCount++] = ctx.stopEvent;
        }
        DWORD procSlot = static_cast<DWORD>(-1);
        if (targetProcHandle.IsValid()) {
            procSlot = handleCount;
            waitHandles[handleCount++] = targetProcHandle.Get();
        }

        const DWORD waitRes = WaitForMultipleObjects(handleCount, waitHandles, FALSE, 50u);
        if (waitRes == WAIT_FAILED) {
            const DWORD err = GetLastError();
            std::fprintf(stderr, "[capture] WaitForMultipleObjects failed: err=%lu\n",
                         static_cast<unsigned long>(err));
            endReason = "wait_failed_" + std::to_string(err);
            break;
        }
        if (ctx.stopRequested.load(std::memory_order_acquire) ||
            (stopSlot != static_cast<DWORD>(-1) && waitRes == WAIT_OBJECT_0 + stopSlot)) {
            endReason = "stop_requested";
            break;
        }
        if (procSlot != static_cast<DWORD>(-1) && waitRes == WAIT_OBJECT_0 + procSlot) {
            endReason = "target_process_exited";
            break;
        }

        if (!targetProcHandle.IsValid()) {
            LARGE_INTEGER checkNow{};
            QueryPerformanceCounter(&checkNow);
            if (staged::QpcTicksTo100ns(checkNow.QuadPart - lastAliveCheckQpc.QuadPart, ctx.qpcFreq) >= 10000000LL) {
                lastAliveCheckQpc = checkNow;
                if (!IsProcessAlivePid(target.pid)) {
                    endReason = "target_process_gone";
                    break;
                }
            }
        }

        bool sessionFailed = false;
        while (!ctx.stopRequested.load(std::memory_order_acquire)) {
            UINT32 nextFrames = 0u;
            hr = captureClient->GetNextPacketSize(&nextFrames);
            if (FAILED(hr)) {
                endReason = "get_next_packet_size_" + staged::FormatHResultHex(hr);
                sessionFailed = true;
                break;
            }
            if (nextFrames == 0u) {
                break;
            }

            BYTE* pData = nullptr;
            UINT32 numFrames = 0u;
            DWORD flags = 0u;
            UINT64 devPos = 0ULL;
            UINT64 qpcPos = 0ULL;
            hr = captureClient->GetBuffer(&pData, &numFrames, &flags, &devPos, &qpcPos);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                break;
            }
            if (FAILED(hr)) {
                endReason = "get_buffer_" + staged::FormatHResultHex(hr);
                sessionFailed = true;
                break;
            }

            LARGE_INTEGER pktNowQpc{};
            QueryPerformanceCounter(&pktNowQpc);
            const int64_t now100nsSigned = staged::QpcTicksTo100ns(pktNowQpc.QuadPart, ctx.qpcFreq);
            const uint64_t now100ns = (now100nsSigned > 0) ? static_cast<uint64_t>(now100nsSigned) : 0ULL;

            const bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0u;
            const bool isDiscontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0u;
            const bool isTsError = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0u;

            CaptureQueueItem pktItem;
            pktItem.type = CaptureQueueItem::Type::Packet;
            pktItem.sourceFrames = numFrames;
            pktItem.outFrames48k = numFrames;
            pktItem.devPos = devPos;
            pktItem.qpcPos100ns = qpcPos;
            pktItem.nowQpc100ns = now100ns;
            pktItem.silentFlag = isSilent;
            pktItem.discontinuityFlag = isDiscontinuity;
            pktItem.timestampErrorFlag = isTsError;

            if (numFrames > 0u && !isSilent) {
                staged::ConvertCaptureChunkToStereoInt16(
                    selectedFormat.validatedFormat, pData, numFrames, false, scratchStereo);
                pktItem.stereo48kLR = scratchStereo;
            }

            const HRESULT relHr = captureClient->ReleaseBuffer(numFrames);
            if (FAILED(relHr)) {
                endReason = "release_buffer_" + staged::FormatHResultHex(relHr);
                sessionFailed = true;
                break;
            }

            if (numFrames == 0u && !isDiscontinuity) {
                continue;
            }

            ctx.queue->Push(std::move(pktItem));
        }

        if (sessionFailed) {
            break;
        }
    }

    client->Stop();

    CaptureQueueItem endItem;
    endItem.type = CaptureQueueItem::Type::StreamEnded;
    endItem.reasonUtf8 = endReason;
    ctx.queue->Push(std::move(endItem));
}

void CaptureWorkerProc(CaptureWorkerContext* ctx) {
    ComMtaInit com;
    if (!com.Usable()) {
        std::fprintf(stderr, "[capture] CoInitializeEx failed: %s\n",
                     staged::FormatHResultHex(com.hr).c_str());
        return;
    }

    DWORD mmcssTaskIndex = 0u;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);

    while (!ctx->stopRequested.load(std::memory_order_acquire)) {
        ResolvedCaptureTarget target;
        std::string resolveDetail;
        if (!ResolveCaptureTarget(ctx->opts, target, resolveDetail)) {
            if (!WaitInterruptibleMs(ctx->stopEvent, ctx->stopRequested, 350u)) {
                break;
            }
            continue;
        }

        staged::FormatCandidateResult selectedFormat;
        std::string formatErr;
        if (!SelectBest48kLoopbackFormat(target.pid, ctx->opts.mode, selectedFormat, formatErr)) {
            std::fprintf(stderr, "[capture] Format probe failed for PID %u: %s\n",
                         target.pid, formatErr.c_str());
            if (!WaitInterruptibleMs(ctx->stopEvent, ctx->stopRequested, 350u)) {
                break;
            }
            continue;
        }

        std::printf("[capture] Attached PID=%u (%s) via %s, format=%s, bufFrames=%u\n",
                    target.pid,
                    staged::WideToUtf8(target.exeName).c_str(),
                    target.resolutionTier.c_str(),
                    selectedFormat.label.c_str(),
                    selectedFormat.bufferFrames);
        std::fflush(stdout);

        RunSingleSessionCapture(*ctx, target, selectedFormat);

        if (!ctx->stopRequested.load(std::memory_order_acquire)) {
            if (!WaitInterruptibleMs(ctx->stopEvent, ctx->stopRequested, 350u)) {
                break;
            }
        }
    }

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
}

staged::IpcTelemetryCounters ReadIpcCounters(const stagec::StageCIpcRingHeader* hdr) noexcept {
    staged::IpcTelemetryCounters ipc{};
    if (!hdr) {
        return ipc;
    }
    ipc.sessionGeneration = stagec::AtomicLoadU64(&hdr->session_generation);
    ipc.producerState = stagec::AtomicLoad32(&hdr->producer_state);
    ipc.writeReserveFrame = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
    ipc.writeCommitFrame = stagec::AtomicLoadU64(&hdr->write_commit_frame);
    ipc.consumerReadFrame = stagec::AtomicLoadU64(&hdr->consumer_read_frame);
    ipc.watermarkFrames = stagec::AtomicLoadU64(&hdr->watermark_frames);
    ipc.underrunEvents = stagec::AtomicLoadU64(&hdr->underrun_events);
    ipc.underrunFrames = stagec::AtomicLoadU64(&hdr->underrun_frames);
    ipc.overrunEvents = stagec::AtomicLoadU64(&hdr->overrun_events);
    ipc.overrunSkippedFrames = stagec::AtomicLoadU64(&hdr->overrun_skipped_frames);
    ipc.overwriteConflictEvents = stagec::AtomicLoadU64(&hdr->overwrite_conflict_events);
    ipc.lastFrameAgeUs = stagec::AtomicLoadU64(&hdr->last_frame_age_us);
    ipc.consumerActiveReaders = stagec::AtomicLoad32(&hdr->consumer_active_readers);
    return ipc;
}

class TelemetryFileWriter {
public:
    ~TelemetryFileWriter() { Close(); }

    bool Open(const std::wstring& path, std::string& outError) {
        Close();
        if (path.empty()) {
            return true;
        }
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path()) {
            std::filesystem::create_directories(fsPath.parent_path(), ec);
        }
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) {
            outError = "Failed to open telemetry JSONL file: " + staged::WideToUtf8(path);
            return false;
        }
        fp_ = fp;
        return true;
    }

    void WriteLine(const std::string& jsonLine) {
        if (!fp_ || jsonLine.empty()) {
            return;
        }
        std::fwrite(jsonLine.data(), 1u, jsonLine.size(), fp_);
        std::fputc('\n', fp_);
        std::fflush(fp_);
    }

    void Close() noexcept {
        if (fp_) {
            std::fflush(fp_);
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

private:
    FILE* fp_ = nullptr;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    ProducerOptions opts;
    bool showHelp = false;
    if (!ParseCommandLine(argc, argv, opts, showHelp)) {
        return showHelp ? 0 : 1;
    }

    SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    // Stage C Single-Producer Mutex
    const std::wstring mutexName = opts.mappingName + stagec::kProducerMutexSuffix;
    ScopedHandle hMutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (!hMutex.IsValid()) {
        std::fprintf(stderr, "CreateMutexW(%ls) failed: err=%lu\n",
                     mutexName.c_str(), static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    const DWORD mutexWait = WaitForSingleObject(hMutex.Get(), 0u);
    if (mutexWait == WAIT_TIMEOUT) {
        std::fprintf(stderr, "Another producer already holds mutex %ls; exiting with code 2.\n",
                     mutexName.c_str());
        return 2;
    }
    if (mutexWait != WAIT_OBJECT_0 && mutexWait != WAIT_ABANDONED) {
        std::fprintf(stderr, "WaitForSingleObject(%ls) failed: res=%lu err=%lu\n",
                     mutexName.c_str(),
                     static_cast<unsigned long>(mutexWait),
                     static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    ScopedHandle hMapping(CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        stagec::kTotalSharedBytes,
        opts.mappingName.c_str()));
    if (!hMapping.IsValid()) {
        std::fprintf(stderr, "CreateFileMappingW(%ls) failed: err=%lu\n",
                     opts.mappingName.c_str(), static_cast<unsigned long>(GetLastError()));
        ReleaseMutex(hMutex.Get());
        return 1;
    }

    void* view = MapViewOfFile(hMapping.Get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!view) {
        std::fprintf(stderr, "MapViewOfFile failed: err=%lu\n",
                     static_cast<unsigned long>(GetLastError()));
        ReleaseMutex(hMutex.Get());
        return 1;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < stagec::kTotalSharedBytes) {
        std::fprintf(stderr, "VirtualQuery region size check failed (size=%zu, expected>=%u).\n",
                     mbi.RegionSize, static_cast<unsigned>(stagec::kTotalSharedBytes));
        UnmapViewOfFile(view);
        ReleaseMutex(hMutex.Get());
        return 1;
    }

    auto* shared = static_cast<stagec::StageCIpcSharedBlock*>(view);
    stagec::StageCIpcRingHeader* hdr = &shared->header;

    LARGE_INTEGER qpcFreqLi{};
    QueryPerformanceFrequency(&qpcFreqLi);
    const uint64_t qpcFreq = static_cast<uint64_t>(qpcFreqLi.QuadPart);

    LARGE_INTEGER startQpcLi{};
    QueryPerformanceCounter(&startQpcLi);

    // Stage C Odd-to-Even generation takeover
    const uint64_t prevGen = stagec::AtomicLoadU64(&hdr->session_generation);
    const uint64_t oddGen = stagec::NextOddInitGeneration(prevGen);
    const uint64_t evenGen = oddGen + 1ULL;

    stagec::AtomicStoreU64(&hdr->session_generation, oddGen);
    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Starting));

    const uint64_t sessionId =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
        static_cast<uint64_t>(startQpcLi.QuadPart);

    // Cache Line 0
    stagec::AtomicStore32(&hdr->magic, stagec::kIpcMagic);
    stagec::AtomicStore32(&hdr->version, stagec::kIpcVersion);
    stagec::AtomicStore32(&hdr->header_bytes, stagec::kHeaderBytes);
    stagec::AtomicStore32(&hdr->total_bytes, stagec::kTotalSharedBytes);
    stagec::AtomicStore32(&hdr->sample_rate, stagec::kSampleRate);
    stagec::AtomicStore32(&hdr->channels, stagec::kChannels);
    stagec::AtomicStore32(&hdr->bits_per_sample, stagec::kBitsPerSample);
    stagec::AtomicStore32(&hdr->capacity_frames, stagec::kRingCapacityFrames);
    stagec::AtomicStoreU64(&hdr->session_id, sessionId);
    stagec::AtomicStoreU64(&hdr->qpc_freq, qpcFreq);
    stagec::AtomicStore32(&hdr->producer_pid, GetCurrentProcessId());
#ifdef _WIN64
    constexpr uint32_t kProducerArchBits = 64u;
#else
    constexpr uint32_t kProducerArchBits = 32u;
#endif
    stagec::AtomicStore32(&hdr->producer_arch, kProducerArchBits);

    // Cache Line 1
    stagec::AtomicStoreU64(&hdr->write_reserve_frame, 0ULL);
    stagec::AtomicStoreU64(&hdr->write_commit_frame, 0ULL);
    stagec::AtomicStoreI64(&hdr->start_qpc, startQpcLi.QuadPart);
    stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, startQpcLi.QuadPart);
    stagec::AtomicStoreU64(&hdr->producer_tick_count, 0ULL);
    stagec::AtomicStore32(&hdr->pattern_mode, static_cast<uint32_t>(stagec::ProducerPattern::Tone));
    stagec::AtomicStoreU64(&hdr->reserved_cl1[0], 0ULL);
    stagec::AtomicStoreU64(&hdr->reserved_cl1[1], 0ULL);

    RingSharedMemorySink ringSink(shared);
    staged::StageDTimelineBridge bridge(ringSink);

    TelemetryFileWriter telemetryWriter;
    if (!opts.telemetryJsonlPath.empty()) {
        std::string telErr;
        if (!telemetryWriter.Open(opts.telemetryJsonlPath, telErr)) {
            std::fprintf(stderr, "%s\n", telErr.c_str());
            stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Stopped));
            UnmapViewOfFile(view);
            ReleaseMutex(hMutex.Get());
            return 1;
        }
    }

    const uint64_t prefillFrames = std::min<uint64_t>(
        staged::MulDivFloorU64(static_cast<uint64_t>(opts.prefillMs), staged::kOutputSampleRate, 1000ULL),
        static_cast<uint64_t>(stagec::kRingCapacityFrames / 2u));

    QueryPerformanceCounter(&startQpcLi);
    const int64_t start100nsSigned = staged::QpcTicksTo100ns(startQpcLi.QuadPart, qpcFreq);
    const uint64_t start100ns = (start100nsSigned > 0) ? static_cast<uint64_t>(start100nsSigned) : 0ULL;
    const uint64_t prefill100ns = staged::MulDivFloorU64(prefillFrames, 10000000ULL, staged::kOutputSampleRate);
    const uint64_t origin100ns = (start100ns >= prefill100ns) ? (start100ns - prefill100ns) : 0ULL;

    bridge.Reset(origin100ns);
    if (prefillFrames > 0ULL) {
        bridge.PrefillZeroFrames(prefillFrames, true);
    }

    stagec::AtomicStoreI64(&hdr->start_qpc, startQpcLi.QuadPart);
    stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, startQpcLi.QuadPart);
    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Streaming));
    stagec::AtomicStoreU64(&hdr->session_generation, evenGen);

    ScopedHandle hExternalStopEvent(OpenOrCreateNamedEvent(opts.stopEventName, true));
    ScopedHandle hWorkerStopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    SignalNamedEvent(opts.readyEventName);

    BoundedCaptureQueue captureQueue;
    CaptureWorkerContext workerCtx;
    workerCtx.opts = opts;
    workerCtx.qpcFreq = qpcFreq;
    workerCtx.stopEvent = hWorkerStopEvent.Get();
    workerCtx.queue = &captureQueue;

    staged::TelemetryLineContext telCtx;
    telCtx.targetPid = opts.targetPid;
    telCtx.targetExeUtf8 = opts.targetExe.empty() ? "AMPLibraryAgent.exe" : staged::WideToUtf8(opts.targetExe);
    telCtx.formatLabelUtf8 = "unstarted";
    telCtx.nowQpc100ns = start100ns;
    telCtx.ipc = ReadIpcCounters(hdr);
    telCtx.kind = "startup";
    telemetryWriter.WriteLine(staged::FormatTelemetryJsonLine(bridge.Snapshot(), telCtx));

    std::printf("Stage D Producer streaming: mapping=%ls gen=%llu arch=64 prefill=%llu frames mode=%s\n",
                opts.mappingName.c_str(),
                static_cast<unsigned long long>(evenGen),
                static_cast<unsigned long long>(prefillFrames),
                (opts.mode == PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE) ? "exclude" : "include");
    std::fflush(stdout);

    timeBeginPeriod(1);
    std::thread captureThread(&CaptureWorkerProc, &workerCtx);

    const uint64_t telemetryInterval100ns =
        static_cast<uint64_t>(opts.telemetryIntervalMs) * 10000ULL;
    uint64_t nextTelemetryQpc100ns = start100ns + telemetryInterval100ns;

    uint64_t prevDiscont = 0ULL;
    uint64_t prevTsErr = 0ULL;
    uint64_t prevFallback = 0ULL;
    uint64_t prevRestart = 0ULL;

    std::vector<CaptureQueueItem> drained;

    while (!ShouldStopNow(hExternalStopEvent.Get())) {
        captureQueue.DrainAll(drained);
        for (const auto& item : drained) {
            if (item.type == CaptureQueueItem::Type::StreamStarted) {
                bridge.OnStreamStarted(item.sampleRate, item.convDelayFrames48k);
                telCtx.targetPid = item.targetPid;
                telCtx.targetExeUtf8 = item.targetExeUtf8;
                telCtx.formatLabelUtf8 = item.formatLabelUtf8;

                LARGE_INTEGER evQpc{};
                QueryPerformanceCounter(&evQpc);
                const int64_t ev100ns = staged::QpcTicksTo100ns(evQpc.QuadPart, qpcFreq);
                telCtx.nowQpc100ns = (ev100ns > 0) ? static_cast<uint64_t>(ev100ns) : 0ULL;
                telCtx.ipc = ReadIpcCounters(hdr);
                telCtx.kind = "stream_started";
                telemetryWriter.WriteLine(staged::FormatTelemetryJsonLine(bridge.Snapshot(), telCtx));
            } else if (item.type == CaptureQueueItem::Type::Packet) {
                staged::TimelinePacketInput pkt;
                pkt.sourceFrames = item.sourceFrames;
                pkt.stereo48kLR = item.stereo48kLR.empty() ? nullptr : item.stereo48kLR.data();
                pkt.outFrames48k = item.outFrames48k;
                pkt.devPos = item.devPos;
                pkt.qpcPos100ns = item.qpcPos100ns;
                pkt.nowQpc100ns = item.nowQpc100ns;
                pkt.silentFlag = item.silentFlag;
                pkt.discontinuityFlag = item.discontinuityFlag;
                pkt.timestampErrorFlag = item.timestampErrorFlag;
                bridge.IngestPacket(pkt);
            } else if (item.type == CaptureQueueItem::Type::StreamEnded) {
                if (!item.reasonUtf8.empty()) {
                    std::fprintf(stderr, "[capture] StreamEnded reason=%s\n", item.reasonUtf8.c_str());
                }
                bridge.OnStreamEnded();
                LARGE_INTEGER evQpc{};
                QueryPerformanceCounter(&evQpc);
                const int64_t ev100ns = staged::QpcTicksTo100ns(evQpc.QuadPart, qpcFreq);
                telCtx.nowQpc100ns = (ev100ns > 0) ? static_cast<uint64_t>(ev100ns) : 0ULL;
                telCtx.ipc = ReadIpcCounters(hdr);
                telCtx.kind = "stream_ended";
                telemetryWriter.WriteLine(staged::FormatTelemetryJsonLine(bridge.Snapshot(), telCtx));
            }
        }

        LARGE_INTEGER nowQpcLi{};
        QueryPerformanceCounter(&nowQpcLi);
        const int64_t now100nsSigned = staged::QpcTicksTo100ns(nowQpcLi.QuadPart, qpcFreq);
        const uint64_t now100ns = (now100nsSigned > 0) ? static_cast<uint64_t>(now100nsSigned) : 0ULL;

        bridge.Tick(now100ns);

        stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, nowQpcLi.QuadPart);
        stagec::AtomicAddU64(&hdr->producer_tick_count, 1ULL);

        const staged::TimelineStatsSnapshot snap = bridge.Snapshot();
        const bool edgeEvent =
            (snap.discontinuityCount != prevDiscont) ||
            (snap.timestampErrorCount != prevTsErr) ||
            (snap.timestampFallbackAnchors != prevFallback) ||
            (snap.streamRestartCount != prevRestart);

        if (edgeEvent) {
            prevDiscont = snap.discontinuityCount;
            prevTsErr = snap.timestampErrorCount;
            prevFallback = snap.timestampFallbackAnchors;
            prevRestart = snap.streamRestartCount;
            telCtx.nowQpc100ns = now100ns;
            telCtx.ipc = ReadIpcCounters(hdr);
            telCtx.kind = "event";
            telemetryWriter.WriteLine(staged::FormatTelemetryJsonLine(snap, telCtx));
        }

        if (now100ns >= nextTelemetryQpc100ns) {
            telCtx.nowQpc100ns = now100ns;
            telCtx.ipc = ReadIpcCounters(hdr);
            telCtx.kind = "periodic";
            telemetryWriter.WriteLine(staged::FormatTelemetryJsonLine(snap, telCtx));
            while (nextTelemetryQpc100ns <= now100ns) {
                nextTelemetryQpc100ns += telemetryInterval100ns;
            }
        }

        const double elapsedSec =
            static_cast<double>(nowQpcLi.QuadPart - startQpcLi.QuadPart) /
            static_cast<double>(qpcFreq);
        if (opts.durationSec > 0.0 && elapsedSec >= opts.durationSec) {
            break;
        }

        if (hExternalStopEvent.IsValid()) {
            if (WaitForSingleObject(hExternalStopEvent.Get(), 3u) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            Sleep(3u);
        }
    }

    // Signal capture worker to stop cleanly, join it, drain residual packets, and flush holdback tail.
    workerCtx.stopRequested.store(true, std::memory_order_release);
    if (hWorkerStopEvent.IsValid()) {
        SetEvent(hWorkerStopEvent.Get());
    }
    if (captureThread.joinable()) {
        captureThread.join();
    }

    captureQueue.DrainAll(drained);
    for (const auto& item : drained) {
        if (item.type == CaptureQueueItem::Type::StreamStarted) {
            bridge.OnStreamStarted(item.sampleRate, item.convDelayFrames48k);
            telCtx.targetPid = item.targetPid;
            telCtx.targetExeUtf8 = item.targetExeUtf8;
            telCtx.formatLabelUtf8 = item.formatLabelUtf8;
        } else if (item.type == CaptureQueueItem::Type::Packet) {
            staged::TimelinePacketInput pkt;
            pkt.sourceFrames = item.sourceFrames;
            pkt.stereo48kLR = item.stereo48kLR.empty() ? nullptr : item.stereo48kLR.data();
            pkt.outFrames48k = item.outFrames48k;
            pkt.devPos = item.devPos;
            pkt.qpcPos100ns = item.qpcPos100ns;
            pkt.nowQpc100ns = item.nowQpc100ns;
            pkt.silentFlag = item.silentFlag;
            pkt.discontinuityFlag = item.discontinuityFlag;
            pkt.timestampErrorFlag = item.timestampErrorFlag;
            bridge.IngestPacket(pkt);
        } else if (item.type == CaptureQueueItem::Type::StreamEnded) {
            if (!item.reasonUtf8.empty()) {
                std::fprintf(stderr, "[capture] StreamEnded reason=%s\n", item.reasonUtf8.c_str());
            }
            bridge.OnStreamEnded();
        }
    }

    bridge.OnStreamEnded();
    bridge.FlushHoldbackOnStop();

    LARGE_INTEGER endQpcLi{};
    QueryPerformanceCounter(&endQpcLi);
    const int64_t end100nsSigned = staged::QpcTicksTo100ns(endQpcLi.QuadPart, qpcFreq);
    const uint64_t end100ns = (end100nsSigned > 0) ? static_cast<uint64_t>(end100nsSigned) : 0ULL;

    stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, endQpcLi.QuadPart);
    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Stopped));

    const staged::TimelineStatsSnapshot finalSnap = bridge.Snapshot();
    telCtx.nowQpc100ns = end100ns;
    telCtx.ipc = ReadIpcCounters(hdr);
    telCtx.kind = "shutdown_flush";
    telemetryWriter.WriteLine(staged::FormatTelemetryJsonLine(finalSnap, telCtx));
    telemetryWriter.Close();

    timeEndPeriod(1);

    std::printf("Stage D Producer stopped: queued=%llu committed=%llu holdback=%u gapZero=%llu starvZero=%llu lateDrop=%llu invOk=%s\n",
                static_cast<unsigned long long>(finalSnap.timelineQueuedFrames),
                static_cast<unsigned long long>(finalSnap.committedRingFrames),
                static_cast<unsigned>(finalSnap.holdbackFrames),
                static_cast<unsigned long long>(finalSnap.sourceGapZeroFrames),
                static_cast<unsigned long long>(finalSnap.starvationZeroFrames),
                static_cast<unsigned long long>(finalSnap.lateOverlapDroppedFrames),
                finalSnap.VerifyInvariants(true) ? "true" : "false");
    std::fflush(stdout);

    UnmapViewOfFile(view);
    hMapping.Reset();
    ReleaseMutex(hMutex.Get());
    hMutex.Reset();
    return 0;
}
