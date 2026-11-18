#include <windows.h>
#include <wmsdk.h>

#include "ipc_ring_protocol.h"

#include <algorithm>
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

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime       = 1099511628211ULL;

constexpr uint64_t kExpectedFullSamples = 1877ULL;
constexpr uint64_t kExpectedFullBytes   = 8647104ULL;
constexpr uint64_t kTailScanStart100ns  = 250480000ULL;
constexpr uint64_t kSeekPos1_100ns      = 132270000ULL;
constexpr uint64_t kSeekPos2_100ns      = 79360000ULL;
constexpr uint64_t kOutOfRange100ns     = 9999990000000ULL;

struct DecodeSummary {
    bool ok = false;
    uint64_t duration100ns = 0;
    uint64_t sampleCount = 0;
    uint64_t totalBytes = 0;
    uint64_t fnv1aHash = kFnvOffsetBasis;
    bool comIdentityOk = false;
    ULONG finalRef = 0;
    uint64_t firstPts100ns = 0;
    bool hasFirstPts = false;
    HRESULT lastHr = S_OK;
    std::vector<int16_t> samples;
};

void UpdateFnv1a(uint64_t& h, const BYTE* data, DWORD len) {
    for (DWORD i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnvPrime;
    }
}

void AppendInterleavedSamples(std::vector<int16_t>& dst, const BYTE* data, DWORD lenBytes) {
    const size_t count = lenBytes / sizeof(int16_t);
    if (count == 0) return;
    const size_t oldSize = dst.size();
    dst.resize(oldSize + count);
    std::memcpy(dst.data() + oldSize, data, count * sizeof(int16_t));
}

bool FileExistsRegular(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool WriteAsciiFile(const std::wstring& path, const char* content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::printf("[FAIL] CreateFileW(%ls) failed: err=%lu\n",
                    path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    const DWORD len = static_cast<DWORD>(std::strlen(content));
    DWORD written = 0;
    BOOL ok = (len == 0) ? TRUE : WriteFile(hFile, content, len, &written, nullptr);
    CloseHandle(hFile);
    if (!ok || written != len) {
        std::printf("[FAIL] WriteFile(%ls) failed: err=%lu\n",
                    path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    return true;
}

bool DeleteFileIfPresent(const std::wstring& path) {
    if (!DeleteFileW(path.c_str())) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            std::printf("[FAIL] DeleteFileW(%ls) failed: err=%lu\n",
                        path.c_str(), static_cast<unsigned long>(err));
            return false;
        }
    }
    return true;
}

bool WriteCanonicalPcmWav(const std::wstring& path,
                          uint32_t sampleRate,
                          uint16_t channels,
                          uint16_t bitsPerSample,
                          const std::vector<int16_t>& pcmSamples) {
    const uint64_t dataBytes64 = static_cast<uint64_t>(pcmSamples.size()) * sizeof(int16_t);
    if (dataBytes64 > 0xFFFFFFFFULL - 36ULL) {
        std::printf("[FAIL] WAV payload too large for RIFF header.\n");
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);
    const uint32_t riffChunkSize = 36U + dataBytes;
    const uint32_t fmtChunkSize = 16U;
    const uint16_t audioFormat = 1U;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8U));
    const uint32_t byteRate = sampleRate * static_cast<uint32_t>(blockAlign);

    uint8_t hdr[44]{};
    std::memcpy(hdr + 0, "RIFF", 4);
    std::memcpy(hdr + 4, &riffChunkSize, 4);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    std::memcpy(hdr + 16, &fmtChunkSize, 4);
    std::memcpy(hdr + 20, &audioFormat, 2);
    std::memcpy(hdr + 22, &channels, 2);
    std::memcpy(hdr + 24, &sampleRate, 4);
    std::memcpy(hdr + 28, &byteRate, 4);
    std::memcpy(hdr + 32, &blockAlign, 2);
    std::memcpy(hdr + 34, &bitsPerSample, 2);
    std::memcpy(hdr + 36, "data", 4);
    std::memcpy(hdr + 40, &dataBytes, 4);

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::printf("[FAIL] CreateFileW(%ls) failed: err=%lu\n",
                    path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, hdr, sizeof(hdr), &written, nullptr) && written == sizeof(hdr);
    if (ok && dataBytes > 0) {
        const BYTE* ptr = reinterpret_cast<const BYTE*>(pcmSamples.data());
        DWORD remaining = dataBytes;
        while (remaining > 0) {
            DWORD chunkWritten = 0;
            if (!WriteFile(hFile, ptr, remaining, &chunkWritten, nullptr) || chunkWritten == 0) {
                ok = FALSE;
                break;
            }
            ptr += chunkWritten;
            remaining -= chunkWritten;
        }
    }
    CloseHandle(hFile);
    if (!ok) {
        std::printf("[FAIL] WriteFile(%ls) failed: err=%lu\n",
                    path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    return true;
}

bool ValidateWavFileOnDisk(const std::wstring& path,
                           uint32_t expectedRate,
                           uint16_t expectedChannels,
                           uint16_t expectedBits,
                           uint64_t expectedDataBytes) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::printf("[FAIL] ValidateWavFileOnDisk: cannot open %ls (err=%lu)\n",
                    path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 44) {
        CloseHandle(hFile);
        std::printf("[FAIL] ValidateWavFileOnDisk: %ls too small\n", path.c_str());
        return false;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(fileSize.QuadPart));
    DWORD totalRead = 0;
    while (totalRead < raw.size()) {
        DWORD chunk = 0;
        if (!ReadFile(hFile, raw.data() + totalRead, static_cast<DWORD>(raw.size() - totalRead), &chunk, nullptr) || chunk == 0) {
            CloseHandle(hFile);
            std::printf("[FAIL] ValidateWavFileOnDisk: ReadFile failed on %ls\n", path.c_str());
            return false;
        }
        totalRead += chunk;
    }
    CloseHandle(hFile);

    if (std::memcmp(raw.data(), "RIFF", 4) != 0 || std::memcmp(raw.data() + 8, "WAVE", 4) != 0) {
        std::printf("[FAIL] ValidateWavFileOnDisk: %ls missing RIFF/WAVE\n", path.c_str());
        return false;
    }

    bool gotFmt = false;
    bool gotData = false;
    uint16_t fmtTag = 0, ch = 0, bps = 0, align = 0;
    uint32_t rate = 0, dataBytes = 0;

    size_t pos = 12;
    while (pos + 8 <= raw.size()) {
        const char* id = reinterpret_cast<const char*>(raw.data() + pos);
        uint32_t sz = 0;
        std::memcpy(&sz, raw.data() + pos + 4, 4);
        pos += 8;
        if (pos + sz > raw.size()) break;
        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            std::memcpy(&fmtTag, raw.data() + pos + 0, 2);
            std::memcpy(&ch, raw.data() + pos + 2, 2);
            std::memcpy(&rate, raw.data() + pos + 4, 4);
            std::memcpy(&align, raw.data() + pos + 12, 2);
            std::memcpy(&bps, raw.data() + pos + 14, 2);
            gotFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataBytes = sz;
            gotData = true;
        }
        pos += sz + (sz & 1U);
    }

    bool ok = gotFmt && gotData &&
              fmtTag == 1 &&
              ch == expectedChannels &&
              rate == expectedRate &&
              bps == expectedBits &&
              align == static_cast<uint16_t>(expectedChannels * (expectedBits / 8U)) &&
              static_cast<uint64_t>(dataBytes) == expectedDataBytes;
    if (!ok) {
        std::printf("[FAIL] ValidateWavFileOnDisk(%ls): fmt=%d data=%d tag=%u rate=%lu ch=%u bps=%u align=%u dataBytes=%lu (expected %llu)\n",
                    path.c_str(), gotFmt ? 1 : 0, gotData ? 1 : 0,
                    static_cast<unsigned>(fmtTag), static_cast<unsigned long>(rate),
                    static_cast<unsigned>(ch), static_cast<unsigned>(bps),
                    static_cast<unsigned>(align), static_cast<unsigned long>(dataBytes),
                    static_cast<unsigned long long>(expectedDataBytes));
    }
    return ok;
}

bool FileContainsSubstring(const std::wstring& path, const char* needle) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::printf("[FAIL] FileContainsSubstring: cannot open %ls (err=%lu)\n",
                    path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hFile, &sz) || sz.QuadPart <= 0) {
        CloseHandle(hFile);
        return false;
    }
    std::string text(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD totalRead = 0;
    while (totalRead < text.size()) {
        DWORD chunk = 0;
        if (!ReadFile(hFile, text.data() + totalRead, static_cast<DWORD>(text.size() - totalRead), &chunk, nullptr) || chunk == 0) {
            break;
        }
        totalRead += chunk;
    }
    CloseHandle(hFile);
    text.resize(totalRead);
    return text.find(needle) != std::string::npos;
}

std::vector<int16_t> GenerateValidTestWavPcm(size_t frameCount) {
    std::vector<int16_t> pcm(frameCount * 2, 0);
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTwoPi = 2.0 * kPi;
    constexpr size_t kFadeOutFrames = 4800;

    for (size_t f = 0; f < frameCount; ++f) {
        const double t = static_cast<double>(f) / 48000.0;
        double env = 1.0;
        if (frameCount > kFadeOutFrames && f >= frameCount - kFadeOutFrames) {
            const size_t rem = frameCount - 1 - f;
            env = static_cast<double>(rem) / static_cast<double>(kFadeOutFrames);
        }
        const double lVal = env * (12000.0 * std::sin(kTwoPi * 330.0 * t) + 4500.0 * std::sin(kTwoPi * 660.0 * t));
        const double rVal = env * (12000.0 * std::sin(kTwoPi * 440.0 * t) + 4500.0 * std::sin(kTwoPi * 880.0 * t));
        pcm[2 * f]     = static_cast<int16_t>(std::clamp(std::llround(lVal), -22000LL, 22000LL));
        pcm[2 * f + 1] = static_cast<int16_t>(std::clamp(std::llround(rVal), -22000LL, 22000LL));
    }
    return pcm;
}

class ScopedAsiModule {
public:
    ScopedAsiModule() = default;
    ~ScopedAsiModule() { Unload(false); }

    ScopedAsiModule(const ScopedAsiModule&) = delete;
    ScopedAsiModule& operator=(const ScopedAsiModule&) = delete;

    bool Load(const std::wstring& asiPath) {
        Unload(false);
        hModule_ = LoadLibraryW(asiPath.c_str());
        if (!hModule_) {
            std::printf("[FAIL] LoadLibraryW(%ls) failed: err=%lu\n",
                        asiPath.c_str(), static_cast<unsigned long>(GetLastError()));
            return false;
        }
        return true;
    }

