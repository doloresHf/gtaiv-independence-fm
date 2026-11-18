#include "ipc_ring_protocol.h"
#include "stage_c_ipc.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool FileExistsRegular(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring FindProducerExe(const std::wstring& explicitPath) {
    if (!explicitPath.empty()) {
        return FileExistsRegular(explicitPath) ? explicitPath : L"";
    }

    wchar_t exePathBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    const fs::path exeDir = fs::path(exePathBuf).parent_path();

    const fs::path candidates[] = {
        exeDir / L"iv_stage_c_producer.exe",
        (exeDir / L"..\\..\\..\\x64_main\\bin\\Release\\iv_stage_c_producer.exe").lexically_normal(),
        (exeDir / L"..\\..\\..\\worker_b_x64\\bin\\Release\\iv_stage_c_producer.exe").lexically_normal(),
        (exeDir / L"..\\..\\..\\worker_a_x64\\bin\\Release\\iv_stage_c_producer.exe").lexically_normal(),
    };

    for (const fs::path& cand : candidates) {
        const std::wstring w = cand.wstring();
        if (FileExistsRegular(w)) {
            return w;
        }
    }
    return L"";
}

std::wstring MakeUniqueMappingName(uint32_t idx) {
    wchar_t buf[128]{};
    std::swprintf(buf, 128, L"Local\\GTAIV_StageC_ProtoTest_%lu_%u",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned>(idx));
    return buf;
}

std::wstring MakeUniqueEventName(const wchar_t* tag, uint32_t idx) {
    wchar_t buf[128]{};
    std::swprintf(buf, 128, L"Local\\GTAIV_StageC_ProtoEv_%s_%lu_%u",
                  tag,
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned>(idx));
    return buf;
}

std::wstring QuoteArg(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t ch : s) {
        if (ch == L'"') out += L"\\\"";
        else out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
}

struct ProducerProcess {
    HANDLE hProcess = nullptr;
    DWORD pid = 0;

    ~ProducerProcess() { Close(); }

    void Close() {
        if (hProcess) {
            CloseHandle(hProcess);
            hProcess = nullptr;
        }
        pid = 0;
    }

    bool Launch(const std::wstring& exePath, const std::wstring& args) {
        Close();
        std::wstring cmdLine = QuoteArg(exePath) + L" " + args;
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exePath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::printf("[FAIL] CreateProcessW(%ls) failed: err=%lu\n",
                        exePath.c_str(), static_cast<unsigned long>(GetLastError()));
            return false;
        }
        CloseHandle(pi.hThread);
        hProcess = pi.hProcess;
        pid = pi.dwProcessId;
        return true;
    }

    bool WaitExit(DWORD timeoutMs, DWORD& outExitCode) {
        outExitCode = STILL_ACTIVE;
        if (!hProcess) return false;
        DWORD w = WaitForSingleObject(hProcess, timeoutMs);
        if (w != WAIT_OBJECT_0) {
            return false;
        }
        GetExitCodeProcess(hProcess, &outExitCode);
        return true;
    }

    void TerminateAndWait(DWORD exitCode = 1) {
        if (hProcess) {
            TerminateProcess(hProcess, exitCode);
            WaitForSingleObject(hProcess, 3000);
        }
    }
};

struct SyntheticMapping {
    HANDLE hMap = nullptr;
    stagec::StageCIpcSharedBlock* block = nullptr;
    size_t mappedBytes = 0;

    ~SyntheticMapping() { Close(); }

    bool Create(const std::wstring& name, size_t sizeBytes) {
        Close();
        hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  0, static_cast<DWORD>(sizeBytes), name.c_str());
        if (!hMap) {
            std::printf("[FAIL] CreateFileMappingW(%ls, %zu) failed: err=%lu\n",
                        name.c_str(), sizeBytes, static_cast<unsigned long>(GetLastError()));
            return false;
        }
        void* view = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeBytes);
        if (!view) {
            std::printf("[FAIL] MapViewOfFile(%ls) failed: err=%lu\n",
                        name.c_str(), static_cast<unsigned long>(GetLastError()));
            CloseHandle(hMap);
            hMap = nullptr;
            return false;
        }
        std::memset(view, 0, sizeBytes);
        block = static_cast<stagec::StageCIpcSharedBlock*>(view);
        mappedBytes = sizeBytes;
        return true;
    }

    void InitValidHeader(uint64_t gen, uint64_t sessionId, uint64_t commitFrames) {
        LARGE_INTEGER freq{}, now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);

        auto& h = block->header;
        std::memset(&h, 0, sizeof(h));
        h.magic = stagec::kIpcMagic;
        h.version = stagec::kIpcVersion;
        h.header_bytes = stagec::kHeaderBytes;
        h.total_bytes = stagec::kTotalSharedBytes;
        h.sample_rate = stagec::kSampleRate;
        h.channels = stagec::kChannels;
        h.bits_per_sample = stagec::kBitsPerSample;
        h.capacity_frames = stagec::kRingCapacityFrames;
        h.session_id = sessionId;
        h.qpc_freq = static_cast<uint64_t>(freq.QuadPart);
        h.producer_pid = GetCurrentProcessId();
        h.producer_arch = 32u;
        h.start_qpc = now.QuadPart;
        stagec::AtomicStoreI64(&h.last_heartbeat_qpc, now.QuadPart);
        stagec::AtomicStore32(&h.producer_state, static_cast<uint32_t>(stagec::ProducerState::Streaming));
        stagec::AtomicStore32(&h.pattern_mode, static_cast<uint32_t>(stagec::ProducerPattern::FrameCode));
        stagec::AtomicStoreU64(&h.write_reserve_frame, commitFrames);
        stagec::AtomicStoreU64(&h.write_commit_frame, commitFrames);
        stagec::AtomicStoreU64(&h.session_generation, gen);
    }

    void WriteFrameCodeRange(uint64_t startFrame, uint64_t count, uint64_t gen) {
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t f = startFrame + i;
            const uint32_t slot = static_cast<uint32_t>(f & stagec::kRingFrameMask);
            int16_t l = 0, r = 0;
            stagec::EncodeFrameCode(f, gen, l, r);
            block->pcm_samples[slot * 2u] = l;
            block->pcm_samples[slot * 2u + 1u] = r;
        }
    }

    void RefreshHeartbeat() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        stagec::AtomicStoreI64(&block->header.last_heartbeat_qpc, now.QuadPart);
    }

    void Close() {
        if (block) {
            UnmapViewOfFile(block);
            block = nullptr;
        }
        if (hMap) {
            CloseHandle(hMap);
            hMap = nullptr;
        }
        mappedBytes = 0;
    }
};

struct ReadOnlyMappingView {
    HANDLE hMap = nullptr;
    const stagec::StageCIpcSharedBlock* block = nullptr;

    ~ReadOnlyMappingView() { Close(); }

    bool Open(const std::wstring& name) {
        Close();
        hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        if (!hMap) return false;
        block = static_cast<const stagec::StageCIpcSharedBlock*>(
            MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(stagec::StageCIpcRingHeader)));
        if (!block) {
            CloseHandle(hMap);
            hMap = nullptr;
            return false;
        }
        return true;
    }

    void Close() {
        if (block) {
            UnmapViewOfFile(block);
            block = nullptr;
        }
        if (hMap) {
            CloseHandle(hMap);
            hMap = nullptr;
        }
    }
};

