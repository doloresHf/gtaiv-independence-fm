#include "stage_b_source.h"
#include "async_logger.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>

namespace probe {

namespace {

volatile LONG g_csInitState = 0; // 0 = uninitialized, 1 = initializing, 2 = ready

std::wstring TrimWide(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) {
        --end;
    }
    return s.substr(start, end - start);
}

bool IsRelativePath(const std::wstring& p) {
    if (p.empty()) return true;
    if (p[0] == L'\\' || p[0] == L'/') return false;
    if (p.find(L':') != std::wstring::npos) return false;
    return true;
}

bool ReadExactBytes(HANDLE hFile, void* dst, DWORD bytesToRead) {
    auto* out = static_cast<uint8_t*>(dst);
    DWORD totalRead = 0;
    while (totalRead < bytesToRead) {
        DWORD chunkRead = 0;
        if (!ReadFile(hFile, out + totalRead, bytesToRead - totalRead, &chunkRead, nullptr) || chunkRead == 0) {
            return false;
        }
        totalRead += chunkRead;
    }
    return true;
}

struct ScopedFileHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~ScopedFileHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

} // namespace

StageBManager& StageBManager::Instance() {
    static StageBManager inst;
    return inst;
}

void StageBManager::SetModuleHandle(HMODULE hModule) {
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(hModule, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        moduleDir_ = slash ? std::wstring(path, slash + 1) : L".\\";
    } else {
        moduleDir_ = L".\\";
    }
}

void StageBManager::EnsureInitialized() {
    if (InterlockedCompareExchange(&initialized_, 0, 0) == 1) {
        return;
    }

    while (true) {
        LONG prev = InterlockedCompareExchange(&g_csInitState, 1, 0);
        if (prev == 0) {
            InitializeCriticalSection(&initCs_);
            csReady_ = true;
            InterlockedExchange(&g_csInitState, 2);
            break;
        }
        if (prev == 2) {
            break;
        }
        Sleep(0);
    }

    EnterCriticalSection(&initCs_);
    if (InterlockedCompareExchange(&initialized_, 0, 0) == 0) {
        InitializeImpl();
        InterlockedExchange(&initialized_, 1);
    }
    LeaveCriticalSection(&initCs_);
}

void StageBManager::Shutdown() {
    StageCIpcManager::Instance().Shutdown();
    if (InterlockedCompareExchange(&g_csInitState, 0, 0) == 2 && csReady_) {
        EnterCriticalSection(&initCs_);
        wavSamples_.clear();
        config_ = StageBConfigSnapshot{};
        InterlockedExchange(&initialized_, 0);
        LeaveCriticalSection(&initCs_);

        DeleteCriticalSection(&initCs_);
        csReady_ = false;
        InterlockedExchange(&g_csInitState, 0);
    } else {
        wavSamples_.clear();
        config_ = StageBConfigSnapshot{};
        InterlockedExchange(&initialized_, 0);
    }
}

const StageBConfigSnapshot& StageBManager::GetConfig() const {
    return config_;
}

bool StageBManager::IsTargetTrackPath(const wchar_t* fullOrRelPath) {
    if (!fullOrRelPath) return false;
    const wchar_t* leaf = fullOrRelPath;
    for (const wchar_t* p = fullOrRelPath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
    }
    return _wcsicmp(leaf, kTargetFileName) == 0;
}

bool StageBManager::MatchesActiveTargetTrack(const wchar_t* fullOrRelPath) const {
    if (!fullOrRelPath) return false;
    const wchar_t* leaf = fullOrRelPath;
    for (const wchar_t* p = fullOrRelPath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
    }
    if (config_.effectiveMode == StageBMode::Ipc) {
        if (config_.ipcConfig.targetFiles.empty()) {
            return _wcsicmp(leaf, kTargetFileName) == 0;
        }
        for (const auto& t : config_.ipcConfig.targetFiles) {
            if (_wcsicmp(leaf, t.c_str()) == 0) return true;
        }
        return false;
    }
    return _wcsicmp(leaf, kTargetFileName) == 0;
}

