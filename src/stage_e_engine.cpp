#include "stage_e_engine.h"

#include <avrt.h>
#include <mmsystem.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

namespace stagee {

namespace {

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE h) noexcept : h_(h) {}
    ~ScopedHandle() { Reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : h_(other.h_) {
        other.h_ = nullptr;
    }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            h_ = other.h_;
            other.h_ = nullptr;
        }
        return *this;
    }

    void Reset(HANDLE h = nullptr) noexcept {
        if (h_ && h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
        h_ = h;
    }

    HANDLE Get() const noexcept { return h_; }
    bool IsValid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    void Attach(T* raw) noexcept {
        Reset();
        ptr_ = raw;
    }

    void Reset() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

class ScopedCoInit {
public:
    explicit ScopedCoInit(DWORD coInit = COINIT_MULTITHREADED) noexcept {
        hr_ = CoInitializeEx(nullptr, coInit);
        needUninit_ = (hr_ == S_OK || hr_ == S_FALSE);
    }
    ~ScopedCoInit() {
        if (needUninit_) {
            CoUninitialize();
        }
    }
    ScopedCoInit(const ScopedCoInit&) = delete;
    ScopedCoInit& operator=(const ScopedCoInit&) = delete;

private:
    HRESULT hr_ = E_FAIL;
    bool needUninit_ = false;
};

class RingSharedMemorySink final : public staged::IRingFrameSink {
public:
    explicit RingSharedMemorySink(stagec::StageCIpcSharedBlock* shared) noexcept
        : shared_(shared) {}

    bool WriteCommittedFrames(uint64_t startFrame,
                              const int16_t* stereoLR,
                              uint32_t frameCount) override {
        if (!shared_ || !stereoLR) {
            return false;
        }
        if (frameCount == 0u) {
            return true;
        }

        stagec::StageCIpcRingHeader* hdr = &shared_->header;
        int16_t* ringPcm = shared_->pcm_samples;

        uint32_t remaining = frameCount;
        uint64_t cursor = startFrame;
        const int16_t* src = stereoLR;

        while (remaining > 0u) {
            const uint32_t chunk = std::min<uint32_t>(remaining, stagec::kRingCapacityFrames / 2u);

            stagec::AtomicStoreU64(&hdr->write_reserve_frame, cursor + chunk);

            for (uint32_t i = 0u; i < chunk; ++i) {
                const uint32_t ringSlot = static_cast<uint32_t>((cursor + i) & stagec::kRingFrameMask);
                ringPcm[ringSlot * 2u] = src[i * 2u];
                ringPcm[ringSlot * 2u + 1u] = src[i * 2u + 1u];
            }

            stagec::AtomicStoreU64(&hdr->write_commit_frame, cursor + chunk);

            cursor += chunk;
            src += static_cast<size_t>(chunk) * 2u;
            remaining -= chunk;
        }
        return true;
    }

private:
    stagec::StageCIpcSharedBlock* shared_ = nullptr;
};

staged::IpcTelemetryCounters ReadIpcCounters(const stagec::StageCIpcRingHeader* hdr) noexcept {
    staged::IpcTelemetryCounters c{};
    if (!hdr) {
        return c;
    }
    c.sessionGeneration = stagec::AtomicLoadU64(&hdr->session_generation);
    c.producerState = stagec::AtomicLoad32(&hdr->producer_state);
    c.writeReserveFrame = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
    c.writeCommitFrame = stagec::AtomicLoadU64(&hdr->write_commit_frame);
    c.consumerReadFrame = stagec::AtomicLoadU64(&hdr->consumer_read_frame);
    c.watermarkFrames = stagec::AtomicLoadU64(&hdr->watermark_frames);
    c.underrunEvents = stagec::AtomicLoadU64(&hdr->underrun_events);
    c.underrunFrames = stagec::AtomicLoadU64(&hdr->underrun_frames);
    c.overrunEvents = stagec::AtomicLoadU64(&hdr->overrun_events);
    c.overrunSkippedFrames = stagec::AtomicLoadU64(&hdr->overrun_skipped_frames);
    c.overwriteConflictEvents = stagec::AtomicLoadU64(&hdr->overwrite_conflict_events);
    c.lastFrameAgeUs = stagec::AtomicLoadU64(&hdr->last_frame_age_us);
    c.consumerActiveReaders = stagec::AtomicLoad32(&hdr->consumer_active_readers);
    return c;
}

bool ContainsAsciiCaseInsensitiveW(const std::wstring& haystack, const wchar_t* needle) {
    if (!needle || !*needle) {
        return true;
    }
    std::wstring hLower = haystack;
    std::wstring nLower = needle;
    auto toLowerCh = [](wchar_t ch) -> wchar_t {
        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    };
    std::transform(hLower.begin(), hLower.end(), hLower.begin(), toLowerCh);
    std::transform(nLower.begin(), nLower.end(), nLower.begin(), toLowerCh);
    return hLower.find(nLower) != std::wstring::npos;
}

bool IsAppleMusicStorePath(const std::wstring& fullPath) {
    return ContainsAsciiCaseInsensitiveW(fullPath, L"AppleInc.AppleMusicWin");
}

bool IsProcessAlivePid(uint32_t pid) noexcept {
    if (pid == 0u) {
        return false;
    }
    ScopedHandle hProc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!hProc.IsValid()) {
        return false;
    }
    DWORD exitCode = 0u;
    if (!GetExitCodeProcess(hProc.Get(), &exitCode)) {
        return false;
    }
    return exitCode == STILL_ACTIVE;
}

bool QueryProcessImagePath(uint32_t pid, std::wstring& outFullPath) {
    outFullPath.clear();
    ScopedHandle hProc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!hProc.IsValid()) {
        return false;
    }
    wchar_t buf[MAX_PATH * 4]{};
    DWORD sz = static_cast<DWORD>(std::size(buf));
    if (!QueryFullProcessImageNameW(hProc.Get(), 0u, buf, &sz) || sz == 0u) {
        return false;
    }
    outFullPath.assign(buf, sz);
    return true;
}

bool ResolveTargetSessionMatches(const staged::AudioSessionRecord& sess,
                                 const std::wstring& targetLeaf) {
    if (sess.isSystemSoundsSession || sess.isMultiProcessSession || sess.processId == 0u) {
        return false;
    }
    const std::wstring leaf = !sess.exeName.empty()
        ? sess.exeName
        : staged::ExtractLeafFilename(sess.exeFullPath);
    return staged::EqualsIgnoreCase(leaf, targetLeaf);
}

bool NormalizeAudioSourceConfig(const AudioSourceConfig& rawSrc,
                                AudioSourceConfig& outNormalized) {
    outNormalized = rawSrc;
    if (outNormalized.mode == AudioSourceMode::AppleMusicPreset) {
        outNormalized.customExeName.clear();
        outNormalized.customExePath.clear();
        return true;
    }

    const std::wstring leaf = !outNormalized.customExeName.empty()
        ? staged::ExtractLeafFilename(outNormalized.customExeName)
        : staged::ExtractLeafFilename(outNormalized.customExePath);
    if (leaf.empty()) {
        return false;
    }
    if (IsAppleMusicShellOrAgentExe(leaf)) {
        outNormalized.mode = AudioSourceMode::AppleMusicPreset;
        outNormalized.customExeName.clear();
        outNormalized.customExePath.clear();
        return true;
    }
    if (IsForbiddenCaptureTargetExe(leaf)) {
        return false;
    }
    outNormalized.customExeName = leaf;
    return true;
}