    bool Unload(bool strict = true) {
        if (!hModule_) return true;
        BOOL ok = FreeLibrary(hModule_);
        hModule_ = nullptr;
        if (!ok && strict) {
            std::printf("[FAIL] FreeLibrary failed: err=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }
        return ok != FALSE;
    }

private:
    HMODULE hModule_ = nullptr;
};

bool SetupReaderAndSeek(const wchar_t* mp3Path,
                        bool doInitialZeroSeek,
                        uint64_t seekPos100ns,
                        IWMSyncReader** outReader,
                        DecodeSummary& summary) {
    *outReader = nullptr;
    IWMSyncReader* reader = nullptr;
    HRESULT hr = WMCreateSyncReader(nullptr, 0, &reader);
    if (FAILED(hr) || !reader) {
        std::printf("[FAIL] WMCreateSyncReader failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    hr = reader->Open(mp3Path);
    if (FAILED(hr)) {
        std::printf("[FAIL] reader->Open(%ls) failed: 0x%08lX\n", mp3Path, static_cast<unsigned long>(hr));
        reader->Release();
        return false;
    }

    // Verify IWMHeaderInfo3 + COM Identity Rule (IUnknown pointers must match)
    IWMHeaderInfo3* hi3 = nullptr;
    hr = reader->QueryInterface(IID_IWMHeaderInfo3, reinterpret_cast<void**>(&hi3));
    if (SUCCEEDED(hr) && hi3) {
        IUnknown* unkFromReader = nullptr;
        IUnknown* unkFromHi3 = nullptr;
        reader->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unkFromReader));
        hi3->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unkFromHi3));
        summary.comIdentityOk = (unkFromReader != nullptr && unkFromReader == unkFromHi3);
        if (unkFromReader) unkFromReader->Release();
        if (unkFromHi3) unkFromHi3->Release();

        // Query Duration via GetAttributeIndices + GetAttributeByIndexEx (same sequence GTA IV uses)
        WORD count = 0;
        if (SUCCEEDED(hi3->GetAttributeIndices(0, L"Duration", nullptr, nullptr, &count)) && count > 0) {
            WORD idx = 0;
            hi3->GetAttributeIndices(0, L"Duration", nullptr, &idx, &count);
            WORD nameLen = 0;
            WMT_ATTR_DATATYPE dtype = WMT_TYPE_QWORD;
            WORD lang = 0;
            DWORD dataLen = sizeof(QWORD);
            QWORD dur = 0;
            hi3->GetAttributeByIndexEx(0, idx, nullptr, &nameLen, &dtype, &lang, nullptr, &dataLen);
            std::wstring nameBuf(nameLen, L'\0');
            if (SUCCEEDED(hi3->GetAttributeByIndexEx(0, idx, nameBuf.data(), &nameLen, &dtype, &lang,
                                                     reinterpret_cast<BYTE*>(&dur), &dataLen))) {
                summary.duration100ns = dur;
            }
        }
        hi3->Release();
    }

    DWORD outCount = 0;
    reader->GetOutputCount(&outCount);
    if (outCount > 0) {
        IWMOutputMediaProps* props = nullptr;
        if (SUCCEEDED(reader->GetOutputProps(0, &props)) && props) {
            props->Release();
        }
    }

    if (doInitialZeroSeek) {
        hr = reader->SetRange(0, 0);
        if (FAILED(hr)) {
            std::printf("[FAIL] SetRange(0, 0) failed: 0x%08lX\n", static_cast<unsigned long>(hr));
            reader->Close();
            reader->Release();
            return false;
        }
    }
    if (seekPos100ns > 0 || !doInitialZeroSeek) {
        hr = reader->SetRange(seekPos100ns, 0);
        if (FAILED(hr)) {
            std::printf("[FAIL] SetRange(%llu, 0) failed: 0x%08lX\n",
                        static_cast<unsigned long long>(seekPos100ns),
                        static_cast<unsigned long>(hr));
            reader->Close();
            reader->Release();
            return false;
        }
    }

    *outReader = reader;
    return true;
}

void DrainSamplesLoop(IWMSyncReader* reader, bool captureSamples, DecodeSummary& summary) {
    while (true) {
        INSSBuffer* sample = nullptr;
        QWORD pts = 0;
        QWORD dur = 0;
        DWORD flags = 0;
        DWORD outNum = 0;
        WORD streamNum = 0;
        HRESULT hr = reader->GetNextSample(0, &sample, &pts, &dur, &flags, &outNum, &streamNum);
        summary.lastHr = hr;
        if (hr == NS_E_NO_MORE_SAMPLES) {
            summary.ok = true;
            return;
        }
        if (FAILED(hr)) {
            std::printf("[FAIL] GetNextSample failed: 0x%08lX\n", static_cast<unsigned long>(hr));
            summary.ok = false;
            return;
        }
        if (sample) {
            if (!summary.hasFirstPts) {
                summary.firstPts100ns = pts;
                summary.hasFirstPts = true;
            }
            BYTE* buf = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(sample->GetBufferAndLength(&buf, &len)) && buf && len > 0) {
                summary.totalBytes += len;
                UpdateFnv1a(summary.fnv1aHash, buf, len);
                if (captureSamples) {
                    AppendInterleavedSamples(summary.samples, buf, len);
                }
            }
            ++summary.sampleCount;
            sample->Release();
        }
    }
}

struct WorkerDrainArgs {
    IWMSyncReader* reader = nullptr;
    bool captureSamples = false;
    DecodeSummary* summary = nullptr;
};

DWORD WINAPI WorkerDrainThreadProc(LPVOID param) {
    auto* args = static_cast<WorkerDrainArgs*>(param);
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DrainSamplesLoop(args->reader, args->captureSamples, *args->summary);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return args->summary->ok ? 0 : 1;
}

