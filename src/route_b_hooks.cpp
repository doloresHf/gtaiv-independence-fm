#include "route_b_hooks.h"

#include <MinHook.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "route_b_ipc_consumer.h"
#include "route_b_offsets.h"

namespace routeb {

namespace {

// ============================================================================
// Self-Contained Asynchronous Logger (IVRadioBridge.log / IVRadioBridge.jsonl)
// ============================================================================

class BridgeLogger {
public:
    static BridgeLogger& Instance() {
        static BridgeLogger inst;
        return inst;
    }

    void Init(const std::wstring& moduleDir) {
        if (InterlockedCompareExchange(&initialized_, 1, 0) != 0) {
            return;
        }
        QueryPerformanceFrequency(&qpcFreq_);
        QueryPerformanceCounter(&qpcStart_);

        slots_.resize(kRingSize);
        const std::wstring logPath   = moduleDir + L"IVRadioBridge.log";
        const std::wstring jsonlPath = moduleDir + L"IVRadioBridge.jsonl";

        logFile_ = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        jsonlFile_ = CreateFileW(jsonlPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        InterlockedExchange(&running_, 1);
        workerThread_ = CreateThread(nullptr, 0, &BridgeLogger::WorkerProc, this, 0, nullptr);
    }

    void Shutdown() {
        if (InterlockedCompareExchange(&running_, 0, 1) == 1) {
            if (wakeEvent_) {
                SetEvent(wakeEvent_);
            }
            if (workerThread_) {
                WaitForSingleObject(workerThread_, 2000);
                CloseHandle(workerThread_);
                workerThread_ = nullptr;
            }
        }
        DrainAvailable();
        if (wakeEvent_) {
            CloseHandle(wakeEvent_);
            wakeEvent_ = nullptr;
        }
        if (logFile_ != INVALID_HANDLE_VALUE) {
            CloseHandle(logFile_);
            logFile_ = INVALID_HANDLE_VALUE;
        }
        if (jsonlFile_ != INVALID_HANDLE_VALUE) {
            CloseHandle(jsonlFile_);
            jsonlFile_ = INVALID_HANDLE_VALUE;
        }
        InterlockedExchange(&initialized_, 0);
    }

    int64_t NowMicros() const noexcept {
        if (qpcFreq_.QuadPart <= 0) {
            return 0;
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const LONGLONG delta = now.QuadPart - qpcStart_.QuadPart;
        const LONGLONG sec   = delta / qpcFreq_.QuadPart;
        const LONGLONG rem   = delta % qpcFreq_.QuadPart;
        return static_cast<int64_t>(sec) * 1000000LL + (static_cast<int64_t>(rem) * 1000000LL) / qpcFreq_.QuadPart;
    }

    void LogText(const char* fmt, ...) {
        if (InterlockedCompareExchange(&running_, 0, 0) == 0 || slots_.empty()) {
            return;
        }
        char msg[440]{};
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        const double ms = static_cast<double>(NowMicros()) / 1000.0;
        const DWORD tid = GetCurrentThreadId();
        char line[512]{};
        std::snprintf(line, sizeof(line), "[%10.3f ms][TID:%5lu] %s\r\n", ms, static_cast<unsigned long>(tid), msg);
        Enqueue(false, line);
    }

    void LogJson(const char* eventName, const char* fieldsFmt, ...) {
        if (InterlockedCompareExchange(&running_, 0, 0) == 0 || slots_.empty()) {
            return;
        }
        char fields[420]{};
        if (fieldsFmt && *fieldsFmt) {
            va_list ap;
            va_start(ap, fieldsFmt);
            std::vsnprintf(fields, sizeof(fields), fieldsFmt, ap);
            va_end(ap);
        }
        const int64_t tsUs = NowMicros();
        const DWORD tid    = GetCurrentThreadId();
        char line[512]{};
        if (fields[0] != '\0') {
            std::snprintf(line, sizeof(line),
                          "{\"ts_us\":%lld,\"tid\":%lu,\"event\":\"%s\",%s}\r\n",
                          static_cast<long long>(tsUs),
                          static_cast<unsigned long>(tid),
                          eventName ? eventName : "Unknown",
                          fields);
        } else {
            std::snprintf(line, sizeof(line),
                          "{\"ts_us\":%lld,\"tid\":%lu,\"event\":\"%s\"}\r\n",
                          static_cast<long long>(tsUs),
                          static_cast<unsigned long>(tid),
                          eventName ? eventName : "Unknown");
        }
        Enqueue(true, line);
    }

    static std::string WideToUtf8Escaped(const wchar_t* ws) {
        if (!ws || !*ws) {
            return {};
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            return {};
        }
        std::string utf8(static_cast<size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, utf8.data(), needed, nullptr, nullptr);

        std::string escaped;
        escaped.reserve(utf8.size() + 8);
        for (unsigned char c : utf8) {
            switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\r': escaped += "\\r"; break;
            case '\n': escaped += "\\n"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    escaped += buf;
                } else {
                    escaped.push_back(static_cast<char>(c));
                }
                break;
            }
        }
        return escaped;
    }

private:
    BridgeLogger() = default;
    ~BridgeLogger() { Shutdown(); }

    struct Slot {
        volatile LONG ready = 0; // 0=empty, 1=writing, 2=ready
        bool isJson = false;
        char text[512]{};
    };

    static constexpr uint32_t kRingSize = 4096u;
    static constexpr uint32_t kRingMask = kRingSize - 1u;

    void Enqueue(bool isJson, const char* text) {
        const LONG seq = InterlockedIncrement(&writeIdx_) - 1;
        Slot& slot = slots_[static_cast<uint32_t>(seq) & kRingMask];
        if (InterlockedCompareExchange(&slot.ready, 1, 0) != 0) {
            return;
        }
        slot.isJson = isJson;
        std::strncpy(slot.text, text, sizeof(slot.text) - 1u);
        slot.text[sizeof(slot.text) - 1u] = '\0';
        InterlockedExchange(&slot.ready, 2);
        if (wakeEvent_) {
            SetEvent(wakeEvent_);
        }
    }

    static DWORD WINAPI WorkerProc(LPVOID param) {
        auto* self = static_cast<BridgeLogger*>(param);
        while (InterlockedCompareExchange(&self->running_, 0, 0) != 0) {
            WaitForSingleObject(self->wakeEvent_, 20);
            self->DrainAvailable();
        }
        return 0;
    }

    void DrainAvailable() {
        if (slots_.empty()) {
            return;
        }
        for (uint32_t iter = 0u; iter < kRingSize; ++iter) {
            Slot& slot = slots_[readIdx_ & kRingMask];
            if (InterlockedCompareExchange(&slot.ready, 2, 2) != 2) {
                break;
            }
            HANDLE hTarget = slot.isJson ? jsonlFile_ : logFile_;
            if (hTarget != INVALID_HANDLE_VALUE) {
                const DWORD len = static_cast<DWORD>(std::strlen(slot.text));
                DWORD written = 0;
                WriteFile(hTarget, slot.text, len, &written, nullptr);
            }
            InterlockedExchange(&slot.ready, 0);
            ++readIdx_;
        }
    }

    std::vector<Slot> slots_;
    volatile LONG initialized_ = 0;
    volatile LONG running_ = 0;
    volatile LONG writeIdx_ = 0;
    uint32_t readIdx_ = 0u;
    HANDLE wakeEvent_ = nullptr;
    HANDLE workerThread_ = nullptr;
    HANDLE logFile_ = INVALID_HANDLE_VALUE;
    HANDLE jsonlFile_ = INVALID_HANDLE_VALUE;
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER qpcStart_{};
};

// ============================================================================
// String / Config Utilities
// ============================================================================

std::wstring TrimWide(const std::wstring& s) {
    size_t start = 0u;
    while (start < s.size() && std::iswspace(s[start])) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1u])) {
        --end;
    }
    return s.substr(start, end - start);
}

const wchar_t* ExtractLeafWide(const wchar_t* path) noexcept {
    if (!path) {
        return L"";
    }
    const wchar_t* leaf = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            leaf = p + 1;
        }
    }
    return leaf;
}

bool EqualsCaseInsensitiveWide(const wchar_t* a, const wchar_t* b) noexcept {
    if (!a || !b) {
        return false;
    }
    return _wcsicmp(a, b) == 0;
}

std::vector<std::wstring> ParseCsvWide(const std::wstring& csv) {
    std::vector<std::wstring> items;
    size_t pos = 0u;
    while (pos <= csv.size()) {
        size_t comma = csv.find(L',', pos);
        if (comma == std::wstring::npos) {
            comma = csv.size();
        }
        std::wstring token = TrimWide(csv.substr(pos, comma - pos));
        if (!token.empty()) {
            items.push_back(token);
        }
        if (comma >= csv.size()) {
            break;
        }
        pos = comma + 1u;
    }
    return items;
}

std::wstring GetModuleDirectoryWide(HMODULE hModule) {
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(hModule, path, MAX_PATH);
    if (len == 0u || len >= MAX_PATH) {
        return L".\\";
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return L".\\";
    }
    return std::wstring(path, slash + 1);
}