double ElapsedMs(LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER freq) {
    return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

bool AllSamplesExactZero(const int16_t* buf, size_t frameCount) {
    for (size_t i = 0; i < frameCount * 2; ++i) {
        if (buf[i] != 0) return false;
    }
    return true;
}

bool ContainsSentinelValue(const int16_t* buf, size_t frameCount, int16_t posVal, int16_t negVal) {
    for (size_t i = 0; i < frameCount * 2; ++i) {
        if (buf[i] == posVal || buf[i] == negVal) return true;
    }
    return false;
}

probe::StageCIpcConfig MakeBaseConfig(const std::wstring& mapName,
                                      uint32_t steadyLeadMs,
                                      uint32_t fadeMs,
                                      uint32_t heartbeatTimeoutMs = 500u,
                                      bool allowHotReattach = true) {
    probe::StageCIpcConfig cfg;
    cfg.mappingName = mapName;
    cfg.targetFiles = { L"IV_Probe_Target_45s.mp3" };
    cfg.steadyLeadMs = steadyLeadMs;
    cfg.heartbeatTimeoutMs = heartbeatTimeoutMs;
    cfg.fadeMs = fadeMs;
    cfg.allowHotReattach = allowHotReattach;
    return cfg;
}

// ============================================================================
// Test 1 (C0-1): Layout, FrameCode Bijection & Malformed Mapping Defense
// ============================================================================
bool RunTest1_LayoutFrameCodeAndMalformed() {
    std::printf("\n=== Test 1 (C0-1): Layout, FrameCode Bijection & Malformed Mapping Defense ===\n");
    auto& mgr = probe::StageCIpcManager::Instance();
    mgr.Shutdown();

    // 1a: FrameCode bijection, silence non-collision, parity rejection, cross-gen separation
    const uint64_t boundaryFrames[] = {
        0ULL, 1ULL, 2ULL, 32767ULL, 32768ULL,
        65535ULL, 65536ULL,
        stagec::kRingCapacityFrames - 1ULL,
        stagec::kRingCapacityFrames,
        stagec::kRingCapacityFrames + 1ULL,
        stagec::kFrameCodeMod - 2ULL,
        stagec::kFrameCodeMod - 1ULL,
        stagec::kFrameCodeMod,
        stagec::kFrameCodeMod + 12345ULL
    };
    const uint64_t generations[] = { 2ULL, 4ULL, 6ULL };

    for (uint64_t gen : generations) {
        for (uint64_t f : boundaryFrames) {
            int16_t l = 0, r = 0;
            stagec::EncodeFrameCode(f, gen, l, r);
            if (stagec::IsExactSilenceFrame(l, r) || r == 0) {
                std::printf("[FAIL] C0-1: EncodeFrameCode(%llu, gen=%llu) collided with silence (%d, %d)\n",
                            static_cast<unsigned long long>(f), static_cast<unsigned long long>(gen),
                            static_cast<int>(l), static_cast<int>(r));
                return false;
            }
            uint64_t dec = 0;
            if (!stagec::DecodeFrameCode(l, r, gen, dec) || dec != (f % stagec::kFrameCodeMod)) {
                std::printf("[FAIL] C0-1: DecodeFrameCode round-trip failed for f=%llu gen=%llu (got ok=%d dec=%llu)\n",
                            static_cast<unsigned long long>(f), static_cast<unsigned long long>(gen),
                            stagec::DecodeFrameCode(l, r, gen, dec) ? 1 : 0,
                            static_cast<unsigned long long>(dec));
                return false;
            }
        }
    }

    // Continuous range + parity rejection (L from f paired with R from f+1)
    for (uint64_t f = 0; f < 4096ULL; ++f) {
        int16_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
        stagec::EncodeFrameCode(f, 2ULL, l0, r0);
        stagec::EncodeFrameCode(f + 1ULL, 2ULL, l1, r1);
        uint64_t dec = 0;
        if (!stagec::DecodeFrameCode(l0, r0, 2ULL, dec) || dec != f) {
            std::printf("[FAIL] C0-1: range round-trip failed at f=%llu\n", static_cast<unsigned long long>(f));
            return false;
        }
        if (stagec::DecodeFrameCode(l0, r1, 2ULL, dec)) {
            std::printf("[FAIL] C0-1: parity check failed to reject torn L(f=%llu)/R(f+1)\n",
                        static_cast<unsigned long long>(f));
            return false;
        }
        uint64_t wrongGenDec = 0;
        if (stagec::DecodeFrameCode(l0, r0, 4ULL, wrongGenDec) && wrongGenDec == f) {
            std::printf("[FAIL] C0-1: cross-generation decode collided at f=%llu\n",
                        static_cast<unsigned long long>(f));
            return false;
        }
    }
    {
        uint64_t dummy = 0;
        if (stagec::DecodeFrameCode(0, 0, 2ULL, dummy) ||
            stagec::DecodeFrameCode(100, 0, 2ULL, dummy) ||
            stagec::DecodeFrameCode(-16385, 1, 2ULL, dummy) ||
            stagec::DecodeFrameCode(16384, -1, 2ULL, dummy) ||
            stagec::DecodeFrameCode(0, 16385, 2ULL, dummy)) {
            std::printf("[FAIL] C0-1: invalid FrameCode input was unexpectedly accepted\n");
            return false;
        }
    }
    std::printf("  [1a] FrameCode bijection, silence separation, parity & cross-gen checks passed.\n");

    // 1b: Undersized mapping (1024 bytes)
    {
        const std::wstring mapName = MakeUniqueMappingName(101);
        HANDLE hSmall = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 1024, mapName.c_str());
        if (!hSmall) {
            std::printf("[FAIL] C0-1: CreateFileMappingW(1024) failed: err=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }
        mgr.Configure(MakeBaseConfig(mapName, 1000u, 0u));
        mgr.NotifyReaderOpened(1);
        std::vector<int16_t> buf(1152 * 2, 0x1234);
        auto res = mgr.PullFramesForReader(1, 222, 1152, buf.data());
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        CloseHandle(hSmall);

        if (res.producerOnline || res.liveFramesDelivered != 0 || !AllSamplesExactZero(buf.data(), 1152)) {
            std::printf("[FAIL] C0-1: undersized mapping was not safely silenced (online=%d live=%u)\n",
                        res.producerOnline ? 1 : 0, res.liveFramesDelivered);
            return false;
        }
        std::printf("  [1b] Undersized mapping (1024B) cleanly rejected with 100%% silence.\n");
    }

    // 1c: Full-size malformed mappings (bad magic, bad sample_rate=44100, odd session_generation=3 with Streaming)
    struct BadCase {
        const char* label;
        uint32_t idx;
        int kind; // 0=bad magic, 1=bad rate, 2=odd gen
    };
    const BadCase badCases[] = {
        { "bad_magic",       102, 0 },
        { "bad_sample_rate", 103, 1 },
        { "odd_generation",  104, 2 },
    };
    for (const auto& bc : badCases) {
        const std::wstring mapName = MakeUniqueMappingName(bc.idx);
        SyntheticMapping m;
        if (!m.Create(mapName, stagec::kTotalSharedBytes)) return false;
        m.InitValidHeader(2ULL, 1000ULL + bc.idx, 200000ULL);
        m.WriteFrameCodeRange(0ULL, 200000ULL, 2ULL);
        if (bc.kind == 0) {
            m.block->header.magic = 0xDEADBEEFu;
        } else if (bc.kind == 1) {
            m.block->header.sample_rate = 44100u;
        } else {
            stagec::AtomicStoreU64(&m.block->header.session_generation, 3ULL);
            stagec::AtomicStore32(&m.block->header.producer_state,
                                  static_cast<uint32_t>(stagec::ProducerState::Streaming));
        }

        mgr.Configure(MakeBaseConfig(mapName, 1000u, 0u));
        mgr.NotifyReaderOpened(1);
        std::vector<int16_t> buf(1152 * 2, 0x2345);
        auto res = mgr.PullFramesForReader(1, 222, 1152, buf.data());
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        m.Close();

        if (res.producerOnline || res.liveFramesDelivered != 0 || !AllSamplesExactZero(buf.data(), 1152)) {
            std::printf("[FAIL] C0-1: malformed mapping (%s) was not rejected (online=%d live=%u)\n",
                        bc.label, res.producerOnline ? 1 : 0, res.liveFramesDelivered);
            return false;
        }
        std::printf("  [1c] Malformed mapping (%s) cleanly rejected with 100%% silence.\n", bc.label);
    }

    std::printf("[PASS] Test 1 (C0-1)\n");
    return true;
}

// ============================================================================
// Test 2 (C0-2): Deterministic [P1-2] Uncommitted Overwrite Race Fault Injection
// ============================================================================
bool RunTest2_OverwriteRace(const std::wstring& producerExe) {
    std::printf("\n=== Test 2 (C0-2): Deterministic [P1-2] Uncommitted Overwrite Race Fault Injection ===\n");
    auto& mgr = probe::StageCIpcManager::Instance();
    mgr.Shutdown();

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);

    constexpr int16_t kCorruptPos = 0x5555;
    constexpr int16_t kCorruptNeg = -0x5555;

    // 2a: Wrap-boundary uncommitted overwrite (consumer anchored at liveReadFrame=0..4,
    // producer sets commit=kRingCapacityFrames, reserve=kRingCapacityFrames+480, overwrites 0..479 without updating commit)
    {
        const std::wstring mapName = MakeUniqueMappingName(201);
        SyntheticMapping m;
        if (!m.Create(mapName, stagec::kTotalSharedBytes)) return false;
        m.InitValidHeader(2ULL, 2001ULL, 48000ULL);
        m.WriteFrameCodeRange(0ULL, 48000ULL, 2ULL);

        mgr.Configure(MakeBaseConfig(mapName, 1000u, 0u));
        mgr.NotifyReaderOpened(1);

        // Anchor at liveReadFrame = 0 by pulling 4 frames (so liveReadFrame advances to 4, inside [0..479])
        std::vector<int16_t> anchorBuf(4 * 2, 0);
        auto rAnchor = mgr.PullFramesForReader(1, 222, 4, anchorBuf.data());
        if (!rAnchor.producerOnline || rAnchor.liveFramesDelivered != 4 || rAnchor.liveReadFrameStart != 0ULL) {
            std::printf("[FAIL] C0-2a: failed to anchor at frame 0 (online=%d live=%u start=%llu)\n",
                        rAnchor.producerOnline ? 1 : 0, rAnchor.liveFramesDelivered,
                        static_cast<unsigned long long>(rAnchor.liveReadFrameStart));
            mgr.NotifyReaderEofOrClosed(1);
            mgr.Shutdown();
            return false;
        }

        // Fill up to kRingCapacityFrames, then write corrupt sentinel into 0..479 with reserve = cap+480 (paused before commit)
        stagec::AtomicStoreU64(&m.block->header.write_commit_frame, stagec::kRingCapacityFrames);
        stagec::AtomicStoreU64(&m.block->header.write_reserve_frame, stagec::kRingCapacityFrames + 480ULL);
        for (uint32_t s = 0; s < 480u; ++s) {
            m.block->pcm_samples[s * 2u] = kCorruptPos;
            m.block->pcm_samples[s * 2u + 1u] = kCorruptNeg;
        }
        m.RefreshHeartbeat();

        std::vector<int16_t> outBuf(1152 * 2, 0);
        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        auto rRace = mgr.PullFramesForReader(1, 223, 1152, outBuf.data());
        QueryPerformanceCounter(&t1);
        const double ms = ElapsedMs(t0, t1, freq);

        stagec::StageCIpcRingHeader snap{};
        mgr.ReadSharedHeaderSnapshot(snap);
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        m.Close();

        if (!rRace.filled || ms >= 50.0) {
            std::printf("[FAIL] C0-2a: PullFramesForReader took %.3f ms (limit <50ms) or not filled (%d)\n",
                        ms, rRace.filled ? 1 : 0);
            return false;
        }
        if (ContainsSentinelValue(outBuf.data(), 1152, kCorruptPos, kCorruptNeg)) {
            std::printf("[FAIL] C0-2a: corrupt sentinel (0x5555/-0x5555) leaked into output!\n");
            return false;
        }
        if (snap.overwrite_conflict_events + snap.overrun_events == 0) {
            std::printf("[FAIL] C0-2a: neither overwrite_conflict_events nor overrun_events incremented!\n");
            return false;
        }
        std::printf("  [2a] Wrap-boundary overwrite blocked in %.3f ms (0 sentinel leaks, conflict=%llu overrun=%llu).\n",
                    ms,
                    static_cast<unsigned long long>(snap.overwrite_conflict_events),
                    static_cast<unsigned long long>(snap.overrun_events));
    }

    // 2b: Two-thread deterministic race specifically exercising post-copy reserveAfter check
    // Consumer starts at liveReadFrame=0..4 with commit=400000, reserve=400000 (well below guard!).
    {
        const std::wstring mapName = MakeUniqueMappingName(202);
        SyntheticMapping m;
        if (!m.Create(mapName, stagec::kTotalSharedBytes)) return false;

        std::atomic<bool> stopThreads{ false };
        std::atomic<uint32_t> trialGo{ 0 };
        std::atomic<uint32_t> trialAck{ 0 };
        uint64_t totalConflictEvents = 0;
        uint64_t totalOverrunEvents = 0;

        struct RaceWriterArgs {
            SyntheticMapping* m;
            std::atomic<bool>* stop;
            std::atomic<uint32_t>* go;
            std::atomic<uint32_t>* ack;
        } wArgs{ &m, &stopThreads, &trialGo, &trialAck };

        HANDLE hWriter = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            auto* a = static_cast<RaceWriterArgs*>(p);
            uint32_t seen = 0;
            while (!a->stop->load(std::memory_order_acquire)) {
                const uint32_t cur = a->go->load(std::memory_order_acquire);
                if (cur == seen) {
                    YieldProcessor();
                    continue;
                }
                seen = cur;
                // Stagger spin across trials so some writes land before reserveBefore and some mid-copy!
                const uint32_t spins = (cur % 96u) * 8u;
                for (volatile uint32_t s = 0; s < spins; ++s) {
                    YieldProcessor();
                }
                stagec::AtomicStoreU64(&a->m->block->header.write_reserve_frame, stagec::kRingCapacityFrames + 1152ULL);
                for (uint32_t i = 0; i < 1152u; ++i) {
                    a->m->block->pcm_samples[i * 2u] = 0x5555;
                    a->m->block->pcm_samples[i * 2u + 1u] = -0x5555;
                }
                a->ack->store(cur, std::memory_order_release);
            }
            return 0;
        }, &wArgs, 0, nullptr);

        if (!hWriter) {
            std::printf("[FAIL] C0-2b: CreateThread failed.\n");
            return false;
        }

        constexpr uint32_t kMaxTrials = 600u;
        for (uint32_t t = 1; t <= kMaxTrials; ++t) {
            const uint64_t gen = 2ULL * static_cast<uint64_t>(t);
            m.InitValidHeader(gen, 2100ULL + t, 48000ULL);
            m.WriteFrameCodeRange(0ULL, 400000ULL, gen);

            mgr.Configure(MakeBaseConfig(mapName, 1000u, 0u));
            mgr.NotifyReaderOpened(1);

            std::vector<int16_t> anchorBuf(4 * 2, 0);
            auto rA = mgr.PullFramesForReader(1, 222, 4, anchorBuf.data());
            if (!rA.producerOnline || rA.liveFramesDelivered != 4) {
                std::printf("[FAIL] C0-2b trial %u: anchor failed\n", t);
                stopThreads.store(true);
                WaitForSingleObject(hWriter, 2000);
                CloseHandle(hWriter);
                mgr.Shutdown();
                return false;
            }

            // Advance commit=400000, reserve=400000 (well below guard: 400000 << 1048576 - 48000)
            stagec::AtomicStoreU64(&m.block->header.write_reserve_frame, 400000ULL);
            stagec::AtomicStoreU64(&m.block->header.write_commit_frame, 400000ULL);
            m.RefreshHeartbeat();

            std::vector<int16_t> outBuf(1152 * 2, 0);
            trialGo.store(t, std::memory_order_release);
            LARGE_INTEGER c0{}, c1{};
            QueryPerformanceCounter(&c0);
            auto rPull = mgr.PullFramesForReader(1, 223, 1152, outBuf.data());
            QueryPerformanceCounter(&c1);
            const double pullMs = ElapsedMs(c0, c1, freq);

            while (trialAck.load(std::memory_order_acquire) != t) {
                YieldProcessor();
            }

            stagec::StageCIpcRingHeader snap{};
            mgr.ReadSharedHeaderSnapshot(snap);
            totalConflictEvents += snap.overwrite_conflict_events;
            totalOverrunEvents += snap.overrun_events;

            mgr.NotifyReaderEofOrClosed(1);
            mgr.Shutdown();

            if (!rPull.filled || pullMs >= 50.0) {
                std::printf("[FAIL] C0-2b trial %u: pull boundedness failed (%.3f ms, filled=%d)\n",
                            t, pullMs, rPull.filled ? 1 : 0);
                stopThreads.store(true);
                WaitForSingleObject(hWriter, 2000);
                CloseHandle(hWriter);
                return false;
            }
            if (ContainsSentinelValue(outBuf.data(), 1152, kCorruptPos, kCorruptNeg)) {
                std::printf("[FAIL] C0-2b trial %u: corrupt sentinel (0x5555/-0x5555) leaked into consumer output!\n", t);
                stopThreads.store(true);
                WaitForSingleObject(hWriter, 2000);
                CloseHandle(hWriter);
                return false;
            }
            if (totalConflictEvents >= 3 && t >= 64) {
                break;
            }
        }

        stopThreads.store(true, std::memory_order_release);
        WaitForSingleObject(hWriter, 3000);
        CloseHandle(hWriter);
        m.Close();

        if (totalConflictEvents == 0 && totalOverrunEvents == 0) {
            std::printf("[FAIL] C0-2b: neither overwrite_conflict_events nor overrun_events observed across race trials!\n");
            return false;
        }
        std::printf("  [2b] Two-thread race passed (0 sentinel leaks; overwrite_conflict_events=%llu, overrun_events=%llu).\n",
                    static_cast<unsigned long long>(totalConflictEvents),
                    static_cast<unsigned long long>(totalOverrunEvents));
    }

    // 2c: Real x64 producer --test-pause-before-commit-at-wrap
    {
        const std::wstring mapName = MakeUniqueMappingName(203);
        const std::wstring pauseEvName = MakeUniqueEventName(L"Pause", 203);
        const std::wstring resumeEvName = MakeUniqueEventName(L"Resume", 203);
        const std::wstring stopEvName = MakeUniqueEventName(L"Stop", 203);

        HANDLE hPause = CreateEventW(nullptr, TRUE, FALSE, pauseEvName.c_str());
        HANDLE hResume = CreateEventW(nullptr, TRUE, FALSE, resumeEvName.c_str());
        HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, stopEvName.c_str());
        if (!hPause || !hResume || !hStop) {
            if (hPause) CloseHandle(hPause);
            if (hResume) CloseHandle(hResume);
            if (hStop) CloseHandle(hStop);
            std::printf("[FAIL] C0-2c: CreateEventW failed\n");
            return false;
        }

        ProducerProcess prod;
        const std::wstring args =
            L"--mapping-name " + QuoteArg(mapName) +
            L" --pattern framecode --duration-sec 10 --stop-event-name " + QuoteArg(stopEvName) +
            L" --test-pause-before-commit-at-wrap " +
            QuoteArg(pauseEvName) + L" " + QuoteArg(resumeEvName);
        if (!prod.Launch(producerExe, args)) {
            CloseHandle(hPause);
            CloseHandle(hResume);
            CloseHandle(hStop);
            return false;
        }

        if (WaitForSingleObject(hPause, 15000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] C0-2c: producer did not signal pause event within 15s\n");
            prod.TerminateAndWait(1);
            CloseHandle(hPause);
            CloseHandle(hResume);
            CloseHandle(hStop);
            return false;
        }

        ReadOnlyMappingView ro;
        if (!ro.Open(mapName)) {
            std::printf("[FAIL] C0-2c: could not open mapping %ls\n", mapName.c_str());
            SetEvent(hResume);
            SetEvent(hStop);
            prod.TerminateAndWait(1);
            CloseHandle(hPause);
            CloseHandle(hResume);
            CloseHandle(hStop);
            return false;
        }

        const uint64_t commitNow = stagec::AtomicLoadU64(&ro.block->header.write_commit_frame);
        const uint64_t reserveNow = stagec::AtomicLoadU64(&ro.block->header.write_reserve_frame);
        const uint64_t genNow = stagec::AtomicLoadU64(&ro.block->header.session_generation);
        if (commitNow != stagec::kRingCapacityFrames ||
            reserveNow != stagec::kRingCapacityFrames + 480ULL ||
            !stagec::IsReadyEvenGeneration(genNow)) {
            std::printf("[FAIL] C0-2c: unexpected paused wrap header (gen=%llu commit=%llu reserve=%llu)\n",
                        static_cast<unsigned long long>(genNow),
                        static_cast<unsigned long long>(commitNow),
                        static_cast<unsigned long long>(reserveNow));
            ro.Close();
            SetEvent(hResume);
            SetEvent(hStop);
            prod.TerminateAndWait(1);
            CloseHandle(hPause);
            CloseHandle(hResume);
            CloseHandle(hStop);
            return false;
        }
        ro.Close();

        mgr.Configure(MakeBaseConfig(mapName, 1000u, 0u));
        mgr.NotifyReaderOpened(1);
        std::vector<int16_t> outBuf(1152 * 2, 0);
        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        auto res = mgr.PullFramesForReader(1, 222, 1152, outBuf.data());
        QueryPerformanceCounter(&t1);
        const double ms = ElapsedMs(t0, t1, freq);

        bool allValid = res.filled && res.producerOnline && res.liveFramesDelivered == 1152u && ms < 50.0;
        if (allValid) {
            uint64_t prevMod = 0;
            for (uint32_t f = 0; f < 1152u; ++f) {
                const int16_t l = outBuf[f * 2u];
                const int16_t r = outBuf[f * 2u + 1u];
                if (l == kCorruptPos || l == kCorruptNeg || r == kCorruptPos || r == kCorruptNeg) {
                    allValid = false;
                    break;
                }
                uint64_t dec = 0;
                if (!stagec::DecodeFrameCode(l, r, res.sessionGeneration, dec)) {
                    allValid = false;
                    break;
                }
                if (f > 0 && dec != ((prevMod + 1ULL) % stagec::kFrameCodeMod)) {
                    allValid = false;
                    break;
                }
                prevMod = dec;
            }
        }

        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();

        SetEvent(hResume);
        Sleep(50);
        SetEvent(hStop);
        DWORD exitCode = STILL_ACTIVE;
        bool exited = prod.WaitExit(10000, exitCode);
        CloseHandle(hPause);
        CloseHandle(hResume);
        CloseHandle(hStop);

        if (!allValid || !exited || exitCode != 0) {
            std::printf("[FAIL] C0-2c: x64 wrap-pause test failed (allValid=%d ms=%.3f exited=%d exit=%lu)\n",
                        allValid ? 1 : 0, ms, exited ? 1 : 0, static_cast<unsigned long>(exitCode));
            if (!exited) prod.TerminateAndWait(1);
            return false;
        }
        std::printf("  [2c] x64 --test-pause-before-commit-at-wrap verified (%.3f ms, 1152 clean frames, exit=0).\n", ms);
    }

    std::printf("[PASS] Test 2 (C0-2)\n");
    return true;
}