void StageBManager::RenderSynthFrames(uint64_t startFrame, uint32_t frameCount, int16_t* outInterleavedLR) {
    if (!outInterleavedLR || frameCount == 0) return;

    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTwoPi = 2.0 * kPi;
    constexpr double kSampleRate = 48000.0;
    static constexpr double kBassFreqs[4] = { 110.0, 82.4069, 98.0, 130.8128 };

    for (uint32_t i = 0; i < frameCount; ++i) {
        const uint64_t f = startFrame + static_cast<uint64_t>(i);
        const double t = static_cast<double>(f) / kSampleRate;

        // 120 BPM bass groove (24000 frames = 0.5s per beat)
        const uint64_t beatIdx = (f / 24000ULL) % 4ULL;
        const uint64_t fBeat = f % 24000ULL;
        const double freq = kBassFreqs[beatIdx];

        double envBass = 1.0;
        if (fBeat < 480ULL) {
            envBass = 0.5 * (1.0 - std::cos(kPi * static_cast<double>(fBeat) / 480.0));
        } else if (fBeat >= 23520ULL) {
            envBass = 0.5 * (1.0 - std::cos(kPi * static_cast<double>(24000ULL - fBeat) / 480.0));
        }
        envBass *= (0.45 + 0.55 * std::exp(-3.0 * static_cast<double>(fBeat) / 24000.0));

        const double bassWave =
            0.65 * std::sin(kTwoPi * freq * t) +
            0.25 * std::sin(2.0 * kTwoPi * freq * t) +
            0.10 * std::sin(3.0 * kTwoPi * freq * t);
        const double bass = envBass * bassWave;

        // Crisp metallic hi-hat every half-beat (12000 frames = 0.25s)
        const uint64_t fHalf = f % 12000ULL;
        double hat = 0.0;
        if (fHalf < 1920ULL) {
            const double x = static_cast<double>(fHalf) / 1920.0;
            const double envHat = std::sin(kPi * x) * std::exp(-4.0 * x);
            const double hatWave =
                0.35 * std::sin(kTwoPi * 6371.0 * t) +
                0.30 * std::sin(kTwoPi * 7919.0 * t) +
                0.20 * std::sin(kTwoPi * 9437.0 * t) +
                0.15 * std::sin(kTwoPi * 11113.0 * t);
            hat = envHat * hatWave;
        }

        // Stereo L/R ping-pong chime within each 8-beat bar (192000 frames)
        const uint64_t fBar = f % 192000ULL;
        double chimeL = 0.0;
        double chimeR = 0.0;
        if (fBar >= 144000ULL && fBar < 156000ULL) {
            const double envChime = std::sin(kPi * static_cast<double>(fBar - 144000ULL) / 12000.0);
            const double c = envChime * std::sin(kTwoPi * 880.0 * t);
            chimeL = 0.85 * c;
            chimeR = 0.15 * c;
        } else if (fBar >= 168000ULL && fBar < 180000ULL) {
            const double envChime = std::sin(kPi * static_cast<double>(fBar - 168000ULL) / 12000.0);
            const double c = envChime * std::sin(kTwoPi * 1174.66 * t);
            chimeL = 0.15 * c;
            chimeR = 0.85 * c;
        }

        const double leftF  = 16500.0 * bass + 9500.0 * hat + 3500.0 * chimeL;
        const double rightF = 16500.0 * bass + 9500.0 * hat + 3500.0 * chimeR;

        const long long leftI  = std::clamp(std::llround(leftF), -24000LL, 24000LL);
        const long long rightI = std::clamp(std::llround(rightF), -24000LL, 24000LL);

        outInterleavedLR[2 * i]     = static_cast<int16_t>(leftI);
        outInterleavedLR[2 * i + 1] = static_cast<int16_t>(rightI);
    }
}