bool IsTargetCarrierLeaf(const RouteBIniConfig& cfg, const wchar_t* fullPath) noexcept {
    if (!fullPath || !*fullPath) {
        return false;
    }
    const wchar_t* leaf = ExtractLeafWide(fullPath);
    if (!*leaf) {
        return false;
    }
    for (const auto& t : cfg.targetFiles) {
        if (_wcsicmp(leaf, t.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Runtime Plugin State, Control SHM & GTA IV Function Detours
// ============================================================================

struct DecoderEntry {
    void* waveSlot = nullptr;
    bool isTarget = false;
    std::string leafUtf8;
    void* block0 = nullptr;
    void* block1 = nullptr;
    uint32_t blockBytes = 0u;
    bool preloadedReady = false;
    uint32_t steadyCallCount = 0u;
};

struct SlotSnapshot {
    bool active = false;
    bool isUser = false;
    uint8_t category = 0u;
    uint8_t state = 0u;
    uint8_t isUserFlag = 0u;
    int32_t playMs = 0;
    int32_t durationMs = 0;
    int32_t outroMs = 0;
    void* waveSlot = nullptr;
    void* decoder = nullptr;
};

SlotSnapshot ReadSlotSnapshot(const uint8_t* stationBytes, uint32_t slotIdx) noexcept {
    SlotSnapshot s{};
    if (!stationBytes || slotIdx > 1u) {
        return s;
    }
    const uint8_t* slot = stationBytes + slotIdx * kOffStationSlotStride;
    s.active     = (*reinterpret_cast<const uint8_t*>(slot + kOffSlotActive) != 0);
    s.category   = *reinterpret_cast<const uint8_t*>(slot + kOffSlotCategory);
    s.state      = *reinterpret_cast<const uint8_t*>(slot + kOffSlotState);
    s.isUserFlag = *reinterpret_cast<const uint8_t*>(slot + kOffSlotIsUserTrack);
    s.isUser     = (s.category == kSlotCategoryUserTrack) && (s.isUserFlag != 0);
    s.playMs     = *reinterpret_cast<const int32_t*>(slot + kOffSlotPlayTimeMs);
    s.durationMs = *reinterpret_cast<const int32_t*>(slot + kOffSlotDurationMs);
    s.outroMs    = *reinterpret_cast<const int32_t*>(slot + kOffSlotOutroMs);
    s.waveSlot   = *reinterpret_cast<void* const*>(slot + kOffSlotWaveSlot);
    if (s.waveSlot) {
        const auto* ws = static_cast<const uint8_t*>(s.waveSlot);
        s.decoder = *reinterpret_cast<void* const*>(ws + kOffWaveSlotDecoder);
    }
    return s;
}

using PFN_AudWaveSlot_OpenUser   = void(__thiscall*)(void* waveSlot, const wchar_t* widePath);
using PFN_UserTrack_FillBlocks   = uint8_t(__thiscall*)(void* decoderPtr, void* block0, void* block1, uint32_t blockBytes);
using PFN_AudRadioSlot_Play      = void(__thiscall*)(void* slotPtr);
using PFN_AudRadioSlot_Stop      = void(__thiscall*)(void* slotPtr);
using PFN_AudRadioStation_Update = void(__thiscall*)(void* stationPtr, uint32_t timeInMs);
using PFN_SkipForward            = void(__thiscall*)(void* stationPtr);
using PFN_SkipBackward           = void(__thiscall*)(void* stationPtr);

struct PluginRuntime {
    CRITICAL_SECTION stateCs{};
    bool csInitialized = false;
    bool hooksInstalled = false;
    bool mhInitialized = false;

    RouteBIniConfig cfg{};
    ResolvedTargets targets{};
    std::unique_ptr<RouteBIpcConsumer> consumer;

    HANDLE hCtrlMapping = nullptr;
    RadioBridgeCtrlBlock* ctrl = nullptr;
    LARGE_INTEGER qpcFreq{};

    std::unordered_map<void*, DecoderEntry> decoders;

    // Originals
    PFN_AudWaveSlot_OpenUser   origOpenUser      = nullptr;
    PFN_UserTrack_FillBlocks   origFillBlocks    = nullptr;
    PFN_AudRadioSlot_Play      origSlotPlay      = nullptr;
    PFN_AudRadioSlot_Stop      origSlotStop      = nullptr;
    PFN_AudRadioStation_Update origStationUpdate = nullptr;
    PFN_SkipForward            origSkipForward   = nullptr;
    PFN_SkipBackward           origSkipBackward  = nullptr;

    // Station update observation & throttling state
    uint32_t lastObservedTimeInMs = 0u;
    int64_t  lastTimeAdvanceQpc = 0;
    int64_t  lastHeartbeatLogQpc = 0;

    bool     hasPrevState = false;
    uint32_t prevRadioMode = 0xFFFFFFFFu;
    uint8_t  prevTuned = 0xFFu;
    uint8_t  prevActiveIdx = 0xFFu;
    uint32_t prevPauseMask = 0xFFFFFFFFu;
    uint8_t  prevActActive = 0xFFu;
    uint8_t  prevActCat = 0xFFu;
    uint8_t  prevActUser = 0xFFu;
    uint8_t  prevStbyActive = 0xFFu;
    uint8_t  prevStbyCat = 0xFFu;
    uint8_t  prevStbyUser = 0xFFu;

    uint32_t slotTransitionSeq = 0u;
    uint32_t userSlotPlaySeq = 0u;
    uint32_t pendingSkipSeq = 0u;
    uint32_t lastAckTriggerDjSeq = 0u;
    uint32_t lastAppliedSkipAckSeq = 0u;
    uint32_t lastAppliedEscSpliceSeq = 0u;
    uint8_t  shortenedSlotMask = 0u;
    uint64_t lastDecodedHeadFrame = 0ULL;
    uint64_t prerollFillCount = 0ULL;
    uint32_t blockFramesTotal = 0u;
    int64_t  lastSteadyFillQpc = 0;

    void*    userStationPtr = nullptr;
    void*    activeWaveSlotPtr = nullptr;
    void*    activeDecoderPtr = nullptr;
    void*    liveConsumerDecoderPtr = nullptr;
    uint8_t  stationTuned = 0u;

    int64_t  lastNextKeyQpc = 0;
    int64_t  lastPrevKeyQpc = 0;

    HANDLE   heartbeatStopEvent = nullptr;
    HANDLE   heartbeatThread    = nullptr;
};

PluginRuntime g_rt;

int64_t QueryQpcNow() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

DWORD WINAPI HeartbeatThreadProc(LPVOID /*param*/) {
    while (g_rt.heartbeatStopEvent &&
           WaitForSingleObject(g_rt.heartbeatStopEvent, 200u) == WAIT_TIMEOUT) {
        RadioBridgeCtrlBlock* ctrl = g_rt.ctrl;
        if (!ctrl || AtomicLoad32(&ctrl->game_pid) == 0u) {
            continue;
        }
        const uint8_t g12845C8 = g_rt.targets.globalPause12845C8
            ? *reinterpret_cast<const uint8_t*>(g_rt.targets.globalPause12845C8)
            : 0u;
        const uint8_t g115DBFC = g_rt.targets.globalAudioMute115DBFC
            ? *reinterpret_cast<const uint8_t*>(g_rt.targets.globalAudioMute115DBFC)
            : 0u;
        const uint32_t escPaused = (g12845C8 != 0u && g115DBFC != 0u) ? 1u : 0u;
        AtomicStore32(&ctrl->esc_paused, escPaused);
        AtomicStoreI64(&ctrl->game_heartbeat_qpc, QueryQpcNow());
        if (escPaused != 0u &&
            !g_rt.cfg.observeOnly &&
            g_rt.consumer &&
            AtomicLoad32(&ctrl->station_tuned) != 0u &&
            AtomicLoad32(&ctrl->active_slot_is_user) != 0u) {
            g_rt.consumer->PreserveUnreadTailIfApproachingLap();
        }
    }
    return 0u;
}

int64_t QpcDeltaToMs(int64_t deltaTicks) noexcept {
    if (g_rt.qpcFreq.QuadPart <= 0 || deltaTicks <= 0) {
        return 0;
    }
    const int64_t sec = deltaTicks / g_rt.qpcFreq.QuadPart;
    const int64_t rem = deltaTicks % g_rt.qpcFreq.QuadPart;
    return sec * 1000LL + (rem * 1000LL) / g_rt.qpcFreq.QuadPart;
}

bool OpenOrCreateControlShm(const RouteBIniConfig& cfg) {
    if (cfg.ctrlMappingName.empty()) {
        return false;
    }
    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     0u, kCtrlBytes, cfg.ctrlMappingName.c_str());
    if (!hMap) {
        BridgeLogger::Instance().LogText("[WARN] CreateFileMappingW('%s') failed gle=%lu",
                                         BridgeLogger::Instance().WideToUtf8Escaped(cfg.ctrlMappingName.c_str()).c_str(),
                                         static_cast<unsigned long>(GetLastError()));
        return false;
    }
    const bool created = (GetLastError() != ERROR_ALREADY_EXISTS);
    void* view = MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0u, 0u, kCtrlBytes);
    if (!view) {
        BridgeLogger::Instance().LogText("[WARN] MapViewOfFile ctrl SHM failed gle=%lu",
                                         static_cast<unsigned long>(GetLastError()));
        CloseHandle(hMap);
        return false;
    }

    auto* blk = static_cast<RadioBridgeCtrlBlock*>(view);
    const uint32_t desiredFlags = cfg.observeOnly ? kCtrlFlagObserveOnly : 0u;
    if (created || AtomicLoad32(&blk->magic) != kCtrlMagic || AtomicLoad32(&blk->total_bytes) != kCtrlBytes) {
        std::memset(blk, 0, sizeof(*blk));
        AtomicStoreU64(&blk->qpc_freq, static_cast<uint64_t>(g_rt.qpcFreq.QuadPart));
        AtomicStore32(&blk->version, kCtrlVersion);
        AtomicStore32(&blk->total_bytes, kCtrlBytes);
        AtomicStore32(&blk->flags, desiredFlags);
        AtomicStore32(&blk->magic, kCtrlMagic);
    } else {
        AtomicStore32(&blk->flags, desiredFlags);
    }

    g_rt.hCtrlMapping = hMap;
    g_rt.ctrl = blk;
    BridgeLogger::Instance().LogText("Control SHM ready (%s, created=%d, observeOnly=%d)",
                                     BridgeLogger::Instance().WideToUtf8Escaped(cfg.ctrlMappingName.c_str()).c_str(),
                                     created ? 1 : 0, cfg.observeOnly ? 1 : 0);
    return true;
}

void CloseControlShm() noexcept {
    if (g_rt.ctrl) {
        AtomicStore32(&g_rt.ctrl->game_pid, 0u);
        AtomicStoreI64(&g_rt.ctrl->game_heartbeat_qpc, 0LL);
        UnmapViewOfFile(g_rt.ctrl);
        g_rt.ctrl = nullptr;
    }
    if (g_rt.hCtrlMapping) {
        CloseHandle(g_rt.hCtrlMapping);
        g_rt.hCtrlMapping = nullptr;
    }
}

void ClearPreloadedByDecoderLocked(void* decoderPtr) {
    if (!decoderPtr) {
        return;
    }
    auto it = g_rt.decoders.find(decoderPtr);
    if (it != g_rt.decoders.end()) {
        it->second.preloadedReady = false;
        it->second.block0 = nullptr;
        it->second.block1 = nullptr;
        it->second.blockBytes = 0u;
    }
}

bool HasAnyPreloadedStandbyLocked() noexcept {
    for (const auto& kv : g_rt.decoders) {
        if (kv.second.isTarget && kv.second.preloadedReady && kv.second.block0 && kv.second.block1) {
            return true;
        }
    }
    return false;
}

bool IsUserStationSlotLocked(const void* slotPtr) noexcept {
    if (!slotPtr || !g_rt.userStationPtr) {
        return false;
    }
    const auto* stationBytes = static_cast<const uint8_t*>(g_rt.userStationPtr);
    return (slotPtr == stationBytes) || (slotPtr == (stationBytes + kOffStationSlotStride));
}

// ============================================================================
// Hook Detours (Win32 x86 __fastcall wrapping __thiscall)
// ============================================================================

void __fastcall Detour_AudWaveSlot_OpenUser(void* waveSlot, void* /*edx*/, const wchar_t* widePath) {
    g_rt.origOpenUser(waveSlot, widePath);

    void* decoderPtr = nullptr;
    if (waveSlot) {
        const auto* ws = static_cast<const uint8_t*>(waveSlot);
        decoderPtr = *reinterpret_cast<void* const*>(ws + kOffWaveSlotDecoder);
    }

    const wchar_t* leaf = ExtractLeafWide(widePath);
    const std::string leafUtf8 = BridgeLogger::WideToUtf8Escaped(leaf);
    const bool isTarget = IsTargetCarrierLeaf(g_rt.cfg, widePath);

    if (decoderPtr && g_rt.csInitialized) {
        EnterCriticalSection(&g_rt.stateCs);
        DecoderEntry& entry = g_rt.decoders[decoderPtr];
        entry.waveSlot = waveSlot;
        entry.isTarget = isTarget;
        entry.leafUtf8 = leafUtf8;
        entry.block0 = nullptr;
        entry.block1 = nullptr;
        entry.blockBytes = 0u;
        entry.preloadedReady = false;
        entry.steadyCallCount = 0u;
        LeaveCriticalSection(&g_rt.stateCs);
    }

    BridgeLogger::Instance().LogJson(
        "WaveSlot_OpenUser",
        "\"wave_slot\":\"%p\",\"decoder\":\"%p\",\"is_target\":%s,\"leaf\":\"%s\"",
        waveSlot, decoderPtr, isTarget ? "true" : "false", leafUtf8.c_str());
}

uint8_t __fastcall Detour_UserTrack_FillBlocks(void* decoderPtr,
                                               void* /*edx*/,
                                               void* block0,
                                               void* block1,
                                               uint32_t blockBytes) {
    bool isTarget = false;
    bool logSteady = false;
    bool isActiveTuneIn = false;
    bool isStaleOutgoingDecoder = false;
    uint32_t steadyIdx = 0u;
    std::string leafUtf8;

    const bool isAsync = decoderPtr &&
        (*reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(decoderPtr) + kOffDecoderAsyncFlag) != 0u);
    const bool isPreload = (block1 != nullptr);
    const uint32_t framesPerBlock = ComputeBlockFrameCount(blockBytes);

    if (decoderPtr && g_rt.csInitialized) {
        EnterCriticalSection(&g_rt.stateCs);
        auto it = g_rt.decoders.find(decoderPtr);
        if (it != g_rt.decoders.end()) {
            DecoderEntry& e = it->second;
            isTarget = e.isTarget;
            leafUtf8 = e.leafUtf8;
            if (isPreload && isAsync) {
                isActiveTuneIn = (g_rt.stationTuned != 0u) &&
                    ((decoderPtr == g_rt.activeDecoderPtr) ||
                     (e.waveSlot != nullptr && e.waveSlot == g_rt.activeWaveSlotPtr));
                e.block0 = block0;
                e.block1 = block1;
                e.blockBytes = blockBytes;
                e.preloadedReady = !isActiveTuneIn;
                e.steadyCallCount = 0u;
                g_rt.blockFramesTotal = framesPerBlock * 2u;
                if (isActiveTuneIn && !g_rt.cfg.observeOnly && isTarget) {
                    g_rt.liveConsumerDecoderPtr = decoderPtr;
                }
            } else if (!isPreload && isAsync) {
                ++e.steadyCallCount;
                steadyIdx = e.steadyCallCount;
                logSteady = (steadyIdx == 1u);
                if (!g_rt.cfg.observeOnly && isTarget) {
                    if (g_rt.liveConsumerDecoderPtr != nullptr && g_rt.liveConsumerDecoderPtr != decoderPtr) {
                        isStaleOutgoingDecoder = true;
                    } else if (g_rt.liveConsumerDecoderPtr == nullptr) {
                        g_rt.liveConsumerDecoderPtr = decoderPtr;
                    }
                }
            }
        }
        LeaveCriticalSection(&g_rt.stateCs);
    }

    if (isPreload) {
        BridgeLogger::Instance().LogJson(
            "FillBlocks_Preload",
            "\"decoder\":\"%p\",\"block0\":\"%p\",\"block1\":\"%p\",\"block_bytes\":%u,\"frames_per_block\":%u,\"is_async\":%s,\"is_target\":%s,\"active_tune_in\":%s,\"observe_only\":%s,\"leaf\":\"%s\"",
            decoderPtr, block0, block1,
            static_cast<unsigned>(blockBytes),
            static_cast<unsigned>(framesPerBlock),
            isAsync ? "true" : "false",
            isTarget ? "true" : "false",
            isActiveTuneIn ? "true" : "false",
            g_rt.cfg.observeOnly ? "true" : "false",
            leafUtf8.c_str());
    } else if (logSteady) {
        BridgeLogger::Instance().LogJson(
            "FillBlocks_FirstSteady",
            "\"decoder\":\"%p\",\"block0\":\"%p\",\"block_bytes\":%u,\"frames_per_block\":%u,\"is_target\":%s,\"observe_only\":%s,\"leaf\":\"%s\"",
            decoderPtr, block0,
            static_cast<unsigned>(blockBytes),
            static_cast<unsigned>(framesPerBlock),
            isTarget ? "true" : "false",
            g_rt.cfg.observeOnly ? "true" : "false",
            leafUtf8.c_str());
    }

    if (g_rt.cfg.observeOnly || !isTarget || !isAsync) {
        return g_rt.origFillBlocks(decoderPtr, block0, block1, blockBytes);
    }

    if (isPreload) {
        if (isActiveTuneIn && g_rt.consumer) {
            uint64_t endFrame = 0ULL;
            const uint32_t reserveForBurst2 = framesPerBlock + 96000u;
            const RouteBPullStatus st = g_rt.consumer->FillPreloadedBlocksAtPlay(
                block0, block1, blockBytes, false, 0ULL, reserveForBurst2, &endFrame, 0u);
            if (st.ok && g_rt.csInitialized) {
                const int64_t fillQpc = QueryQpcNow();
                EnterCriticalSection(&g_rt.stateCs);
                g_rt.lastDecodedHeadFrame = endFrame;
                g_rt.lastSteadyFillQpc = fillQpc;
                LeaveCriticalSection(&g_rt.stateCs);
            }
        } else {
            ZeroPlanarBlock(block0, blockBytes);
            ZeroPlanarBlock(block1, blockBytes);
        }
        return 1u;
    }

    if (isStaleOutgoingDecoder) {
        ZeroPlanarBlock(block0, blockBytes);
    } else if (g_rt.consumer) {
        uint64_t endFrame = 0ULL;
        const RouteBPullStatus st = g_rt.consumer->FillSteadyPlanarBlock(block0, blockBytes, &endFrame);
        if (st.ok && g_rt.csInitialized) {
            const int64_t fillQpc = QueryQpcNow();
            EnterCriticalSection(&g_rt.stateCs);
            g_rt.lastDecodedHeadFrame = endFrame;
            g_rt.lastSteadyFillQpc = fillQpc;
            LeaveCriticalSection(&g_rt.stateCs);
        }
    } else {
        ZeroPlanarBlock(block0, blockBytes);
    }
    return 1u;
}

void __fastcall Detour_AudRadioSlot_Play(void* slotPtr, void* /*edx*/) {
    if (!slotPtr) {
        g_rt.origSlotPlay(slotPtr);
        return;
    }

    bool isUserStationSlot = false;
    if (g_rt.csInitialized) {
        EnterCriticalSection(&g_rt.stateCs);
        isUserStationSlot = IsUserStationSlotLocked(slotPtr);
        LeaveCriticalSection(&g_rt.stateCs);
    }
    if (!isUserStationSlot) {
        g_rt.origSlotPlay(slotPtr);
        return;
    }

    const auto* slotBytes = static_cast<const uint8_t*>(slotPtr);
    const uint8_t category   = *reinterpret_cast<const uint8_t*>(slotBytes + kOffSlotCategory);
    const uint8_t isUserFlag = *reinterpret_cast<const uint8_t*>(slotBytes + kOffSlotIsUserTrack);
    const bool isUser        = (category == kSlotCategoryUserTrack) && (isUserFlag != 0);
    const int32_t playMs     = *reinterpret_cast<const int32_t*>(slotBytes + kOffSlotPlayTimeMs);
    const int32_t durMs      = *reinterpret_cast<const int32_t*>(slotBytes + kOffSlotDurationMs);
    const int32_t outroMs    = *reinterpret_cast<const int32_t*>(slotBytes + kOffSlotOutroMs);
    void* waveSlot           = *reinterpret_cast<void* const*>(slotBytes + kOffSlotWaveSlot);
    void* decoderPtr         = waveSlot
        ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(waveSlot) + kOffWaveSlotDecoder)
        : nullptr;

    void* block0 = nullptr;
    void* block1 = nullptr;
    uint32_t blockBytes = 0u;
    bool hasPreload = false;
    bool isTarget   = false;
    std::string leafUtf8;

    if (decoderPtr && g_rt.csInitialized) {
        EnterCriticalSection(&g_rt.stateCs);
        ++g_rt.slotTransitionSeq;
        if (isUser) {
            ++g_rt.userSlotPlaySeq;
        }
        auto it = g_rt.decoders.find(decoderPtr);
        if (it != g_rt.decoders.end()) {
            DecoderEntry& e = it->second;
            isTarget   = e.isTarget;
            leafUtf8   = e.leafUtf8;
            hasPreload = e.preloadedReady && e.block0 && e.block1 && (e.blockBytes > 0u);
            block0     = e.block0;
            block1     = e.block1;
            blockBytes = e.blockBytes;
            if (!g_rt.cfg.observeOnly && isUser && isTarget) {
                g_rt.liveConsumerDecoderPtr = decoderPtr;
                if (hasPreload) {
                    e.preloadedReady = false; // consumed by Play() prefill
                }
            }
        }
        LeaveCriticalSection(&g_rt.stateCs);
    } else if (g_rt.csInitialized) {
        EnterCriticalSection(&g_rt.stateCs);
        ++g_rt.slotTransitionSeq;
        if (isUser) {
            ++g_rt.userSlotPlaySeq;
        }
        LeaveCriticalSection(&g_rt.stateCs);
    }

    bool prefilled = false;
    uint32_t prefillLiveFrames = 0u;
    uint32_t loggedMinHeadPad = 0u;
    if (!g_rt.cfg.observeOnly && isUser && isTarget && hasPreload && g_rt.consumer) {
        bool hasPrerollAnchor = false;
        uint64_t prerollStartFrame = 0ULL;
        uint32_t bridgeState = 0u;
        if (g_rt.ctrl) {
            bridgeState = AtomicLoad32(&g_rt.ctrl->bridge_state);
            if (AtomicLoad32(&g_rt.ctrl->preroll_active) != 0u) {
                hasPrerollAnchor  = true;
                prerollStartFrame = AtomicLoadU64(&g_rt.ctrl->preroll_start_frame);
            }
        }
        const uint32_t reserveForBurst2 = ComputeBlockFrameCount(blockBytes) + 48000u;
        const uint32_t minHeadPad = hasPrerollAnchor
            ? ((bridgeState == static_cast<uint32_t>(BridgeState::PreRollingForDj)) ? 43200u : 14400u)
            : 0u;
        loggedMinHeadPad = minHeadPad;
        uint64_t endFrame = 0ULL;
        const RouteBPullStatus st = g_rt.consumer->FillPreloadedBlocksAtPlay(
            block0, block1, blockBytes, hasPrerollAnchor, prerollStartFrame, reserveForBurst2, &endFrame, minHeadPad);
        if (st.ok && g_rt.csInitialized) {
            prefilled = true;
            prefillLiveFrames = st.liveFramesDelivered;
            const int64_t fillQpc = QueryQpcNow();
            EnterCriticalSection(&g_rt.stateCs);
            ++g_rt.prerollFillCount;
            g_rt.lastDecodedHeadFrame = endFrame;
            g_rt.lastSteadyFillQpc = fillQpc;
            LeaveCriticalSection(&g_rt.stateCs);
        }
    }

    BridgeLogger::Instance().LogJson(
        "RadioSlot_Play",
        "\"slot\":\"%p\",\"cat\":%u,\"is_user\":%s,\"is_target\":%s,\"play_ms\":%d,\"dur_ms\":%d,\"outro_ms\":%d,\"has_preload\":%s,\"prefilled\":%s,\"prefill_live\":%u,\"min_head_pad\":%u,\"leaf\":\"%s\"",
        slotPtr,
        static_cast<unsigned>(category),
        isUser ? "true" : "false",
        isTarget ? "true" : "false",
        playMs, durMs, outroMs,
        hasPreload ? "true" : "false",
        prefilled ? "true" : "false",
        static_cast<unsigned>(prefillLiveFrames),
        static_cast<unsigned>(loggedMinHeadPad),
        leafUtf8.c_str());

    g_rt.origSlotPlay(slotPtr);
}