AudioSourceConfig DeriveEffectiveSourceFromOptions(const StageEEngineOptions& opts) {
    if (!opts.targetExe.empty()) {
        AudioSourceConfig cliSrc{};
        const std::wstring leaf = staged::ExtractLeafFilename(opts.targetExe);
        if (IsAppleMusicShellOrAgentExe(leaf)) {
            cliSrc.mode = AudioSourceMode::AppleMusicPreset;
        } else {
            cliSrc.mode = AudioSourceMode::CustomProcess;
            cliSrc.customExeName = leaf;
            if (opts.targetExe.find_first_of(L"\\/") != std::wstring::npos) {
                cliSrc.customExePath = opts.targetExe;
            }
        }
        AudioSourceConfig norm{};
        if (NormalizeAudioSourceConfig(cliSrc, norm)) {
            return norm;
        }
    }
    AudioSourceConfig norm{};
    if (NormalizeAudioSourceConfig(opts.source, norm)) {
        return norm;
    }
    return AudioSourceConfig{};
}

struct ResolvedCaptureTarget {
    uint32_t pid = 0u;
    std::wstring exeName;
    std::wstring exeFullPath;
    std::string resolutionTier;
};

bool ResolveCaptureTarget(uint32_t explicitTargetPid,
                          const AudioSourceConfig& sourceCfg,
                          ResolvedCaptureTarget& outTarget,
                          std::string& outDetail) {
    outTarget = ResolvedCaptureTarget{};
    outDetail.clear();

    if (explicitTargetPid != 0u) {
        if (!IsProcessAlivePid(explicitTargetPid)) {
            outDetail = "Explicit targetPid=" + std::to_string(explicitTargetPid) + " is not alive";
            return false;
        }
        outTarget.pid = explicitTargetPid;
        QueryProcessImagePath(explicitTargetPid, outTarget.exeFullPath);
        outTarget.exeName = staged::ExtractLeafFilename(outTarget.exeFullPath);
        if (outTarget.exeName.empty()) {
            outTarget.exeName = L"<pid:" + std::to_wstring(explicitTargetPid) + L">";
        }
        outTarget.resolutionTier = "explicit_pid";
        return true;
    }

    const bool defaultAppleMusicMode = (sourceCfg.mode == AudioSourceMode::AppleMusicPreset);
    const std::wstring targetLeaf = defaultAppleMusicMode
        ? std::wstring(L"AMPLibraryAgent.exe")
        : staged::ExtractLeafFilename(sourceCfg.customExeName);
    if (!defaultAppleMusicMode && (targetLeaf.empty() || IsForbiddenCaptureTargetExe(targetLeaf))) {
        outDetail = "Invalid or forbidden custom target executable";
        return false;
    }
    const bool hasPreferredFullPath = !defaultAppleMusicMode && !sourceCfg.customExePath.empty();

    staged::AudioTopologyReport topo;
    std::string topoErr;
    const bool hasTopo = staged::DiscoverAudioTopology(topo, topoErr);

    auto promoteToRootProcess = [&](uint32_t matchedPid,
                                    const std::wstring& matchedLeaf,
                                    const std::wstring& matchedFullPath,
                                    const std::vector<staged::ProcessEntry>* procs,
                                    ResolvedCaptureTarget& dst) -> bool {
        uint32_t chosenPid = matchedPid;
        std::wstring chosenLeaf = !matchedLeaf.empty() ? matchedLeaf : targetLeaf;
        std::wstring chosenFullPath = matchedFullPath;

        if (!defaultAppleMusicMode && procs && !procs->empty()) {
            const uint32_t rootPid = ResolveSameImageRootPid(*procs, matchedPid);
            if (rootPid != 0u && rootPid != matchedPid && IsProcessAlivePid(rootPid)) {
                chosenPid = rootPid;
                for (const auto& p : *procs) {
                    if (p.pid == rootPid) {
                        if (!p.exeName.empty()) chosenLeaf = p.exeName;
                        if (!p.exeFullPath.empty()) chosenFullPath = p.exeFullPath;
                        break;
                    }
                }
            }
        }
        if (!IsProcessAlivePid(chosenPid)) {
            return false;
        }
        if (chosenFullPath.empty()) {
            QueryProcessImagePath(chosenPid, chosenFullPath);
        }
        dst.pid = chosenPid;
        dst.exeName = chosenLeaf;
        dst.exeFullPath = chosenFullPath;
        return true;
    };

    auto pathPassMatches = [&](int pass, const std::wstring& candidateFullPath) -> bool {
        if (pass != 0) {
            return true;
        }
        if (defaultAppleMusicMode) {
            return IsAppleMusicStorePath(candidateFullPath);
        }
        if (hasPreferredFullPath) {
            return !candidateFullPath.empty() &&
                   staged::EqualsIgnoreCase(candidateFullPath, sourceCfg.customExePath);
        }
        return true;
    };

    const int totalPasses = (defaultAppleMusicMode || hasPreferredFullPath) ? 2 : 1;

    if (hasTopo) {
        const std::vector<staged::ProcessEntry>* topoProcs =
            topo.processes.empty() ? nullptr : &topo.processes;

        for (int pass = 0; pass < totalPasses; ++pass) {
            for (const auto& ep : topo.endpoints) {
                for (const auto& sess : ep.sessions) {
                    if (!ResolveTargetSessionMatches(sess, targetLeaf)) continue;
                    if (sess.sessionState != AudioSessionStateActive) continue;
                    if (!pathPassMatches(pass, sess.exeFullPath)) continue;

                    if (promoteToRootProcess(sess.processId, sess.exeName, sess.exeFullPath, topoProcs, outTarget)) {
                        outTarget.resolutionTier = (pass == 0 && defaultAppleMusicMode)
                            ? "apple_store_active_session"
                            : "active_session";
                        return true;
                    }
                }
            }
        }

        for (int pass = 0; pass < totalPasses; ++pass) {
            for (const auto& ep : topo.endpoints) {
                for (const auto& sess : ep.sessions) {
                    if (!ResolveTargetSessionMatches(sess, targetLeaf)) continue;
                    if (sess.sessionState != AudioSessionStateInactive) continue;
                    if (!pathPassMatches(pass, sess.exeFullPath)) continue;

                    if (promoteToRootProcess(sess.processId, sess.exeName, sess.exeFullPath, topoProcs, outTarget)) {
                        outTarget.resolutionTier = (pass == 0 && defaultAppleMusicMode)
                            ? "apple_store_inactive_session"
                            : "inactive_session";
                        return true;
                    }
                }
            }
        }
    }

    std::vector<staged::ProcessEntry> procs;
    std::string procErr;
    const std::vector<staged::ProcessEntry>* procList = nullptr;
    if (hasTopo && !topo.processes.empty()) {
        procList = &topo.processes;
    } else if (staged::SnapshotProcesses(procs, procErr)) {
        procList = &procs;
    }

    if (procList) {
        for (int pass = 0; pass < totalPasses; ++pass) {
            for (const auto& p : *procList) {
                if (p.pid == 0u) continue;
                const std::wstring leaf = !p.exeName.empty()
                    ? p.exeName
                    : staged::ExtractLeafFilename(p.exeFullPath);
                if (!staged::EqualsIgnoreCase(leaf, targetLeaf)) continue;
                if (!pathPassMatches(pass, p.exeFullPath)) continue;

                if (promoteToRootProcess(p.pid, p.exeName, p.exeFullPath, procList, outTarget)) {
                    outTarget.resolutionTier = (pass == 0 && defaultAppleMusicMode)
                        ? "apple_store_process_snapshot"
                        : "process_snapshot";
                    return true;
                }
            }
        }
    }

    outDetail = "Target process not found";
    return false;
}

bool SelectBest48kLoopbackFormat(uint32_t targetPid,
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
        outError = "No 48000 Hz loopback format succeeded";
        return false;
    }

    outSelected = *chosen;
    return true;
}

struct CaptureQueueItem {
    enum class Type : uint8_t {
        StreamStarted = 0,
        Packet        = 1,
        StreamEnded   = 2,
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
    static constexpr size_t kMaxQueuedPackets = 512u;