// ============================================================================
// Test 3 (C0-3): [P1-3] Single-Producer Mutex, Mid-Init Crash & Existing-Mapping Takeover
// ============================================================================
bool RunTest3_MutexCrashAndTakeover(const std::wstring& producerExe) {
    std::printf("\n=== Test 3 (C0-3): [P1-3] Single-Producer Mutex, Mid-Init Crash & Existing-Mapping Takeover ===\n");
    auto& mgr = probe::StageCIpcManager::Instance();
    mgr.Shutdown();

    // 3a: Second producer rejected with exitCode == 2 while first stream remains undisturbed
    {
        const std::wstring mapName = MakeUniqueMappingName(301);
        const std::wstring readyEv1 = MakeUniqueEventName(L"Rdy3a", 301);
        const std::wstring stopEv1 = MakeUniqueEventName(L"Stp3a", 301);
        HANDLE hReady1 = CreateEventW(nullptr, TRUE, FALSE, readyEv1.c_str());
        HANDLE hStop1 = CreateEventW(nullptr, TRUE, FALSE, stopEv1.c_str());

        ProducerProcess p1;
        if (!p1.Launch(producerExe,
                       L"--mapping-name " + QuoteArg(mapName) +
                       L" --pattern framecode --prefill-sec 1.5 --duration-sec 10 --ready-event-name " +
                       QuoteArg(readyEv1) + L" --stop-event-name " + QuoteArg(stopEv1))) {
            CloseHandle(hReady1);
            CloseHandle(hStop1);
            return false;
        }
        if (WaitForSingleObject(hReady1, 10000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] C0-3a: producer #1 did not signal ready\n");
            p1.TerminateAndWait(1);
            CloseHandle(hReady1);
            CloseHandle(hStop1);
            return false;
        }

        mgr.Configure(MakeBaseConfig(mapName, 1000u, 0u));
        mgr.NotifyReaderOpened(1);
        std::vector<int16_t> buf1(1152 * 2, 0);
        auto r1 = mgr.PullFramesForReader(1, 222, 1152, buf1.data());
        if (!r1.producerOnline || r1.liveFramesDelivered != 1152u || !stagec::IsReadyEvenGeneration(r1.sessionGeneration)) {
            std::printf("[FAIL] C0-3a: producer #1 initial pull failed (online=%d live=%u gen=%llu)\n",
                        r1.producerOnline ? 1 : 0, r1.liveFramesDelivered,
                        static_cast<unsigned long long>(r1.sessionGeneration));
            SetEvent(hStop1);
            p1.TerminateAndWait(1);
            mgr.Shutdown();
            CloseHandle(hReady1);
            CloseHandle(hStop1);
            return false;
        }
        const uint64_t gen1 = r1.sessionGeneration;
        uint64_t lastFrame1 = 0;
        for (uint32_t f = 0; f < 1152u; ++f) {
            uint64_t dec = 0;
            if (!stagec::DecodeFrameCode(buf1[f * 2u], buf1[f * 2u + 1u], gen1, dec)) {
                std::printf("[FAIL] C0-3a: FrameCode decode failed before #2 launch\n");
                SetEvent(hStop1);
                p1.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady1);
                CloseHandle(hStop1);
                return false;
            }
            if (f > 0 && dec != ((lastFrame1 + 1ULL) % stagec::kFrameCodeMod)) {
                std::printf("[FAIL] C0-3a: FrameCode discontinuity before #2 launch\n");
                SetEvent(hStop1);
                p1.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady1);
                CloseHandle(hStop1);
                return false;
            }
            lastFrame1 = dec;
        }

        // Launch duplicate producer #2 on same mapping
        ProducerProcess p2;
        if (!p2.Launch(producerExe,
                       L"--mapping-name " + QuoteArg(mapName) +
                       L" --pattern framecode --duration-sec 5")) {
            SetEvent(hStop1);
            p1.TerminateAndWait(1);
            mgr.Shutdown();
            CloseHandle(hReady1);
            CloseHandle(hStop1);
            return false;
        }
        DWORD p2Exit = STILL_ACTIVE;
        if (!p2.WaitExit(2000, p2Exit) || p2Exit != 2u) {
            std::printf("[FAIL] C0-3a: duplicate producer #2 expected exitCode=2 within 2s (got exit=%lu)\n",
                        static_cast<unsigned long>(p2Exit));
            p2.TerminateAndWait(1);
            SetEvent(hStop1);
            p1.TerminateAndWait(1);
            mgr.Shutdown();
            CloseHandle(hReady1);
            CloseHandle(hStop1);
            return false;
        }

        // Verify #1 stream stayed undisturbed
        for (int b = 0; b < 3; ++b) {
            Sleep(24);
            std::vector<int16_t> bufNext(1152 * 2, 0);
            auto rNext = mgr.PullFramesForReader(1, 223 + b, 1152, bufNext.data());
            if (!rNext.producerOnline || rNext.sessionGeneration != gen1 || rNext.liveFramesDelivered != 1152u) {
                std::printf("[FAIL] C0-3a: producer #1 disturbed after #2 rejected (online=%d gen=%llu live=%u)\n",
                            rNext.producerOnline ? 1 : 0,
                            static_cast<unsigned long long>(rNext.sessionGeneration),
                            rNext.liveFramesDelivered);
                SetEvent(hStop1);
                p1.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady1);
                CloseHandle(hStop1);
                return false;
            }
            for (uint32_t f = 0; f < 1152u; ++f) {
                uint64_t dec = 0;
                if (!stagec::DecodeFrameCode(bufNext[f * 2u], bufNext[f * 2u + 1u], gen1, dec) ||
                    dec != ((lastFrame1 + 1ULL) % stagec::kFrameCodeMod)) {
                    std::printf("[FAIL] C0-3a: producer #1 FrameCode stream disrupted after #2 rejected\n");
                    SetEvent(hStop1);
                    p1.TerminateAndWait(1);
                    mgr.Shutdown();
                    CloseHandle(hReady1);
                    CloseHandle(hStop1);
                    return false;
                }
                lastFrame1 = dec;
            }
        }

        SetEvent(hStop1);
        DWORD p1Exit = STILL_ACTIVE;
        p1.WaitExit(5000, p1Exit);
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        CloseHandle(hReady1);
        CloseHandle(hStop1);
        std::printf("  [3a] Duplicate producer rejected (exit=2) while #1 stream remained undisturbed.\n");
    }

    // 3b & 3c: Mid-init crash -> odd generation rejected -> #3 recovers abandoned mutex ->
    // consumer stays open while #3 stops and #4 starts on same mapping (ERROR_ALREADY_EXISTS)
    {
        const std::wstring mapName = MakeUniqueMappingName(302);

        // Keep the shared memory section object alive across pCrash's TerminateProcess,
        // simulating an already-open consumer / persistent mapping across producer crash.
        HANDLE hKeepAliveMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            stagec::kTotalSharedBytes, mapName.c_str());
        if (!hKeepAliveMap) {
            std::printf("[FAIL] C0-3b: CreateFileMappingW keepalive failed: err=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }

        // 3b step 1: Mid-init crash
        ProducerProcess pCrash;
        if (!pCrash.Launch(producerExe,
                           L"--mapping-name " + QuoteArg(mapName) + L" --test-crash-mid-init")) {
            CloseHandle(hKeepAliveMap);
            return false;
        }
        DWORD crashExit = STILL_ACTIVE;
        if (!pCrash.WaitExit(10000, crashExit) || crashExit != 99u) {
            std::printf("[FAIL] C0-3b: mid-init crash expected exitCode=99 (got %lu)\n",
                        static_cast<unsigned long>(crashExit));
            pCrash.TerminateAndWait(1);
            CloseHandle(hKeepAliveMap);
            return false;
        }

        // Keep consumer mapping open across 3b & 3c
        mgr.Configure(MakeBaseConfig(mapName, 1000u, 0u, 500u, true));
        mgr.NotifyReaderOpened(1);

        std::vector<int16_t> crashBuf(1152 * 2, 0x1111);
        auto rCrash = mgr.PullFramesForReader(1, 222, 1152, crashBuf.data());
        stagec::StageCIpcRingHeader crashHdr{};
        const bool gotSnap = mgr.ReadSharedHeaderSnapshot(crashHdr);
        if (!gotSnap || (crashHdr.session_generation & 1ULL) == 0ULL ||
            rCrash.producerOnline || !AllSamplesExactZero(crashBuf.data(), 1152)) {
            std::printf("[FAIL] C0-3b: consumer did not reject mid-init crashed odd generation (snap=%d gen=%llu online=%d)\n",
                        gotSnap ? 1 : 0, static_cast<unsigned long long>(crashHdr.session_generation),
                        rCrash.producerOnline ? 1 : 0);
            mgr.Shutdown();
            CloseHandle(hKeepAliveMap);
            return false;
        }
        const uint64_t oddCrashGen = crashHdr.session_generation;
        std::printf("  [3b-1] Mid-init crash left odd gen=%llu; consumer stayed offline with 100%% silence.\n",
                    static_cast<unsigned long long>(oddCrashGen));

        // 3b step 2: Launch producer #3 on same mapping -> recovers abandoned mutex, publishes even gen >= 4
        const std::wstring readyEv3 = MakeUniqueEventName(L"Rdy3b", 302);
        const std::wstring stopEv3 = MakeUniqueEventName(L"Stp3b", 302);
        HANDLE hReady3 = CreateEventW(nullptr, TRUE, FALSE, readyEv3.c_str());
        HANDLE hStop3 = CreateEventW(nullptr, TRUE, FALSE, stopEv3.c_str());

        ProducerProcess p3;
        if (!p3.Launch(producerExe,
                       L"--mapping-name " + QuoteArg(mapName) +
                       L" --pattern framecode --prefill-sec 1.5 --duration-sec 10 --ready-event-name " +
                       QuoteArg(readyEv3) + L" --stop-event-name " + QuoteArg(stopEv3))) {
            CloseHandle(hReady3);
            CloseHandle(hStop3);
            mgr.Shutdown();
            CloseHandle(hKeepAliveMap);
            return false;
        }
        if (WaitForSingleObject(hReady3, 10000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] C0-3b: producer #3 failed to recover abandoned mutex & signal ready\n");
            p3.TerminateAndWait(1);
            CloseHandle(hReady3);
            CloseHandle(hStop3);
            mgr.Shutdown();
            CloseHandle(hKeepAliveMap);
            return false;
        }

        // Wait briefly until commit > 50,000 (prefill-sec 1.5 is 72,000 frames) and pull with SAME open consumer
        std::vector<int16_t> p3Buf(1152 * 2, 0);
        auto r3 = mgr.PullFramesForReader(1, 223, 1152, p3Buf.data());
        stagec::StageCIpcRingHeader hdr3{};
        mgr.ReadSharedHeaderSnapshot(hdr3);
        if (!r3.producerOnline || r3.sessionGeneration < 4ULL ||
            (r3.sessionGeneration & 1ULL) != 0ULL || r3.liveFramesDelivered != 1152u ||
            hdr3.write_commit_frame <= 50000ULL) {
            std::printf("[FAIL] C0-3b: producer #3 attach failed (online=%d gen=%llu live=%u commit=%llu)\n",
                        r3.producerOnline ? 1 : 0, static_cast<unsigned long long>(r3.sessionGeneration),
                        r3.liveFramesDelivered, static_cast<unsigned long long>(hdr3.write_commit_frame));
            SetEvent(hStop3);
            p3.TerminateAndWait(1);
            CloseHandle(hReady3);
            CloseHandle(hStop3);
            mgr.Shutdown();
            CloseHandle(hKeepAliveMap);
            return false;
        }
        const uint64_t gen3 = r3.sessionGeneration;
        for (uint32_t f = 0; f < 1152u; ++f) {
            uint64_t dec = 0;
            if (!stagec::DecodeFrameCode(p3Buf[f * 2u], p3Buf[f * 2u + 1u], gen3, dec)) {
                std::printf("[FAIL] C0-3b: producer #3 FrameCode decode failed at frame %u\n", f);
                SetEvent(hStop3);
                p3.TerminateAndWait(1);
                CloseHandle(hReady3);
                CloseHandle(hStop3);
                mgr.Shutdown();
                CloseHandle(hKeepAliveMap);
                return false;
            }
        }
        std::printf("  [3b-2] Producer #3 recovered abandoned mutex -> gen=%llu, commit=%llu, valid FrameCode.\n",
                    static_cast<unsigned long long>(gen3),
                    static_cast<unsigned long long>(hdr3.write_commit_frame));

        // 3c: Stop #3 while consumer keeps mapping open, then start #4 (--prefill-sec 0) on SAME mapping
        SetEvent(hStop3);
        DWORD p3Exit = STILL_ACTIVE;
        p3.WaitExit(5000, p3Exit);
        CloseHandle(hReady3);
        CloseHandle(hStop3);

        // Pull once after #3 stopped so consumer observes disconnect/offline cleanly
        Sleep(50);
        std::vector<int16_t> gapBuf(1152 * 2, 0);
        auto rGap = mgr.PullFramesForReader(1, 224, 1152, gapBuf.data());
        (void)rGap;

        const std::wstring readyEv4 = MakeUniqueEventName(L"Rdy3c", 302);
        const std::wstring stopEv4 = MakeUniqueEventName(L"Stp3c", 302);
        HANDLE hReady4 = CreateEventW(nullptr, TRUE, FALSE, readyEv4.c_str());
        HANDLE hStop4 = CreateEventW(nullptr, TRUE, FALSE, stopEv4.c_str());

        ProducerProcess p4;
        if (!p4.Launch(producerExe,
                       L"--mapping-name " + QuoteArg(mapName) +
                       L" --pattern framecode --prefill-sec 0 --duration-sec 8 --ready-event-name " +
                       QuoteArg(readyEv4) + L" --stop-event-name " + QuoteArg(stopEv4))) {
            CloseHandle(hReady4);
            CloseHandle(hStop4);
            mgr.Shutdown();
            CloseHandle(hKeepAliveMap);
            return false;
        }
        if (WaitForSingleObject(hReady4, 10000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] C0-3c: producer #4 did not signal ready\n");
            p4.TerminateAndWait(1);
            CloseHandle(hReady4);
            CloseHandle(hStop4);
            mgr.Shutdown();
            CloseHandle(hKeepAliveMap);
            return false;
        }

        // Pull at ~24ms cadence up to 3s until SteadyLeadMs=1000 fills and #4 delivers live FrameCode
        uint64_t seq = 225;
        bool gotLiveGen4 = false;
        uint64_t gen4 = 0;
        for (int iter = 0; iter < 130; ++iter) {
            std::vector<int16_t> buf4(1152 * 2, 0);
            auto r4 = mgr.PullFramesForReader(1, seq++, 1152, buf4.data());
            stagec::StageCIpcRingHeader hdr4{};
            mgr.ReadSharedHeaderSnapshot(hdr4);
            if (hdr4.watermark_frames >= stagec::kRingCapacityFrames * 2ULL) {
                std::printf("[FAIL] C0-3c: unsigned underflow detected in watermark_frames=%llu!\n",
                            static_cast<unsigned long long>(hdr4.watermark_frames));
                SetEvent(hStop4);
                p4.TerminateAndWait(1);
                CloseHandle(hReady4);
                CloseHandle(hStop4);
                mgr.Shutdown();
                CloseHandle(hKeepAliveMap);
                return false;
            }
            if (r4.liveFramesDelivered > 0) {
                gen4 = r4.sessionGeneration;
                if (gen4 <= gen3 || (gen4 & 1ULL) != 0ULL) {
                    std::printf("[FAIL] C0-3c: expected new even gen > %llu, got %llu\n",
                                static_cast<unsigned long long>(gen3),
                                static_cast<unsigned long long>(gen4));
                    SetEvent(hStop4);
                    p4.TerminateAndWait(1);
                    CloseHandle(hReady4);
                    CloseHandle(hStop4);
                    mgr.Shutdown();
                    CloseHandle(hKeepAliveMap);
                    return false;
                }
                for (uint32_t f = r4.silenceFramesDelivered; f < 1152u; ++f) {
                    uint64_t dec = 0;
                    if (!stagec::DecodeFrameCode(buf4[f * 2u], buf4[f * 2u + 1u], gen4, dec)) {
                        std::printf("[FAIL] C0-3c: FrameCode decode failed for gen4=%llu\n",
                                    static_cast<unsigned long long>(gen4));
                        SetEvent(hStop4);
                        p4.TerminateAndWait(1);
                        CloseHandle(hReady4);
                        CloseHandle(hStop4);
                        mgr.Shutdown();
                        CloseHandle(hKeepAliveMap);
                        return false;
                    }
                }
                if (hdr4.session_attach_count >= 2ULL) {
                    gotLiveGen4 = true;
                    break;
                }
            }
            Sleep(24);
        }

        stagec::StageCIpcRingHeader finalHdr4{};
        mgr.ReadSharedHeaderSnapshot(finalHdr4);

        SetEvent(hStop4);
        DWORD p4Exit = STILL_ACTIVE;
        p4.WaitExit(5000, p4Exit);
        CloseHandle(hReady4);
        CloseHandle(hStop4);
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        CloseHandle(hKeepAliveMap);

        if (!gotLiveGen4) {
            std::printf("[FAIL] C0-3c: did not attach & decode live gen4 frames (attach_count=%llu)\n",
                        static_cast<unsigned long long>(finalHdr4.session_attach_count));
            return false;
        }
        std::printf("  [3c] Existing mapping takeover succeeded across restart (gen3=%llu -> gen4=%llu, attach_count=%llu).\n",
                    static_cast<unsigned long long>(gen3),
                    static_cast<unsigned long long>(gen4),
                    static_cast<unsigned long long>(finalHdr4.session_attach_count));
    }

    std::printf("[PASS] Test 3 (C0-3)\n");
    return true;
}