void __fastcall Detour_AudRadioSlot_Stop(void* slotPtr, void* /*edx*/) {
    if (slotPtr) {
        bool isUserStationSlot = false;
        if (g_rt.csInitialized) {
            EnterCriticalSection(&g_rt.stateCs);
            isUserStationSlot = IsUserStationSlotLocked(slotPtr);
            LeaveCriticalSection(&g_rt.stateCs);
        }
        if (isUserStationSlot) {
            const auto* slotBytes = static_cast<const uint8_t*>(slotPtr);
            const uint8_t category   = *reinterpret_cast<const uint8_t*>(slotBytes + kOffSlotCategory);
            const uint8_t isUserFlag = *reinterpret_cast<const uint8_t*>(slotBytes + kOffSlotIsUserTrack);
            const int32_t playMs     = *reinterpret_cast<const int32_t*>(slotBytes + kOffSlotPlayTimeMs);
            const int32_t durMs      = *reinterpret_cast<const int32_t*>(slotBytes + kOffSlotDurationMs);
            void* waveSlot           = *reinterpret_cast<void* const*>(slotBytes + kOffSlotWaveSlot);
            void* decoderPtr         = waveSlot
                ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(waveSlot) + kOffWaveSlotDecoder)
                : nullptr;

            if (g_rt.csInitialized) {
                EnterCriticalSection(&g_rt.stateCs);
                if (g_rt.userStationPtr != nullptr) {
                    const auto* stBytes = static_cast<const uint8_t*>(g_rt.userStationPtr);
                    if (slotPtr == stBytes) {
                        g_rt.shortenedSlotMask &= static_cast<uint8_t>(~1u);
                    } else if (slotPtr == (stBytes + kOffStationSlotStride)) {
                        g_rt.shortenedSlotMask &= static_cast<uint8_t>(~2u);
                    }
                }
                if (decoderPtr) {
                    if (g_rt.liveConsumerDecoderPtr == decoderPtr) {
                        g_rt.liveConsumerDecoderPtr = nullptr;
                    }
                    ClearPreloadedByDecoderLocked(decoderPtr);
                }
                LeaveCriticalSection(&g_rt.stateCs);
            }

            BridgeLogger::Instance().LogJson(
                "RadioSlot_Stop",
                "\"slot\":\"%p\",\"cat\":%u,\"is_user\":%u,\"play_ms\":%d,\"dur_ms\":%d,\"decoder\":\"%p\"",
                slotPtr,
                static_cast<unsigned>(category),
                static_cast<unsigned>(isUserFlag),
                playMs, durMs, decoderPtr);
        }
    }

    g_rt.origSlotStop(slotPtr);
}