    void Push(CaptureQueueItem&& item) {
        std::lock_guard<std::mutex> lock(mu_);
        if (item.type == CaptureQueueItem::Type::StreamStarted ||
            item.type == CaptureQueueItem::Type::StreamEnded) {
            markNextDiscontinuity_ = false;
        } else if (item.type == CaptureQueueItem::Type::Packet) {
            size_t packetItems = 0u;
            for (const auto& existing : items_) {
                if (existing.type == CaptureQueueItem::Type::Packet) {
                    ++packetItems;
                }
            }
            if (packetItems >= kMaxQueuedPackets) {
                for (auto it = items_.begin(); it != items_.end(); ++it) {
                    if (it->type == CaptureQueueItem::Type::Packet) {
                        auto nextIt = items_.erase(it);
                        bool markedSurvivor = false;
                        bool crossedStreamStart = false;
                        for (; nextIt != items_.end(); ++nextIt) {
                            if (nextIt->type == CaptureQueueItem::Type::StreamStarted ||
                                nextIt->type == CaptureQueueItem::Type::StreamEnded) {
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

bool WaitInterruptibleOrReconfigMs(HANDLE stopEvent,
                                   HANDLE reconfigEvent,
                                   const std::atomic<bool>& stopFlag,
                                   const std::atomic<uint64_t>* configSeq,
                                   uint64_t observedSeq,
                                   DWORD waitMs) {
    if (stopFlag.load(std::memory_order_acquire)) {
        return false;
    }
    if (configSeq && configSeq->load(std::memory_order_acquire) != observedSeq) {
        return true; // Wake immediately without sleeping so caller can apply new source
    }
    HANDLE handles[2]{};
    DWORD count = 0u;
    DWORD stopSlot = static_cast<DWORD>(-1);
    if (stopEvent) {
        stopSlot = count;
        handles[count++] = stopEvent;
    }
    if (reconfigEvent) {
        handles[count++] = reconfigEvent;
    }
    if (count == 0u) {
        Sleep(waitMs);
        return !stopFlag.load(std::memory_order_acquire);
    }
    const DWORD res = WaitForMultipleObjects(count, handles, FALSE, waitMs);
    if (stopSlot != static_cast<DWORD>(-1) && res == WAIT_OBJECT_0 + stopSlot) {
        return false;
    }
    return !stopFlag.load(std::memory_order_acquire);
}

struct CaptureWorkerContext {
    StageEEngineOptions opts;
    uint64_t qpcFreq = 10000000ULL;
    HANDLE stopEvent = nullptr;
    HANDLE reconfigEvent = nullptr;
    std::mutex* sourceMu = nullptr;
    const AudioSourceConfig* sharedSource = nullptr;
    const std::atomic<uint64_t>* sourceConfigSeq = nullptr;
    std::atomic<bool> stopRequested{ false };
    BoundedCaptureQueue* queue = nullptr;
};

void RunSingleSessionCapture(CaptureWorkerContext& ctx,
                             uint64_t sessionConfigSeq,
                             const ResolvedCaptureTarget& target,
                             const staged::FormatCandidateResult& selectedFormat,
                             bool& outReconfigured) {
    outReconfigured = false;
    constexpr REFERENCE_TIME kLoopbackBufferDuration100ns = 200000; // 20 ms

    staged::LoopbackActivationResult act = staged::ActivateProcessLoopbackClient(
        target.pid, ctx.opts.mode, 5000u);
    ComPtr<IAudioClient> client;
    client.Attach(act.audioClient);
    const HRESULT actHr = SUCCEEDED(act.asyncCallHr) ? act.activateResultHr : act.asyncCallHr;
    if (FAILED(actHr) || !client) {
        return;
    }

    if (selectedFormat.rawWaveFormatBytes.size() < sizeof(WAVEFORMATEX)) {
        return;
    }
    const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(selectedFormat.rawWaveFormatBytes.data());
    if (selectedFormat.rawWaveFormatBytes.size() < sizeof(WAVEFORMATEX) + wfx->cbSize) {
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
        return;
    }

    ScopedHandle sampleEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!sampleEvent.IsValid()) {
        return;
    }

    hr = client->SetEventHandle(sampleEvent.Get());
    if (FAILED(hr)) {
        return;
    }

    ComPtr<IAudioCaptureClient> captureClient;
    hr = client->GetService(__uuidof(IAudioCaptureClient),
                            reinterpret_cast<void**>(captureClient.Put()));
    if (FAILED(hr) || !captureClient) {
        return;
    }

    ScopedHandle targetProcHandle(OpenProcess(SYNCHRONIZE, FALSE, target.pid));

    hr = client->Start();
    if (FAILED(hr)) {
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
        if (ctx.sourceConfigSeq &&
            ctx.sourceConfigSeq->load(std::memory_order_acquire) != sessionConfigSeq) {
            endReason = "source_reconfigured";
            outReconfigured = true;
            break;
        }

        HANDLE waitHandles[4]{};
        DWORD handleCount = 0u;
        waitHandles[handleCount++] = sampleEvent.Get();

        DWORD stopSlot = static_cast<DWORD>(-1);
        if (ctx.stopEvent) {
            stopSlot = handleCount;
            waitHandles[handleCount++] = ctx.stopEvent;
        }
        DWORD reconfigSlot = static_cast<DWORD>(-1);
        if (ctx.reconfigEvent) {
            reconfigSlot = handleCount;
            waitHandles[handleCount++] = ctx.reconfigEvent;
        }
        DWORD procSlot = static_cast<DWORD>(-1);
        if (targetProcHandle.IsValid()) {
            procSlot = handleCount;
            waitHandles[handleCount++] = targetProcHandle.Get();
        }

        const DWORD waitRes = WaitForMultipleObjects(handleCount, waitHandles, FALSE, 50u);
        if (waitRes == WAIT_FAILED) {
            endReason = "wait_failed_" + std::to_string(GetLastError());
            break;
        }
        if (ctx.stopRequested.load(std::memory_order_acquire) ||
            (stopSlot != static_cast<DWORD>(-1) && waitRes == WAIT_OBJECT_0 + stopSlot)) {
            endReason = "stop_requested";
            break;
        }
        if ((reconfigSlot != static_cast<DWORD>(-1) && waitRes == WAIT_OBJECT_0 + reconfigSlot) ||
            (ctx.sourceConfigSeq &&
             ctx.sourceConfigSeq->load(std::memory_order_acquire) != sessionConfigSeq)) {
            endReason = "source_reconfigured";
            outReconfigured = true;
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
            if (ctx.sourceConfigSeq &&
                ctx.sourceConfigSeq->load(std::memory_order_acquire) != sessionConfigSeq) {
                endReason = "source_reconfigured";
                outReconfigured = true;
                sessionFailed = true;
                break;
            }
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

            if (numFrames > 0u) {
                ctx.queue->Push(std::move(pktItem));
            }
        }

        if (sessionFailed) {
            break;
        }
    }

    client->Stop();

    CaptureQueueItem endItem;
    endItem.type = CaptureQueueItem::Type::StreamEnded;
    endItem.targetPid = target.pid;
    endItem.reasonUtf8 = endReason;
    ctx.queue->Push(std::move(endItem));
}

void CaptureWorkerProc(CaptureWorkerContext* ctx) {
    if (!ctx || !ctx->queue) {
        return;
    }
    ScopedCoInit co(COINIT_MULTITHREADED);

    DWORD mmcssTaskIndex = 0u;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);

    while (!ctx->stopRequested.load(std::memory_order_acquire)) {
        AudioSourceConfig curSrc{};
        uint64_t curSeq = 0ULL;
        if (ctx->sourceMu && ctx->sharedSource && ctx->sourceConfigSeq) {
            std::lock_guard<std::mutex> lock(*ctx->sourceMu);
            curSrc = *ctx->sharedSource;
            curSeq = ctx->sourceConfigSeq->load(std::memory_order_acquire);
        } else {
            curSrc = DeriveEffectiveSourceFromOptions(ctx->opts);
        }

        ResolvedCaptureTarget target;
        std::string resolveDetail;
        if (!ResolveCaptureTarget(ctx->opts.targetPid, curSrc, target, resolveDetail)) {
            if (!WaitInterruptibleOrReconfigMs(
                    ctx->stopEvent, ctx->reconfigEvent, ctx->stopRequested, ctx->sourceConfigSeq, curSeq, 350u)) {
                break;
            }
            continue;
        }

        staged::FormatCandidateResult selectedFmt;
        std::string fmtError;
        if (!SelectBest48kLoopbackFormat(target.pid, ctx->opts.mode, selectedFmt, fmtError)) {
            if (!WaitInterruptibleOrReconfigMs(
                    ctx->stopEvent, ctx->reconfigEvent, ctx->stopRequested, ctx->sourceConfigSeq, curSeq, 500u)) {
                break;
            }
            continue;
        }

        bool reconfigured = false;
        RunSingleSessionCapture(*ctx, curSeq, target, selectedFmt, reconfigured);
        if (reconfigured) {
            continue; // Immediately rebind to new audio source without 350 ms retry pause
        }

        if (!WaitInterruptibleOrReconfigMs(
                ctx->stopEvent, ctx->reconfigEvent, ctx->stopRequested, ctx->sourceConfigSeq, curSeq, 350u)) {
            break;
        }
    }

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
}

struct SharedTopologyContext {
    StageEEngineOptions opts;
    HANDLE stopEvent = nullptr;
    HANDLE reconfigEvent = nullptr;
    std::mutex* sourceMu = nullptr;
    const AudioSourceConfig* sharedSource = nullptr;
    const std::atomic<uint64_t>* sourceConfigSeq = nullptr;
    std::atomic<bool> stopRequested{ false };
    std::atomic<uint32_t> activeTargetPid{ 0u };
    const std::atomic<uint8_t>* uiLanguage = nullptr;

    std::mutex mu;
    RoutingDiagnostic latestRouting{};
    uint64_t latestSourceSeq = 0ULL;
    uint64_t updateSeq = 0ULL;
};

void TopologyMonitorProc(SharedTopologyContext* ctx) {
    if (!ctx) {
        return;
    }
    ScopedCoInit co(COINIT_MULTITHREADED);

    RoutingIsolatorTracker tracker;
    uint64_t trackedSourceSeq = 0ULL;
    const DWORD pollMs = std::max<DWORD>(250u, ctx->opts.topologyPollIntervalMs);

    while (!ctx->stopRequested.load(std::memory_order_acquire)) {
        AudioSourceConfig curSrc{};
        uint64_t curSeq = 0ULL;
        if (ctx->sourceMu && ctx->sharedSource && ctx->sourceConfigSeq) {
            std::lock_guard<std::mutex> lock(*ctx->sourceMu);
            curSrc = *ctx->sharedSource;
            curSeq = ctx->sourceConfigSeq->load(std::memory_order_acquire);
        } else {
            curSrc = DeriveEffectiveSourceFromOptions(ctx->opts);
        }

        if (curSeq != trackedSourceSeq) {
            trackedSourceSeq = curSeq;
            tracker.Reset();
        }

        const std::wstring targetLeaf = (curSrc.mode == AudioSourceMode::AppleMusicPreset)
            ? std::wstring(L"AMPLibraryAgent.exe")
            : staged::ExtractLeafFilename(curSrc.customExeName);

        staged::AudioTopologyReport topo;
        std::string topoErr;
        if (staged::DiscoverAudioTopology(topo, topoErr)) {
            const uint32_t curPid = ctx->activeTargetPid.load(std::memory_order_acquire);
            const UiLanguage curLang = ctx->uiLanguage
                ? ResolveEffectiveLanguage(static_cast<UiLanguage>(ctx->uiLanguage->load(std::memory_order_acquire)))
                : ResolveEffectiveLanguage(ctx->opts.language);
            RoutingDiagnostic diag = EvaluateRoutingIsolation(topo, curPid, targetLeaf, tracker, curLang);
            {
                std::lock_guard<std::mutex> lock(ctx->mu);
                ctx->latestRouting = std::move(diag);
                ctx->latestSourceSeq = curSeq;
                ++ctx->updateSeq;
            }
        }

        if (!WaitInterruptibleOrReconfigMs(
                ctx->stopEvent, ctx->reconfigEvent, ctx->stopRequested, ctx->sourceConfigSeq, curSeq, pollMs)) {
            break;
        }
    }
}

std::string FormatMutexBusyDetail(const std::string& mutexNameUtf8, UiLanguage effLang) {
    if (mutexNameUtf8.empty()) {
        return std::string();
    }
    if (effLang == UiLanguage::EnUs) {
        return "Shared memory mutex [" + mutexNameUtf8 +
               "] is currently held by another producer process; please close other Stage C/D producers before starting.";
    }
    return "共享内存互斥量 [" + mutexNameUtf8 +
           "] 正被另一生产者进程持有；请先退出其他 Stage C/D 生产者后再启动。";
}

} // namespace

// ============================================================================
// StageEBridgeEngine Lifecycle Implementation
// ============================================================================

StageEBridgeEngine::StageEBridgeEngine() {
    const UiLanguage eff = GetEffectiveLanguage();
    lastDiagInput_ = BridgeDiagnosticInput{};
    lastDiagTracker_ = BridgeStatusTracker{};
    snapshot_.report = EvaluateBridgeDiagnostics(lastDiagInput_, lastDiagTracker_, eff);
}

StageEBridgeEngine::~StageEBridgeEngine() {
    Stop();
}

bool StageEBridgeEngine::IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

StageELiveSnapshot StageEBridgeEngine::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(stateMu_);
    return snapshot_;
}

UiLanguage StageEBridgeEngine::GetLanguage() const noexcept {
    return static_cast<UiLanguage>(uiLanguage_.load(std::memory_order_acquire));
}

UiLanguage StageEBridgeEngine::GetEffectiveLanguage() const noexcept {
    return ResolveEffectiveLanguage(GetLanguage());
}

void StageEBridgeEngine::SetLanguage(UiLanguage lang) noexcept {
    uiLanguage_.store(static_cast<uint8_t>(lang), std::memory_order_release);
    const UiLanguage eff = ResolveEffectiveLanguage(lang);

    std::lock_guard<std::mutex> lock(stateMu_);
    if (snapshot_.routing.state != RoutingIsolationState::Unknown) {
        FormatRoutingDiagnosticText(snapshot_.routing, eff);
    }
    if (snapshot_.producerMutexBusy && !lastBusyMutexNameUtf8_.empty()) {
        const std::string busyMsg = FormatMutexBusyDetail(lastBusyMutexNameUtf8_, eff);
        snapshot_.errorDetailUtf8 = busyMsg;
        lastDiagInput_.mutexBusyDetailUtf8 = busyMsg;
    }
    lastDiagInput_.routing = snapshot_.routing;
    BridgeStatusTracker tempTracker = lastDiagTracker_;
    snapshot_.report = EvaluateBridgeDiagnostics(lastDiagInput_, tempTracker, eff);
}

bool StageEBridgeEngine::SetAudioSource(const AudioSourceConfig& rawSource) {
    AudioSourceConfig normalized{};
    if (!NormalizeAudioSourceConfig(rawSource, normalized)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> srcLock(sourceMu_);
        sourceConfig_ = normalized;
        sourceConfigSeq_.fetch_add(1ULL, std::memory_order_acq_rel);
        if (captureReconfigEvent_) {
            SetEvent(captureReconfigEvent_);
        }
        if (topoReconfigEvent_) {
            SetEvent(topoReconfigEvent_);
        }
    }

    const UiLanguage eff = GetEffectiveLanguage();
    const std::string dispUtf8 = FormatAudioSourceDisplayNameUtf8(normalized);
    {
        std::lock_guard<std::mutex> lock(stateMu_);
        snapshot_.activeSource = normalized;
        snapshot_.targetDisplayNameUtf8 = dispUtf8;
        snapshot_.routing = RoutingDiagnostic{};
        snapshot_.routing.isAppleMusicMode = (normalized.mode == AudioSourceMode::AppleMusicPreset);
        snapshot_.routing.targetDisplayNameUtf8 = dispUtf8;

        lastDiagInput_.sourceMode = normalized.mode;
        lastDiagInput_.targetDisplayNameUtf8 = dispUtf8;
        lastDiagInput_.captureStreamActive = false;
        lastDiagInput_.captureTargetPid = 0u;
        lastDiagInput_.routing = snapshot_.routing;
        lastDiagTracker_.Reset();
        BridgeStatusTracker tempTracker = lastDiagTracker_;
        snapshot_.report = EvaluateBridgeDiagnostics(lastDiagInput_, tempTracker, eff);
    }
    return true;
}

AudioSourceConfig StageEBridgeEngine::GetAudioSource() const {
    std::lock_guard<std::mutex> srcLock(sourceMu_);
    return sourceConfig_;
}

bool StageEBridgeEngine::Start(const StageEEngineOptions& opts, std::string& outError) {
    std::lock_guard<std::mutex> lifeLock(lifecycleMu_);
    outError.clear();

    const AudioSourceConfig effectiveSrc = DeriveEffectiveSourceFromOptions(opts);
    SetAudioSource(effectiveSrc);
    SetLanguage(opts.language);

    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (commitThread_.joinable()) {
        commitThread_.join();
    }

    stopRequested_.store(false, std::memory_order_release);
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        outError = "CreateEventW(stopEvent) failed: err=" + std::to_string(GetLastError());
        return false;
    }

    ScopedHandle startupDoneEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!startupDoneEvent.IsValid()) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        outError = "CreateEventW(startupDoneEvent) failed: err=" + std::to_string(GetLastError());
        return false;
    }

    startupSucceeded_ = false;
    startupError_.clear();

    commitThread_ = std::thread(&StageEBridgeEngine::TimelineCommitThreadMain,
                                this,
                                opts,
                                startupDoneEvent.Get());

    WaitForSingleObject(startupDoneEvent.Get(), INFINITE);

    if (!startupSucceeded_) {
        outError = startupError_;
        if (commitThread_.joinable()) {
            commitThread_.join();
        }
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        return false;
    }

    return true;
}

void StageEBridgeEngine::Stop() noexcept {
    std::lock_guard<std::mutex> lifeLock(lifecycleMu_);

    stopRequested_.store(true, std::memory_order_release);
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    if (commitThread_.joinable()) {
        commitThread_.join();
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
}

void StageEBridgeEngine::TimelineCommitThreadMain(StageEEngineOptions opts, HANDLE startupDoneEvent) {
    const UiLanguage initLang = GetEffectiveLanguage();
    const bool initEn = (initLang == UiLanguage::EnUs);

    auto failStartup = [&](bool mutexBusy, const std::string& busyMutexUtf8, const std::string& errText) {
        startupError_ = errText;
        startupSucceeded_ = false;
        running_.store(false, std::memory_order_release);
        {
            const UiLanguage eff = GetEffectiveLanguage();
            std::lock_guard<std::mutex> lock(stateMu_);
            snapshot_.running = false;
            snapshot_.producerMutexBusy = mutexBusy;
            snapshot_.errorDetailUtf8 = errText;
            lastBusyMutexNameUtf8_ = busyMutexUtf8;
            BridgeDiagnosticInput in{};
            in.engineRunning = false;
            in.producerMutexBusy = mutexBusy;
            in.mutexBusyDetailUtf8 = errText;
            in.lastShutdownVerified = snapshot_.lastShutdownVerified;
            in.lastShutdownClean = snapshot_.lastShutdownClean;
            in.lastShutdownQueuedFrames = snapshot_.lastShutdownQueuedFrames;
            in.lastShutdownCommittedFrames = snapshot_.lastShutdownCommittedFrames;
            in.lastShutdownHoldbackFrames = snapshot_.lastShutdownHoldbackFrames;
            BridgeStatusTracker tr{};
            lastDiagInput_ = in;
            lastDiagTracker_ = tr;
            snapshot_.report = EvaluateBridgeDiagnostics(in, tr, eff);
        }
        if (startupDoneEvent) {
            SetEvent(startupDoneEvent);
        }
    };

    const std::wstring mutexName = opts.mappingName + stagec::kProducerMutexSuffix;
    const std::string mutexNameUtf8 = staged::WideToUtf8(mutexName);
    ScopedHandle hMutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (!hMutex.IsValid()) {
        const DWORD err = GetLastError();
        failStartup(
            false,
            "",
            initEn ? ("Failed to create producer mutex " + mutexNameUtf8 + " (err=" + std::to_string(err) + ")")
                   : ("无法创建生产者互斥量 " + mutexNameUtf8 + " (err=" + std::to_string(err) + ")"));
        return;
    }

    const DWORD mutexWait = WaitForSingleObject(hMutex.Get(), 250u);
    if (mutexWait == WAIT_TIMEOUT) {
        failStartup(true, mutexNameUtf8, FormatMutexBusyDetail(mutexNameUtf8, initLang));
        return;
    }
    if (mutexWait != WAIT_OBJECT_0 && mutexWait != WAIT_ABANDONED) {
        const DWORD err = GetLastError();
        failStartup(
            false,
            "",
            initEn ? ("Waiting for producer mutex failed (res=" + std::to_string(mutexWait) + ", err=" + std::to_string(err) + ")")
                   : ("等待生产者互斥量失败 (res=" + std::to_string(mutexWait) + ", err=" + std::to_string(err) + ")"));
        return;
    }

    ScopedHandle hMapping(CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        stagec::kTotalSharedBytes,
        opts.mappingName.c_str()));
    if (!hMapping.IsValid()) {
        const DWORD err = GetLastError();
        ReleaseMutex(hMutex.Get());
        failStartup(
            false,
            "",
            "CreateFileMappingW(" + staged::WideToUtf8(opts.mappingName) +
                (initEn ? ") failed (err=" : ") 失败 (err=") + std::to_string(err) + ")");
        return;
    }

    void* view = MapViewOfFile(hMapping.Get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!view) {
        const DWORD err = GetLastError();
        ReleaseMutex(hMutex.Get());
        failStartup(
            false,
            "",
            (initEn ? "MapViewOfFile failed (err=" : "MapViewOfFile 失败 (err=") + std::to_string(err) + ")");
        return;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < stagec::kTotalSharedBytes) {
        UnmapViewOfFile(view);
        ReleaseMutex(hMutex.Get());
        failStartup(
            false,
            "",
            initEn ? "Shared memory mapping size validation failed" : "共享内存映射尺寸校验失败");
        return;
    }

    RotatingTelemetryLogger telemetryLogger;
    std::string logOpenErr;
    if (!telemetryLogger.Open(opts.logger, logOpenErr)) {
        UnmapViewOfFile(view);
        ReleaseMutex(hMutex.Get());
        failStartup(
            false,
            "",
            (initEn ? "Failed to open session log: " : "打开会话日志失败：") + logOpenErr);
        return;
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
    RollingLevelMeter levelMeter;
    BridgeStatusTracker statusTracker;

    const uint64_t prefillFrames = std::min<uint64_t>(
        staged::MulDivFloorU64(static_cast<uint64_t>(opts.prefillMs), staged::kOutputSampleRate, 1000ULL),
        static_cast<uint64_t>(stagec::kRingCapacityFrames / 2u));

    QueryPerformanceCounter(&startQpcLi);
    const int64_t start100nsSigned = staged::QpcTicksTo100ns(startQpcLi.QuadPart, qpcFreq);
    const uint64_t start100ns = (start100nsSigned > 0) ? static_cast<uint64_t>(start100nsSigned) : 0ULL;
    const uint64_t prefill100ns = staged::MulDivFloorU64(prefillFrames, 10000000ULL, staged::kOutputSampleRate);
    const uint64_t origin100ns = (start100ns >= prefill100ns) ? (start100ns - prefill100ns) : 0ULL;

    bridge.Reset(origin100ns);
    levelMeter.Reset(start100ns);
    if (prefillFrames > 0ULL) {
        bridge.PrefillZeroFrames(prefillFrames, true);
    }

    stagec::AtomicStoreI64(&hdr->start_qpc, startQpcLi.QuadPart);
    stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, startQpcLi.QuadPart);
    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Streaming));
    stagec::AtomicStoreU64(&hdr->session_generation, evenGen);