// ============================================================================
// Test 4 (C0-4): Real 1x Cold/Warm Startup Modes & FadeMs=0 vs FadeMs=10 Envelope
// ============================================================================
bool RunStartupModeFadeZeroSubtest(const std::wstring& producerExe,
                                   const char* label,
                                   uint32_t idx,
                                   double prefillSec,
                                   bool consumerStartsFirst) {
    const std::wstring mapName = MakeUniqueMappingName(idx);
    const std::wstring readyEv = MakeUniqueEventName(L"Rdy4", idx);
    const std::wstring stopEv = MakeUniqueEventName(L"Stp4", idx);
    HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());
    HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, stopEv.c_str());

    auto& mgr = probe::StageCIpcManager::Instance();
    mgr.Shutdown();
    mgr.Configure(MakeBaseConfig(mapName, 3500u, 0u, 500u, true));
    mgr.NotifyReaderOpened(1);

    uint64_t seq = 1;
    if (consumerStartsFirst) {
        // Pull seq 1..225 while no producer exists -> must be 100% silence & offline
        for (; seq <= 225ULL; ++seq) {
            const uint32_t n = (seq == 1ULL) ? 624u : 1152u;
            std::vector<int16_t> buf(n * 2u, 0x0707);
            auto res = mgr.PullFramesForReader(1, seq, n, buf.data());
            if (res.producerOnline || res.liveFramesDelivered != 0 || !AllSamplesExactZero(buf.data(), n)) {
                std::printf("[FAIL] C0-4 (%s): pre-producer seq=%llu was not 100%% offline silence\n",
                            label, static_cast<unsigned long long>(seq));
                mgr.Shutdown();
                CloseHandle(hReady);
                CloseHandle(hStop);
                return false;
            }
        }
    }

    wchar_t prefillArg[32]{};
    std::swprintf(prefillArg, 32, L"%.3f", prefillSec);

    ProducerProcess prod;
    if (!prod.Launch(producerExe,
                   L"--mapping-name " + QuoteArg(mapName) +
                   L" --pattern framecode --prefill-sec " + prefillArg +
                   L" --duration-sec 10 --ready-event-name " + QuoteArg(readyEv) +
                   L" --stop-event-name " + QuoteArg(stopEv))) {
        mgr.Shutdown();
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
        std::printf("[FAIL] C0-4 (%s): producer did not signal ready\n", label);
        prod.TerminateAndWait(1);
        mgr.Shutdown();
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    if (!consumerStartsFirst) {
        for (; seq <= 221ULL; ++seq) {
            const uint32_t n = (seq == 1ULL) ? 624u : 1152u;
            std::vector<int16_t> buf(n * 2u, 0x0505);
            auto res = mgr.PullFramesForReader(1, seq, n, buf.data());
            if (!res.filled || !res.undecidedPreloadSilence || !AllSamplesExactZero(buf.data(), n)) {
                std::printf("[FAIL] C0-4 (%s): undecided preload seq=%llu was not exact 0 silence\n",
                            label, static_cast<unsigned long long>(seq));
                SetEvent(hStop);
                prod.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady);
                CloseHandle(hStop);
                return false;
            }
        }
    }

    // Pull at 24ms cadence for up to ~6s (after 3.5s cushion fills, collect >= 48000 contiguous live frames)
    bool seenLive = false;
    uint64_t expectedNextMod = 0;
    uint64_t liveGen = 0;
    uint64_t liveFramesVerified = 0;
    uint64_t cushionSilenceFrames = 0;

    for (int iter = 0; iter < 260; ++iter) {
        std::vector<int16_t> buf(1152 * 2, 0);
        auto res = mgr.PullFramesForReader(1, seq++, 1152, buf.data());
        if (!res.filled) {
            std::printf("[FAIL] C0-4 (%s): PullFramesForReader returned filled=false\n", label);
            SetEvent(hStop);
            prod.TerminateAndWait(1);
            mgr.Shutdown();
            CloseHandle(hReady);
            CloseHandle(hStop);
            return false;
        }
        if (res.liveFramesDelivered == 0) {
            if (seenLive) {
                std::printf("[FAIL] C0-4 (%s): gap (0 live frames) after live streaming already started!\n", label);
                SetEvent(hStop);
                prod.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady);
                CloseHandle(hStop);
                return false;
            }
            if (!AllSamplesExactZero(buf.data(), 1152)) {
                std::printf("[FAIL] C0-4 (%s): leading cushion buffer contained non-zero samples\n", label);
                SetEvent(hStop);
                prod.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady);
                CloseHandle(hStop);
                return false;
            }
            cushionSilenceFrames += 1152ULL;
        } else {
            const uint32_t sil = res.silenceFramesDelivered;
            if (sil > 0 && seenLive) {
                std::printf("[FAIL] C0-4 (%s): silence mid-stream after live audio started (sil=%u)\n", label, sil);
                SetEvent(hStop);
                prod.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady);
                CloseHandle(hStop);
                return false;
            }
            if (!AllSamplesExactZero(buf.data(), sil)) {
                std::printf("[FAIL] C0-4 (%s): leading silence portion in transition buffer not exact 0\n", label);
                SetEvent(hStop);
                prod.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady);
                CloseHandle(hStop);
                return false;
            }
            cushionSilenceFrames += sil;
            for (uint32_t f = sil; f < 1152u; ++f) {
                uint64_t dec = 0;
                if (!stagec::DecodeFrameCode(buf[f * 2u], buf[f * 2u + 1u], res.sessionGeneration, dec)) {
                    std::printf("[FAIL] C0-4 (%s): DecodeFrameCode failed at liveFrame=%llu (L=%d R=%d)\n",
                                label, static_cast<unsigned long long>(liveFramesVerified),
                                static_cast<int>(buf[f * 2u]), static_cast<int>(buf[f * 2u + 1u]));
                    SetEvent(hStop);
                    prod.TerminateAndWait(1);
                    mgr.Shutdown();
                    CloseHandle(hReady);
                    CloseHandle(hStop);
                    return false;
                }
                if (!seenLive) {
                    seenLive = true;
                    liveGen = res.sessionGeneration;
                    expectedNextMod = dec;
                } else {
                    if (res.sessionGeneration != liveGen || dec != expectedNextMod) {
                        std::printf("[FAIL] C0-4 (%s): frame discontinuity at liveFrame=%llu (expected=%llu got=%llu)\n",
                                    label, static_cast<unsigned long long>(liveFramesVerified),
                                    static_cast<unsigned long long>(expectedNextMod),
                                    static_cast<unsigned long long>(dec));
                        SetEvent(hStop);
                        prod.TerminateAndWait(1);
                        mgr.Shutdown();
                        CloseHandle(hReady);
                        CloseHandle(hStop);
                        return false;
                    }
                }
                expectedNextMod = (expectedNextMod + 1ULL) % stagec::kFrameCodeMod;
                ++liveFramesVerified;
            }
            if (liveFramesVerified >= 48000ULL) {
                break;
            }
        }
        Sleep(24);
    }

    SetEvent(hStop);
    DWORD exitCode = STILL_ACTIVE;
    prod.WaitExit(5000, exitCode);
    mgr.NotifyReaderEofOrClosed(1);
    mgr.Shutdown();
    CloseHandle(hReady);
    CloseHandle(hStop);

    if (!seenLive || liveFramesVerified < 48000ULL) {
        std::printf("[FAIL] C0-4 (%s): insufficient live frames verified (%llu)\n",
                    label, static_cast<unsigned long long>(liveFramesVerified));
        return false;
    }
    std::printf("  [4-%s] Verified %llu contiguous live FrameCode frames (0 errors, cushion=%llu frames, gen=%llu).\n",
                label,
                static_cast<unsigned long long>(liveFramesVerified),
                static_cast<unsigned long long>(cushionSilenceFrames),
                static_cast<unsigned long long>(liveGen));
    return true;
}