bool StageBManager::CopyWavFrames(uint64_t startFrame, uint32_t frameCount, int16_t* outInterleavedLR) const {
    if (config_.effectiveMode != StageBMode::Wav || wavSamples_.empty() || !outInterleavedLR) {
        return false;
    }

    constexpr uint64_t kTailToleranceFrames = 4800ULL;
    const uint64_t totalWavFrames = static_cast<uint64_t>(wavSamples_.size() / 2);
    const uint64_t limitFrames = totalWavFrames + kTailToleranceFrames;

    if (startFrame > limitFrames || static_cast<uint64_t>(frameCount) > (limitFrames - startFrame)) {
        return false;
    }

    if (frameCount == 0) {
        return true;
    }

    const uint64_t avail = (startFrame < totalWavFrames)
        ? std::min<uint64_t>(frameCount, totalWavFrames - startFrame)
        : 0ULL;

    if (avail > 0) {
        std::memcpy(
            outInterleavedLR,
            wavSamples_.data() + static_cast<size_t>(startFrame * 2ULL),
            static_cast<size_t>(avail * 2ULL * sizeof(int16_t)));
    }
    if (avail < frameCount) {
        std::memset(
            outInterleavedLR + static_cast<size_t>(avail * 2ULL),
            0,
            static_cast<size_t>((frameCount - avail) * 2ULL * sizeof(int16_t)));
    }

    return true;
}

const char* StageBManager::ModeToString(StageBMode mode) {
    switch (mode) {
    case StageBMode::Synth: return "synth";
    case StageBMode::Wav:   return "wav";
    case StageBMode::Ipc:   return "ipc";
    default:                return "off";
    }
}

const char* StageBManager::StatusToString(StageBInitStatus status) {
    switch (status) {
    case StageBInitStatus::DisabledDefaultOff:        return "disabled_default_off";
    case StageBInitStatus::DisabledIniOff:            return "disabled_ini_off";
    case StageBInitStatus::DisabledInvalidMode:       return "disabled_invalid_mode";
    case StageBInitStatus::SynthReady:                return "synth_ready";
    case StageBInitStatus::WavReady:                  return "wav_ready";
    case StageBInitStatus::FallbackWavMissing:        return "fallback_wav_missing";
    case StageBInitStatus::FallbackWavBadFormat:      return "fallback_wav_bad_format";
    case StageBInitStatus::FallbackWavLengthMismatch: return "fallback_wav_length_mismatch";
    case StageBInitStatus::IpcReady:                  return "ipc_ready";
    default:                                          return "unknown";
    }
}