void __fastcall Detour_AudRadioStation_Update(void* stationPtr, void* /*edx*/, uint32_t timeInMs) {
    bool trackedUserStation = false;
    if (stationPtr) {
        auto* stationBytes = static_cast<uint8_t*>(stationPtr);
        const uint8_t isUserStation = *reinterpret_cast<const uint8_t*>(stationBytes + kOffStationIsUserStation);

        if (isUserStation != 0u) {
            trackedUserStation = true;
            const int64_t nowQpc = QueryQpcNow();
            const uint8_t activeIdx     = (*reinterpret_cast<const uint8_t*>(stationBytes + kOffStationActiveSlotIdx)) & 1u;
            const uint8_t standbyIdx    = activeIdx ^ 1u;
            const uint8_t tuned         = *reinterpret_cast<const uint8_t*>(stationBytes + kOffStationTuned);
            const uint8_t stationCrossfading = *reinterpret_cast<const uint8_t*>(stationBytes + kOffStationCrossfading);
            const uint8_t stationPaused = *reinterpret_cast<const uint8_t*>(stationBytes + kOffStationPausedFlag);

            SlotSnapshot act  = ReadSlotSnapshot(stationBytes, activeIdx);
            const SlotSnapshot stby = ReadSlotSnapshot(stationBytes, standbyIdx);

            const uint32_t radioMode = g_rt.targets.globalRadioMode
                ? *reinterpret_cast<const uint32_t*>(g_rt.targets.globalRadioMode)
                : 0xFFFFFFFFu;
            const uint8_t g12845C8 = g_rt.targets.globalPause12845C8
                ? *reinterpret_cast<const uint8_t*>(g_rt.targets.globalPause12845C8)
                : 0u;
            const uint8_t g12845C9 = g_rt.targets.globalPause12845C9
                ? *reinterpret_cast<const uint8_t*>(g_rt.targets.globalPause12845C9)
                : 0u;
            const uint8_t g115DBFC = g_rt.targets.globalAudioMute115DBFC
                ? *reinterpret_cast<const uint8_t*>(g_rt.targets.globalAudioMute115DBFC)
                : 0u;
            const uint32_t audioTimeMs = g_rt.targets.globalAudioTimeMs
                ? *reinterpret_cast<const uint32_t*>(g_rt.targets.globalAudioTimeMs)
                : 0u;

            bool timeFrozen = false;
            bool stateChanged = false;
            bool logHeartbeat = false;
            bool logDurationShortened = false;
            int32_t shortenedOldDur = 0;
            int32_t shortenedNewDur = 0;
            int32_t shortenedPlayMs = 0;
            bool logSkipJump = false;
            uint32_t loggedSkipAckSeq = 0u;
            uint64_t loggedSkipFrame = 0ULL;
            bool logEscGap = false;
            uint32_t loggedEscSeq = 0u;
            uint64_t loggedEscStart = 0ULL;
            uint64_t loggedEscEnd = 0ULL;
            bool needStopShortenedSlot = false;
            int32_t stoppedSlotPlayMs = 0;
            int32_t stoppedSlotDurMs = 0;
            bool setSongEndBarrier = false;
            uint64_t songEndBarrierFrame = 0ULL;

            uint32_t pauseMask = 0u;
            uint32_t standbyPreloadReady = 0u;
            uint32_t transitionSeqSnap = 0u;
            uint32_t userPlaySeqSnap = 0u;
            uint32_t skipSeqSnap = 0u;
            uint32_t ackDjSeqSnap = 0u;
            uint64_t decodedHeadSnap = 0ULL;
            uint64_t prerollFillSnap = 0ULL;
            uint32_t blockFramesSnap = 0u;

            if (g12845C8 != 0u)      pauseMask |= kPauseBit12845C8;
            if (g12845C9 != 0u)      pauseMask |= kPauseBit12845C9;
            if (g115DBFC != 0u)      pauseMask |= kPauseBit115DBFC;
            if (stationPaused != 0u) pauseMask |= kPauseBitStation1927;

            if (g_rt.csInitialized) {
                EnterCriticalSection(&g_rt.stateCs);

                g_rt.userStationPtr    = stationPtr;
                g_rt.activeWaveSlotPtr = act.waveSlot;
                g_rt.activeDecoderPtr  = act.decoder;
                g_rt.stationTuned      = tuned;

                if (g_rt.lastTimeAdvanceQpc == 0 || timeInMs != g_rt.lastObservedTimeInMs) {
                    g_rt.lastObservedTimeInMs = timeInMs;
                    g_rt.lastTimeAdvanceQpc = nowQpc;
                } else if (tuned != 0u && QpcDeltaToMs(nowQpc - g_rt.lastTimeAdvanceQpc) >= 100LL) {
                    timeFrozen = true;
                }
                if (timeFrozen) {
                    pauseMask |= kPauseBitTimeInMsFrozen;
                }

                if (!act.active || !act.isUser) {
                    g_rt.shortenedSlotMask &= static_cast<uint8_t>(~(1u << activeIdx));
                }

                // Calibrated dynamic song-end trigger for live mode (ObserveOnly=0)
                if (!g_rt.cfg.observeOnly && radioMode == 0u && g_rt.ctrl) {
                    const uint32_t reqSeq = AtomicLoad32(&g_rt.ctrl->request_trigger_dj_seq);
                    if (reqSeq != 0u && reqSeq != g_rt.lastAckTriggerDjSeq && act.active && act.isUser) {
                        const uint64_t endCommit = AtomicLoadU64(&g_rt.ctrl->song_end_commit_frame);
                        if (endCommit > 0ULL) {
                            setSongEndBarrier = true;
                            songEndBarrierFrame = endCommit;
                        }
                        if (act.durationMs > 200000) {
                            const int64_t elapsedSinceFillMs = (g_rt.lastSteadyFillQpc > 0)
                                ? std::clamp<int64_t>(QpcDeltaToMs(nowQpc - g_rt.lastSteadyFillQpc), 0LL, 3000LL)
                                : 1300LL;
                            const int64_t audWaveSlotRemMs = std::max<int64_t>(0LL, 5291LL - elapsedSinceFillMs);
                            const int64_t ringTailMs = (endCommit > g_rt.lastDecodedHeadFrame)
                                ? std::min<int64_t>(15000LL,
                                    static_cast<int64_t>(((endCommit - g_rt.lastDecodedHeadFrame) * 1000ULL) / stagec::kSampleRate))
                                : 0LL;
                            const int64_t totalRemMs = std::clamp<int64_t>(audWaveSlotRemMs + ringTailMs, 0LL, 15000LL);
                            const int32_t validOutroMs = (act.outroMs >= 0 && act.outroMs <= 30000) ? act.outroMs : 0;
                            const int32_t targetDurMs = act.playMs + static_cast<int32_t>(totalRemMs) + validOutroMs + 1000;
                            if (targetDurMs < act.durationMs) {
                                auto* durPtr = reinterpret_cast<int32_t*>(
                                    stationBytes + activeIdx * kOffStationSlotStride + kOffSlotDurationMs);
                                shortenedOldDur = act.durationMs;
                                shortenedNewDur = targetDurMs;
                                shortenedPlayMs = act.playMs;
                                *durPtr = targetDurMs;
                                act.durationMs = targetDurMs;
                                g_rt.shortenedSlotMask |= static_cast<uint8_t>(1u << activeIdx);
                                logDurationShortened = true;
                            }
                        }
                        g_rt.lastAckTriggerDjSeq = reqSeq;
                    }

                    const uint32_t skipAckSeq = AtomicLoad32(&g_rt.ctrl->request_skip_ack_seq);
                    if (skipAckSeq != 0u && skipAckSeq != g_rt.lastAppliedSkipAckSeq) {
                        const uint64_t skipFrame = AtomicLoadU64(&g_rt.ctrl->skip_audio_ready_frame);
                        if (skipFrame > 0ULL && g_rt.consumer) {
                            g_rt.consumer->JumpReadCursorToFrame(skipFrame);
                            logSkipJump = true;
                            loggedSkipAckSeq = skipAckSeq;
                            loggedSkipFrame = skipFrame;
                        }
                        g_rt.lastAppliedSkipAckSeq = skipAckSeq;
                    }

                    const uint32_t escSeq = AtomicLoad32(&g_rt.ctrl->esc_splice_seq);
                    if (escSeq != 0u && escSeq != g_rt.lastAppliedEscSpliceSeq) {
                        const uint64_t escStart = AtomicLoadU64(&g_rt.ctrl->esc_splice_start_frame);
                        const uint64_t escEnd   = AtomicLoadU64(&g_rt.ctrl->esc_splice_end_frame);
                        if (escEnd > escStart && g_rt.consumer) {
                            g_rt.consumer->QueueSkipGap(escStart, escEnd);
                            logEscGap = true;
                            loggedEscSeq = escSeq;
                            loggedEscStart = escStart;
                            loggedEscEnd = escEnd;
                        }
                        g_rt.lastAppliedEscSpliceSeq = escSeq;
                    }

                    if (act.active && act.isUser && (stationCrossfading != 0u) &&
                        ((g_rt.shortenedSlotMask & static_cast<uint8_t>(1u << activeIdx)) != 0u) &&
                        act.playMs >= act.durationMs) {
                        needStopShortenedSlot = true;
                        stoppedSlotPlayMs = act.playMs;
                        stoppedSlotDurMs  = act.durationMs;
                        g_rt.shortenedSlotMask &= static_cast<uint8_t>(~(1u << activeIdx));
                        if (act.decoder) {
                            ClearPreloadedByDecoderLocked(act.decoder);
                        }
                    }
                }

                standbyPreloadReady = HasAnyPreloadedStandbyLocked() ? 1u : 0u;
                transitionSeqSnap   = g_rt.slotTransitionSeq;
                userPlaySeqSnap     = g_rt.userSlotPlaySeq;
                skipSeqSnap         = g_rt.pendingSkipSeq;
                ackDjSeqSnap        = g_rt.lastAckTriggerDjSeq;
                decodedHeadSnap     = g_rt.lastDecodedHeadFrame;
                prerollFillSnap     = g_rt.prerollFillCount;
                blockFramesSnap     = g_rt.blockFramesTotal;

                stateChanged = !g_rt.hasPrevState ||
                    (radioMode != g_rt.prevRadioMode) ||
                    (tuned != g_rt.prevTuned) ||
                    (activeIdx != g_rt.prevActiveIdx) ||
                    (pauseMask != g_rt.prevPauseMask) ||
                    ((act.active ? 1u : 0u) != g_rt.prevActActive) ||
                    (act.category != g_rt.prevActCat) ||
                    ((act.isUser ? 1u : 0u) != g_rt.prevActUser) ||
                    ((stby.active ? 1u : 0u) != g_rt.prevStbyActive) ||
                    (stby.category != g_rt.prevStbyCat) ||
                    ((stby.isUser ? 1u : 0u) != g_rt.prevStbyUser);

                if (stateChanged) {
                    g_rt.hasPrevState   = true;
                    g_rt.prevRadioMode  = radioMode;
                    g_rt.prevTuned      = tuned;
                    g_rt.prevActiveIdx  = activeIdx;
                    g_rt.prevPauseMask  = pauseMask;
                    g_rt.prevActActive  = act.active ? 1u : 0u;
                    g_rt.prevActCat     = act.category;
                    g_rt.prevActUser    = act.isUser ? 1u : 0u;
                    g_rt.prevStbyActive = stby.active ? 1u : 0u;
                    g_rt.prevStbyCat    = stby.category;
                    g_rt.prevStbyUser   = stby.isUser ? 1u : 0u;
                }

                const int64_t hbIntervalMs = (tuned != 0u) ? 1000LL : 5000LL;
                if (g_rt.lastHeartbeatLogQpc == 0 || QpcDeltaToMs(nowQpc - g_rt.lastHeartbeatLogQpc) >= hbIntervalMs) {
                    g_rt.lastHeartbeatLogQpc = nowQpc;
                    logHeartbeat = true;
                }

                LeaveCriticalSection(&g_rt.stateCs);
            }

            if (g_rt.consumer) {
                if (setSongEndBarrier) {
                    g_rt.consumer->SetSongEndBarrier(songEndBarrierFrame);
                } else if (tuned == 0u || !act.active || !act.isUser || needStopShortenedSlot) {
                    g_rt.consumer->ClearSongEndBarrier();
                }
                g_rt.consumer->SetActiveReaderCount((tuned != 0u && act.active && act.isUser && !needStopShortenedSlot) ? 1u : 0u);
            }

            if (g_rt.ctrl) {
                const uint32_t escPaused = (g12845C8 != 0u && g115DBFC != 0u) ? 1u : 0u;
                AtomicStore32(&g_rt.ctrl->game_pid, GetCurrentProcessId());
                AtomicStore32(&g_rt.ctrl->radio_mode, radioMode);
                AtomicStore32(&g_rt.ctrl->station_tuned, tuned != 0u ? 1u : 0u);
                AtomicStore32(&g_rt.ctrl->esc_paused, escPaused);
                AtomicStore32(&g_rt.ctrl->active_slot_active, act.active ? 1u : 0u);
                AtomicStore32(&g_rt.ctrl->active_slot_is_user, act.isUser ? 1u : 0u);
                AtomicStoreI32(&g_rt.ctrl->active_slot_play_ms, act.playMs);
                AtomicStoreI32(&g_rt.ctrl->active_slot_duration_ms, act.durationMs);
                AtomicStoreI32(&g_rt.ctrl->active_slot_outro_ms, act.outroMs);
                AtomicStore32(&g_rt.ctrl->standby_slot_active, stby.active ? 1u : 0u);
                AtomicStore32(&g_rt.ctrl->standby_slot_is_user, stby.isUser ? 1u : 0u);
                AtomicStore32(&g_rt.ctrl->standby_preloaded_ready, standbyPreloadReady);
                AtomicStore32(&g_rt.ctrl->active_slot_index, activeIdx);
                AtomicStore32(&g_rt.ctrl->slot_transition_seq, transitionSeqSnap);
                AtomicStore32(&g_rt.ctrl->user_slot_play_seq, userPlaySeqSnap);
                AtomicStore32(&g_rt.ctrl->station_crossfading, stationCrossfading != 0u ? 1u : 0u);
                AtomicStore32(&g_rt.ctrl->last_ack_trigger_dj_seq, ackDjSeqSnap);
                AtomicStore32(&g_rt.ctrl->block_frames_total, blockFramesSnap);
                AtomicStore32(&g_rt.ctrl->raw_pause_flags_mask, pauseMask);
                AtomicStore32(&g_rt.ctrl->game_time_ms, timeInMs);
                AtomicStoreU64(&g_rt.ctrl->game_decoded_head_frame, decodedHeadSnap);
                AtomicStoreU64(&g_rt.ctrl->preroll_fill_count, prerollFillSnap);
                AtomicStore32(&g_rt.ctrl->pending_skip_seq, skipSeqSnap);
                AtomicStoreI64(&g_rt.ctrl->game_heartbeat_qpc, nowQpc);
            }

            if (logDurationShortened) {
                BridgeLogger::Instance().LogJson(
                    "RouteB_DurationShortened",
                    "\"play_ms\":%d,\"old_dur_ms\":%d,\"new_dur_ms\":%d,\"ack_seq\":%u",
                    shortenedPlayMs, shortenedOldDur, shortenedNewDur, static_cast<unsigned>(ackDjSeqSnap));
            }
            if (logSkipJump) {
                BridgeLogger::Instance().LogJson(
                    "RouteB_SkipCursorJumped",
                    "\"skip_ack_seq\":%u,\"target_frame\":%llu",
                    static_cast<unsigned>(loggedSkipAckSeq),
                    static_cast<unsigned long long>(loggedSkipFrame));
            }
            if (logEscGap) {
                BridgeLogger::Instance().LogJson(
                    "RouteB_EscGapQueued",
                    "\"esc_seq\":%u,\"start\":%llu,\"end\":%llu",
                    static_cast<unsigned>(loggedEscSeq),
                    static_cast<unsigned long long>(loggedEscStart),
                    static_cast<unsigned long long>(loggedEscEnd));
            }
            if (needStopShortenedSlot) {
                BridgeLogger::Instance().LogJson(
                    "RouteB_ShortenedSlotStopped",
                    "\"slot_idx\":%u,\"play_ms\":%d,\"dur_ms\":%d",
                    static_cast<unsigned>(activeIdx), stoppedSlotPlayMs, stoppedSlotDurMs);
            }

            if (stateChanged) {
                BridgeLogger::Instance().LogJson(
                    "Station_StateTransition",
                    "\"radio_mode\":%u,\"tuned\":%u,\"pause_mask\":%u,\"g12845C8\":%u,\"g12845C9\":%u,\"g115DBFC\":%u,\"st1927\":%u,\"time_frozen\":%s,\"time_ms\":%u,\"audio_ms\":%u,\"act_idx\":%u,\"act_active\":%u,\"act_cat\":%u,\"act_user\":%u,\"act_play_ms\":%d,\"act_dur_ms\":%d,\"stby_active\":%u,\"stby_cat\":%u,\"stby_user\":%u",
                    static_cast<unsigned>(radioMode),
                    static_cast<unsigned>(tuned),
                    static_cast<unsigned>(pauseMask),
                    static_cast<unsigned>(g12845C8),
                    static_cast<unsigned>(g12845C9),
                    static_cast<unsigned>(g115DBFC),
                    static_cast<unsigned>(stationPaused),
                    timeFrozen ? "true" : "false",
                    static_cast<unsigned>(timeInMs),
                    static_cast<unsigned>(audioTimeMs),
                    static_cast<unsigned>(activeIdx),
                    static_cast<unsigned>(act.active ? 1u : 0u),
                    static_cast<unsigned>(act.category),
                    static_cast<unsigned>(act.isUser ? 1u : 0u),
                    act.playMs, act.durationMs,
                    static_cast<unsigned>(stby.active ? 1u : 0u),
                    static_cast<unsigned>(stby.category),
                    static_cast<unsigned>(stby.isUser ? 1u : 0u));
            } else if (logHeartbeat) {
                BridgeLogger::Instance().LogJson(
                    "Station_Heartbeat",
                    "\"radio_mode\":%u,\"tuned\":%u,\"pause_mask\":%u,\"time_ms\":%u,\"audio_ms\":%u,\"act_cat\":%u,\"act_user\":%u,\"act_play_ms\":%d,\"act_dur_ms\":%d,\"stby_cat\":%u,\"stby_user\":%u",
                    static_cast<unsigned>(radioMode),
                    static_cast<unsigned>(tuned),
                    static_cast<unsigned>(pauseMask),
                    static_cast<unsigned>(timeInMs),
                    static_cast<unsigned>(audioTimeMs),
                    static_cast<unsigned>(act.category),
                    static_cast<unsigned>(act.isUser ? 1u : 0u),
                    act.playMs, act.durationMs,
                    static_cast<unsigned>(stby.category),
                    static_cast<unsigned>(stby.isUser ? 1u : 0u));
            }
            if (needStopShortenedSlot && g_rt.origSlotStop) {
                g_rt.origSlotStop(stationBytes + static_cast<size_t>(activeIdx) * kOffStationSlotStride);
            }
        }
    }

    g_rt.origStationUpdate(stationPtr, timeInMs);

    if (trackedUserStation && g_rt.csInitialized) {
        const auto* stationBytes = static_cast<const uint8_t*>(stationPtr);
        const uint8_t activeIdx = (*reinterpret_cast<const uint8_t*>(stationBytes + kOffStationActiveSlotIdx)) & 1u;
        const uint8_t tuned     = *reinterpret_cast<const uint8_t*>(stationBytes + kOffStationTuned);
        const SlotSnapshot act  = ReadSlotSnapshot(stationBytes, activeIdx);
        EnterCriticalSection(&g_rt.stateCs);
        g_rt.activeWaveSlotPtr = act.waveSlot;
        g_rt.activeDecoderPtr  = act.decoder;
        g_rt.stationTuned      = tuned;
        LeaveCriticalSection(&g_rt.stateCs);
    }
}