bool RunTest4_StartupAndFadeSeparation(const std::wstring& producerExe) {
    std::printf("\n=== Test 4 (C0-4): Real 1x Cold/Warm Startup Modes & FadeMs=0 vs FadeMs=10 ===\n");

    if (!RunStartupModeFadeZeroSubtest(producerExe, "cold_prefill_0", 401, 0.0, false)) return false;
    if (!RunStartupModeFadeZeroSubtest(producerExe, "warm_prefill_1.5s", 402, 1.5, false)) return false;
    if (!RunStartupModeFadeZeroSubtest(producerExe, "consumer_first_cold", 403, 0.0, true)) return false;

    // 4d: FadeMs = 10 (480 frames cosine fade-in) exact envelope audit + frame 480+ exact DecodeFrameCode
    {
        const std::wstring mapName = MakeUniqueMappingName(404);
        const std::wstring readyEv = MakeUniqueEventName(L"Rdy4d", 404);
        const std::wstring stopEv = MakeUniqueEventName(L"Stp4d", 404);
        HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());
        HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, stopEv.c_str());

        ProducerProcess prod;
        if (!prod.Launch(producerExe,
                       L"--mapping-name " + QuoteArg(mapName) +
                       L" --pattern framecode --prefill-sec 2.0 --duration-sec 8 --ready-event-name " +
                       QuoteArg(readyEv) + L" --stop-event-name " + QuoteArg(stopEv))) {
            CloseHandle(hReady);
            CloseHandle(hStop);
            return false;
        }
        if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] C0-4d: producer did not signal ready\n");
            prod.TerminateAndWait(1);
            CloseHandle(hReady);
            CloseHandle(hStop);
            return false;
        }

        auto& mgr = probe::StageCIpcManager::Instance();
        mgr.Shutdown();
        mgr.Configure(MakeBaseConfig(mapName, 1500u, 10u));
        mgr.NotifyReaderOpened(1);

        // Undecided preload seq 1..221 -> must be exact 0
        for (uint64_t seq = 1; seq <= 221ULL; ++seq) {
            const uint32_t n = (seq == 1ULL) ? 624u : 1152u;
            std::vector<int16_t> buf(n * 2u, 0x0909);
            auto rPre = mgr.PullFramesForReader(1, seq, n, buf.data());
            if (!rPre.undecidedPreloadSilence || !AllSamplesExactZero(buf.data(), n)) {
                std::printf("[FAIL] C0-4d: preload seq=%llu not exact 0\n", static_cast<unsigned long long>(seq));
                SetEvent(hStop);
                prod.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady);
                CloseHandle(hStop);
                return false;
            }
        }

        // Pull 4 live buffers (4608 frames) starting at seq = 222
        std::vector<int16_t> livePcm;
        livePcm.reserve(4608 * 2);
        uint64_t f0 = 0;
        uint64_t gen = 0;
        bool anchored = false;

        for (uint64_t seq = 222; seq <= 225ULL; ++seq) {
            std::vector<int16_t> buf(1152 * 2, 0);
            auto r = mgr.PullFramesForReader(1, seq, 1152, buf.data());
            if (!r.producerOnline || r.liveFramesDelivered != 1152u) {
                std::printf("[FAIL] C0-4d: expected 1152 live frames at seq=%llu (online=%d live=%u)\n",
                            static_cast<unsigned long long>(seq),
                            r.producerOnline ? 1 : 0, r.liveFramesDelivered);
                SetEvent(hStop);
                prod.TerminateAndWait(1);
                mgr.Shutdown();
                CloseHandle(hReady);
                CloseHandle(hStop);
                return false;
            }
            if (!anchored) {
                anchored = true;
                f0 = r.liveReadFrameStart;
                gen = r.sessionGeneration;
            }
            livePcm.insert(livePcm.end(), buf.begin(), buf.end());
        }

        SetEvent(hStop);
        DWORD exitCode = STILL_ACTIVE;
        prod.WaitExit(5000, exitCode);
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        CloseHandle(hReady);
        CloseHandle(hStop);

        // Verify 0..479 cosine envelope within +/-2 LSB and 480..4607 exact DecodeFrameCode
        int32_t maxFadeErr = 0;
        for (uint32_t k = 0; k < 480u; ++k) {
            int16_t rawL = 0, rawR = 0;
            stagec::EncodeFrameCode(f0 + k, gen, rawL, rawR);
            const double gain = 0.5 * (1.0 - std::cos(kPi * (static_cast<double>(k) + 0.5) / 480.0));
            const int32_t expL = static_cast<int32_t>(std::llround(static_cast<double>(rawL) * gain));
            const int32_t expR = static_cast<int32_t>(std::llround(static_cast<double>(rawR) * gain));
            const int32_t actL = livePcm[k * 2u];
            const int32_t actR = livePcm[k * 2u + 1u];
            const int32_t errL = std::abs(actL - expL);
            const int32_t errR = std::abs(actR - expR);
            maxFadeErr = std::max({ maxFadeErr, errL, errR });
            if (errL > 2 || errR > 2 || std::abs(actL) >= 32767 || std::abs(actR) >= 32767) {
                std::printf("[FAIL] C0-4d: cosine fade-in mismatch at k=%u (got=%d,%d exp=%d,%d)\n",
                            k, actL, actR, expL, expR);
                return false;
            }
        }

        for (uint32_t k = 480u; k < 4608u; ++k) {
            uint64_t dec = 0;
            if (!stagec::DecodeFrameCode(livePcm[k * 2u], livePcm[k * 2u + 1u], gen, dec) ||
                dec != ((f0 + k) % stagec::kFrameCodeMod)) {
                std::printf("[FAIL] C0-4d: post-fade frame %u failed exact DecodeFrameCode\n", k);
                return false;
            }
        }
        std::printf("  [4d] FadeMs=10 cosine envelope verified (0..479 maxErr=%d LSB, 480..4607 0 bit errors).\n",
                    maxFadeErr);
    }

    std::printf("[PASS] Test 4 (C0-4)\n");
    return true;
}

