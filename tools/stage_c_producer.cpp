#include "ipc_ring_protocol.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

struct ProducerOptions {
    std::wstring mappingName = stagec::kDefaultMappingName;
    stagec::ProducerPattern pattern = stagec::ProducerPattern::Tone;
    double prefillSec = 0.0;
    double durationSec = 0.0;
    double freqDriftPpm = 0.0;
    uint32_t burstStallMs = 0;
    uint32_t stallAtMs = 0;
    std::wstring stopEventName;
    std::wstring readyEventName;
    bool testCrashMidInit = false;
    bool testPauseBeforeCommitAtWrap = false;
    std::wstring pauseEventName;
    std::wstring resumeEventName;
};

bool ParseDoubleArg(const wchar_t* s, double& outVal) {
    if (!s || !*s) return false;
    wchar_t* endPtr = nullptr;
    double v = std::wcstod(s, &endPtr);
    if (endPtr == s || *endPtr != L'\0' || !std::isfinite(v)) return false;
    outVal = v;
    return true;
}

bool ParseU32Arg(const wchar_t* s, uint32_t& outVal) {
    if (!s || !*s) return false;
    wchar_t* endPtr = nullptr;
    unsigned long long v = std::wcstoull(s, &endPtr, 10);
    if (endPtr == s || *endPtr != L'\0' || v > 0xFFFFFFFFULL) return false;
    outVal = static_cast<uint32_t>(v);
    return true;
}

void PrintUsage() {
    std::printf(
        "Usage: iv_stage_c_producer.exe [options]\n"
        "  --mapping-name <wstr>\n"
        "  --pattern <tone|framecode>\n"
        "  --prefill-sec <double>\n"
        "  --duration-sec <double>\n"
        "  --freq-drift-ppm <double>\n"
        "  --burst-stall-ms <uint32>\n"
        "  --stall-at-ms <uint32>\n"
        "  --stop-event-name <wstr>\n"
        "  --ready-event-name <wstr>\n"
        "  --test-crash-mid-init\n"
        "  --test-pause-before-commit-at-wrap <pause_event> <resume_event>\n");
}