    ScopedHandle hWorkerStopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ScopedHandle hCaptureReconfigEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    ScopedHandle hTopoReconfigEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));

    AudioSourceConfig curSourceCfg{};
    uint64_t appliedSourceSeq = 0ULL;
    {
        std::lock_guard<std::mutex> srcLock(sourceMu_);
        captureReconfigEvent_ = hCaptureReconfigEvent.Get();
        topoReconfigEvent_ = hTopoReconfigEvent.Get();
        curSourceCfg = sourceConfig_;
        appliedSourceSeq = sourceConfigSeq_.load(std::memory_order_acquire);
    }
    std::string curTargetDisplayNameUtf8 = FormatAudioSourceDisplayNameUtf8(curSourceCfg);

    BoundedCaptureQueue captureQueue;
    CaptureWorkerContext captureCtx;
    captureCtx.opts = opts;
    captureCtx.qpcFreq = qpcFreq;
    captureCtx.stopEvent = hWorkerStopEvent.Get();
    captureCtx.reconfigEvent = hCaptureReconfigEvent.Get();
    captureCtx.sourceMu = &sourceMu_;
    captureCtx.sharedSource = &sourceConfig_;
    captureCtx.sourceConfigSeq = &sourceConfigSeq_;
    captureCtx.queue = &captureQueue;

    SharedTopologyContext topoCtx;
    topoCtx.opts = opts;
    topoCtx.stopEvent = hWorkerStopEvent.Get();
    topoCtx.reconfigEvent = hTopoReconfigEvent.Get();
    topoCtx.sourceMu = &sourceMu_;
    topoCtx.sharedSource = &sourceConfig_;
    topoCtx.sourceConfigSeq = &sourceConfigSeq_;
    topoCtx.activeTargetPid.store(opts.targetPid, std::memory_order_release);
    topoCtx.uiLanguage = &uiLanguage_;

    staged::TelemetryLineContext telCtx;
    telCtx.targetPid = opts.targetPid;
    telCtx.targetExeUtf8 = (curSourceCfg.mode == AudioSourceMode::AppleMusicPreset)
        ? std::string("AMPLibraryAgent.exe")
        : staged::WideToUtf8(curSourceCfg.customExeName);
    telCtx.formatLabelUtf8 = "unstarted";
    telCtx.nowQpc100ns = start100ns;
    telCtx.ipc = ReadIpcCounters(hdr);

    bool captureStreamActive = false;
    uint32_t captureTargetPid = opts.targetPid;
    std::string captureTargetExeUtf8 = telCtx.targetExeUtf8;
    std::string captureFormatUtf8 = "unstarted";
    std::string lastStreamEndReasonUtf8;
    uint64_t lastStreamEndQpc100ns = 0ULL;
    bool hasStreamEndedOnce = false;
    RoutingDiagnostic cachedRouting{};
    cachedRouting.isAppleMusicMode = (curSourceCfg.mode == AudioSourceMode::AppleMusicPreset);
    cachedRouting.targetDisplayNameUtf8 = curTargetDisplayNameUtf8;
    BridgeDiagnosticInput latestBuiltInput{};

    auto buildDiagReport = [&](uint64_t now100ns,
                               const PcmLevelSnapshot& lvl,
                               const staged::TimelineStatsSnapshot& tlSnap,
                               const staged::IpcTelemetryCounters& ipcSnap,
                               bool isEngineRunning) -> BridgeDiagnosticReport {
        const UiLanguage curLang = GetEffectiveLanguage();
        cachedRouting.isAppleMusicMode = (curSourceCfg.mode == AudioSourceMode::AppleMusicPreset);
        cachedRouting.targetDisplayNameUtf8 = curTargetDisplayNameUtf8;
        if (cachedRouting.state != RoutingIsolationState::Unknown) {
            FormatRoutingDiagnosticText(cachedRouting, curLang);
        }

        BridgeDiagnosticInput in{};
        in.nowQpc100ns = now100ns;
        in.engineRunning = isEngineRunning;
        in.sourceMode = curSourceCfg.mode;
        in.targetDisplayNameUtf8 = curTargetDisplayNameUtf8;
        in.captureStreamActive = captureStreamActive;
        in.captureTargetPid = captureTargetPid;
        in.captureTargetExeUtf8 = captureTargetExeUtf8;
        in.captureFormatUtf8 = captureFormatUtf8;
        in.streamRestartCount = tlSnap.streamRestartCount;
        if (hasStreamEndedOnce && now100ns >= lastStreamEndQpc100ns) {
            in.msSinceLastStreamEnd = (now100ns - lastStreamEndQpc100ns) / 10000ULL;
        }
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            in.lastShutdownVerified = snapshot_.lastShutdownVerified;
            in.lastShutdownClean = snapshot_.lastShutdownClean;
            in.lastShutdownQueuedFrames = snapshot_.lastShutdownQueuedFrames;
            in.lastShutdownCommittedFrames = snapshot_.lastShutdownCommittedFrames;
            in.lastShutdownHoldbackFrames = snapshot_.lastShutdownHoldbackFrames;
        }
        in.level = lvl;
        in.routing = cachedRouting;
        in.ipc = ipcSnap;
        latestBuiltInput = in;
        return EvaluateBridgeDiagnostics(in, statusTracker, curLang);
    };

    auto publishSnapshot = [&](double uptimeSec,
                               bool isEngineRunning,
                               const staged::TimelineStatsSnapshot& tlSnap,
                               const staged::IpcTelemetryCounters& ipcSnap,
                               const PcmLevelSnapshot& lvl,
                               const BridgeDiagnosticReport& rep) {
        std::lock_guard<std::mutex> lock(stateMu_);
        snapshot_.running = isEngineRunning;
        snapshot_.producerMutexBusy = false;
        snapshot_.errorDetailUtf8.clear();
        lastBusyMutexNameUtf8_.clear();
        snapshot_.activeSource = curSourceCfg;
        snapshot_.targetDisplayNameUtf8 = curTargetDisplayNameUtf8;
        snapshot_.sessionGeneration = evenGen;
        snapshot_.uptimeSec = uptimeSec;
        snapshot_.captureStreamActive = captureStreamActive;
        snapshot_.captureTargetPid = captureTargetPid;
        snapshot_.captureTargetExeUtf8 = captureTargetExeUtf8;
        snapshot_.captureFormatUtf8 = captureFormatUtf8;
        snapshot_.lastStreamEndReasonUtf8 = lastStreamEndReasonUtf8;
        snapshot_.activeLogPath = telemetryLogger.ActiveFilePath();
        snapshot_.logDirectory = telemetryLogger.LogDirectory();
        snapshot_.activeLogPartIndex = telemetryLogger.CurrentPartIndex();
        snapshot_.activeLogPartBytes = telemetryLogger.CurrentPartBytes();
        snapshot_.retainedLogSessions = telemetryLogger.RetainedSessionCountAfterPrune();
        snapshot_.timeline = tlSnap;
        snapshot_.ipc = ipcSnap;
        snapshot_.level = lvl;
        snapshot_.routing = cachedRouting;
        snapshot_.report = rep;
        lastDiagInput_ = latestBuiltInput;
        lastDiagTracker_ = statusTracker;
    };

    {
        const staged::TimelineStatsSnapshot initSnap = bridge.Snapshot();
        const PcmLevelSnapshot initLevel = levelMeter.Snapshot(start100ns);
        const BridgeDiagnosticReport initRep = buildDiagReport(start100ns, initLevel, initSnap, telCtx.ipc, true);
        telCtx.kind = "startup";
        telemetryLogger.WriteLine(FormatStageETelemetryJsonLine(initSnap, telCtx, initLevel, cachedRouting, initRep));
        publishSnapshot(0.0, true, initSnap, telCtx.ipc, initLevel, initRep);
    }

    running_.store(true, std::memory_order_release);
    startupSucceeded_ = true;
    if (startupDoneEvent) {
        SetEvent(startupDoneEvent);
    }

    timeBeginPeriod(1);
    std::thread topoThread(&TopologyMonitorProc, &topoCtx);
    std::thread captureThread(&CaptureWorkerProc, &captureCtx);

    const uint64_t telemetryInterval100ns =
        static_cast<uint64_t>(std::max<uint32_t>(100u, opts.telemetryIntervalMs)) * 10000ULL;
    uint64_t nextTelemetryQpc100ns = start100ns + telemetryInterval100ns;
    uint64_t nextUiPublishQpc100ns = start100ns;

    uint64_t prevDiscont = 0ULL;
    uint64_t prevTsErr = 0ULL;
    uint64_t prevFallback = 0ULL;
    uint64_t prevRestart = 0ULL;
    PipelineHeadlineState prevHeadline = PipelineHeadlineState::WaitingForAppleMusic;
    RoutingIsolationState prevRoutingState = RoutingIsolationState::Unknown;
    bool hasPrevDiagEdge = false;

    std::vector<CaptureQueueItem> drained;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        bool forceUiPublish = false;

        if (sourceConfigSeq_.load(std::memory_order_acquire) != appliedSourceSeq) {
            {
                std::lock_guard<std::mutex> srcLock(sourceMu_);
                curSourceCfg = sourceConfig_;
                appliedSourceSeq = sourceConfigSeq_.load(std::memory_order_acquire);
            }
            curTargetDisplayNameUtf8 = FormatAudioSourceDisplayNameUtf8(curSourceCfg);
            captureTargetExeUtf8 = (curSourceCfg.mode == AudioSourceMode::AppleMusicPreset)
                ? std::string("AMPLibraryAgent.exe")
                : staged::WideToUtf8(curSourceCfg.customExeName);
            if (!captureStreamActive) {
                telCtx.targetExeUtf8 = captureTargetExeUtf8;
            }
            cachedRouting = RoutingDiagnostic{};
            cachedRouting.isAppleMusicMode = (curSourceCfg.mode == AudioSourceMode::AppleMusicPreset);
            cachedRouting.targetDisplayNameUtf8 = curTargetDisplayNameUtf8;
            statusTracker.Reset();
            forceUiPublish = true;
        }

        {
            std::lock_guard<std::mutex> lock(topoCtx.mu);
            if (topoCtx.latestSourceSeq == appliedSourceSeq) {
                cachedRouting = topoCtx.latestRouting;
            }
        }

        captureQueue.DrainAll(drained);

        for (const auto& item : drained) {
            if (item.type == CaptureQueueItem::Type::StreamStarted) {
                bridge.OnStreamStarted(item.sampleRate, item.convDelayFrames48k);
                captureStreamActive = true;
                captureTargetPid = item.targetPid;
                captureTargetExeUtf8 = item.targetExeUtf8;
                captureFormatUtf8 = item.formatLabelUtf8;
                topoCtx.activeTargetPid.store(item.targetPid, std::memory_order_release);

                telCtx.targetPid = item.targetPid;
                telCtx.targetExeUtf8 = item.targetExeUtf8;
                telCtx.formatLabelUtf8 = item.formatLabelUtf8;

                LARGE_INTEGER evQpc{};
                QueryPerformanceCounter(&evQpc);
                const int64_t ev100nsSigned = staged::QpcTicksTo100ns(evQpc.QuadPart, qpcFreq);
                const uint64_t ev100ns = (ev100nsSigned > 0) ? static_cast<uint64_t>(ev100nsSigned) : 0ULL;
                telCtx.nowQpc100ns = ev100ns;
                telCtx.ipc = ReadIpcCounters(hdr);
                telCtx.kind = "stream_started";

                const staged::TimelineStatsSnapshot sSnap = bridge.Snapshot();
                const PcmLevelSnapshot sLvl = levelMeter.Snapshot(ev100ns);
                const BridgeDiagnosticReport sRep = buildDiagReport(ev100ns, sLvl, sSnap, telCtx.ipc, true);
                telemetryLogger.WriteLine(FormatStageETelemetryJsonLine(sSnap, telCtx, sLvl, cachedRouting, sRep));
                forceUiPublish = true;
            } else if (item.type == CaptureQueueItem::Type::Packet) {
                const int16_t* ptr = item.stereo48kLR.empty() ? nullptr : item.stereo48kLR.data();
                levelMeter.IngestStereoInt16(item.nowQpc100ns, ptr, item.outFrames48k, item.silentFlag);

                staged::TimelinePacketInput pkt;
                pkt.sourceFrames = item.sourceFrames;
                pkt.stereo48kLR = ptr;
                pkt.outFrames48k = item.outFrames48k;
                pkt.devPos = item.devPos;
                pkt.qpcPos100ns = item.qpcPos100ns;
                pkt.nowQpc100ns = item.nowQpc100ns;
                pkt.silentFlag = item.silentFlag;
                pkt.discontinuityFlag = item.discontinuityFlag;
                pkt.timestampErrorFlag = item.timestampErrorFlag;
                bridge.IngestPacket(pkt);
            } else if (item.type == CaptureQueueItem::Type::StreamEnded) {
                bridge.OnStreamEnded();
                captureStreamActive = false;
                captureTargetPid = opts.targetPid;
                lastStreamEndReasonUtf8 = item.reasonUtf8;
                topoCtx.activeTargetPid.store(opts.targetPid, std::memory_order_release);

                LARGE_INTEGER evQpc{};
                QueryPerformanceCounter(&evQpc);
                const int64_t ev100nsSigned = staged::QpcTicksTo100ns(evQpc.QuadPart, qpcFreq);
                const uint64_t ev100ns = (ev100nsSigned > 0) ? static_cast<uint64_t>(ev100nsSigned) : 0ULL;
                lastStreamEndQpc100ns = ev100ns;
                hasStreamEndedOnce = true;

                telCtx.nowQpc100ns = ev100ns;
                telCtx.ipc = ReadIpcCounters(hdr);
                telCtx.kind = "stream_ended";

                const staged::TimelineStatsSnapshot eSnap = bridge.Snapshot();
                const PcmLevelSnapshot eLvl = levelMeter.Snapshot(ev100ns);
                const BridgeDiagnosticReport eRep = buildDiagReport(ev100ns, eLvl, eSnap, telCtx.ipc, true);
                telemetryLogger.WriteLine(FormatStageETelemetryJsonLine(eSnap, telCtx, eLvl, cachedRouting, eRep));
                forceUiPublish = true;
            }
        }

        LARGE_INTEGER nowQpcLi{};
        QueryPerformanceCounter(&nowQpcLi);
        const int64_t now100nsSigned = staged::QpcTicksTo100ns(nowQpcLi.QuadPart, qpcFreq);
        const uint64_t now100ns = (now100nsSigned > 0) ? static_cast<uint64_t>(now100nsSigned) : 0ULL;

        bridge.Tick(now100ns);
        const PcmLevelSnapshot curLevel = levelMeter.Snapshot(now100ns);

        stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, nowQpcLi.QuadPart);
        stagec::AtomicAddU64(&hdr->producer_tick_count, 1ULL);

        const staged::TimelineStatsSnapshot snap = bridge.Snapshot();
        const staged::IpcTelemetryCounters ipcCounters = ReadIpcCounters(hdr);
        const BridgeDiagnosticReport diagRep = buildDiagReport(now100ns, curLevel, snap, ipcCounters, true);

        const bool diagStateChanged =
            !hasPrevDiagEdge ||
            (diagRep.headline != prevHeadline) ||
            (cachedRouting.state != prevRoutingState);
        const bool timelineEdge =
            (snap.discontinuityCount != prevDiscont) ||
            (snap.timestampErrorCount != prevTsErr) ||
            (snap.timestampFallbackAnchors != prevFallback) ||
            (snap.streamRestartCount != prevRestart);

        if (timelineEdge || diagStateChanged) {
            prevDiscont = snap.discontinuityCount;
            prevTsErr = snap.timestampErrorCount;
            prevFallback = snap.timestampFallbackAnchors;
            prevRestart = snap.streamRestartCount;
            prevHeadline = diagRep.headline;
            prevRoutingState = cachedRouting.state;
            hasPrevDiagEdge = true;

            telCtx.nowQpc100ns = now100ns;
            telCtx.ipc = ipcCounters;
            telCtx.kind = "event";
            telemetryLogger.WriteLine(FormatStageETelemetryJsonLine(snap, telCtx, curLevel, cachedRouting, diagRep));
            forceUiPublish = true;
        }

        if (now100ns >= nextTelemetryQpc100ns) {
            telCtx.nowQpc100ns = now100ns;
            telCtx.ipc = ipcCounters;
            telCtx.kind = "periodic";
            telemetryLogger.WriteLine(FormatStageETelemetryJsonLine(snap, telCtx, curLevel, cachedRouting, diagRep));
            while (nextTelemetryQpc100ns <= now100ns) {
                nextTelemetryQpc100ns += telemetryInterval100ns;
            }
        }

        if (forceUiPublish || now100ns >= nextUiPublishQpc100ns) {
            const double uptimeSec =
                static_cast<double>(nowQpcLi.QuadPart - startQpcLi.QuadPart) /
                static_cast<double>(qpcFreq);
            publishSnapshot(uptimeSec, true, snap, ipcCounters, curLevel, diagRep);
            nextUiPublishQpc100ns = now100ns + 330000ULL; // ~33 ms (30 Hz max snapshot refresh)
        }

        if (stopEvent_) {
            if (WaitForSingleObject(stopEvent_, 3u) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            Sleep(3u);
        }
    }

    // Orderly worker shutdown & clean holdback flush
    captureCtx.stopRequested.store(true, std::memory_order_release);
    topoCtx.stopRequested.store(true, std::memory_order_release);
    if (hWorkerStopEvent.IsValid()) {
        SetEvent(hWorkerStopEvent.Get());
    }
    if (captureThread.joinable()) {
        captureThread.join();
    }
    if (topoThread.joinable()) {
        topoThread.join();
    }
    {
        std::lock_guard<std::mutex> srcLock(sourceMu_);
        captureReconfigEvent_ = nullptr;
        topoReconfigEvent_ = nullptr;
    }

    captureQueue.DrainAll(drained);
    for (const auto& item : drained) {
        if (item.type == CaptureQueueItem::Type::StreamStarted) {
            bridge.OnStreamStarted(item.sampleRate, item.convDelayFrames48k);
            telCtx.targetPid = item.targetPid;
            telCtx.targetExeUtf8 = item.targetExeUtf8;
            telCtx.formatLabelUtf8 = item.formatLabelUtf8;
        } else if (item.type == CaptureQueueItem::Type::Packet) {
            const int16_t* ptr = item.stereo48kLR.empty() ? nullptr : item.stereo48kLR.data();
            levelMeter.IngestStereoInt16(item.nowQpc100ns, ptr, item.outFrames48k, item.silentFlag);

            staged::TimelinePacketInput pkt;
            pkt.sourceFrames = item.sourceFrames;
            pkt.stereo48kLR = ptr;
            pkt.outFrames48k = item.outFrames48k;
            pkt.devPos = item.devPos;
            pkt.qpcPos100ns = item.qpcPos100ns;
            pkt.nowQpc100ns = item.nowQpc100ns;
            pkt.silentFlag = item.silentFlag;
            pkt.discontinuityFlag = item.discontinuityFlag;
            pkt.timestampErrorFlag = item.timestampErrorFlag;
            bridge.IngestPacket(pkt);
        } else if (item.type == CaptureQueueItem::Type::StreamEnded) {
            bridge.OnStreamEnded();
        }
    }

    captureStreamActive = false;
    bridge.OnStreamEnded();
    bridge.FlushHoldbackOnStop();

    LARGE_INTEGER endQpcLi{};
    QueryPerformanceCounter(&endQpcLi);
    const int64_t end100nsSigned = staged::QpcTicksTo100ns(endQpcLi.QuadPart, qpcFreq);
    const uint64_t end100ns = (end100nsSigned > 0) ? static_cast<uint64_t>(end100nsSigned) : 0ULL;

    stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, endQpcLi.QuadPart);
    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Stopped));

    const staged::TimelineStatsSnapshot finalSnap = bridge.Snapshot();
    const staged::IpcTelemetryCounters finalIpc = ReadIpcCounters(hdr);
    const PcmLevelSnapshot finalLevel = levelMeter.Snapshot(end100ns);
    const bool cleanShutdownOk = finalSnap.VerifyInvariants(true);

    {
        std::lock_guard<std::mutex> lock(stateMu_);
        snapshot_.lastShutdownVerified = true;
        snapshot_.lastShutdownClean = cleanShutdownOk;
        snapshot_.lastShutdownQueuedFrames = finalSnap.timelineQueuedFrames;
        snapshot_.lastShutdownCommittedFrames = finalSnap.committedRingFrames;
        snapshot_.lastShutdownHoldbackFrames = finalSnap.holdbackFrames;
    }

    const BridgeDiagnosticReport stoppedRep = buildDiagReport(end100ns, finalLevel, finalSnap, finalIpc, false);

    telCtx.nowQpc100ns = end100ns;
    telCtx.ipc = finalIpc;
    telCtx.kind = "shutdown_flush";
    // allowPartSplit = false guarantees shutdown_flush stays at the tail of the active session log part
    telemetryLogger.WriteLine(
        FormatStageETelemetryJsonLine(finalSnap, telCtx, finalLevel, cachedRouting, stoppedRep),
        false);

    const std::wstring finalLogPath = telemetryLogger.ActiveFilePath();
    const std::wstring finalLogDir = telemetryLogger.LogDirectory();
    const uint32_t finalPartIdx = telemetryLogger.CurrentPartIndex();
    const uint64_t finalPartBytes = telemetryLogger.CurrentPartBytes();
    const uint32_t finalRetained = telemetryLogger.RetainedSessionCountAfterPrune();
    telemetryLogger.Close();

    timeEndPeriod(1);

    const double totalUptimeSec =
        static_cast<double>(endQpcLi.QuadPart - startQpcLi.QuadPart) /
        static_cast<double>(qpcFreq);

    {
        std::lock_guard<std::mutex> lock(stateMu_);
        snapshot_.running = false;
        snapshot_.uptimeSec = totalUptimeSec;
        snapshot_.captureStreamActive = false;
        snapshot_.activeLogPath = finalLogPath;
        snapshot_.logDirectory = finalLogDir;
        snapshot_.activeLogPartIndex = finalPartIdx;
        snapshot_.activeLogPartBytes = finalPartBytes;
        snapshot_.retainedLogSessions = finalRetained;
        snapshot_.timeline = finalSnap;
        snapshot_.ipc = finalIpc;
        snapshot_.level = finalLevel;
        snapshot_.routing = cachedRouting;
        snapshot_.report = stoppedRep;
        lastDiagInput_ = latestBuiltInput;
        lastDiagTracker_ = statusTracker;
    }

    running_.store(false, std::memory_order_release);

    UnmapViewOfFile(view);
    hMapping.Reset();
    ReleaseMutex(hMutex.Get());
    hMutex.Reset();
}

} // namespace stagee