// ============================================================================
// Test 5 (C0-5): Stall Underrun, Lag Overrun, Drift Watermark & TerminateProcess Kill
// ============================================================================
bool RunTest5_FaultResilience(const std::wstring& producerExe) {
    std::printf("\n=== Test 5 (C0-5): Stall Underrun, Lag Overrun, Drift Watermark & Abrupt Kill ===\n");
    auto& mgr = probe::StageCIpcManager::Instance();
    mgr.Shutdown();

    // 5a: Burst stall underrun & recovery (--burst-stall-ms 1200 --stall-at-ms 400)
    {
        const std::wstring mapName = MakeUniqueMappingName(501);
        const std::wstring readyEv = MakeUniqueEventName(L"Rdy5a", 501);
        const std::wstring stopEv = MakeUniqueEventName(L"Stp5a", 501);
        HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());
        HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, stopEv.c_str());

        ProducerProcess prod;
        if (!prod.Launch(producerExe,
                       L"--mapping-name " + QuoteArg(mapName) +
                       L" --pattern framecode --prefill-sec 0.6 --duration-sec 7"
                       L" --burst-stall-ms 1200 --stall-at-ms 400"
                       L" --ready-event-name " + QuoteArg(readyEv) +
                       L" --stop-event-name " + QuoteArg(stopEv))) {
            CloseHandle(hReady);
            CloseHandle(hStop);
            return false;
        }
        if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] C0-5a: producer did not signal ready\n");
            prod.TerminateAndWait(1);
            CloseHandle(hReady);
            CloseHandle(hStop);
            return false;
        }

        mgr.Configure(MakeBaseConfig(mapName, 500u, 10u, 500u, true));
        mgr.NotifyReaderOpened(1);

        std::vector<int16_t> fullAudio;
        fullAudio.reserve(140 * 1152 * 2);
        bool sawUnderrunGap = false;
        bool recoveredAfterUnderrun = false;
        uint32_t validDecodedPostRecovery = 0;
        uint64_t gen = 0;

        for (int iter = 0; iter < 140; ++iter) {
            std::vector<int16_t> buf(1152 * 2, 0);
            auto r = mgr.PullFramesForReader(1, 222ULL + iter, 1152, buf.data());
            if (r.sessionGeneration != 0) gen = r.sessionGeneration;
            fullAudio.insert(fullAudio.end(), buf.begin(), buf.end());

            if (r.silenceFramesDelivered > 0 && iter > 2) {
                sawUnderrunGap = true;
            }
            if (sawUnderrunGap && r.liveFramesDelivered == 1152u) {
                uint32_t cleanCount = 0;
                for (uint32_t f = 480u; f < 1152u; ++f) {
                    uint64_t dec = 0;
                    if (stagec::DecodeFrameCode(buf[f * 2u], buf[f * 2u + 1u], gen, dec)) {
                        ++cleanCount;
                    }
                }
                if (cleanCount == (1152u - 480u)) {
                    recoveredAfterUnderrun = true;
                    validDecodedPostRecovery += cleanCount;
                }
            }
            Sleep(24);
        }

        stagec::StageCIpcRingHeader snap{};
        mgr.ReadSharedHeaderSnapshot(snap);

        SetEvent(hStop);
        DWORD exitCode = STILL_ACTIVE;
        prod.WaitExit(5000, exitCode);
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        CloseHandle(hReady);
        CloseHandle(hStop);

        // Verify smooth fade-out transitions (no harsh step click into silence)
        int32_t maxDeltaIntoZero = 0;
        const size_t totalFrames = fullAudio.size() / 2;
        for (size_t f = 1; f < totalFrames; ++f) {
            if (fullAudio[f * 2] == 0 && fullAudio[f * 2 + 1] == 0 &&
                (fullAudio[(f - 1) * 2] != 0 || fullAudio[(f - 1) * 2 + 1] != 0)) {
                const int32_t dl = std::abs(static_cast<int32_t>(fullAudio[(f - 1) * 2]));
                const int32_t dr = std::abs(static_cast<int32_t>(fullAudio[(f - 1) * 2 + 1]));
                maxDeltaIntoZero = std::max({ maxDeltaIntoZero, dl, dr });
            }
        }

        if (snap.underrun_events < 1ULL || snap.underrun_frames == 0ULL ||
            !sawUnderrunGap || !recoveredAfterUnderrun || maxDeltaIntoZero >= 18000) {
            std::printf("[FAIL] C0-5a: underrun/recovery check failed (events=%llu frames=%llu gap=%d rec=%d maxDeltaToZero=%d)\n",
                        static_cast<unsigned long long>(snap.underrun_events),
                        static_cast<unsigned long long>(snap.underrun_frames),
                        sawUnderrunGap ? 1 : 0, recoveredAfterUnderrun ? 1 : 0, maxDeltaIntoZero);
            return false;
        }
        std::printf("  [5a] Burst stall underrun (%llu events, %llu frames) faded cleanly (maxDeltaToZero=%d) & recovered (%u post-fade frames).\n",
                    static_cast<unsigned long long>(snap.underrun_events),
                    static_cast<unsigned long long>(snap.underrun_frames),
                    maxDeltaIntoZero, validDecodedPostRecovery);
    }

    // 5b: Lag Overrun (> kRingCapacityFrames ahead) -> resync & clean FrameCode decode
    {
        const std::wstring mapName = MakeUniqueMappingName(502);
        SyntheticMapping m;
        if (!m.Create(mapName, stagec::kTotalSharedBytes)) return false;

        m.InitValidHeader(2ULL, 5002ULL, 100000ULL);
        m.WriteFrameCodeRange(0ULL, 100000ULL, 2ULL);

        mgr.Configure(MakeBaseConfig(mapName, 500u, 0u));
        mgr.NotifyReaderOpened(1);

        std::vector<int16_t> buf1(1152 * 2, 0);
        auto r1 = mgr.PullFramesForReader(1, 222, 1152, buf1.data());
        if (!r1.producerOnline || r1.liveFramesDelivered != 1152u) {
            std::printf("[FAIL] C0-5b: initial pull failed\n");
            mgr.Shutdown();
            return false;
        }

        // Advance producer > kRingCapacityFrames ahead of liveReadFrame
        const uint64_t newCommit = r1.liveReadFrameStart + 1152ULL + stagec::kRingCapacityFrames + 60000ULL;
        const uint64_t writeStart = newCommit - 100000ULL;
        m.WriteFrameCodeRange(writeStart, 100000ULL, 2ULL);
        stagec::AtomicStoreU64(&m.block->header.write_reserve_frame, newCommit);
        stagec::AtomicStoreU64(&m.block->header.write_commit_frame, newCommit);
        m.RefreshHeartbeat();

        std::vector<int16_t> buf2(1152 * 2, 0);
        auto r2 = mgr.PullFramesForReader(1, 223, 1152, buf2.data());
        stagec::StageCIpcRingHeader snap{};
        mgr.ReadSharedHeaderSnapshot(snap);
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        m.Close();

        bool resyncClean = (snap.overrun_events >= 1ULL && snap.overrun_skipped_frames > 0ULL &&
                            r2.producerOnline && r2.liveFramesDelivered == 1152u &&
                            r2.liveReadFrameStart >= writeStart);
        uint64_t prevMod = 0;
        if (resyncClean) {
            for (uint32_t f = 0; f < 1152u; ++f) {
                uint64_t dec = 0;
                if (!stagec::DecodeFrameCode(buf2[f * 2u], buf2[f * 2u + 1u], 2ULL, dec)) {
                    resyncClean = false;
                    break;
                }
                if (f > 0 && dec != ((prevMod + 1ULL) % stagec::kFrameCodeMod)) {
                    resyncClean = false;
                    break;
                }
                prevMod = dec;
            }
        }
        if (!resyncClean) {
            std::printf("[FAIL] C0-5b: overrun resync failed (events=%llu skipped=%llu live=%u start=%llu)\n",
                        static_cast<unsigned long long>(snap.overrun_events),
                        static_cast<unsigned long long>(snap.overrun_skipped_frames),
                        r2.liveFramesDelivered, static_cast<unsigned long long>(r2.liveReadFrameStart));
            return false;
        }
        std::printf("  [5b] Lag overrun detected (%llu events, %llu skipped frames) & resynced to frame %llu.\n",
                    static_cast<unsigned long long>(snap.overrun_events),
                    static_cast<unsigned long long>(snap.overrun_skipped_frames),
                    static_cast<unsigned long long>(r2.liveReadFrameStart));
    }

    // 5c: +1500 ppm clock drift over ~3s at exact 1.0x QPC-paced pulls
    {
        const std::wstring mapName = MakeUniqueMappingName(503);
        const std::wstring readyEv = MakeUniqueEventName(L"Rdy5c", 503);
        const std::wstring stopEv = MakeUniqueEventName(L"Stp5c", 503);
        HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());
        HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, stopEv.c_str());

        ProducerProcess prod;
        if (!prod.Launch(producerExe,
                       L"--mapping-name " + QuoteArg(mapName) +
                       L" --pattern framecode --prefill-sec 1.0 --duration-sec 6 --freq-drift-ppm 1500"
                       L" --ready-event-name " + QuoteArg(readyEv) +
                       L" --stop-event-name " + QuoteArg(stopEv))) {
            CloseHandle(hReady);
            CloseHandle(hStop);
            return false;
        }
        if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] C0-5c: producer did not signal ready\n");
            prod.TerminateAndWait(1);
            CloseHandle(hReady);
            CloseHandle(hStop);
            return false;
        }

        mgr.Configure(MakeBaseConfig(mapName, 500u, 0u));
        mgr.NotifyReaderOpened(1);

        LARGE_INTEGER freq{}, nextTick{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&nextTick);
        const int64_t stepQpc = (freq.QuadPart * 24LL) / 1000LL; // exact 24.0ms = 1152 frames at 48kHz

        uint64_t initialWatermark = 0;
        uint64_t finalWatermark = 0;
        uint64_t maxFirstWindow = 0;
        uint64_t maxLastWindow = 0;
        uint64_t sumFirstWindow = 0;
        uint64_t sumLastWindow = 0;
        constexpr int kTotalIters = 170; // ~4.08s at 24ms/iter (+294 frames drift at +1500ppm)
        constexpr int kWindowIters = 25;
        uint64_t expectedMod = 0;
        bool driftOk = true;
        uint64_t gen = 0;

        for (int iter = 0; iter < kTotalIters; ++iter) {
            std::vector<int16_t> buf(1152 * 2, 0);
            auto r = mgr.PullFramesForReader(1, 222ULL + iter, 1152, buf.data());
            if (!r.producerOnline || r.liveFramesDelivered != 1152u) {
                driftOk = false;
                break;
            }
            if (iter == 0) {
                initialWatermark = r.watermarkFrames;
                gen = r.sessionGeneration;
            }
            finalWatermark = r.watermarkFrames;
            if (iter < kWindowIters) {
                maxFirstWindow = std::max(maxFirstWindow, r.watermarkFrames);
                sumFirstWindow += r.watermarkFrames;
            }
            if (iter >= kTotalIters - kWindowIters) {
                maxLastWindow = std::max(maxLastWindow, r.watermarkFrames);
                sumLastWindow += r.watermarkFrames;
            }
            for (uint32_t f = 0; f < 1152u; ++f) {
                uint64_t dec = 0;
                if (!stagec::DecodeFrameCode(buf[f * 2u], buf[f * 2u + 1u], gen, dec)) {
                    driftOk = false;
                    break;
                }
                if (!(iter == 0 && f == 0) && dec != expectedMod) {
                    driftOk = false;
                    break;
                }
                expectedMod = (dec + 1ULL) % stagec::kFrameCodeMod;
            }
            if (!driftOk) break;

            nextTick.QuadPart += stepQpc;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const int64_t remMs = ((nextTick.QuadPart - now.QuadPart) * 1000LL) / freq.QuadPart;
            if (remMs > 2) {
                Sleep(static_cast<DWORD>(remMs - 2));
            }
            do {
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < nextTick.QuadPart);
        }

        SetEvent(hStop);
        DWORD exitCode = STILL_ACTIVE;
        prod.WaitExit(5000, exitCode);
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        CloseHandle(hReady);
        CloseHandle(hStop);

        const double avgFirst = static_cast<double>(sumFirstWindow) / static_cast<double>(kWindowIters);
        const double avgLast = static_cast<double>(sumLastWindow) / static_cast<double>(kWindowIters);
        if (!driftOk || maxLastWindow <= maxFirstWindow || avgLast <= avgFirst) {
            std::printf("[FAIL] C0-5c: drift watermark check failed (ok=%d init=%llu final=%llu max=%llu->%llu avg=%.1f->%.1f)\n",
                        driftOk ? 1 : 0,
                        static_cast<unsigned long long>(initialWatermark),
                        static_cast<unsigned long long>(finalWatermark),
                        static_cast<unsigned long long>(maxFirstWindow),
                        static_cast<unsigned long long>(maxLastWindow),
                        avgFirst, avgLast);
            return false;
        }
        std::printf("  [5c] +1500 ppm drift over 4.1s reflected in watermark (avg %.1f -> %.1f, peak %llu -> %llu, 0 errors).\n",
                    avgFirst, avgLast,
                    static_cast<unsigned long long>(maxFirstWindow),
                    static_cast<unsigned long long>(maxLastWindow));
    }

    // 5d: TerminateProcess hard kill -> fade-out to exact (0, 0) within 480 frames, buffers 2 & 3 100% silence
    {
        const std::wstring mapName = MakeUniqueMappingName(504);
        const std::wstring readyEv = MakeUniqueEventName(L"Rdy5d", 504);
        HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());

        ProducerProcess prod;
        if (!prod.Launch(producerExe,
                       L"--mapping-name " + QuoteArg(mapName) +
                       L" --pattern framecode --prefill-sec 1.5 --duration-sec 20 --ready-event-name " +
                       QuoteArg(readyEv))) {
            CloseHandle(hReady);
            return false;
        }
        if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] C0-5d: producer did not signal ready\n");
            prod.TerminateAndWait(1);
            CloseHandle(hReady);
            return false;
        }

        mgr.Configure(MakeBaseConfig(mapName, 1000u, 10u, 500u, true));
        mgr.NotifyReaderOpened(1);

        std::vector<int16_t> live1(1152 * 2, 0), live2(1152 * 2, 0);
        auto r1 = mgr.PullFramesForReader(1, 222, 1152, live1.data());
        auto r2 = mgr.PullFramesForReader(1, 223, 1152, live2.data());
        if (!r1.producerOnline || !r2.producerOnline || r2.liveFramesDelivered != 1152u) {
            std::printf("[FAIL] C0-5d: initial live pulls failed\n");
            prod.TerminateAndWait(1);
            mgr.Shutdown();
            CloseHandle(hReady);
            return false;
        }

        // Verify non-zero audio before kill
        const int16_t preKillL = live2[1151 * 2];
        const int16_t preKillR = live2[1151 * 2 + 1];
        if (preKillL == 0 && preKillR == 0) {
            std::printf("[FAIL] C0-5d: last sample before kill was unexpectedly zero\n");
            prod.TerminateAndWait(1);
            mgr.Shutdown();
            CloseHandle(hReady);
            return false;
        }

        // Abruptly kill producer leaving producer_state == Streaming
        prod.TerminateAndWait(1);
        Sleep(650); // > HeartbeatTimeoutMs (500ms)

        std::vector<int16_t> d1(1152 * 2, 0), d2(1152 * 2, 0), d3(1152 * 2, 0);
        auto rd1 = mgr.PullFramesForReader(1, 224, 1152, d1.data());
        auto rd2 = mgr.PullFramesForReader(1, 225, 1152, d2.data());
        auto rd3 = mgr.PullFramesForReader(1, 226, 1152, d3.data());

        stagec::StageCIpcRingHeader snap{};
        mgr.ReadSharedHeaderSnapshot(snap);
        mgr.NotifyReaderEofOrClosed(1);
        mgr.Shutdown();
        CloseHandle(hReady);

        // Check smooth fade in d1[0..479] from (preKillL, preKillR) to (0,0), and exact 0 from 480..1151 + d2 + d3
        int32_t maxStepDelta = 0;
        int16_t prevL = preKillL, prevR = preKillR;
        for (uint32_t f = 0; f < 480u; ++f) {
            const int16_t l = d1[f * 2u];
            const int16_t r = d1[f * 2u + 1u];
            maxStepDelta = std::max({ maxStepDelta,
                                      std::abs(static_cast<int32_t>(l) - static_cast<int32_t>(prevL)),
                                      std::abs(static_cast<int32_t>(r) - static_cast<int32_t>(prevR)) });
            prevL = l;
            prevR = r;
        }

        const bool tailZero1 = AllSamplesExactZero(d1.data() + 480u * 2u, 1152u - 480u);
        const bool allZero2 = AllSamplesExactZero(d2.data(), 1152u);
        const bool allZero3 = AllSamplesExactZero(d3.data(), 1152u);

        if (rd1.producerOnline || rd2.producerOnline || rd3.producerOnline ||
            !tailZero1 || !allZero2 || !allZero3 || maxStepDelta >= 18000 ||
            snap.silence_disconnect_frames == 0ULL) {
            std::printf("[FAIL] C0-5d: post-kill disconnect silence check failed (online=%d,%d,%d tail1=%d z2=%d z3=%d maxDelta=%d discFrames=%llu)\n",
                        rd1.producerOnline ? 1 : 0, rd2.producerOnline ? 1 : 0, rd3.producerOnline ? 1 : 0,
                        tailZero1 ? 1 : 0, allZero2 ? 1 : 0, allZero3 ? 1 : 0,
                        maxStepDelta, static_cast<unsigned long long>(snap.silence_disconnect_frames));
            return false;
        }
        std::printf("  [5d] TerminateProcess kill -> smooth fade (maxDelta=%d) to 100%% silence (discFrames=%llu).\n",
                    maxStepDelta, static_cast<unsigned long long>(snap.silence_disconnect_frames));
    }

    std::printf("[PASS] Test 5 (C0-5)\n");
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring explicitProducer;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--producer-exe") == 0) {
            if (i + 1 >= argc) {
                std::printf("[FAIL] Missing path after --producer-exe\n");
                return 2;
            }
            explicitProducer = argv[++i];
        } else {
            std::printf("Usage: ipc_protocol_selftest.exe [--producer-exe <path_to_iv_stage_c_producer.exe>]\n");
            return 2;
        }
    }

    const std::wstring producerExe = FindProducerExe(explicitProducer);
    if (producerExe.empty()) {
        std::printf("[FAIL] Could not locate iv_stage_c_producer.exe (explicit=%ls).\n",
                    explicitProducer.empty() ? L"<auto>" : explicitProducer.c_str());
        return 2;
    }
    std::printf("Using x64 producer: %ls\n", producerExe.c_str());

    timeBeginPeriod(1);

    int failures = 0;
    if (!RunTest1_LayoutFrameCodeAndMalformed()) ++failures;
    if (!RunTest2_OverwriteRace(producerExe)) ++failures;
    if (!RunTest3_MutexCrashAndTakeover(producerExe)) ++failures;
    if (!RunTest4_StartupAndFadeSeparation(producerExe)) ++failures;
    if (!RunTest5_FaultResilience(producerExe)) ++failures;

    timeEndPeriod(1);

    std::printf("\n=== Stage C IPC Protocol Selftest Summary: %s (%d failed test(s)) ===\n",
                failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