void HandleSkipInput(void* stationPtr, int32_t dir) {
    if (!stationPtr) {
        if (dir > 0) g_rt.origSkipForward(stationPtr);
        else g_rt.origSkipBackward(stationPtr);
        return;
    }

    const auto* stationBytes = static_cast<const uint8_t*>(stationPtr);
    const uint8_t isUserStation = *reinterpret_cast<const uint8_t*>(stationBytes + kOffStationIsUserStation);
    if (isUserStation == 0u) {
        if (dir > 0) g_rt.origSkipForward(stationPtr);
        else g_rt.origSkipBackward(stationPtr);
        return;
    }

    const uint8_t activeIdx = (*reinterpret_cast<const uint8_t*>(stationBytes + kOffStationActiveSlotIdx)) & 1u;
    const uint8_t standbyIdx = activeIdx ^ 1u;
    const uint8_t stationCrossfading = *reinterpret_cast<const uint8_t*>(stationBytes + kOffStationCrossfading);
    const SlotSnapshot act  = ReadSlotSnapshot(stationBytes, activeIdx);
    const SlotSnapshot stby = ReadSlotSnapshot(stationBytes, standbyIdx);
    const uint32_t radioMode = g_rt.targets.globalRadioMode
        ? *reinterpret_cast<const uint32_t*>(g_rt.targets.globalRadioMode)
        : 0xFFFFFFFFu;
    const bool duringDj = !(act.active && act.isUser) || (stationCrossfading != 0u && stby.active && !stby.isUser);

    if (g_rt.cfg.observeOnly) {
        BridgeLogger::Instance().LogJson(
            "Skip_Input",
            "\"dir\":%d,\"radio_mode\":%u,\"act_active\":%u,\"act_cat\":%u,\"act_user\":%u,\"act_play_ms\":%d,\"observe_only\":true,\"ignored_during_dj\":%s",
            dir, static_cast<unsigned>(radioMode),
            static_cast<unsigned>(act.active ? 1u : 0u),
            static_cast<unsigned>(act.category),
            static_cast<unsigned>(act.isUser ? 1u : 0u),
            act.playMs,
            duringDj ? "true" : "false");
        if (dir > 0) g_rt.origSkipForward(stationPtr);
        else g_rt.origSkipBackward(stationPtr);
        return;
    }

    const bool bridgeActive = (g_rt.ctrl != nullptr) &&
        (AtomicLoad32(&g_rt.ctrl->bridge_enabled) != 0u) &&
        (AtomicLoad32(&g_rt.ctrl->host_pid) != 0u);
    if (radioMode != 0u || !bridgeActive) {
        if (dir > 0) g_rt.origSkipForward(stationPtr);
        else g_rt.origSkipBackward(stationPtr);
        return;
    }

    // Radio Mode live handling: NEVER interrupt DJ / Ads / Weazel News
    if (duringDj) {
        BridgeLogger::Instance().LogJson(
            "Skip_Input",
            "\"dir\":%d,\"radio_mode\":0,\"act_active\":%u,\"act_cat\":%u,\"act_user\":0,\"act_play_ms\":%d,\"observe_only\":false,\"ignored_during_dj\":true",
            dir, static_cast<unsigned>(act.active ? 1u : 0u),
            static_cast<unsigned>(act.category), act.playMs);
        return;
    }

    bool emitted = false;
    uint32_t newSeq = 0u;
    const int64_t nowQpc = QueryQpcNow();
    const int64_t minDebounceMs = (dir > 0) ? 300LL : 150LL;
    if (g_rt.csInitialized) {
        EnterCriticalSection(&g_rt.stateCs);
        int64_t& lastQpc = (dir > 0) ? g_rt.lastNextKeyQpc : g_rt.lastPrevKeyQpc;
        if (lastQpc == 0 || QpcDeltaToMs(nowQpc - lastQpc) >= minDebounceMs) {
            lastQpc = nowQpc;
            ++g_rt.pendingSkipSeq;
            newSeq = g_rt.pendingSkipSeq;
            emitted = true;
        }
        LeaveCriticalSection(&g_rt.stateCs);
    }

    if (emitted && g_rt.ctrl) {
        AtomicStoreI32(&g_rt.ctrl->pending_skip_dir, dir);
        AtomicStore32(&g_rt.ctrl->pending_skip_seq, newSeq);
    }

    BridgeLogger::Instance().LogJson(
        "Skip_Input",
        "\"dir\":%d,\"radio_mode\":0,\"act_cat\":%u,\"act_user\":1,\"act_play_ms\":%d,\"observe_only\":false,\"ignored_during_dj\":false,\"emitted\":%s,\"skip_seq\":%u",
        dir, static_cast<unsigned>(act.category), act.playMs,
        emitted ? "true" : "false", static_cast<unsigned>(newSeq));
}