DecodeSummary RunDecodePass(const wchar_t* mp3Path,
                            bool useWorkerThread,
                            bool doInitialZeroSeek,
                            uint64_t seekPos100ns,
                            bool captureSamples) {
    DecodeSummary res{};
    IWMSyncReader* reader = nullptr;
    if (!SetupReaderAndSeek(mp3Path, doInitialZeroSeek, seekPos100ns, &reader, res)) {
        return res;
    }

    if (useWorkerThread) {
        WorkerDrainArgs args{ reader, captureSamples, &res };
        HANDLE hThread = CreateThread(nullptr, 0, &WorkerDrainThreadProc, &args, 0, nullptr);
        if (!hThread) {
            std::printf("[FAIL] CreateThread failed: err=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            reader->Close();
            reader->Release();
            return res;
        }
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    } else {
        DrainSamplesLoop(reader, captureSamples, res);
    }

    reader->Close();
    res.finalRef = reader->Release();
    if (res.finalRef != 0) {
        std::printf("[FAIL] RefCount leak detected: final Release returned %lu\n",
                    static_cast<unsigned long>(res.finalRef));
        res.ok = false;
    }
    return res;
}

bool AssertSummaryMatches(const char* label, const DecodeSummary& actual, const DecodeSummary& expected) {
    bool ok = actual.ok &&
              actual.comIdentityOk &&
              actual.finalRef == 0 &&
              actual.duration100ns == expected.duration100ns &&
              actual.sampleCount == expected.sampleCount &&
              actual.totalBytes == expected.totalBytes &&
              actual.fnv1aHash == expected.fnv1aHash;
    if (!ok) {
        std::printf("[FAIL] %s mismatch:\n"
                    "  expected: ok=%d id=%d dur=%llu samples=%llu bytes=%llu hash=0x%016llX\n"
                    "  actual  : ok=%d id=%d ref=%lu dur=%llu samples=%llu bytes=%llu hash=0x%016llX\n",
                    label,
                    expected.ok ? 1 : 0, expected.comIdentityOk ? 1 : 0,
                    static_cast<unsigned long long>(expected.duration100ns),
                    static_cast<unsigned long long>(expected.sampleCount),
                    static_cast<unsigned long long>(expected.totalBytes),
                    static_cast<unsigned long long>(expected.fnv1aHash),
                    actual.ok ? 1 : 0, actual.comIdentityOk ? 1 : 0,
                    static_cast<unsigned long>(actual.finalRef),
                    static_cast<unsigned long long>(actual.duration100ns),
                    static_cast<unsigned long long>(actual.sampleCount),
                    static_cast<unsigned long long>(actual.totalBytes),
                    static_cast<unsigned long long>(actual.fnv1aHash));
    }
    return ok;
}

// ============================================================================
// Case 5b: Failed SetRange State Invariance
// ============================================================================

struct Case5bWorkerArgs {
    IWMSyncReader* reader = nullptr;
    HANDLE phase1DoneEvent = nullptr;
    HANDLE proceedPhase2Event = nullptr;
    std::vector<int16_t> phase1Samples;
    std::vector<int16_t> phase2Samples;
    bool phase1Ok = false;
    bool phase2CrashedOrBad = false;
    HRESULT phase2EndHr = S_OK;
};

DWORD WINAPI Case5bWorkerProc(LPVOID param) {
    auto* args = static_cast<Case5bWorkerArgs*>(param);
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    args->phase1Ok = true;
    for (int i = 0; i < 20; ++i) {
        INSSBuffer* sample = nullptr;
        QWORD pts = 0, dur = 0;
        DWORD flags = 0, outNum = 0;
        WORD streamNum = 0;
        HRESULT hr = args->reader->GetNextSample(0, &sample, &pts, &dur, &flags, &outNum, &streamNum);
        if (FAILED(hr) || hr == NS_E_NO_MORE_SAMPLES) {
            args->phase1Ok = false;
            break;
        }
        if (sample) {
            BYTE* buf = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(sample->GetBufferAndLength(&buf, &len)) && buf && len > 0) {
                AppendInterleavedSamples(args->phase1Samples, buf, len);
            }
            sample->Release();
        }
    }

    SetEvent(args->phase1DoneEvent);
    WaitForSingleObject(args->proceedPhase2Event, INFINITE);

    for (int i = 0; i < 20; ++i) {
        INSSBuffer* sample = nullptr;
        QWORD pts = 0, dur = 0;
        DWORD flags = 0, outNum = 0;
        WORD streamNum = 0;
        HRESULT hr = args->reader->GetNextSample(0, &sample, &pts, &dur, &flags, &outNum, &streamNum);
        if (hr == NS_E_NO_MORE_SAMPLES || FAILED(hr)) {
            args->phase2EndHr = hr;
            break;
        }
        if (sample) {
            BYTE* buf = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(sample->GetBufferAndLength(&buf, &len)) && buf && len > 0) {
                AppendInterleavedSamples(args->phase2Samples, buf, len);
            }
            sample->Release();
        }
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

bool RunCase5bFailedSetRange(const wchar_t* targetMp3, const std::vector<int16_t>& refSynthSamples) {
    DecodeSummary dummy{};
    IWMSyncReader* reader = nullptr;
    if (!SetupReaderAndSeek(targetMp3, true, 0, &reader, dummy)) {
        return false;
    }
    if (!dummy.comIdentityOk) {
        std::printf("[FAIL] Case 5b: COM identity check failed.\n");
        reader->Close();
        reader->Release();
        return false;
    }

    Case5bWorkerArgs args{};
    args.reader = reader;
    args.phase1DoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    args.proceedPhase2Event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HANDLE hThread = CreateThread(nullptr, 0, &Case5bWorkerProc, &args, 0, nullptr);
    if (!hThread) {
        std::printf("[FAIL] Case 5b: CreateThread failed.\n");
        CloseHandle(args.phase1DoneEvent);
        CloseHandle(args.proceedPhase2Event);
        reader->Close();
        reader->Release();
        return false;
    }

    WaitForSingleObject(args.phase1DoneEvent, INFINITE);
    HRESULT hrBad = reader->SetRange(kOutOfRange100ns, 0);
    SetEvent(args.proceedPhase2Event);

    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    CloseHandle(args.phase1DoneEvent);
    CloseHandle(args.proceedPhase2Event);

    reader->Close();
    ULONG finalRef = reader->Release();

    if (!args.phase1Ok || args.phase1Samples.empty()) {
        std::printf("[FAIL] Case 5b: Phase 1 read of 20 samples failed.\n");
        return false;
    }
    if (!FAILED(hrBad)) {
        std::printf("[FAIL] Case 5b: Out-of-range SetRange(%llu) unexpectedly succeeded: hr=0x%08lX\n",
                    static_cast<unsigned long long>(kOutOfRange100ns),
                    static_cast<unsigned long>(hrBad));
        return false;
    }
    if (finalRef != 0) {
        std::printf("[FAIL] Case 5b: RefCount leak detected: finalRef=%lu\n",
                    static_cast<unsigned long>(finalRef));
        return false;
    }

    // Verify phase 1 samples match refSynthSamples[0..]
    if (args.phase1Samples.size() > refSynthSamples.size() ||
        std::memcmp(args.phase1Samples.data(), refSynthSamples.data(),
                    args.phase1Samples.size() * sizeof(int16_t)) != 0) {
        std::printf("[FAIL] Case 5b: Phase 1 samples diverged from refSynthSamples.\n");
        return false;
    }

    const size_t offsetInts = args.phase1Samples.size();
    if (!args.phase2Samples.empty()) {
        if (offsetInts + args.phase2Samples.size() > refSynthSamples.size()) {
            std::printf("[FAIL] Case 5b: Phase 2 samples exceeded refSynthSamples length.\n");
            return false;
        }
        size_t mismatches = 0;
        size_t firstMismatchIdx = 0;
        for (size_t k = 0; k < args.phase2Samples.size(); ++k) {
            if (args.phase2Samples[k] != refSynthSamples[offsetInts + k]) {
                if (mismatches == 0) firstMismatchIdx = k;
                ++mismatches;
            }
        }
        if (mismatches > 0) {
            std::printf("[FAIL] Case 5b: Post-failed-SetRange samples had %zu mismatches (first at %zu: got=%d expected=%d)\n",
                        mismatches, firstMismatchIdx,
                        static_cast<int>(args.phase2Samples[firstMismatchIdx]),
                        static_cast<int>(refSynthSamples[offsetInts + firstMismatchIdx]));
            return false;
        }
        std::printf("  [5b] Failed SetRange hr=0x%08lX; post-seek read yielded %zu int16s matching ref (0 mismatches).\n",
                    static_cast<unsigned long>(hrBad), args.phase2Samples.size());
    } else {
        std::printf("  [5b] Failed SetRange hr=0x%08lX; underlying WMVCore entered EOF/error (0x%08lX), clean Close/Release (ref=0).\n",
                    static_cast<unsigned long>(hrBad), static_cast<unsigned long>(args.phase2EndHr));
    }
    return true;
}

// ============================================================================
// Case 6: Dual Concurrent Target Readers Interleaved Read + Re-Seek & Early Close
// ============================================================================

struct Case6ThreadAArgs {
    IWMSyncReader* readerA = nullptr;
    HANDLE bStartedEvent = nullptr;
    HANDLE aClosedEvent = nullptr;
    bool ok = false;
    ULONG finalRef = 0;
};

struct Case6ThreadBArgs {
    IWMSyncReader* readerB = nullptr;
    HANDLE bStartedEvent = nullptr;
    HANDLE aClosedEvent = nullptr;
    DecodeSummary actualB{};
};

DWORD WINAPI Case6WorkerAProc(LPVOID param) {
    auto* args = static_cast<Case6ThreadAArgs*>(param);
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WaitForSingleObject(args->bStartedEvent, 5000);

    bool ok = true;
    for (int i = 1; i <= 150; ++i) {
        INSSBuffer* sample = nullptr;
        QWORD pts = 0, dur = 0;
        DWORD flags = 0, outNum = 0;
        WORD streamNum = 0;
        HRESULT hr = args->readerA->GetNextSample(0, &sample, &pts, &dur, &flags, &outNum, &streamNum);
        if (FAILED(hr) || hr == NS_E_NO_MORE_SAMPLES) {
            std::printf("[FAIL] Case 6 Reader A pre-seek GetNextSample #%d failed: 0x%08lX\n",
                        i, static_cast<unsigned long>(hr));
            ok = false;
            break;
        }
        if (sample) sample->Release();
        if ((i % 15) == 0) Sleep(0);
    }

    if (ok) {
        HRESULT hrSeek = args->readerA->SetRange(kSeekPos2_100ns, 0);
        if (FAILED(hrSeek)) {
            std::printf("[FAIL] Case 6 Reader A mid-stream SetRange failed: 0x%08lX\n",
                        static_cast<unsigned long>(hrSeek));
            ok = false;
        }
    }

    if (ok) {
        for (int i = 1; i <= 150; ++i) {
            INSSBuffer* sample = nullptr;
            QWORD pts = 0, dur = 0;
            DWORD flags = 0, outNum = 0;
            WORD streamNum = 0;
            HRESULT hr = args->readerA->GetNextSample(0, &sample, &pts, &dur, &flags, &outNum, &streamNum);
            if (FAILED(hr) || hr == NS_E_NO_MORE_SAMPLES) {
                std::printf("[FAIL] Case 6 Reader A post-seek GetNextSample #%d failed: 0x%08lX\n",
                            i, static_cast<unsigned long>(hr));
                ok = false;
                break;
            }
            if (sample) sample->Release();
            if ((i % 15) == 0) Sleep(0);
        }
    }

    args->readerA->Close();
    args->finalRef = args->readerA->Release();
    if (args->finalRef != 0) {
        std::printf("[FAIL] Case 6 Reader A early Release returned %lu\n",
                    static_cast<unsigned long>(args->finalRef));
        ok = false;
    }

    args->ok = ok;
    SetEvent(args->aClosedEvent);

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return ok ? 0 : 1;
}

DWORD WINAPI Case6WorkerBProc(LPVOID param) {
    auto* args = static_cast<Case6ThreadBArgs*>(param);
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    uint64_t count = 0;
    while (true) {
        INSSBuffer* sample = nullptr;
        QWORD pts = 0, dur = 0;
        DWORD flags = 0, outNum = 0;
        WORD streamNum = 0;
        HRESULT hr = args->readerB->GetNextSample(0, &sample, &pts, &dur, &flags, &outNum, &streamNum);
        args->actualB.lastHr = hr;
        if (hr == NS_E_NO_MORE_SAMPLES) {
            args->actualB.ok = true;
            break;
        }
        if (FAILED(hr)) {
            std::printf("[FAIL] Case 6 Reader B GetNextSample failed: 0x%08lX\n",
                        static_cast<unsigned long>(hr));
            args->actualB.ok = false;
            break;
        }
        if (sample) {
            if (!args->actualB.hasFirstPts) {
                args->actualB.firstPts100ns = pts;
                args->actualB.hasFirstPts = true;
            }
            BYTE* buf = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(sample->GetBufferAndLength(&buf, &len)) && buf && len > 0) {
                args->actualB.totalBytes += len;
                UpdateFnv1a(args->actualB.fnv1aHash, buf, len);
                AppendInterleavedSamples(args->actualB.samples, buf, len);
            }
            ++args->actualB.sampleCount;
            ++count;
            sample->Release();
        }
        if ((count % 15) == 0) {
            if (count == 15) {
                SetEvent(args->bStartedEvent);
            }
            // Keep Reader B mid-stream until Reader A completes its re-seek + early Close/Release
            if (WaitForSingleObject(args->aClosedEvent, 0) == WAIT_TIMEOUT) {
                Sleep(count >= 450 ? 2 : 0);
            }
        }
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return args->actualB.ok ? 0 : 1;
}

bool RunCase6DualConcurrent(const wchar_t* targetMp3) {
    DecodeSummary expectedB = RunDecodePass(targetMp3, true, true, kSeekPos1_100ns, true);
    if (!expectedB.ok || !expectedB.comIdentityOk || expectedB.samples.empty()) {
        std::printf("[FAIL] Case 6: Standalone expectedB reference decode failed.\n");
        return false;
    }

    DecodeSummary initA{}, initB{};
    IWMSyncReader* readerA = nullptr;
    IWMSyncReader* readerB = nullptr;
    if (!SetupReaderAndSeek(targetMp3, true, 0, &readerA, initA)) {
        return false;
    }
    if (!SetupReaderAndSeek(targetMp3, true, kSeekPos1_100ns, &readerB, initB)) {
        readerA->Close();
        readerA->Release();
        return false;
    }

    HANDLE bStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE aClosed  = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    Case6ThreadAArgs argsA{ readerA, bStarted, aClosed, false, 0 };
    Case6ThreadBArgs argsB{};
    argsB.readerB = readerB;
    argsB.bStartedEvent = bStarted;
    argsB.aClosedEvent = aClosed;
    argsB.actualB.comIdentityOk = initB.comIdentityOk;
    argsB.actualB.duration100ns = initB.duration100ns;

    HANDLE hThreadB = CreateThread(nullptr, 0, &Case6WorkerBProc, &argsB, 0, nullptr);
    HANDLE hThreadA = CreateThread(nullptr, 0, &Case6WorkerAProc, &argsA, 0, nullptr);
    if (!hThreadA || !hThreadB) {
        std::printf("[FAIL] Case 6: CreateThread failed.\n");
        if (hThreadA) { WaitForSingleObject(hThreadA, INFINITE); CloseHandle(hThreadA); }
        else { readerA->Close(); readerA->Release(); }
        if (hThreadB) { WaitForSingleObject(hThreadB, INFINITE); CloseHandle(hThreadB); }
        readerB->Close();
        readerB->Release();
        CloseHandle(bStarted);
        CloseHandle(aClosed);
        return false;
    }

    HANDLE threads[2] = { hThreadA, hThreadB };
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    CloseHandle(hThreadA);
    CloseHandle(hThreadB);
    CloseHandle(bStarted);
    CloseHandle(aClosed);

    readerB->Close();
    argsB.actualB.finalRef = readerB->Release();
    if (argsB.actualB.finalRef != 0) {
        std::printf("[FAIL] Case 6 Reader B final Release returned %lu\n",
                    static_cast<unsigned long>(argsB.actualB.finalRef));
        argsB.actualB.ok = false;
    }

    if (!argsA.ok) {
        std::printf("[FAIL] Case 6 Reader A failed during interleaved re-seek / early close.\n");
        return false;
    }
    if (!AssertSummaryMatches("Case 6 Reader B vs expectedB", argsB.actualB, expectedB)) {
        return false;
    }
    if (argsB.actualB.samples.size() != expectedB.samples.size() ||
        std::memcmp(argsB.actualB.samples.data(), expectedB.samples.data(),
                    expectedB.samples.size() * sizeof(int16_t)) != 0) {
        std::printf("[FAIL] Case 6 Reader B sample bytes diverged from expectedB!\n");
        return false;
    }
    return true;
}

// ============================================================================
// Stage C Helpers & COM-Level Cases 8 - 10
// ============================================================================

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

std::wstring QuoteArgW(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t ch : s) {
        if (ch == L'"') out += L"\\\"";
        else out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
}

std::string WideToAsciiLossy(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t ch : w) {
        out.push_back((ch >= 0 && ch < 128) ? static_cast<char>(ch) : '?');
    }
    return out;
}

struct ProducerChild {
    HANDLE hProcess = nullptr;

    ~ProducerChild() { Close(); }

    void Close() {
        if (hProcess) {
            CloseHandle(hProcess);
            hProcess = nullptr;
        }
    }

    bool Launch(const std::wstring& exePath, const std::wstring& args) {
        Close();
        std::wstring cmdLine = QuoteArgW(exePath) + L" " + args;
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
        return true;
    }