void StageBManager::InitializeImpl() {
    wavSamples_.clear();
    config_ = StageBConfigSnapshot{};
    const std::wstring baseDir = moduleDir_.empty() ? L".\\" : moduleDir_;
    config_.iniPath = baseDir + L"IVRadioProbe.ini";

    DWORD attr = GetFileAttributesW(config_.iniPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        config_.requestedMode = StageBMode::Off;
        config_.effectiveMode = StageBMode::Off;
        config_.status = StageBInitStatus::DisabledDefaultOff;
    } else {
        wchar_t modeBuf[128]{};
        GetPrivateProfileStringW(L"StageB", L"Mode", L"", modeBuf, 128, config_.iniPath.c_str());
        std::wstring modeStr = TrimWide(modeBuf);

        if (modeStr.empty()) {
            config_.requestedMode = StageBMode::Off;
            config_.effectiveMode = StageBMode::Off;
            config_.status = StageBInitStatus::DisabledDefaultOff;
        } else if (_wcsicmp(modeStr.c_str(), L"off") == 0 ||
                   _wcsicmp(modeStr.c_str(), L"0") == 0 ||
                   _wcsicmp(modeStr.c_str(), L"false") == 0) {
            config_.requestedMode = StageBMode::Off;
            config_.effectiveMode = StageBMode::Off;
            config_.status = StageBInitStatus::DisabledIniOff;
        } else if (_wcsicmp(modeStr.c_str(), L"synth") == 0) {
            config_.requestedMode = StageBMode::Synth;
            config_.effectiveMode = StageBMode::Synth;
            config_.status = StageBInitStatus::SynthReady;
        } else if (_wcsicmp(modeStr.c_str(), L"ipc") == 0) {
            config_.requestedMode = StageBMode::Ipc;
            config_.effectiveMode = StageBMode::Ipc;
            config_.status = StageBInitStatus::IpcReady;

            StageCIpcConfig ipcCfg{};

            wchar_t mapBuf[256]{};
            GetPrivateProfileStringW(L"StageC", L"MappingName", L"", mapBuf, 256, config_.iniPath.c_str());
            std::wstring mapStr = TrimWide(mapBuf);
            ipcCfg.mappingName = mapStr.empty() ? stagec::kDefaultMappingName : mapStr;

            wchar_t targetsBuf[2048]{};
            GetPrivateProfileStringW(L"StageC", L"TargetFiles", L"", targetsBuf, 2048, config_.iniPath.c_str());
            std::wstring targetsRaw = targetsBuf;
            size_t startPos = 0;
            while (startPos <= targetsRaw.size()) {
                size_t commaPos = targetsRaw.find(L',', startPos);
                std::wstring item = (commaPos == std::wstring::npos)
                    ? targetsRaw.substr(startPos)
                    : targetsRaw.substr(startPos, commaPos - startPos);
                item = TrimWide(item);
                if (!item.empty()) {
                    ipcCfg.targetFiles.push_back(item);
                }
                if (commaPos == std::wstring::npos) {
                    break;
                }
                startPos = commaPos + 1;
            }
            if (ipcCfg.targetFiles.empty()) {
                ipcCfg.targetFiles.push_back(kTargetFileName);
            }

            auto parseClampedU32 = [&](const wchar_t* key, uint32_t defVal, uint32_t minVal, uint32_t maxVal) -> uint32_t {
                wchar_t numBuf[64]{};
                GetPrivateProfileStringW(L"StageC", key, L"", numBuf, 64, config_.iniPath.c_str());
                std::wstring numStr = TrimWide(numBuf);
                if (numStr.empty()) {
                    return defVal;
                }
                wchar_t* endPtr = nullptr;
                long long parsed = std::wcstoll(numStr.c_str(), &endPtr, 10);
                if (endPtr == numStr.c_str() || *endPtr != L'\0') {
                    return defVal;
                }
                if (parsed < static_cast<long long>(minVal)) return minVal;
                if (parsed > static_cast<long long>(maxVal)) return maxVal;
                return static_cast<uint32_t>(parsed);
            };

            ipcCfg.steadyLeadMs = parseClampedU32(L"SteadyLeadMs", 3500u, 100u, 18000u);
            ipcCfg.heartbeatTimeoutMs = parseClampedU32(L"HeartbeatTimeoutMs", 500u, 50u, 10000u);
            ipcCfg.fadeMs = parseClampedU32(L"FadeMs", 10u, 0u, 200u);

            wchar_t reattachBuf[64]{};
            GetPrivateProfileStringW(L"StageC", L"AllowHotReattach", L"", reattachBuf, 64, config_.iniPath.c_str());
            std::wstring reattachStr = TrimWide(reattachBuf);
            if (!reattachStr.empty() &&
                (_wcsicmp(reattachStr.c_str(), L"0") == 0 ||
                 _wcsicmp(reattachStr.c_str(), L"false") == 0 ||
                 _wcsicmp(reattachStr.c_str(), L"off") == 0)) {
                ipcCfg.allowHotReattach = false;
            } else {
                ipcCfg.allowHotReattach = true;
            }

            config_.ipcConfig = ipcCfg;
            StageCIpcManager::Instance().Configure(config_.ipcConfig);

            std::string mapUtf8 = AsyncLogger::WideToUtf8Escaped(config_.ipcConfig.mappingName.c_str());
            AsyncLogger::Instance().LogJsonEvent(
                "StageC_ConfigLoaded", 0, "stage_c",
                "\"mapping_name\":\"%s\",\"target_count\":%lu,\"steady_lead_ms\":%u,\"heartbeat_timeout_ms\":%u,\"fade_ms\":%u,\"allow_hot_reattach\":%s",
                mapUtf8.c_str(),
                static_cast<unsigned long>(config_.ipcConfig.targetFiles.size()),
                static_cast<unsigned>(config_.ipcConfig.steadyLeadMs),
                static_cast<unsigned>(config_.ipcConfig.heartbeatTimeoutMs),
                static_cast<unsigned>(config_.ipcConfig.fadeMs),
                config_.ipcConfig.allowHotReattach ? "true" : "false");
        } else if (_wcsicmp(modeStr.c_str(), L"wav") == 0) {
            config_.requestedMode = StageBMode::Wav;

            wchar_t wavBuf[MAX_PATH]{};
            GetPrivateProfileStringW(L"StageB", L"WavPath", L"", wavBuf, MAX_PATH, config_.iniPath.c_str());
            std::wstring rawWavPath = TrimWide(wavBuf);
            if (rawWavPath.empty()) {
                rawWavPath = L"IV_StageB_Target_48k.wav";
            }
            config_.wavPath = IsRelativePath(rawWavPath) ? (baseDir + rawWavPath) : rawWavPath;

            wchar_t expectedBuf[64]{};
            GetPrivateProfileStringW(L"StageB", L"ExpectedPcmBytes", L"", expectedBuf, 64, config_.iniPath.c_str());
            std::wstring expectedStr = TrimWide(expectedBuf);
            uint64_t parsedExpected = 0;
            if (!expectedStr.empty()) {
                wchar_t* endPtr = nullptr;
                parsedExpected = std::wcstoull(expectedStr.c_str(), &endPtr, 10);
                if (endPtr == expectedStr.c_str() || *endPtr != L'\0') {
                    parsedExpected = 0;
                }
            }
            config_.expectedPcmBytes = (parsedExpected == 0) ? kDefaultTargetPcmBytes : parsedExpected;

            config_.status = LoadAndValidateWav(config_.wavPath, config_.expectedPcmBytes);
            if (config_.status == StageBInitStatus::WavReady) {
                config_.effectiveMode = StageBMode::Wav;
            } else {
                wavSamples_.clear();
                config_.loadedWavPcmBytes = 0;
                config_.effectiveMode = StageBMode::Off;
            }
        } else {
            config_.requestedMode = StageBMode::Off;
            config_.effectiveMode = StageBMode::Off;
            config_.status = StageBInitStatus::DisabledInvalidMode;
        }
    }

    std::string iniUtf8 = AsyncLogger::WideToUtf8Escaped(config_.iniPath.c_str());
    std::string wavUtf8 = AsyncLogger::WideToUtf8Escaped(config_.wavPath.c_str());
    AsyncLogger::Instance().LogJsonEvent(
        "StageB_ConfigLoaded", 0, "stage_b",
        "\"req_mode\":\"%s\",\"eff_mode\":\"%s\",\"status\":\"%s\",\"ini_path\":\"%s\",\"wav_path\":\"%s\",\"expected_pcm_bytes\":%llu,\"wav_pcm_bytes\":%llu",
        ModeToString(config_.requestedMode),
        ModeToString(config_.effectiveMode),
        StatusToString(config_.status),
        iniUtf8.c_str(),
        wavUtf8.c_str(),
        static_cast<unsigned long long>(config_.expectedPcmBytes),
        static_cast<unsigned long long>(config_.loadedWavPcmBytes));
}