void __fastcall Detour_SkipForward(void* stationPtr, void* /*edx*/) {
    HandleSkipInput(stationPtr, +1);
}

void __fastcall Detour_SkipBackward(void* stationPtr, void* /*edx*/) {
    HandleSkipInput(stationPtr, -1);
}

bool InstallSingleHook(const ResolvedTargetEntry& entry, LPVOID detour, LPVOID* origOut) {
    const MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(entry.address), detour, origOut);
    BridgeLogger::Instance().LogText("Hook %-24s (va=0x%08lX, rva=0x%08lX) create=%d",
                                     entry.name,
                                     static_cast<unsigned long>(entry.address),
                                     static_cast<unsigned long>(entry.rva),
                                     static_cast<int>(st));
    return st == MH_OK;
}

} // namespace

RouteBIniConfig LoadIniConfig(const std::wstring& iniPath) {
    RouteBIniConfig cfg{};
    cfg.targetFiles = {
        L"IV_Radio_Stream_01.mp3",
        L"IV_Radio_Stream_02.mp3",
        L"IV_Radio_Stream_03.mp3",
    };

    const DWORD attrs = GetFileAttributesW(iniPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return cfg;
    }

    cfg.enabled     = GetPrivateProfileIntW(L"RouteB", L"Enabled", 1, iniPath.c_str()) != 0;
    cfg.observeOnly = GetPrivateProfileIntW(L"RouteB", L"ObserveOnly", 1, iniPath.c_str()) != 0;

    wchar_t buf[1024]{};
    GetPrivateProfileStringW(L"RouteB", L"MappingName", stagec::kDefaultMappingName,
                             buf, static_cast<DWORD>(std::size(buf)), iniPath.c_str());
    std::wstring ringName = TrimWide(buf);
    if (!ringName.empty()) {
        cfg.mappingName = ringName;
    }

    GetPrivateProfileStringW(L"RouteB", L"CtrlMappingName", kDefaultCtrlMappingName,
                             buf, static_cast<DWORD>(std::size(buf)), iniPath.c_str());
    std::wstring ctrlName = TrimWide(buf);
    if (!ctrlName.empty()) {
        cfg.ctrlMappingName = ctrlName;
    }

    GetPrivateProfileStringW(L"RouteB", L"TargetFiles", L"",
                             buf, static_cast<DWORD>(std::size(buf)), iniPath.c_str());
    std::vector<std::wstring> parsed = ParseCsvWide(buf);
    if (!parsed.empty()) {
        cfg.targetFiles = std::move(parsed);
    }

    const UINT lead = GetPrivateProfileIntW(L"RouteB", L"SteadyLeadMs", 5500u, iniPath.c_str());
    cfg.steadyLeadMs = std::clamp<uint32_t>(lead, 500u, 12000u);

    const UINT hb = GetPrivateProfileIntW(L"RouteB", L"HeartbeatTimeoutMs", 500u, iniPath.c_str());
    cfg.heartbeatTimeoutMs = std::clamp<uint32_t>(hb, 100u, 5000u);

    const UINT fade = GetPrivateProfileIntW(L"RouteB", L"FadeMs", 10u, iniPath.c_str());
    cfg.fadeMs = std::clamp<uint32_t>(fade, 0u, 100u);

    return cfg;
}