    bool WaitExit(DWORD timeoutMs, DWORD& outExitCode) {
        outExitCode = STILL_ACTIVE;
        if (!hProcess) return false;
        if (WaitForSingleObject(hProcess, timeoutMs) != WAIT_OBJECT_0) {
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

std::wstring MakeWmfTestMappingName(const wchar_t* tag) {
    wchar_t buf[128]{};
    std::swprintf(buf, 128, L"Local\\GTAIV_StageC_WmfTest_%lu_%s",
                  static_cast<unsigned long>(GetCurrentProcessId()), tag);
    return buf;
}

std::wstring MakeWmfTestEventName(const wchar_t* tag) {
    wchar_t buf[128]{};
    std::swprintf(buf, 128, L"Local\\GTAIV_StageC_WmfEv_%lu_%s",
                  static_cast<unsigned long>(GetCurrentProcessId()), tag);
    return buf;
}

bool WriteStageCIni(const std::wstring& iniPath,
                    const std::wstring& mappingName,
                    const char* targetFilesCsv,
                    uint32_t steadyLeadMs,
                    uint32_t heartbeatTimeoutMs,
                    uint32_t fadeMs,
                    bool allowHotReattach) {
    char buf[1024]{};
    std::snprintf(buf, sizeof(buf),
                  "[StageB]\r\n"
                  "Mode=ipc\r\n\r\n"
                  "[StageC]\r\n"
                  "MappingName=%s\r\n"
                  "TargetFiles=%s\r\n"
                  "SteadyLeadMs=%u\r\n"
                  "HeartbeatTimeoutMs=%u\r\n"
                  "FadeMs=%u\r\n"
                  "AllowHotReattach=%d\r\n",
                  WideToAsciiLossy(mappingName).c_str(),
                  targetFilesCsv,
                  steadyLeadMs,
                  heartbeatTimeoutMs,
                  fadeMs,
                  allowHotReattach ? 1 : 0);
    return WriteAsciiFile(iniPath, buf);
}

bool PullSingleSample(IWMSyncReader* reader, std::vector<int16_t>& dst, HRESULT& outHr) {
    INSSBuffer* sample = nullptr;
    QWORD pts = 0, dur = 0;
    DWORD flags = 0, outNum = 0;
    WORD streamNum = 0;
    outHr = reader->GetNextSample(0, &sample, &pts, &dur, &flags, &outNum, &streamNum);
    if (outHr == NS_E_NO_MORE_SAMPLES) {
        if (sample) sample->Release();
        return false;
    }
    if (FAILED(outHr)) {
        if (sample) sample->Release();
        return false;
    }
    if (sample) {
        BYTE* buf = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(sample->GetBufferAndLength(&buf, &len)) && buf && len > 0) {
            AppendInterleavedSamples(dst, buf, len);
        }
        sample->Release();
    }
    return true;
}

bool VerifyPreloadSilenceWindow(const char* label, const std::vector<int16_t>& pcm, size_t preloadFrames) {
    const size_t expectedInts = preloadFrames * 2;
    if (pcm.size() < expectedInts) {
        std::printf("[FAIL] %s: captured only %zu int16s (< expected preload %zu int16s)\n",
                    label, pcm.size(), expectedInts);
        return false;
    }
    for (size_t i = 0; i < expectedInts; ++i) {
        if (pcm[i] != 0) {
            std::printf("[FAIL] %s: preload int16[%zu] (frame %zu) was %d (expected exact 0)!\n",
                        label, i, i / 2, static_cast<int>(pcm[i]));
            return false;
        }
    }
    return true;
}

bool VerifyContiguousFrameCodeTail(const char* label,
                                   const std::vector<int16_t>& pcm,
                                   size_t startFrame,
                                   uint64_t expectedGen,
                                   uint64_t& outFirstFrame,
                                   uint64_t& outLastFrame,
                                   size_t& outFrameCount) {
    const size_t totalFrames = pcm.size() / 2;
    if (totalFrames <= startFrame) {
        std::printf("[FAIL] %s: no post-preload frames (totalFrames=%zu, startFrame=%zu)\n",
                    label, totalFrames, startFrame);
        return false;
    }

    outFrameCount = totalFrames - startFrame;
    uint64_t expectedNext = 0;
    for (size_t f = startFrame; f < totalFrames; ++f) {
        const int16_t l = pcm[f * 2];
        const int16_t r = pcm[f * 2 + 1];
        uint64_t dec = 0;
        if (!stagec::DecodeFrameCode(l, r, expectedGen, dec)) {
            std::printf("[FAIL] %s: DecodeFrameCode failed at delivered frame %zu (L=%d, R=%d, gen=%llu)\n",
                        label, f, static_cast<int>(l), static_cast<int>(r),
                        static_cast<unsigned long long>(expectedGen));
            return false;
        }
        if (f == startFrame) {
            outFirstFrame = dec;
            expectedNext = (dec + 1ULL) % stagec::kFrameCodeMod;
        } else {
            if (dec != expectedNext) {
                std::printf("[FAIL] %s: FrameCode discontinuity at delivered frame %zu (expected=%llu, got=%llu)\n",
                            label, f,
                            static_cast<unsigned long long>(expectedNext),
                            static_cast<unsigned long long>(dec));
                return false;
            }
            expectedNext = (dec + 1ULL) % stagec::kFrameCodeMod;
        }
        outLastFrame = dec;
    }
    return true;
}

bool ReadCurrentGenerationFromMapping(const std::wstring& mapName, uint64_t& outGen) {
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapName.c_str());
    if (!hMap) return false;
    const auto* block = static_cast<const stagec::StageCIpcSharedBlock*>(
        MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(stagec::StageCIpcRingHeader)));
    if (!block) {
        CloseHandle(hMap);
        return false;
    }
    outGen = stagec::AtomicLoadU64(&block->header.session_generation);
    UnmapViewOfFile(block);
    CloseHandle(hMap);
    return stagec::IsReadyEvenGeneration(outGen);
}

bool AssertByteForByteMatch(const char* label, const DecodeSummary& actual, const DecodeSummary& expected) {
    if (!AssertSummaryMatches(label, actual, expected)) {
        return false;
    }
    if (actual.samples.size() != expected.samples.size() ||
        std::memcmp(actual.samples.data(), expected.samples.data(),
                    expected.samples.size() * sizeof(int16_t)) != 0) {
        std::printf("[FAIL] %s: sample PCM bytes diverged!\n", label);
        return false;
    }
    return true;
}

bool RunCase8_IpcScanAndIsolation(const std::wstring& producerExe,
                                  const std::wstring& sandboxAsi,
                                  const std::wstring& sandboxIni,
                                  const std::wstring& targetMp3,
                                  const std::wstring& controlMp3,
                                  const std::wstring& thirdMp3,
                                  const DecodeSummary& baseTarget,
                                  const DecodeSummary& baseTargetTail,
                                  const DecodeSummary& baseControl,
                                  const DecodeSummary& baseThird) {
    std::printf("\n=== Case 8: Mode=ipc Same-Thread Scan Passthrough & Non-Target Isolation ===\n");
    const std::wstring mapName = MakeWmfTestMappingName(L"Case8");
    const std::wstring readyEv = MakeWmfTestEventName(L"Rdy8");
    const std::wstring stopEv = MakeWmfTestEventName(L"Stp8");
    HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());
    HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, stopEv.c_str());

    ProducerChild prod;
    if (!prod.Launch(producerExe,
                     L"--mapping-name " + QuoteArgW(mapName) +
                     L" --pattern framecode --prefill-sec 2.0 --duration-sec 15 --ready-event-name " +
                     QuoteArgW(readyEv) + L" --stop-event-name " + QuoteArgW(stopEv))) {
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }
    if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
        std::printf("[FAIL] Case 8: x64 producer did not signal ready\n");
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    bool ok = true;
    {
        ScopedAsiModule asi;
        if (!WriteStageCIni(sandboxIni, mapName, "IV_Probe_Target_45s.mp3", 2000u, 500u, 0u, true) ||
            !asi.Load(sandboxAsi)) {
            ok = false;
        } else {
            DecodeSummary scanFull = RunDecodePass(targetMp3.c_str(), false, true, 0, true);
            if (!AssertByteForByteMatch("Case 8 same-thread full scan", scanFull, baseTarget)) ok = false;

            DecodeSummary scanTail = RunDecodePass(targetMp3.c_str(), false, true, kTailScanStart100ns, true);
            if (!AssertByteForByteMatch("Case 8 same-thread tail scan", scanTail, baseTargetTail)) ok = false;

            DecodeSummary ctrl = RunDecodePass(controlMp3.c_str(), true, true, 0, true);
            if (!AssertByteForByteMatch("Case 8 worker Control track", ctrl, baseControl)) ok = false;

            DecodeSummary third = RunDecodePass(thirdMp3.c_str(), true, true, 0, true);
            if (!AssertByteForByteMatch("Case 8 worker Third track", third, baseThird)) ok = false;

            if (!asi.Unload(true)) ok = false;
        }
    }

    SetEvent(hStop);
    DWORD exitCode = STILL_ACTIVE;
    prod.WaitExit(5000, exitCode);
    CloseHandle(hReady);
    CloseHandle(hStop);

    std::printf("[%s] Case 8: Mode=ipc same-thread scan & non-target isolation\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool RunCase9a_Order1ActiveFirst(const std::wstring& producerExe,
                                 const std::wstring& sandboxAsi,
                                 const std::wstring& sandboxIni,
                                 const std::wstring& targetMp3,
                                 const std::wstring& controlMp3) {
    std::printf("\n--- Case 9a: Dual-Slot Order 1 (Active Target FIRST, Preload Control SECOND) ---\n");
    const std::wstring mapName = MakeWmfTestMappingName(L"Case9a");
    const std::wstring readyEv = MakeWmfTestEventName(L"Rdy9a");
    const std::wstring stopEv = MakeWmfTestEventName(L"Stp9a");
    HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());
    HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, stopEv.c_str());

    ProducerChild prod;
    if (!prod.Launch(producerExe,
                     L"--mapping-name " + QuoteArgW(mapName) +
                     L" --pattern framecode --prefill-sec 4.0 --duration-sec 70 --ready-event-name " +
                     QuoteArgW(readyEv) + L" --stop-event-name " + QuoteArgW(stopEv))) {
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }
    if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
        std::printf("[FAIL] Case 9a: x64 producer did not signal ready\n");
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    uint64_t activeGen = 0;
    if (!ReadCurrentGenerationFromMapping(mapName, activeGen)) {
        std::printf("[FAIL] Case 9a: could not read even session_generation\n");
        SetEvent(hStop);
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    ScopedAsiModule asi;
    if (!WriteStageCIni(sandboxIni, mapName,
                        "IV_Probe_Target_45s.mp3,IV_Probe_Control_45s.mp3,IV_Probe_Third_45s.mp3",
                        3500u, 500u, 0u, true) ||
        !asi.Load(sandboxAsi)) {
        SetEvent(hStop);
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    DecodeSummary initA{}, initB{};
    IWMSyncReader* readerA = nullptr;
    IWMSyncReader* readerB = nullptr;
    if (!SetupReaderAndSeek(targetMp3.c_str(), true, 0, &readerA, initA)) {
        asi.Unload(false);
        SetEvent(hStop);
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }
    if (!SetupReaderAndSeek(controlMp3.c_str(), true, 0, &readerB, initB)) {
        readerA->Close();
        readerA->Release();
        asi.Unload(false);
        SetEvent(hStop);
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    struct Args9a {
        IWMSyncReader* readerA = nullptr;
        IWMSyncReader* readerB = nullptr;
        HANDLE evB1Done = nullptr;
        HANDLE evAEof = nullptr;
        std::vector<int16_t> pcmA;
        std::vector<int16_t> pcmB;
        bool okA = false;
        bool okB = false;
    } shared;
    shared.readerA = readerA;
    shared.readerB = readerB;
    shared.evB1Done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    shared.evAEof = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HANDLE hThreadA = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* s = static_cast<Args9a*>(p);
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = true;
        HRESULT hr = S_OK;

        // Burst 1: seq = 1..221 (254,064 frames)
        for (int i = 1; i <= 221; ++i) {
            if (!PullSingleSample(s->readerA, s->pcmA, hr)) {
                ok = false;
                break;
            }
        }
        WaitForSingleObject(s->evB1Done, 10000);

        if (ok) {
            // Burst 2 (111 samples) immediately, then Bursts 3..16 (110 samples each) + Burst 17 (5 samples) paced at ~2.64s
            int remainingToPull = static_cast<int>(kExpectedFullSamples) - 221; // 1656
            bool firstLiveBurst = true;
            while (remainingToPull > 0 && ok) {
                const int burstSize = firstLiveBurst
                    ? std::min(111, remainingToPull)
                    : std::min(110, remainingToPull);
                if (!firstLiveBurst) {
                    Sleep(2640);
                }
                firstLiveBurst = false;
                for (int i = 0; i < burstSize; ++i) {
                    if (!PullSingleSample(s->readerA, s->pcmA, hr)) {
                        ok = false;
                        break;
                    }
                }
                remainingToPull -= burstSize;
            }
        }

        // Drain final EOF (NS_E_NO_MORE_SAMPLES) WITHOUT closing Reader A yet!
        if (ok) {
            if (PullSingleSample(s->readerA, s->pcmA, hr) || hr != NS_E_NO_MORE_SAMPLES) {
                std::printf("[FAIL] Case 9a Reader A expected NS_E_NO_MORE_SAMPLES, got hr=0x%08lX\n",
                            static_cast<unsigned long>(hr));
                ok = false;
            }
        }

        s->okA = ok;
        SetEvent(s->evAEof);
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return ok ? 0 : 1;
    }, &shared, 0, nullptr);

    HANDLE hThreadB = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* s = static_cast<Args9a*>(p);
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = true;
        HRESULT hr = S_OK;

        // Burst 1: seq = 1..221
        for (int i = 1; i <= 221; ++i) {
            if (!PullSingleSample(s->readerB, s->pcmB, hr)) {
                ok = false;
                break;
            }
        }
        SetEvent(s->evB1Done);

        // Sit idle until Reader A hits EOF (still unclosed!), then wake up at seq = 222 for Burst 2 (111 samples)
        WaitForSingleObject(s->evAEof, 65000);
        if (ok) {
            for (int i = 0; i < 111; ++i) {
                if (!PullSingleSample(s->readerB, s->pcmB, hr)) {
                    ok = false;
                    break;
                }
            }
        }

        s->okB = ok;
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return ok ? 0 : 1;
    }, &shared, 0, nullptr);

    HANDLE threads[2] = { hThreadA, hThreadB };
    WaitForMultipleObjects(2, threads, TRUE, 75000);
    CloseHandle(hThreadA);
    CloseHandle(hThreadB);
    CloseHandle(shared.evB1Done);
    CloseHandle(shared.evAEof);

    readerA->Close();
    const ULONG refA = readerA->Release();
    readerB->Close();
    const ULONG refB = readerB->Release();

    bool ok = shared.okA && shared.okB && (refA == 0) && (refB == 0);
    constexpr size_t kPreloadFrames = 254064;
    if (ok) {
        if (!VerifyPreloadSilenceWindow("Case 9a Reader A preload", shared.pcmA, kPreloadFrames)) ok = false;
        if (!VerifyPreloadSilenceWindow("Case 9a Reader B preload", shared.pcmB, kPreloadFrames)) ok = false;
    }
    if (ok) {
        uint64_t aFirst = 0, aLast = 0, bFirst = 0, bLast = 0;
        size_t aCount = 0, bCount = 0;
        if (!VerifyContiguousFrameCodeTail("Case 9a Reader A live", shared.pcmA, kPreloadFrames,
                                           activeGen, aFirst, aLast, aCount) ||
            !VerifyContiguousFrameCodeTail("Case 9a Reader B handoff", shared.pcmB, kPreloadFrames,
                                           activeGen, bFirst, bLast, bCount)) {
            ok = false;
        } else if (bFirst <= aLast) {
            std::printf("[FAIL] Case 9a handoff regressed/duplicated: bFirst=%llu <= aLast=%llu\n",
                        static_cast<unsigned long long>(bFirst),
                        static_cast<unsigned long long>(aLast));
            ok = false;
        } else {
            std::printf("  [9a PASS] Reader A: 254064 preload=0, %zu live frames [%llu..%llu]; "
                        "Reader B: 254064 preload=0, %zu handoff frames [%llu..%llu]\n",
                        aCount, static_cast<unsigned long long>(aFirst), static_cast<unsigned long long>(aLast),
                        bCount, static_cast<unsigned long long>(bFirst), static_cast<unsigned long long>(bLast));
        }
    }

    if (!asi.Unload(true)) ok = false;
    SetEvent(hStop);
    DWORD exitCode = STILL_ACTIVE;
    prod.WaitExit(5000, exitCode);
    CloseHandle(hReady);
    CloseHandle(hStop);
    return ok;
}