bool ParseCommandLine(int argc, wchar_t** argv, ProducerOptions& opts) {
    for (int i = 1; i < argc; ++i) {
        const wchar_t* arg = argv[i];
        if (_wcsicmp(arg, L"--help") == 0 || _wcsicmp(arg, L"-h") == 0) {
            PrintUsage();
            return false;
        } else if (_wcsicmp(arg, L"--mapping-name") == 0 && i + 1 < argc) {
            opts.mappingName = argv[++i];
        } else if (_wcsicmp(arg, L"--pattern") == 0 && i + 1 < argc) {
            const wchar_t* p = argv[++i];
            if (_wcsicmp(p, L"tone") == 0) {
                opts.pattern = stagec::ProducerPattern::Tone;
            } else if (_wcsicmp(p, L"framecode") == 0) {
                opts.pattern = stagec::ProducerPattern::FrameCode;
            } else {
                std::fprintf(stderr, "Invalid --pattern value: %ls\n", p);
                return false;
            }
        } else if (_wcsicmp(arg, L"--prefill-sec") == 0 && i + 1 < argc) {
            if (!ParseDoubleArg(argv[++i], opts.prefillSec) || opts.prefillSec < 0.0) {
                std::fprintf(stderr, "Invalid --prefill-sec value\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--duration-sec") == 0 && i + 1 < argc) {
            if (!ParseDoubleArg(argv[++i], opts.durationSec) || opts.durationSec < 0.0) {
                std::fprintf(stderr, "Invalid --duration-sec value\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--freq-drift-ppm") == 0 && i + 1 < argc) {
            if (!ParseDoubleArg(argv[++i], opts.freqDriftPpm)) {
                std::fprintf(stderr, "Invalid --freq-drift-ppm value\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--burst-stall-ms") == 0 && i + 1 < argc) {
            if (!ParseU32Arg(argv[++i], opts.burstStallMs)) {
                std::fprintf(stderr, "Invalid --burst-stall-ms value\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--stall-at-ms") == 0 && i + 1 < argc) {
            if (!ParseU32Arg(argv[++i], opts.stallAtMs)) {
                std::fprintf(stderr, "Invalid --stall-at-ms value\n");
                return false;
            }
        } else if (_wcsicmp(arg, L"--stop-event-name") == 0 && i + 1 < argc) {
            opts.stopEventName = argv[++i];
        } else if (_wcsicmp(arg, L"--ready-event-name") == 0 && i + 1 < argc) {
            opts.readyEventName = argv[++i];
        } else if (_wcsicmp(arg, L"--test-crash-mid-init") == 0) {
            opts.testCrashMidInit = true;
        } else if (_wcsicmp(arg, L"--test-pause-before-commit-at-wrap") == 0 && i + 2 < argc) {
            opts.testPauseBeforeCommitAtWrap = true;
            opts.pauseEventName = argv[++i];
            opts.resumeEventName = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument: %ls\n", arg);
            return false;
        }
    }
    return true;
}

void GenerateToneFrame(uint64_t frameIdx, int16_t& outL, int16_t& outR) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTwoPi = 2.0 * kPi;
    const double t = static_cast<double>(frameIdx) / static_cast<double>(stagec::kSampleRate);

    // Warm dual-frequency stereo chord + 5s chime marker
    const double baseL = 0.55 * std::sin(kTwoPi * 220.0 * t) + 0.30 * std::sin(kTwoPi * 329.6276 * t);
    const double baseR = 0.55 * std::sin(kTwoPi * 220.0 * t) + 0.30 * std::sin(kTwoPi * 440.0 * t);

    const uint64_t mod5s = frameIdx % (stagec::kSampleRate * 5ULL);
    double chime = 0.0;
    if (mod5s < 14400ULL) { // 300ms chime every 5 seconds
        const double x = static_cast<double>(mod5s) / 14400.0;
        const double env = std::sin(kPi * x) * std::exp(-3.0 * x);
        chime = 0.35 * env * std::sin(kTwoPi * 880.0 * t);
    }

    const long long l = std::clamp(std::llround(12000.0 * (baseL + chime)), -28000LL, 28000LL);
    const long long r = std::clamp(std::llround(12000.0 * (baseR + chime)), -28000LL, 28000LL);
    outL = static_cast<int16_t>(l);
    outR = static_cast<int16_t>(r);
}

void WriteFramesToRing(stagec::StageCIpcSharedBlock* shared,
                       uint64_t startFrame,
                       uint32_t frameCount,
                       uint64_t sessionGeneration,
                       stagec::ProducerPattern pattern) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        const uint64_t f = startFrame + static_cast<uint64_t>(i);
        const uint32_t slot = static_cast<uint32_t>(f & stagec::kRingFrameMask);
        int16_t l = 0;
        int16_t r = 0;
        if (pattern == stagec::ProducerPattern::FrameCode) {
            stagec::EncodeFrameCode(f, sessionGeneration, l, r);
        } else {
            GenerateToneFrame(f, l, r);
        }
        shared->pcm_samples[static_cast<size_t>(slot) * 2u] = l;
        shared->pcm_samples[static_cast<size_t>(slot) * 2u + 1u] = r;
    }
}

void SignalNamedEvent(const std::wstring& eventName) {
    if (eventName.empty()) return;
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
    if (eventName.empty()) return nullptr;
    HANDLE hEvt = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (!hEvt) {
        hEvt = CreateEventW(nullptr, manualReset ? TRUE : FALSE, FALSE, eventName.c_str());
    }
    return hEvt;
}

bool ShouldStopNow(HANDLE hStopEvent) {
    if (InterlockedCompareExchange(&g_ctrlStopRequested, 0, 0) != 0) {
        return true;
    }
    if (hStopEvent && WaitForSingleObject(hStopEvent, 0) == WAIT_OBJECT_0) {
        return true;
    }
    return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    ProducerOptions opts;
    if (!ParseCommandLine(argc, argv, opts)) {
        return 1;
    }

    SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    const std::wstring mutexName = opts.mappingName + stagec::kProducerMutexSuffix;
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!hMutex) {
        std::fprintf(stderr, "CreateMutexW(%ls) failed: err=%lu\n",
                     mutexName.c_str(), static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    const DWORD waitRes = WaitForSingleObject(hMutex, 0);
    if (waitRes == WAIT_TIMEOUT) {
        std::fprintf(stderr, "Another producer already holds mutex %ls; exiting with code 2.\n",
                     mutexName.c_str());
        CloseHandle(hMutex);
        return 2;
    }
    if (waitRes != WAIT_OBJECT_0 && waitRes != WAIT_ABANDONED) {
        std::fprintf(stderr, "WaitForSingleObject(%ls) failed: res=%lu err=%lu\n",
                     mutexName.c_str(),
                     static_cast<unsigned long>(waitRes),
                     static_cast<unsigned long>(GetLastError()));
        CloseHandle(hMutex);
        return 1;
    }

    HANDLE hMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        stagec::kTotalSharedBytes,
        opts.mappingName.c_str());
    if (!hMapping) {
        std::fprintf(stderr, "CreateFileMappingW(%ls) failed: err=%lu\n",
                     opts.mappingName.c_str(), static_cast<unsigned long>(GetLastError()));
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    void* view = MapViewOfFile(hMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!view) {
        std::fprintf(stderr, "MapViewOfFile failed: err=%lu\n", static_cast<unsigned long>(GetLastError()));
        CloseHandle(hMapping);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < stagec::kTotalSharedBytes) {
        std::fprintf(stderr, "VirtualQuery region size check failed (size=%zu, expected>=%u).\n",
                     mbi.RegionSize, static_cast<unsigned>(stagec::kTotalSharedBytes));
        UnmapViewOfFile(view);
        CloseHandle(hMapping);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    auto* shared = static_cast<stagec::StageCIpcSharedBlock*>(view);
    stagec::StageCIpcRingHeader* hdr = &shared->header;

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    LARGE_INTEGER startQpc{};
    QueryPerformanceCounter(&startQpc);

    // Odd-to-Even generation takeover: publish oddGen FIRST before touching static/hot fields
    const uint64_t prevGen = stagec::AtomicLoadU64(&hdr->session_generation);
    const uint64_t oddGen = stagec::NextOddInitGeneration(prevGen);
    const uint64_t evenGen = oddGen + 1ULL;

    stagec::AtomicStoreU64(&hdr->session_generation, oddGen);
    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Starting));

    if (opts.testCrashMidInit) {
        std::printf("Simulating crash mid-init with odd session_generation=%llu.\n",
                    static_cast<unsigned long long>(oddGen));
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 99u);
    }

    const uint64_t sessionId =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
        static_cast<uint64_t>(startQpc.QuadPart);

    // Cache Line 0 static fields
    stagec::AtomicStore32(&hdr->magic, stagec::kIpcMagic);
    stagec::AtomicStore32(&hdr->version, stagec::kIpcVersion);
    stagec::AtomicStore32(&hdr->header_bytes, stagec::kHeaderBytes);
    stagec::AtomicStore32(&hdr->total_bytes, stagec::kTotalSharedBytes);
    stagec::AtomicStore32(&hdr->sample_rate, stagec::kSampleRate);
    stagec::AtomicStore32(&hdr->channels, stagec::kChannels);
    stagec::AtomicStore32(&hdr->bits_per_sample, stagec::kBitsPerSample);
    stagec::AtomicStore32(&hdr->capacity_frames, stagec::kRingCapacityFrames);
    stagec::AtomicStoreU64(&hdr->session_id, sessionId);
    stagec::AtomicStoreU64(&hdr->qpc_freq, static_cast<uint64_t>(qpcFreq.QuadPart));
    stagec::AtomicStore32(&hdr->producer_pid, GetCurrentProcessId());
    stagec::AtomicStore32(&hdr->producer_arch, sizeof(void*) == 8 ? 64u : 32u);

    // Cache Line 1 producer hot state
    stagec::AtomicStoreU64(&hdr->write_reserve_frame, 0ULL);
    stagec::AtomicStoreU64(&hdr->write_commit_frame, 0ULL);
    stagec::AtomicStoreI64(&hdr->start_qpc, startQpc.QuadPart);
    stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, startQpc.QuadPart);
    stagec::AtomicStoreU64(&hdr->producer_tick_count, 0ULL);
    stagec::AtomicStore32(&hdr->pattern_mode, static_cast<uint32_t>(opts.pattern));
    stagec::AtomicStoreU64(&hdr->reserved_cl1[0], 0ULL);
    stagec::AtomicStoreU64(&hdr->reserved_cl1[1], 0ULL);

    uint64_t writtenFrames = 0ULL;
    uint64_t prefillFrames = 0ULL;

    if (opts.testPauseBeforeCommitAtWrap) {
        prefillFrames = stagec::kRingCapacityFrames;
        constexpr uint32_t kBatch = 16384u;
        for (uint64_t off = 0; off < prefillFrames; off += kBatch) {
            const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(kBatch, prefillFrames - off));
            WriteFramesToRing(shared, off, chunk, evenGen, opts.pattern);
        }
        stagec::AtomicStoreU64(&hdr->write_commit_frame, stagec::kRingCapacityFrames);
        stagec::AtomicStoreU64(&hdr->write_reserve_frame, stagec::kRingCapacityFrames + 480ULL);
        for (uint32_t i = 0; i < 480u; ++i) {
            shared->pcm_samples[static_cast<size_t>(i) * 2u] = static_cast<int16_t>(0x5555);
            shared->pcm_samples[static_cast<size_t>(i) * 2u + 1u] = static_cast<int16_t>(-0x5555);
        }
        writtenFrames = stagec::kRingCapacityFrames;
    } else if (opts.prefillSec > 0.0) {
        const double rawPrefill = opts.prefillSec * static_cast<double>(stagec::kSampleRate);
        const uint64_t clampedPrefill = std::min<uint64_t>(
            static_cast<uint64_t>(std::llround(rawPrefill)),
            static_cast<uint64_t>(stagec::kRingCapacityFrames));
        if (clampedPrefill > 0ULL) {
            stagec::AtomicStoreU64(&hdr->write_reserve_frame, clampedPrefill);
            constexpr uint32_t kBatch = 16384u;
            for (uint64_t off = 0; off < clampedPrefill; off += kBatch) {
                const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(kBatch, clampedPrefill - off));
                WriteFramesToRing(shared, off, chunk, evenGen, opts.pattern);
            }
            stagec::AtomicStoreU64(&hdr->write_commit_frame, clampedPrefill);
            writtenFrames = clampedPrefill;
            prefillFrames = clampedPrefill;
        }
    }

    QueryPerformanceCounter(&startQpc);
    stagec::AtomicStoreI64(&hdr->start_qpc, startQpc.QuadPart);
    stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, startQpc.QuadPart);
    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Streaming));

    // Publish evenGen LAST so consumers observe a consistent initialized snapshot
    stagec::AtomicStoreU64(&hdr->session_generation, evenGen);
    SignalNamedEvent(opts.readyEventName);

    HANDLE hStopEvent = OpenOrCreateNamedEvent(opts.stopEventName, true);

    if (opts.testPauseBeforeCommitAtWrap) {
        SignalNamedEvent(opts.pauseEventName);
        HANDLE hResume = OpenOrCreateNamedEvent(opts.resumeEventName, true);
        if (hResume) {
            const DWORD maxWaitMs = 5000u;
            DWORD waitedMs = 0u;
            while (waitedMs < maxWaitMs) {
                if (ShouldStopNow(hStopEvent)) {
                    break;
                }
                const DWORD step = std::min<DWORD>(20u, maxWaitMs - waitedMs);
                if (WaitForSingleObject(hResume, step) == WAIT_OBJECT_0) {
                    break;
                }
                waitedMs += step;
                LARGE_INTEGER hbNow{};
                QueryPerformanceCounter(&hbNow);
                stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, hbNow.QuadPart);
            }
            CloseHandle(hResume);
        }
        WriteFramesToRing(shared, stagec::kRingCapacityFrames, 480u, evenGen, opts.pattern);
        writtenFrames = stagec::kRingCapacityFrames + 480ULL;
        prefillFrames = writtenFrames;
        stagec::AtomicStoreU64(&hdr->write_commit_frame, writtenFrames);
        QueryPerformanceCounter(&startQpc);
    }

    timeBeginPeriod(1);

    const double effectiveRate = static_cast<double>(stagec::kSampleRate) * (1.0 + opts.freqDriftPpm * 1e-6);
    bool stallDone = (opts.burstStallMs == 0u);

    std::printf("Stage C Producer streaming: mapping=%ls gen=%llu arch=%u pattern=%s prefill=%llu frames\n",
                opts.mappingName.c_str(),
                static_cast<unsigned long long>(evenGen),
                static_cast<unsigned>(sizeof(void*) == 8 ? 64u : 32u),
                (opts.pattern == stagec::ProducerPattern::FrameCode) ? "framecode" : "tone",
                static_cast<unsigned long long>(prefillFrames));
    std::fflush(stdout);

    while (!ShouldStopNow(hStopEvent)) {
        LARGE_INTEGER nowQpc{};
        QueryPerformanceCounter(&nowQpc);
        const double elapsedSec = static_cast<double>(nowQpc.QuadPart - startQpc.QuadPart) /
                                  static_cast<double>(qpcFreq.QuadPart);
        const uint64_t elapsedMs = static_cast<uint64_t>(std::max(0.0, elapsedSec * 1000.0));

        if (opts.durationSec > 0.0 && elapsedSec >= opts.durationSec) {
            break;
        }

        if (!stallDone && elapsedMs >= static_cast<uint64_t>(opts.stallAtMs)) {
            stallDone = true;
            DWORD remainingStall = opts.burstStallMs;
            while (remainingStall > 0 && !ShouldStopNow(hStopEvent)) {
                const DWORD chunkSleep = std::min<DWORD>(remainingStall, 20u);
                Sleep(chunkSleep);
                remainingStall -= chunkSleep;
                LARGE_INTEGER hbStall{};
                QueryPerformanceCounter(&hbStall);
                stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, hbStall.QuadPart);
            }
            continue;
        }

        const uint64_t targetFrames = prefillFrames + static_cast<uint64_t>(std::max(0.0, elapsedSec * effectiveRate));
        if (targetFrames > writtenFrames) {
            uint64_t pending = targetFrames - writtenFrames;
            if (pending > stagec::kRingCapacityFrames / 2u) {
                pending = stagec::kRingCapacityFrames / 2u;
            }
            const uint32_t chunk = static_cast<uint32_t>(pending);
            const uint64_t newReserve = writtenFrames + static_cast<uint64_t>(chunk);
            stagec::AtomicStoreU64(&hdr->write_reserve_frame, newReserve);
            WriteFramesToRing(shared, writtenFrames, chunk, evenGen, opts.pattern);
            stagec::AtomicStoreU64(&hdr->write_commit_frame, newReserve);
            writtenFrames = newReserve;
        }

        QueryPerformanceCounter(&nowQpc);
        stagec::AtomicStoreI64(&hdr->last_heartbeat_qpc, nowQpc.QuadPart);
        stagec::AtomicAddU64(&hdr->producer_tick_count, 1ULL);

        Sleep(10);
    }

    timeEndPeriod(1);

    stagec::AtomicStore32(&hdr->producer_state, static_cast<uint32_t>(stagec::ProducerState::Stopped));
    if (hStopEvent) {
        CloseHandle(hStopEvent);
    }

    UnmapViewOfFile(view);
    CloseHandle(hMapping);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}