StageBInitStatus StageBManager::LoadAndValidateWav(const std::wstring& path, uint64_t expectedBytes) {
    wavSamples_.clear();
    config_.loadedWavPcmBytes = 0;

    ScopedFileHandle file;
    file.handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) {
        return StageBInitStatus::FallbackWavMissing;
    }

    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(file.handle, &liSize) || liSize.QuadPart < 12) {
        return StageBInitStatus::FallbackWavBadFormat;
    }
    const uint64_t fileBytes = static_cast<uint64_t>(liSize.QuadPart);

    uint8_t riffHdr[12]{};
    if (!ReadExactBytes(file.handle, riffHdr, sizeof(riffHdr))) {
        return StageBInitStatus::FallbackWavMissing;
    }
    if (std::memcmp(riffHdr, "RIFF", 4) != 0 || std::memcmp(riffHdr + 8, "WAVE", 4) != 0) {
        return StageBInitStatus::FallbackWavBadFormat;
    }

    bool foundFmt = false;
    bool fmtValid = false;
    bool foundData = false;
    uint64_t dataOffset = 0;
    uint64_t dataBytes = 0;

    uint64_t offset = 12ULL;
    while (offset + 8ULL <= fileBytes) {
        LARGE_INTEGER seekPos{};
        seekPos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file.handle, seekPos, nullptr, FILE_BEGIN)) {
            return StageBInitStatus::FallbackWavMissing;
        }

        uint8_t chunkHdr[8]{};
        if (!ReadExactBytes(file.handle, chunkHdr, sizeof(chunkHdr))) {
            return StageBInitStatus::FallbackWavMissing;
        }

        uint32_t chunkSize = 0;
        std::memcpy(&chunkSize, chunkHdr + 4, sizeof(chunkSize));
        const uint64_t chunkDataOffset = offset + 8ULL;

        if (static_cast<uint64_t>(chunkSize) > fileBytes - chunkDataOffset) {
            return StageBInitStatus::FallbackWavBadFormat;
        }

        if (std::memcmp(chunkHdr, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                return StageBInitStatus::FallbackWavBadFormat;
            }
            uint8_t fmtBuf[16]{};
            if (!ReadExactBytes(file.handle, fmtBuf, sizeof(fmtBuf))) {
                return StageBInitStatus::FallbackWavMissing;
            }
            uint16_t formatTag = 0;
            uint16_t channels = 0;
            uint32_t sampleRate = 0;
            uint32_t avgBytesPerSec = 0;
            uint16_t blockAlign = 0;
            uint16_t bitsPerSample = 0;

            std::memcpy(&formatTag, fmtBuf + 0, 2);
            std::memcpy(&channels, fmtBuf + 2, 2);
            std::memcpy(&sampleRate, fmtBuf + 4, 4);
            std::memcpy(&avgBytesPerSec, fmtBuf + 8, 4);
            std::memcpy(&blockAlign, fmtBuf + 12, 2);
            std::memcpy(&bitsPerSample, fmtBuf + 14, 2);

            foundFmt = true;
            fmtValid = (formatTag == 1 &&
                        channels == kExpectedChannels &&
                        sampleRate == kExpectedSampleRate &&
                        bitsPerSample == kExpectedBitsPerSample &&
                        blockAlign == kExpectedBlockAlign &&
                        avgBytesPerSec == 192000U);
            if (!fmtValid) {
                return StageBInitStatus::FallbackWavBadFormat;
            }
        } else if (std::memcmp(chunkHdr, "data", 4) == 0 && !foundData) {
            foundData = true;
            dataOffset = chunkDataOffset;
            dataBytes = static_cast<uint64_t>(chunkSize);
        }

        const uint64_t paddedSize = static_cast<uint64_t>(chunkSize) + (static_cast<uint64_t>(chunkSize) & 1ULL);
        if (paddedSize > fileBytes - chunkDataOffset) {
            offset = fileBytes;
            break;
        }
        offset = chunkDataOffset + paddedSize;
    }

    if (!foundFmt || !fmtValid || !foundData) {
        return StageBInitStatus::FallbackWavBadFormat;
    }
    if (dataBytes == 0 || (dataBytes % kExpectedBlockAlign) != 0 || dataOffset + dataBytes > fileBytes) {
        return StageBInitStatus::FallbackWavBadFormat;
    }
    if (dataBytes != expectedBytes) {
        return StageBInitStatus::FallbackWavLengthMismatch;
    }

    LARGE_INTEGER dataSeek{};
    dataSeek.QuadPart = static_cast<LONGLONG>(dataOffset);
    if (!SetFilePointerEx(file.handle, dataSeek, nullptr, FILE_BEGIN)) {
        return StageBInitStatus::FallbackWavMissing;
    }

    wavSamples_.resize(static_cast<size_t>(dataBytes / sizeof(int16_t)));
    uint8_t* dstBytes = reinterpret_cast<uint8_t*>(wavSamples_.data());
    uint64_t remaining = dataBytes;
    uint64_t readOffset = 0;
    while (remaining > 0) {
        DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(remaining, 1024ULL * 1024ULL));
        if (!ReadExactBytes(file.handle, dstBytes + readOffset, toRead)) {
            wavSamples_.clear();
            return StageBInitStatus::FallbackWavMissing;
        }
        readOffset += toRead;
        remaining -= toRead;
    }

    config_.loadedWavPcmBytes = dataBytes;
    return StageBInitStatus::WavReady;
}

} // namespace probe