bool RunCase9b_Order2StageB2Replay(const std::wstring& producerExe,
                                   const std::wstring& sandboxAsi,
                                   const std::wstring& sandboxIni,
                                   const std::wstring& targetMp3,
                                   const std::wstring& thirdMp3) {
    std::printf("\n--- Case 9b: Dual-Slot Order 2 (Exact stage_b2 Replay: Preload P FIRST, Active M 26.454s SECOND) ---\n");
    const std::wstring mapName = MakeWmfTestMappingName(L"Case9b");
    const std::wstring readyEv = MakeWmfTestEventName(L"Rdy9b");
    const std::wstring stopEv = MakeWmfTestEventName(L"Stp9b");
    HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());
    HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, stopEv.c_str());

    ProducerChild prod;
    if (!prod.Launch(producerExe,
                     L"--mapping-name " + QuoteArgW(mapName) +
                     L" --pattern framecode --prefill-sec 4.0 --duration-sec 40 --ready-event-name " +
                     QuoteArgW(readyEv) + L" --stop-event-name " + QuoteArgW(stopEv))) {
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }
    if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
        std::printf("[FAIL] Case 9b: x64 producer did not signal ready\n");
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    uint64_t activeGen = 0;
    if (!ReadCurrentGenerationFromMapping(mapName, activeGen)) {
        std::printf("[FAIL] Case 9b: could not read even session_generation\n");
        SetEvent(hStop);
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    ScopedAsiModule asi;
    if (!WriteStageCIni(sandboxIni, mapName,
                        "IV_Probe_Target_45s.mp3,IV_Probe_Control_45s.mp3,IV_Probe_Third_45s.mp3",
                        3500u, 500u, 0u, true) ||
        !asi.Load(sandboxAsi)) {
        SetEvent(hStop);
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    // Open Preload Reader P (Third, SetRange(0,0)) FIRST
    DecodeSummary initP{};
    IWMSyncReader* readerP = nullptr;
    if (!SetupReaderAndSeek(thirdMp3.c_str(), true, 0, &readerP, initP)) {
        asi.Unload(false);
        SetEvent(hStop);
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    struct ArgsP {
        IWMSyncReader* readerP = nullptr;
        HANDLE evPBurst1Done = nullptr;
        HANDLE evMEof = nullptr;
        std::vector<int16_t> pcmP;
        bool ok = false;
    } argsP;
    argsP.readerP = readerP;
    argsP.evPBurst1Done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    argsP.evMEof = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HANDLE hThreadP = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* a = static_cast<ArgsP*>(p);
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = true;
        HRESULT hr = S_OK;
        for (int i = 1; i <= 221; ++i) {
            if (!PullSingleSample(a->readerP, a->pcmP, hr)) {
                ok = false;
                break;
            }
        }
        SetEvent(a->evPBurst1Done);

        // Wait until Reader M reaches EOF (still unclosed for 200ms!), then wake up at seq=222..332
        WaitForSingleObject(a->evMEof, 40000);
        if (ok) {
            for (int i = 0; i < 111; ++i) {
                if (!PullSingleSample(a->readerP, a->pcmP, hr)) {
                    ok = false;
                    break;
                }
            }
        }
        a->ok = ok;
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return ok ? 0 : 1;
    }, &argsP, 0, nullptr);

    WaitForSingleObject(argsP.evPBurst1Done, 10000);
    Sleep(30); // 30ms later, open Active Reader M (Target, SetRange(0,0) + SetRange(26.454s, 0)) SECOND

    DecodeSummary initM{};
    IWMSyncReader* readerM = nullptr;
    constexpr uint64_t kStageB2Seek100ns = 264540000ULL;
    if (!SetupReaderAndSeek(targetMp3.c_str(), true, kStageB2Seek100ns, &readerM, initM)) {
        SetEvent(argsP.evMEof);
        WaitForSingleObject(hThreadP, 10000);
        CloseHandle(hThreadP);
        CloseHandle(argsP.evPBurst1Done);
        CloseHandle(argsP.evMEof);
        readerP->Close();
        readerP->Release();
        asi.Unload(false);
        SetEvent(hStop);
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        CloseHandle(hStop);
        return false;
    }

    struct ArgsM {
        IWMSyncReader* readerM = nullptr;
        std::vector<int16_t> pcmM;
        bool ok = false;
    } argsM;
    argsM.readerM = readerM;

    HANDLE hThreadM = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* a = static_cast<ArgsM*>(p);
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = true;
        HRESULT hr = S_OK;

        // Burst 1: seq = 1..221
        for (int i = 1; i <= 221; ++i) {
            if (!PullSingleSample(a->readerM, a->pcmM, hr)) {
                ok = false;
                break;
            }
        }

        // Pull seq = 222..777 (556 samples: 111 immediately, then 4*110 + 5 paced at ~2.64s) -> seq=778 EOF
        if (ok) {
            int burstIdx = 0;
            int countSinceBurst = 0;
            int currentTarget = 111;
            while (true) {
                if (countSinceBurst >= currentTarget) {
                    ++burstIdx;
                    currentTarget = 110;
                    countSinceBurst = 0;
                    Sleep(2640);
                }
                if (!PullSingleSample(a->readerM, a->pcmM, hr)) {
                    if (hr != NS_E_NO_MORE_SAMPLES) {
                        ok = false;
                    }
                    break;
                }
                ++countSinceBurst;
            }
        }

        a->ok = ok;
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return ok ? 0 : 1;
    }, &argsM, 0, nullptr);

    WaitForSingleObject(hThreadM, 40000);
    // Reader M is at EOF and STILL UNCLOSED — signal Reader P to wake up at seq=222..332 while holding M open for 200ms!
    SetEvent(argsP.evMEof);
    Sleep(200);
    WaitForSingleObject(hThreadP, 15000);

    CloseHandle(hThreadM);
    CloseHandle(hThreadP);
    CloseHandle(argsP.evPBurst1Done);
    CloseHandle(argsP.evMEof);

    readerM->Close();
    const ULONG refM = readerM->Release();
    readerP->Close();
    const ULONG refP = readerP->Release();

    bool ok = argsM.ok && argsP.ok && (refM == 0) && (refP == 0);
    constexpr size_t kPreloadFrames = 254064;
    if (ok) {
        if (!VerifyPreloadSilenceWindow("Case 9b Reader P preload", argsP.pcmP, kPreloadFrames)) ok = false;
        if (!VerifyPreloadSilenceWindow("Case 9b Reader M preload", argsM.pcmM, kPreloadFrames)) ok = false;
    }
    if (ok) {
        uint64_t mFirst = 0, mLast = 0, pFirst = 0, pLast = 0;
        size_t mCount = 0, pCount = 0;
        if (!VerifyContiguousFrameCodeTail("Case 9b Reader M live", argsM.pcmM, kPreloadFrames,
                                           activeGen, mFirst, mLast, mCount) ||
            !VerifyContiguousFrameCodeTail("Case 9b Reader P handoff", argsP.pcmP, kPreloadFrames,
                                           activeGen, pFirst, pLast, pCount)) {
            ok = false;
        } else if (pFirst <= mLast) {
            std::printf("[FAIL] Case 9b handoff regressed/duplicated: pFirst=%llu <= mLast=%llu\n",
                        static_cast<unsigned long long>(pFirst),
                        static_cast<unsigned long long>(mLast));
            ok = false;
        } else {
            std::printf("  [9b PASS] Reader M: 254064 preload=0, %zu live frames [%llu..%llu]; "
                        "Reader P: 254064 preload=0, %zu handoff frames [%llu..%llu]\n",
                        mCount, static_cast<unsigned long long>(mFirst), static_cast<unsigned long long>(mLast),
                        pCount, static_cast<unsigned long long>(pFirst), static_cast<unsigned long long>(pLast));
        }
    }

    if (!asi.Unload(true)) ok = false;
    SetEvent(hStop);
    DWORD exitCode = STILL_ACTIVE;
    prod.WaitExit(5000, exitCode);
    CloseHandle(hReady);
    CloseHandle(hStop);
    return ok;
}