bool InitializeRouteBPlugin(HMODULE selfModule) {
    const std::wstring moduleDir = GetModuleDirectoryWide(selfModule);
    BridgeLogger::Instance().Init(moduleDir);

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const wchar_t* exeLeaf = ExtractLeafWide(exePath);
    const std::string exeUtf8 = BridgeLogger::WideToUtf8Escaped(exePath);

    BridgeLogger::Instance().LogText("=== IVRadioBridge Route B attached to %s (PID=%lu) ===",
                                     exeUtf8.c_str(), static_cast<unsigned long>(GetCurrentProcessId()));
    BridgeLogger::Instance().LogJson("Bridge_Attach",
                                     "\"exe\":\"%s\",\"pid\":%lu",
                                     exeUtf8.c_str(), static_cast<unsigned long>(GetCurrentProcessId()));

    if (!EqualsCaseInsensitiveWide(exeLeaf, L"GTAIV.exe")) {
        BridgeLogger::Instance().LogText("[INFO] Non-GTAIV host (%s); skipping Route B hooks.", exeUtf8.c_str());
        return false;
    }

    if (GetModuleHandleW(L"IVRadioProbe.asi") != nullptr) {
        BridgeLogger::Instance().LogText("[ERROR] Conflict: IVRadioProbe.asi is also loaded; aborting IVRadioBridge hooks.");
        BridgeLogger::Instance().LogJson("Bridge_InitError", "\"reason\":\"conflict_iv_radio_probe\"");
        return false;
    }

    const std::wstring iniPath = moduleDir + L"IVRadioBridge.ini";
    const RouteBIniConfig cfg  = LoadIniConfig(iniPath);
    const std::string iniUtf8  = BridgeLogger::WideToUtf8Escaped(iniPath.c_str());

    BridgeLogger::Instance().LogJson(
        "RouteB_ConfigLoaded",
        "\"enabled\":%s,\"observe_only\":%s,\"target_count\":%u,\"steady_lead_ms\":%u,\"fade_ms\":%u,\"ini_path\":\"%s\"",
        cfg.enabled ? "true" : "false",
        cfg.observeOnly ? "true" : "false",
        static_cast<unsigned>(cfg.targetFiles.size()),
        static_cast<unsigned>(cfg.steadyLeadMs),
        static_cast<unsigned>(cfg.fadeMs),
        iniUtf8.c_str());

    if (!cfg.enabled) {
        BridgeLogger::Instance().LogText("[INFO] Disabled via IVRadioBridge.ini ([RouteB] Enabled=0).");
        return false;
    }

    HMODULE hGame = GetModuleHandleW(nullptr);
    const uintptr_t gameBase = reinterpret_cast<uintptr_t>(hGame);
    const size_t gameImageSize = GetModuleImageSize(hGame);
    ResolvedTargets targets{};
    if (!ResolveGtaIvTargets(gameBase, gameImageSize, targets)) {
        BridgeLogger::Instance().LogText("[ERROR] Signature verification failed (base=0x%08lX, imageSize=0x%zX). Zero hooks installed.",
                                         static_cast<unsigned long>(gameBase), gameImageSize);
        for (const auto& e : targets.entries) {
            BridgeLogger::Instance().LogText("  Sig %-24s rva=0x%08lX matched=%d",
                                             e.name, static_cast<unsigned long>(e.rva), e.matched ? 1 : 0);
        }
        BridgeLogger::Instance().LogJson("Bridge_InitError",
                                         "\"reason\":\"signature_mismatch\",\"base\":\"0x%08lX\",\"image_size\":%u",
                                         static_cast<unsigned long>(gameBase),
                                         static_cast<unsigned>(gameImageSize));
        return false;
    }

    BridgeLogger::Instance().LogText("[OK] All 8 GTA IV 1.2.0.59 signatures verified (base=0x%08lX, imageSize=0x%zX).",
                                     static_cast<unsigned long>(gameBase), gameImageSize);

    InitializeCriticalSection(&g_rt.stateCs);
    g_rt.csInitialized = true;
    QueryPerformanceFrequency(&g_rt.qpcFreq);
    g_rt.cfg = cfg;
    g_rt.targets = targets;

    g_rt.consumer = std::make_unique<RouteBIpcConsumer>();
    RouteBConsumerConfig ccfg{};
    ccfg.mappingName = cfg.mappingName;
    ccfg.steadyLeadMs = cfg.steadyLeadMs;
    ccfg.heartbeatTimeoutMs = cfg.heartbeatTimeoutMs;
    ccfg.fadeMs = cfg.fadeMs;
    ccfg.allowHotReattach = true;
    g_rt.consumer->Configure(ccfg);

    OpenOrCreateControlShm(cfg);

    if (MH_Initialize() != MH_OK) {
        BridgeLogger::Instance().LogText("[ERROR] MH_Initialize failed.");
        return false;
    }
    g_rt.mhInitialized = true;

    bool allCreated = true;
    allCreated &= InstallSingleHook(targets.entries[static_cast<size_t>(TargetId::AudWaveSlotOpenUser)],
                                    reinterpret_cast<LPVOID>(&Detour_AudWaveSlot_OpenUser),
                                    reinterpret_cast<LPVOID*>(&g_rt.origOpenUser));
    allCreated &= InstallSingleHook(targets.entries[static_cast<size_t>(TargetId::UserTrackFillBlocks)],
                                    reinterpret_cast<LPVOID>(&Detour_UserTrack_FillBlocks),
                                    reinterpret_cast<LPVOID*>(&g_rt.origFillBlocks));
    allCreated &= InstallSingleHook(targets.entries[static_cast<size_t>(TargetId::AudRadioSlotPlay)],
                                    reinterpret_cast<LPVOID>(&Detour_AudRadioSlot_Play),
                                    reinterpret_cast<LPVOID*>(&g_rt.origSlotPlay));
    allCreated &= InstallSingleHook(targets.entries[static_cast<size_t>(TargetId::AudRadioSlotStop)],
                                    reinterpret_cast<LPVOID>(&Detour_AudRadioSlot_Stop),
                                    reinterpret_cast<LPVOID*>(&g_rt.origSlotStop));
    allCreated &= InstallSingleHook(targets.entries[static_cast<size_t>(TargetId::AudRadioStationUpdate)],
                                    reinterpret_cast<LPVOID>(&Detour_AudRadioStation_Update),
                                    reinterpret_cast<LPVOID*>(&g_rt.origStationUpdate));
    allCreated &= InstallSingleHook(targets.entries[static_cast<size_t>(TargetId::SkipForward)],
                                    reinterpret_cast<LPVOID>(&Detour_SkipForward),
                                    reinterpret_cast<LPVOID*>(&g_rt.origSkipForward));
    allCreated &= InstallSingleHook(targets.entries[static_cast<size_t>(TargetId::SkipBackward)],
                                    reinterpret_cast<LPVOID>(&Detour_SkipBackward),
                                    reinterpret_cast<LPVOID*>(&g_rt.origSkipBackward));

    if (!allCreated) {
        BridgeLogger::Instance().LogText("[ERROR] One or more MinHook hooks failed creation; aborting without enabling.");
        MH_Uninitialize();
        g_rt.mhInitialized = false;
        return false;
    }

    const MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    g_rt.hooksInstalled = (en == MH_OK);
    if (g_rt.hooksInstalled && g_rt.ctrl != nullptr) {
        g_rt.heartbeatStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_rt.heartbeatStopEvent != nullptr) {
            g_rt.heartbeatThread = CreateThread(nullptr, 0, &HeartbeatThreadProc, nullptr, 0, nullptr);
            if (!g_rt.heartbeatThread) {
                CloseHandle(g_rt.heartbeatStopEvent);
                g_rt.heartbeatStopEvent = nullptr;
            }
        }
    }
    BridgeLogger::Instance().LogText("MH_EnableHook(MH_ALL_HOOKS) status=%d (observeOnly=%d)",
                                     static_cast<int>(en), cfg.observeOnly ? 1 : 0);
    BridgeLogger::Instance().LogJson("Bridge_Initialized",
                                     "\"mh_enable\":%d,\"observe_only\":%s,\"base\":\"0x%08lX\"",
                                     static_cast<int>(en),
                                     cfg.observeOnly ? "true" : "false",
                                     static_cast<unsigned long>(gameBase));
    return g_rt.hooksInstalled;
}