bool RunCase10_ComTerminateProducerSilence(const std::wstring& producerExe,
                                           const std::wstring& sandboxAsi,
                                           const std::wstring& sandboxIni,
                                           const std::wstring& sandboxJsonl,
                                           const std::wstring& targetMp3) {
    std::printf("\n=== Case 10: COM-Level TerminateProcess Producer Kill -> Safe Cosine Silence ===\n");
    const std::wstring mapName = MakeWmfTestMappingName(L"Case10");
    const std::wstring readyEv = MakeWmfTestEventName(L"Rdy10");
    HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, readyEv.c_str());

    ProducerChild prod;
    if (!prod.Launch(producerExe,
                     L"--mapping-name " + QuoteArgW(mapName) +
                     L" --pattern framecode --prefill-sec 2.0 --duration-sec 25 --ready-event-name " +
                     QuoteArgW(readyEv))) {
        CloseHandle(hReady);
        return false;
    }
    if (WaitForSingleObject(hReady, 10000) != WAIT_OBJECT_0) {
        std::printf("[FAIL] Case 10: x64 producer did not signal ready\n");
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        return false;
    }

    uint64_t activeGen = 0;
    if (!ReadCurrentGenerationFromMapping(mapName, activeGen)) {
        std::printf("[FAIL] Case 10: could not read even session_generation\n");
        prod.TerminateAndWait(1);
        CloseHandle(hReady);
        return false;
    }

    bool ok = true;
    {
        ScopedAsiModule asi;
        if (!WriteStageCIni(sandboxIni, mapName, "IV_Probe_Target_45s.mp3", 1000u, 500u, 10u, true) ||
            !asi.Load(sandboxAsi)) {
            prod.TerminateAndWait(1);
            CloseHandle(hReady);
            return false;
        }

        DecodeSummary initR{};
        IWMSyncReader* reader = nullptr;
        if (!SetupReaderAndSeek(targetMp3.c_str(), true, 0, &reader, initR)) {
            asi.Unload(false);
            prod.TerminateAndWait(1);
            CloseHandle(hReady);
            return false;
        }

        struct Args10 {
            IWMSyncReader* reader = nullptr;
            ProducerChild* prod = nullptr;
            uint64_t activeGen = 0;
            std::vector<int16_t> preloadPcm;
            std::vector<int16_t> livePcm;
            std::vector<int16_t> postKillPcm;
            bool ok = false;
        } args10;
        args10.reader = reader;
        args10.prod = &prod;
        args10.activeGen = activeGen;

        HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            auto* a = static_cast<Args10*>(p);
            HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool threadOk = true;
            HRESULT hr = S_OK;

            for (int i = 1; i <= 221; ++i) {
                if (!PullSingleSample(a->reader, a->preloadPcm, hr)) {
                    threadOk = false;
                    break;
                }
            }
            for (int i = 0; i < 5 && threadOk; ++i) {
                if (!PullSingleSample(a->reader, a->livePcm, hr)) {
                    threadOk = false;
                    break;
                }
            }

            // Abruptly terminate producer, wait > HeartbeatTimeoutMs (500ms), then pull 4 buffers
            a->prod->TerminateAndWait(1);
            Sleep(650);

            for (int i = 0; i < 4 && threadOk; ++i) {
                if (!PullSingleSample(a->reader, a->postKillPcm, hr)) {
                    threadOk = false;
                    break;
                }
            }

            a->ok = threadOk;
            if (SUCCEEDED(hrCo)) CoUninitialize();
            return threadOk ? 0 : 1;
        }, &args10, 0, nullptr);

        WaitForSingleObject(hThread, 20000);
        CloseHandle(hThread);

        reader->Close();
        const ULONG finalRef = reader->Release();
        if (!asi.Unload(true)) ok = false;
        if (!args10.ok || finalRef != 0) ok = false;

        if (ok) {
            if (!VerifyPreloadSilenceWindow("Case 10 preload", args10.preloadPcm, 254064)) ok = false;
        }
        if (ok) {
            // Live frames [480..] after FadeMs=10 fade-in must decode cleanly via FrameCode
            uint64_t fStart = 0, fEnd = 0;
            size_t fCnt = 0;
            if (!VerifyContiguousFrameCodeTail("Case 10 post-fade live", args10.livePcm, 480,
                                               activeGen, fStart, fEnd, fCnt)) {
                ok = false;
            }
        }
        if (ok) {
            // Post-kill: first 480 frames smoothly fade from last live sample to 0; remaining frames 100% exact 0
            if (args10.livePcm.size() < 2 || args10.postKillPcm.size() < 4 * 1152 * 2) {
                std::printf("[FAIL] Case 10: insufficient post-kill samples\n");
                ok = false;
            } else {
                const int16_t preL = args10.livePcm[args10.livePcm.size() - 2];
                const int16_t preR = args10.livePcm[args10.livePcm.size() - 1];
                int32_t maxDelta = 0;
                int16_t prevL = preL, prevR = preR;
                for (size_t f = 0; f < 480; ++f) {
                    const int16_t l = args10.postKillPcm[f * 2];
                    const int16_t r = args10.postKillPcm[f * 2 + 1];
                    maxDelta = std::max({ maxDelta,
                                          std::abs(static_cast<int32_t>(l) - static_cast<int32_t>(prevL)),
                                          std::abs(static_cast<int32_t>(r) - static_cast<int32_t>(prevR)) });
                    prevL = l;
                    prevR = r;
                }
                bool tailExactZero = true;
                for (size_t i = 480 * 2; i < args10.postKillPcm.size(); ++i) {
                    if (args10.postKillPcm[i] != 0) {
                        tailExactZero = false;
                        break;
                    }
                }
                if (maxDelta >= 18000 || !tailExactZero) {
                    std::printf("[FAIL] Case 10: post-kill fade/silence check failed (maxDelta=%d, tailZero=%d)\n",
                                maxDelta, tailExactZero ? 1 : 0);
                    ok = false;
                } else {
                    std::printf("  [10] Producer killed -> smooth fade (maxDelta=%d) to 100%% silence (%zu trailing int16s == 0).\n",
                                maxDelta, args10.postKillPcm.size() - 480 * 2);
                }
            }
        }
        if (ok) {
            if (!FileContainsSubstring(sandboxJsonl, "\"StageC_StreamEvent\"") ||
                !FileContainsSubstring(sandboxJsonl, "\"producer_disconnected\"")) {
                std::printf("[FAIL] Case 10: missing StageC_StreamEvent / producer_disconnected in JSONL.\n");
                ok = false;
            }
        }
    }

    CloseHandle(hReady);
    std::printf("[%s] Case 10: COM-level producer kill -> safe cosine silence & clean unload\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("Usage: wmf_selftest.exe <path_to_asi> <assets_dir_or_target_mp3> [--producer-exe <path>]\n");
        return 2;
    }

    std::wstring explicitProducer;
    for (int i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--producer-exe") == 0) {
            if (i + 1 >= argc) {
                std::printf("[FAIL] Missing path after --producer-exe\n");
                return 2;
            }
            explicitProducer = argv[++i];
        } else {
            std::printf("Usage: wmf_selftest.exe <path_to_asi> <assets_dir_or_target_mp3> [--producer-exe <path>]\n");
            return 2;
        }
    }

    const std::wstring asiSrc = argv[1];
    if (!FileExistsRegular(asiSrc)) {
        std::printf("[FAIL] ASI not found: %ls\n", asiSrc.c_str());
        return 2;
    }

    fs::path arg2Path(argv[2]);
    std::wstring ext = arg2Path.extension().wstring();
    fs::path assetsDir;
    if (_wcsicmp(ext.c_str(), L".mp3") == 0 || (fs::exists(arg2Path) && fs::is_regular_file(arg2Path))) {
        assetsDir = arg2Path.has_parent_path() ? arg2Path.parent_path() : fs::current_path();
    } else {
        assetsDir = arg2Path;
    }

    const std::wstring targetMp3  = (assetsDir / L"IV_Probe_Target_45s.mp3").wstring();
    const std::wstring controlMp3 = (assetsDir / L"IV_Probe_Control_45s.mp3").wstring();
    const std::wstring thirdMp3   = (assetsDir / L"IV_Probe_Third_45s.mp3").wstring();

    if (!FileExistsRegular(targetMp3) || !FileExistsRegular(controlMp3) || !FileExistsRegular(thirdMp3)) {
        std::printf("[FAIL] Required baseline MP3 assets missing in %ls\n", assetsDir.c_str());
        std::printf("  Target : %ls (%s)\n", targetMp3.c_str(), FileExistsRegular(targetMp3) ? "ok" : "MISSING");
        std::printf("  Control: %ls (%s)\n", controlMp3.c_str(), FileExistsRegular(controlMp3) ? "ok" : "MISSING");
        std::printf("  Third  : %ls (%s)\n", thirdMp3.c_str(), FileExistsRegular(thirdMp3) ? "ok" : "MISSING");
        return 2;
    }

    // Create isolated sandbox next to wmf_selftest.exe
    wchar_t exePathBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    fs::path exeDir = fs::path(exePathBuf).parent_path();
    fs::path sandboxDir = exeDir / L"selftest_sandbox";
    std::error_code ec;
    fs::create_directories(sandboxDir, ec);
    if (ec) {
        std::printf("[FAIL] Could not create sandbox dir %ls: %s\n",
                    sandboxDir.c_str(), ec.message().c_str());
        return 2;
    }

    const std::wstring sandboxAsi = (sandboxDir / L"IVRadioProbe.asi").wstring();
    const std::wstring sandboxIni = (sandboxDir / L"IVRadioProbe.ini").wstring();

    if (!CopyFileW(asiSrc.c_str(), sandboxAsi.c_str(), FALSE)) {
        std::printf("[FAIL] CopyFileW to %ls failed: err=%lu\n",
                    sandboxAsi.c_str(), static_cast<unsigned long>(GetLastError()));
        return 2;
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    int failures = 0;

    // ========================================================================
    // Baseline Pre-Pass (without ASI loaded)
    // ========================================================================
    std::printf("=== Baseline Pre-Pass (without ASI loaded) ===\n");
    const DecodeSummary baseTarget     = RunDecodePass(targetMp3.c_str(), false, true, 0, true);
    const DecodeSummary baseTargetTail = RunDecodePass(targetMp3.c_str(), false, true, kTailScanStart100ns, true);
    const DecodeSummary baseControl    = RunDecodePass(controlMp3.c_str(), false, true, 0, true);
    const DecodeSummary baseThird      = RunDecodePass(thirdMp3.c_str(), false, true, 0, true);

    auto checkBaselineFull = [](const char* name, const DecodeSummary& s) -> bool {
        bool ok = s.ok && s.comIdentityOk && s.finalRef == 0 &&
                  s.sampleCount == kExpectedFullSamples && s.totalBytes == kExpectedFullBytes;
        std::printf("  [%s] %s | dur=%.3fs samples=%llu bytes=%llu hash=0x%016llX id=%d ref=%lu\n",
                    ok ? "PASS" : "FAIL", name,
                    static_cast<double>(s.duration100ns) / 10000000.0,
                    static_cast<unsigned long long>(s.sampleCount),
                    static_cast<unsigned long long>(s.totalBytes),
                    static_cast<unsigned long long>(s.fnv1aHash),
                    s.comIdentityOk ? 1 : 0,
                    static_cast<unsigned long>(s.finalRef));
        return ok;
    };

    if (!checkBaselineFull("Baseline Target", baseTarget)) ++failures;
    if (!checkBaselineFull("Baseline Control", baseControl)) ++failures;
    if (!checkBaselineFull("Baseline Third", baseThird)) ++failures;

    bool tailOk = baseTargetTail.ok && baseTargetTail.comIdentityOk && baseTargetTail.finalRef == 0 &&
                  baseTargetTail.sampleCount > 0 && baseTargetTail.totalBytes > 0;
    std::printf("  [%s] Baseline Target Tail (SetRange %llu) | samples=%llu bytes=%llu hash=0x%016llX\n",
                tailOk ? "PASS" : "FAIL",
                static_cast<unsigned long long>(kTailScanStart100ns),
                static_cast<unsigned long long>(baseTargetTail.sampleCount),
                static_cast<unsigned long long>(baseTargetTail.totalBytes),
                static_cast<unsigned long long>(baseTargetTail.fnv1aHash));
    if (!tailOk) ++failures;

    if (failures > 0) {
        std::printf("[FAIL] Baseline Pre-Pass failed (%d errors); aborting.\n", failures);
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return 1;
    }

    // ========================================================================
    // Case 1: Default Off / Missing INI / Invalid Mode Passthrough
    // ========================================================================
    std::printf("\n=== Case 1: Default Off / Missing INI / Invalid Mode Passthrough ===\n");
    bool case1Ok = true;
    {
        // 1a: Missing INI
        ScopedAsiModule asi;
        if (!DeleteFileIfPresent(sandboxIni) || !asi.Load(sandboxAsi)) {
            case1Ok = false;
        } else {
            DecodeSummary r1a = RunDecodePass(targetMp3.c_str(), true, true, 0, false);
            if (!AssertSummaryMatches("Case 1a (missing INI)", r1a, baseTarget)) case1Ok = false;
            if (!asi.Unload(true)) case1Ok = false;
        }
    }
    {
        // 1b: Mode=off
        ScopedAsiModule asi;
        if (!WriteAsciiFile(sandboxIni, "[StageB]\r\nMode=off\r\n") || !asi.Load(sandboxAsi)) {
            case1Ok = false;
        } else {
            DecodeSummary r1b = RunDecodePass(targetMp3.c_str(), true, true, 0, false);
            if (!AssertSummaryMatches("Case 1b (Mode=off)", r1b, baseTarget)) case1Ok = false;
            if (!asi.Unload(true)) case1Ok = false;
        }
    }
    {
        // 1c: Mode=invalid_xyz
        ScopedAsiModule asi;
        if (!WriteAsciiFile(sandboxIni, "[StageB]\r\nMode=invalid_xyz\r\n") || !asi.Load(sandboxAsi)) {
            case1Ok = false;
        } else {
            DecodeSummary r1c = RunDecodePass(targetMp3.c_str(), true, true, 0, false);
            if (!AssertSummaryMatches("Case 1c (Mode=invalid_xyz)", r1c, baseTarget)) case1Ok = false;
            if (!asi.Unload(true)) case1Ok = false;
        }
    }
    std::printf("[%s] Case 1: Missing INI / Mode=off / Mode=invalid_xyz passthrough\n",
                case1Ok ? "PASS" : "FAIL");
    if (!case1Ok) ++failures;

    // ========================================================================
    // Cases 2 - 6 under Mode=synth
    // ========================================================================
    ScopedAsiModule synthAsi;
    if (!WriteAsciiFile(sandboxIni, "[StageB]\r\nMode=synth\r\n") || !synthAsi.Load(sandboxAsi)) {
        std::printf("[FAIL] Could not load ASI with Mode=synth.\n");
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return 1;
    }

    // Case 2: Same-Thread Scan Passthrough
    std::printf("\n=== Case 2: Mode=synth Same-Thread Scan Passthrough ===\n");
    bool case2Ok = true;
    {
        DecodeSummary scanFull = RunDecodePass(targetMp3.c_str(), false, true, 0, false);
        if (!AssertSummaryMatches("Case 2 full scan", scanFull, baseTarget)) case2Ok = false;

        DecodeSummary scanTail = RunDecodePass(targetMp3.c_str(), false, true, kTailScanStart100ns, false);
        if (!AssertSummaryMatches("Case 2 tail scan", scanTail, baseTargetTail)) case2Ok = false;
    }
    std::printf("[%s] Case 2: Same-thread scan passthrough (full + tail)\n", case2Ok ? "PASS" : "FAIL");
    if (!case2Ok) ++failures;

    // Case 3: Control Tracks Isolation
    std::printf("\n=== Case 3: Mode=synth Control Tracks Isolation ===\n");
    bool case3Ok = true;
    {
        DecodeSummary ctrl = RunDecodePass(controlMp3.c_str(), true, true, 0, false);
        if (!AssertSummaryMatches("Case 3 Control", ctrl, baseControl)) case3Ok = false;

        DecodeSummary third = RunDecodePass(thirdMp3.c_str(), true, true, 0, false);
        if (!AssertSummaryMatches("Case 3 Third", third, baseThird)) case3Ok = false;
    }
    std::printf("[%s] Case 3: Control & Third tracks 100%% untouched\n", case3Ok ? "PASS" : "FAIL");
    if (!case3Ok) ++failures;

    // Case 4: Target Replacement + Quantitative Waveform Quality Audit
    std::printf("\n=== Case 4: Mode=synth Target Replacement & Waveform Quality Audit ===\n");
    DecodeSummary refSynth = RunDecodePass(targetMp3.c_str(), true, true, 0, true);
    bool case4Ok = refSynth.ok &&
                   refSynth.comIdentityOk &&
                   refSynth.finalRef == 0 &&
                   refSynth.duration100ns == baseTarget.duration100ns &&
                   refSynth.sampleCount == baseTarget.sampleCount &&
                   refSynth.totalBytes == baseTarget.totalBytes &&
                   refSynth.fnv1aHash != baseTarget.fnv1aHash &&
                   (refSynth.totalBytes % 4ULL) == 0 &&
                   refSynth.samples.size() == (refSynth.totalBytes / sizeof(int16_t));

    if (!case4Ok) {
        std::printf("[FAIL] Case 4 structural/hash check failed: ok=%d id=%d ref=%lu dur=%llu samples=%llu bytes=%llu hash=0x%016llX (base=0x%016llX)\n",
                    refSynth.ok ? 1 : 0, refSynth.comIdentityOk ? 1 : 0,
                    static_cast<unsigned long>(refSynth.finalRef),
                    static_cast<unsigned long long>(refSynth.duration100ns),
                    static_cast<unsigned long long>(refSynth.sampleCount),
                    static_cast<unsigned long long>(refSynth.totalBytes),
                    static_cast<unsigned long long>(refSynth.fnv1aHash),
                    static_cast<unsigned long long>(baseTarget.fnv1aHash));
    } else {
        const size_t totalInt16 = refSynth.samples.size();
        const size_t totalFrames = totalInt16 / 2;
        double sumSq = 0.0;
        int32_t maxAbs = 0;
        uint64_t clipCount = 0;
        int32_t maxDelta = 0;
        size_t maxDeltaFrame = 0;
        uint64_t stereoDiffFrames = 0;

        for (size_t i = 0; i < totalInt16; ++i) {
            const double v = static_cast<double>(refSynth.samples[i]);
            sumSq += v * v;
            const int32_t a = std::abs(static_cast<int32_t>(refSynth.samples[i]));
            if (a > maxAbs) maxAbs = a;
            if (a >= 32767) ++clipCount;
        }
        for (size_t f = 0; f < totalFrames; ++f) {
            const int32_t l = refSynth.samples[2 * f];
            const int32_t r = refSynth.samples[2 * f + 1];
            if (l != r) ++stereoDiffFrames;
            if (f > 0) {
                const int32_t dl = std::abs(l - static_cast<int32_t>(refSynth.samples[2 * (f - 1)]));
                const int32_t dr = std::abs(r - static_cast<int32_t>(refSynth.samples[2 * (f - 1) + 1]));
                const int32_t d = std::max(dl, dr);
                if (d > maxDelta) {
                    maxDelta = d;
                    maxDeltaFrame = f;
                }
            }
        }

        const double rms = std::sqrt(sumSq / static_cast<double>(totalInt16));
        const int32_t boundary624Delta = (totalFrames > 624)
            ? std::max(std::abs(static_cast<int32_t>(refSynth.samples[2 * 624]) - static_cast<int32_t>(refSynth.samples[2 * 623])),
                       std::abs(static_cast<int32_t>(refSynth.samples[2 * 624 + 1]) - static_cast<int32_t>(refSynth.samples[2 * 623 + 1])))
            : 0;

        std::printf("  Metrics: rms=%.1f maxAbs=%d clipCount=%llu maxDelta=%d (at frame %zu, frame624Delta=%d) stereoDiffFrames=%llu\n",
                    rms, maxAbs,
                    static_cast<unsigned long long>(clipCount),
                    maxDelta, maxDeltaFrame, boundary624Delta,
                    static_cast<unsigned long long>(stereoDiffFrames));

        if (rms < 1500.0 || clipCount != 0 || maxAbs > 28000 || maxDelta >= 18000 || stereoDiffFrames <= 10000) {
            std::printf("[FAIL] Case 4 waveform quality assertion failed.\n");
            case4Ok = false;
        }
    }
    std::printf("[%s] Case 4: Target synth replacement & quantitative waveform audit\n",
                case4Ok ? "PASS" : "FAIL");
    if (!case4Ok) ++failures;

    // Case 5: SetRange Direct Seek Equivalence & Failed SetRange Invariance
    std::printf("\n=== Case 5: SetRange Direct Seek Equivalence & Failed SetRange State Invariance ===\n");
    bool case5Ok = true;
    if (!case4Ok) {
        std::printf("[FAIL] Case 5 skipped/failed because Case 4 refSynthSamples unavailable.\n");
        case5Ok = false;
    } else {
        const uint64_t seekTargets[2] = { kSeekPos1_100ns, kSeekPos2_100ns };
        for (uint64_t seekPos : seekTargets) {
            DecodeSummary seekRes = RunDecodePass(targetMp3.c_str(), true, true, seekPos, true);
            if (!seekRes.ok || !seekRes.comIdentityOk || seekRes.finalRef != 0 || !seekRes.hasFirstPts || seekRes.samples.empty()) {
                std::printf("[FAIL] Case 5a seek=%llu decode failed.\n", static_cast<unsigned long long>(seekPos));
                case5Ok = false;
                continue;
            }
            const uint64_t frameOffset = (seekRes.firstPts100ns * 48000ULL + 5000000ULL) / 10000000ULL;
            const size_t startIdx = static_cast<size_t>(frameOffset) * 2;
            const size_t maxToleranceInts = 4800 * 2; // 100ms MP3 frame-snap / encoder-delay trim tolerance within 45.048s container
            if (startIdx >= refSynth.samples.size() ||
                startIdx + seekRes.samples.size() > refSynth.samples.size() + maxToleranceInts) {
                std::printf("[FAIL] Case 5a seek=%llu out of allowed bounds (startIdx=%zu, len=%zu, ref=%zu).\n",
                            static_cast<unsigned long long>(seekPos),
                            startIdx, seekRes.samples.size(), refSynth.samples.size());
                case5Ok = false;
                continue;
            }
            const size_t overlapInts = std::min(seekRes.samples.size(), refSynth.samples.size() - startIdx);
            const size_t tailInts = seekRes.samples.size() - overlapInts;
            size_t mismatches = 0;
            size_t firstMismatchIdx = 0;
            for (size_t k = 0; k < overlapInts; ++k) {
                if (seekRes.samples[k] != refSynth.samples[startIdx + k]) {
                    if (mismatches == 0) firstMismatchIdx = k;
                    ++mismatches;
                }
            }
            bool tailQualityOk = true;
            for (size_t k = overlapInts; k < seekRes.samples.size(); ++k) {
                const int32_t v = static_cast<int32_t>(seekRes.samples[k]);
                if (std::abs(v) > 28000) {
                    tailQualityOk = false;
                    break;
                }
                if (k >= 2) {
                    const int32_t dv = std::abs(v - static_cast<int32_t>(seekRes.samples[k - 2]));
                    if (dv >= 18000) {
                        tailQualityOk = false;
                        break;
                    }
                }
            }
            if (mismatches > 0 || !tailQualityOk) {
                std::printf("[FAIL] Case 5a seek=%llu firstPts=%llu frameOffset=%llu mismatches=%zu tailQualityOk=%d.\n",
                            static_cast<unsigned long long>(seekPos),
                            static_cast<unsigned long long>(seekRes.firstPts100ns),
                            static_cast<unsigned long long>(frameOffset),
                            mismatches, tailQualityOk ? 1 : 0);
                case5Ok = false;
            } else {
                std::printf("  [5a] seek=%llu -> firstPts=%llu frameOffset=%llu overlap=%zu tail=%zu int16s (0 mismatches, tail continuous)\n",
                            static_cast<unsigned long long>(seekPos),
                            static_cast<unsigned long long>(seekRes.firstPts100ns),
                            static_cast<unsigned long long>(frameOffset),
                            overlapInts, tailInts);
            }
        }

        if (!RunCase5bFailedSetRange(targetMp3.c_str(), refSynth.samples)) {
            case5Ok = false;
        }
    }
    std::printf("[%s] Case 5: Direct seek waveform equivalence & failed SetRange invariance\n",
                case5Ok ? "PASS" : "FAIL");
    if (!case5Ok) ++failures;

    // Case 6: Dual Concurrent Target Readers Interleaved Read + Re-Seek & Early Close
    std::printf("\n=== Case 6: Dual Concurrent Target Readers Isolation ===\n");
    bool case6Ok = RunCase6DualConcurrent(targetMp3.c_str());
    std::printf("[%s] Case 6: Dual concurrent Target readers interleaved + re-seek + early close\n",
                case6Ok ? "PASS" : "FAIL");
    if (!case6Ok) ++failures;

    if (!synthAsi.Unload(true)) ++failures;

    // ========================================================================
    // Case 7: Mode=wav In-Memory Replacement & Invalid WAV Fallback Suite (Step B2)
    // ========================================================================
    std::printf("\n=== Case 7: Mode=wav In-Memory Replacement & Invalid WAV Fallback Suite ===\n");
    bool case7Ok = true;
    const std::wstring sandboxJsonl   = (sandboxDir / L"IVRadioProbe.jsonl").wstring();
    const std::wstring validWavPath   = (sandboxDir / L"valid_test_48k.wav").wstring();
    const std::wstring badRateWavPath = (sandboxDir / L"bad_rate_44k.wav").wstring();
    const std::wstring shortWavPath   = (sandboxDir / L"short_5s.wav").wstring();
    const std::wstring assetWavPath   = (assetsDir / L"IV_StageB_Target_48k.wav").wstring();

    // Verify deployed B2 asset WAV on disk if present (must exist for Step B2 gate)
    if (!FileExistsRegular(assetWavPath) ||
        !ValidateWavFileOnDisk(assetWavPath, 48000, 2, 16, kExpectedFullBytes)) {
        std::printf("[FAIL] Case 7: Stage B2 asset WAV missing or invalid: %ls\n", assetWavPath.c_str());
        case7Ok = false;
    } else {
        std::printf("  [7-asset] Verified assets/IV_StageB_Target_48k.wav (48000Hz 2ch 16bit, %llu PCM bytes).\n",
                    static_cast<unsigned long long>(kExpectedFullBytes));
    }

    // 7a: Valid 48000Hz/2ch/16bit/8647104B WAV exact replacement + scan passthrough + control isolation + seek + dual concurrent
    const size_t expectedFrames = static_cast<size_t>(kExpectedFullBytes / 4ULL);
    const std::vector<int16_t> expectedWavPcm = GenerateValidTestWavPcm(expectedFrames);
    if (!WriteCanonicalPcmWav(validWavPath, 48000, 2, 16, expectedWavPcm)) {
        case7Ok = false;
    } else {
        ScopedAsiModule wavAsi;
        if (!WriteAsciiFile(sandboxIni, "[StageB]\r\nMode=wav\r\nWavPath=valid_test_48k.wav\r\nExpectedPcmBytes=8647104\r\n") ||
            !wavAsi.Load(sandboxAsi)) {
            case7Ok = false;
        } else {
            // Same-thread scan must still passthrough original MP3
            DecodeSummary wavScan = RunDecodePass(targetMp3.c_str(), false, true, 0, false);
            if (!AssertSummaryMatches("Case 7a same-thread scan", wavScan, baseTarget)) case7Ok = false;

            // Worker-thread Control track must still passthrough original MP3
            DecodeSummary wavCtrl = RunDecodePass(controlMp3.c_str(), true, true, 0, false);
            if (!AssertSummaryMatches("Case 7a control track", wavCtrl, baseControl)) case7Ok = false;

            // Worker-thread full decode on Target must match expectedWavPcm 100% byte-for-byte
            DecodeSummary wavFull = RunDecodePass(targetMp3.c_str(), true, true, 0, true);
            if (!wavFull.ok || !wavFull.comIdentityOk || wavFull.finalRef != 0 ||
                wavFull.sampleCount != kExpectedFullSamples || wavFull.totalBytes != kExpectedFullBytes ||
                wavFull.samples.size() != expectedWavPcm.size() ||
                std::memcmp(wavFull.samples.data(), expectedWavPcm.data(), expectedWavPcm.size() * sizeof(int16_t)) != 0) {
                std::printf("[FAIL] Case 7a worker-thread WAV payload did not match expectedWavPcm byte-for-byte!\n");
                case7Ok = false;
            } else {
                std::printf("  [7a] Full WAV replacement matched 100%% byte-for-byte (%llu bytes).\n",
                            static_cast<unsigned long long>(wavFull.totalBytes));
            }

            // Worker-thread SetRange(13.227s) seek must match expectedWavPcm on overlap and zero-pad MP3 frame-snap tail
            DecodeSummary wavSeek = RunDecodePass(targetMp3.c_str(), true, true, kSeekPos1_100ns, true);
            if (!wavSeek.ok || !wavSeek.hasFirstPts || wavSeek.samples.empty()) {
                std::printf("[FAIL] Case 7a WAV seek decode failed.\n");
                case7Ok = false;
            } else {
                const uint64_t fOff = (wavSeek.firstPts100ns * 48000ULL + 5000000ULL) / 10000000ULL;
                const size_t sIdx = static_cast<size_t>(fOff) * 2;
                const size_t ov = std::min(wavSeek.samples.size(), expectedWavPcm.size() - sIdx);
                bool seekMatch = (std::memcmp(wavSeek.samples.data(), expectedWavPcm.data() + sIdx, ov * sizeof(int16_t)) == 0);
                bool zeroTailOk = true;
                for (size_t k = ov; k < wavSeek.samples.size(); ++k) {
                    if (wavSeek.samples[k] != 0) {
                        zeroTailOk = false;
                        break;
                    }
                }
                if (!seekMatch || !zeroTailOk) {
                    std::printf("[FAIL] Case 7a WAV seek mismatch (seekMatch=%d, zeroTailOk=%d).\n",
                                seekMatch ? 1 : 0, zeroTailOk ? 1 : 0);
                    case7Ok = false;
                } else {
                    std::printf("  [7a] WAV seek=13.227s matched %zu int16s + %zu zero-padded tail int16s.\n",
                                ov, wavSeek.samples.size() - ov);
                }
            }

            // Dual concurrent readers under Mode=wav
            if (!RunCase6DualConcurrent(targetMp3.c_str())) {
                std::printf("[FAIL] Case 7a dual concurrent WAV readers failed.\n");
                case7Ok = false;
            }

            if (!wavAsi.Unload(true)) case7Ok = false;
            if (!FileContainsSubstring(sandboxJsonl, "\"status\":\"wav_ready\"")) {
                std::printf("[FAIL] Case 7a missing status=wav_ready in JSONL.\n");
                case7Ok = false;
            }
        }
    }

    // 7b: Missing WAV file -> safe passthrough fallback
    {
        ScopedAsiModule asi;
        if (!WriteAsciiFile(sandboxIni, "[StageB]\r\nMode=wav\r\nWavPath=does_not_exist_missing.wav\r\nExpectedPcmBytes=8647104\r\n") ||
            !asi.Load(sandboxAsi)) {
            case7Ok = false;
        } else {
            DecodeSummary r7b = RunDecodePass(targetMp3.c_str(), true, true, 0, false);
            if (!AssertSummaryMatches("Case 7b (missing WAV fallback)", r7b, baseTarget)) case7Ok = false;
            if (!asi.Unload(true)) case7Ok = false;
            if (!FileContainsSubstring(sandboxJsonl, "\"status\":\"fallback_wav_missing\"")) {
                std::printf("[FAIL] Case 7b missing fallback_wav_missing in JSONL.\n");
                case7Ok = false;
            } else {
                std::printf("  [7b] Missing WAV cleanly fell back to original MP3 passthrough.\n");
            }
        }
    }

    // 7c: Bad format WAV (44100 Hz instead of 48000 Hz) -> safe passthrough fallback
    {
        std::vector<int16_t> dummy44k(44100 * 2, 0);
        if (!WriteCanonicalPcmWav(badRateWavPath, 44100, 2, 16, dummy44k)) {
            case7Ok = false;
        } else {
            ScopedAsiModule asi;
            if (!WriteAsciiFile(sandboxIni, "[StageB]\r\nMode=wav\r\nWavPath=bad_rate_44k.wav\r\nExpectedPcmBytes=8647104\r\n") ||
                !asi.Load(sandboxAsi)) {
                case7Ok = false;
            } else {
                DecodeSummary r7c = RunDecodePass(targetMp3.c_str(), true, true, 0, false);
                if (!AssertSummaryMatches("Case 7c (bad format 44.1kHz WAV fallback)", r7c, baseTarget)) case7Ok = false;
                if (!asi.Unload(true)) case7Ok = false;
                if (!FileContainsSubstring(sandboxJsonl, "\"status\":\"fallback_wav_bad_format\"")) {
                    std::printf("[FAIL] Case 7c missing fallback_wav_bad_format in JSONL.\n");
                    case7Ok = false;
                } else {
                    std::printf("  [7c] Bad-format (44.1kHz) WAV cleanly fell back to original MP3 passthrough.\n");
                }
            }
        }
    }

    // 7d: Wrong length WAV (5 seconds = 960,000 bytes != 8,647,104 bytes) -> safe passthrough fallback
    {
        std::vector<int16_t> short5s(48000 * 5 * 2, 1000);
        if (!WriteCanonicalPcmWav(shortWavPath, 48000, 2, 16, short5s)) {
            case7Ok = false;
        } else {
            ScopedAsiModule asi;
            if (!WriteAsciiFile(sandboxIni, "[StageB]\r\nMode=wav\r\nWavPath=short_5s.wav\r\nExpectedPcmBytes=8647104\r\n") ||
                !asi.Load(sandboxAsi)) {
                case7Ok = false;
            } else {
                DecodeSummary r7d = RunDecodePass(targetMp3.c_str(), true, true, 0, false);
                if (!AssertSummaryMatches("Case 7d (wrong length 5s WAV fallback)", r7d, baseTarget)) case7Ok = false;
                if (!asi.Unload(true)) case7Ok = false;
                if (!FileContainsSubstring(sandboxJsonl, "\"status\":\"fallback_wav_length_mismatch\"")) {
                    std::printf("[FAIL] Case 7d missing fallback_wav_length_mismatch in JSONL.\n");
                    case7Ok = false;
                } else {
                    std::printf("  [7d] Wrong-length (5s) WAV cleanly fell back to original MP3 passthrough.\n");
                }
            }
        }
    }

    // Clean up temporary sandbox WAV fixtures
    DeleteFileIfPresent(validWavPath);
    DeleteFileIfPresent(badRateWavPath);
    DeleteFileIfPresent(shortWavPath);

    std::printf("[%s] Case 7: Mode=wav replacement & 3 negative fallback tests\n", case7Ok ? "PASS" : "FAIL");
    if (!case7Ok) ++failures;

    // ========================================================================
    // Cases 8 - 10: Stage C Cross-Process IPC COM-Level Verification
    // ========================================================================
    const std::wstring producerExe = FindProducerExe(explicitProducer);
    if (producerExe.empty()) {
        std::printf("\n[FAIL] Stage C Cases 8-10: could not locate iv_stage_c_producer.exe (explicit=%ls).\n",
                    explicitProducer.empty() ? L"<auto>" : explicitProducer.c_str());
        failures += 3;
    } else {
        std::printf("\n=== Stage C COM-Level Cases 8-10 (x64 producer: %ls) ===\n", producerExe.c_str());

        if (!RunCase8_IpcScanAndIsolation(producerExe, sandboxAsi, sandboxIni,
                                          targetMp3, controlMp3, thirdMp3,
                                          baseTarget, baseTargetTail, baseControl, baseThird)) {
            ++failures;
        }

        const bool case9aOk = RunCase9a_Order1ActiveFirst(producerExe, sandboxAsi, sandboxIni, targetMp3, controlMp3);
        const bool case9bOk = RunCase9b_Order2StageB2Replay(producerExe, sandboxAsi, sandboxIni, targetMp3, thirdMp3);
        const bool case9Ok = case9aOk && case9bOk;
        std::printf("[%s] Case 9: Dual-candidate tune-in full-PCM audit (Order 1=%s, Order 2=%s)\n",
                    case9Ok ? "PASS" : "FAIL",
                    case9aOk ? "PASS" : "FAIL",
                    case9bOk ? "PASS" : "FAIL");
        if (!case9Ok) ++failures;

        if (!RunCase10_ComTerminateProducerSilence(producerExe, sandboxAsi, sandboxIni, sandboxJsonl, targetMp3)) {
            ++failures;
        }
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();

    std::printf("\n=== Stage B + Stage C Selftest Summary: %s (%d failed case(s)) ===\n",
                failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