void ShutdownRouteBPlugin(bool processTerminating) {
    BridgeLogger::Instance().LogText("=== IVRadioBridge detaching (terminating=%d) ===",
                                     processTerminating ? 1 : 0);
    BridgeLogger::Instance().LogJson("Bridge_Detach",
                                     "\"process_terminating\":%s",
                                     processTerminating ? "true" : "false");

    if (!processTerminating && g_rt.mhInitialized) {
        if (g_rt.hooksInstalled) {
            MH_DisableHook(MH_ALL_HOOKS);
            g_rt.hooksInstalled = false;
        }
        MH_Uninitialize();
        g_rt.mhInitialized = false;
    }

    if (!processTerminating) {
        if (g_rt.heartbeatStopEvent) {
            SetEvent(g_rt.heartbeatStopEvent);
        }
        if (g_rt.heartbeatThread) {
            WaitForSingleObject(g_rt.heartbeatThread, 1500u);
            CloseHandle(g_rt.heartbeatThread);
            g_rt.heartbeatThread = nullptr;
        }
        if (g_rt.heartbeatStopEvent) {
            CloseHandle(g_rt.heartbeatStopEvent);
            g_rt.heartbeatStopEvent = nullptr;
        }
    } else {
        g_rt.heartbeatThread = nullptr;
        g_rt.heartbeatStopEvent = nullptr;
    }

    if (g_rt.consumer) {
        g_rt.consumer->Shutdown();
        g_rt.consumer.reset();
    }
    CloseControlShm();

    if (!processTerminating && g_rt.csInitialized) {
        EnterCriticalSection(&g_rt.stateCs);
        g_rt.decoders.clear();
        LeaveCriticalSection(&g_rt.stateCs);
        DeleteCriticalSection(&g_rt.stateCs);
        g_rt.csInitialized = false;
    }

    BridgeLogger::Instance().Shutdown();
}

} // namespace routeb
