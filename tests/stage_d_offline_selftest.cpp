#include "stage_d_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

// NOTE: This test suite runs 100% pure offline (in-memory mock sessions + temp files only),
// with zero live audio hardware access, zero live endpoint enumeration, and zero process kills.

namespace {

using namespace staged;

int g_totalChecks = 0;
int g_failedChecks = 0;

void CheckTrue(bool cond, const char* expr, int line) {
    ++g_totalChecks;
    if (!cond) {
        ++g_failedChecks;
        std::fprintf(stderr, "FAIL [line %d]: %s\n", line, expr);
    }
}

void CheckEqI32(int32_t actual, int32_t expected, const char* expr, int line) {
    ++g_totalChecks;
    if (actual != expected) {
        ++g_failedChecks;
        std::fprintf(stderr, "FAIL [line %d]: %s (actual=%d, expected=%d)\n",
                     line, expr, actual, expected);
    }
}

void CheckEqU64(uint64_t actual, uint64_t expected, const char* expr, int line) {
    ++g_totalChecks;
    if (actual != expected) {
        ++g_failedChecks;
        std::fprintf(stderr, "FAIL [line %d]: %s (actual=%llu, expected=%llu)\n",
                     line, expr,
                     static_cast<unsigned long long>(actual),
                     static_cast<unsigned long long>(expected));
    }
}

#define CHECK(expr) CheckTrue((expr), #expr, __LINE__)
#define CHECK_EQ_I32(a, b) CheckEqI32(static_cast<int32_t>(a), static_cast<int32_t>(b), #a " == " #b, __LINE__)
#define CHECK_EQ_U64(a, b) CheckEqU64(static_cast<uint64_t>(a), static_cast<uint64_t>(b), #a " == " #b, __LINE__)

bool FileExistsW(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

class TempDirGuard {
public:
    TempDirGuard() {
        wchar_t tmpRoot[MAX_PATH]{};
        const DWORD len = GetTempPathW(MAX_PATH, tmpRoot);
        const std::filesystem::path base = (len > 0 && len < MAX_PATH)
            ? std::filesystem::path(tmpRoot)
            : std::filesystem::current_path();
        const std::wstring sub = L"stage_d_offline_selftest_" +
                                 std::to_wstring(GetCurrentProcessId()) + L"_" +
                                 std::to_wstring(GetTickCount64());
        dir_ = (base / sub).wstring();
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
    }

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::wstring ChildPath(const std::wstring& filename) const {
        return (std::filesystem::path(dir_) / filename).wstring();
    }

private:
    std::wstring dir_;
};

WAVEFORMATEX MakePcmWfx(uint32_t rate, uint16_t ch, uint16_t bits, uint16_t align) {
    WAVEFORMATEX w{};
    w.wFormatTag = WAVE_FORMAT_PCM;
    w.nChannels = ch;
    w.nSamplesPerSec = rate;
    w.wBitsPerSample = bits;
    w.nBlockAlign = align;
    w.nAvgBytesPerSec = rate * align;
    w.cbSize = 0;
    return w;
}

WAVEFORMATEX MakeFloatWfx(uint32_t rate, uint16_t ch, uint16_t bits, uint16_t align) {
    WAVEFORMATEX w{};
    w.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    w.nChannels = ch;
    w.nSamplesPerSec = rate;
    w.wBitsPerSample = bits;
    w.nBlockAlign = align;
    w.nAvgBytesPerSec = rate * align;
    w.cbSize = 0;
    return w;
}

WAVEFORMATEXTENSIBLE MakeExtensibleWfx(uint32_t rate,
                                        uint16_t ch,
                                        uint16_t containerBits,
                                        uint16_t validBits,
                                        uint16_t blockAlign,
                                        DWORD channelMask,
                                        const GUID& subFormat,
                                        WORD cbSize = 22u) {
    WAVEFORMATEXTENSIBLE wfxe{};
    wfxe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfxe.Format.nChannels = ch;
    wfxe.Format.nSamplesPerSec = rate;
    wfxe.Format.wBitsPerSample = containerBits;
    wfxe.Format.nBlockAlign = blockAlign;
    wfxe.Format.nAvgBytesPerSec = rate * blockAlign;
    wfxe.Format.cbSize = cbSize;
    wfxe.Samples.wValidBitsPerSample = validBits;
    wfxe.dwChannelMask = channelMask;
    wfxe.SubFormat = subFormat;
    return wfxe;
}

// ============================================================================
// Suite 1: Format Validation Tests
// ============================================================================

void TestFormatValidation() {
    std::printf("Running TestFormatValidation...\n");
    ValidatedCaptureFormat fmt{};
    std::string err;

    // Null check
    CHECK(!ValidateCaptureWaveFormat(nullptr, fmt, err));
    CHECK(!err.empty());

    // Valid PCM16 formats
    {
        const WAVEFORMATEX w = MakePcmWfx(48000, 2, 16, 4);
        CHECK(ValidateCaptureWaveFormat(&w, fmt, err));
        CHECK(fmt.sampleRate == 48000u);
        CHECK(fmt.channels == 2u);
        CHECK(fmt.bitsPerSample == 16u);
        CHECK(fmt.blockAlign == 4u);
        CHECK(fmt.sampleType == CaptureSampleType::Pcm16);
        CHECK(!fmt.isExtensible);
    }
    {
        const WAVEFORMATEX w = MakePcmWfx(44100, 1, 16, 2);
        CHECK(ValidateCaptureWaveFormat(&w, fmt, err));
        CHECK(fmt.channels == 1u);
        CHECK(fmt.sampleType == CaptureSampleType::Pcm16);
    }
    {
        const WAVEFORMATEX wMin = MakePcmWfx(8000, 1, 16, 2);
        const WAVEFORMATEX wMax = MakePcmWfx(384000, 2, 16, 4);
        CHECK(ValidateCaptureWaveFormat(&wMin, fmt, err));
        CHECK(ValidateCaptureWaveFormat(&wMax, fmt, err));
    }

    // Invalid PCM rates / channels / bits / blockAlign
    {
        const WAVEFORMATEX wLowRate = MakePcmWfx(7999, 2, 16, 4);
        const WAVEFORMATEX wHighRate = MakePcmWfx(384001, 2, 16, 4);
        const WAVEFORMATEX w0ch = MakePcmWfx(48000, 0, 16, 2);
        const WAVEFORMATEX w3ch = MakePcmWfx(48000, 3, 16, 6);
        const WAVEFORMATEX w6ch = MakePcmWfx(48000, 6, 16, 12);
        const WAVEFORMATEX w8bit = MakePcmWfx(48000, 2, 8, 2);
        const WAVEFORMATEX w24bit = MakePcmWfx(48000, 2, 24, 6);
        const WAVEFORMATEX wBadAlign = MakePcmWfx(48000, 2, 16, 6);
        CHECK(!ValidateCaptureWaveFormat(&wLowRate, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&wHighRate, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w0ch, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w3ch, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w6ch, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w8bit, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w24bit, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&wBadAlign, fmt, err));
    }

    // Valid IEEE_FLOAT formats
    {
        const WAVEFORMATEX w2 = MakeFloatWfx(48000, 2, 32, 8);
        const WAVEFORMATEX w1 = MakeFloatWfx(96000, 1, 32, 4);
        CHECK(ValidateCaptureWaveFormat(&w2, fmt, err));
        CHECK(fmt.sampleType == CaptureSampleType::Float32);
        CHECK(fmt.channels == 2u);
        CHECK(ValidateCaptureWaveFormat(&w1, fmt, err));
        CHECK(fmt.sampleType == CaptureSampleType::Float32);
        CHECK(fmt.channels == 1u);
    }

    // Invalid IEEE_FLOAT formats
    {
        const WAVEFORMATEX w64 = MakeFloatWfx(48000, 2, 64, 16);
        const WAVEFORMATEX w16 = MakeFloatWfx(48000, 2, 16, 4);
        const WAVEFORMATEX wBadAlign = MakeFloatWfx(48000, 2, 32, 4);
        const WAVEFORMATEX w4ch = MakeFloatWfx(48000, 4, 32, 16);
        CHECK(!ValidateCaptureWaveFormat(&w64, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w16, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&wBadAlign, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w4ch, fmt, err));
    }

    // Valid WAVE_FORMAT_EXTENSIBLE cases
    {
        const auto w2f = MakeExtensibleWfx(
            48000, 2, 32, 32, 8, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        CHECK(ValidateCaptureWaveFormat(&w2f.Format, fmt, err));
        CHECK(fmt.isExtensible);
        CHECK(fmt.sampleType == CaptureSampleType::Float32);
        CHECK(fmt.channelMask == 0x3u);

        const auto w2pZeroMask = MakeExtensibleWfx(
            44100, 2, 16, 16, 4, 0u, KSDATAFORMAT_SUBTYPE_PCM, 40);
        CHECK(ValidateCaptureWaveFormat(&w2pZeroMask.Format, fmt, err));
        CHECK(fmt.isExtensible);
        CHECK(fmt.sampleType == CaptureSampleType::Pcm16);

        const auto w1cCenter = MakeExtensibleWfx(
            48000, 1, 32, 32, 4, SPEAKER_FRONT_CENTER,
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        CHECK(ValidateCaptureWaveFormat(&w1cCenter.Format, fmt, err));
        CHECK(fmt.channels == 1u);

        const auto w1cLeft = MakeExtensibleWfx(
            48000, 1, 16, 16, 2, SPEAKER_FRONT_LEFT,
            KSDATAFORMAT_SUBTYPE_PCM, 22);
        CHECK(ValidateCaptureWaveFormat(&w1cLeft.Format, fmt, err));
        CHECK(fmt.channels == 1u);
    }

    // Invalid WAVE_FORMAT_EXTENSIBLE cases
    {
        // Truncated cbSize
        const auto wTrunc = MakeExtensibleWfx(
            48000, 2, 32, 32, 8, 0x3u, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 20);
        CHECK(!ValidateCaptureWaveFormat(&wTrunc.Format, fmt, err));

        // Padded 24-in-32 container (wValidBitsPerSample != wBitsPerSample)
        const auto wPadded24 = MakeExtensibleWfx(
            48000, 2, 32, 24, 8, 0x3u, KSDATAFORMAT_SUBTYPE_PCM, 22);
        CHECK(!ValidateCaptureWaveFormat(&wPadded24.Format, fmt, err));

        // Padded 20-in-24 container
        const auto wPadded20 = MakeExtensibleWfx(
            48000, 2, 24, 20, 6, 0x3u, KSDATAFORMAT_SUBTYPE_PCM, 22);
        CHECK(!ValidateCaptureWaveFormat(&wPadded20.Format, fmt, err));

        // 24-bit packed PCM rejected (only 16-bit PCM supported)
        const auto wPcm24 = MakeExtensibleWfx(
            48000, 2, 24, 24, 6, 0x3u, KSDATAFORMAT_SUBTYPE_PCM, 22);
        CHECK(!ValidateCaptureWaveFormat(&wPcm24.Format, fmt, err));

        // 6ch surround rejected
        const auto w51 = MakeExtensibleWfx(
            48000, 6, 32, 32, 24, 0x3Fu, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        CHECK(!ValidateCaptureWaveFormat(&w51.Format, fmt, err));

        // Mismatched dwChannelMask for 1ch
        const auto w1BadStereoMask = MakeExtensibleWfx(
            48000, 1, 32, 32, 4, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        const auto w1BadRightMask = MakeExtensibleWfx(
            48000, 1, 32, 32, 4, SPEAKER_FRONT_RIGHT,
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        CHECK(!ValidateCaptureWaveFormat(&w1BadStereoMask.Format, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w1BadRightMask.Format, fmt, err));

        // Mismatched dwChannelMask for 2ch
        const auto w2BadMask = MakeExtensibleWfx(
            48000, 2, 32, 32, 8, SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT,
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        const auto w2QuadMask = MakeExtensibleWfx(
            48000, 2, 32, 32, 8, 0xFu, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        CHECK(!ValidateCaptureWaveFormat(&w2BadMask.Format, fmt, err));
        CHECK(!ValidateCaptureWaveFormat(&w2QuadMask.Format, fmt, err));

        // Unknown SubFormat GUID
        const GUID kUnknownGuid = { 0x11223344, 0x5566, 0x7788, { 1, 2, 3, 4, 5, 6, 7, 8 } };
        const auto wUnknownSub = MakeExtensibleWfx(
            48000, 2, 32, 32, 8, 0x3u, kUnknownGuid, 22);
        CHECK(!ValidateCaptureWaveFormat(&wUnknownSub.Format, fmt, err));

        // Bad blockAlign in extensible
        const auto wExtBadAlign = MakeExtensibleWfx(
            48000, 2, 32, 32, 4, 0x3u, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        CHECK(!ValidateCaptureWaveFormat(&wExtBadAlign.Format, fmt, err));

        // Unknown wFormatTag
        WAVEFORMATEX wMp3 = MakePcmWfx(48000, 2, 16, 4);
        wMp3.wFormatTag = 0x0055;
        CHECK(!ValidateCaptureWaveFormat(&wMp3, fmt, err));

        // FormatWaveFormatSummary / DescribeValidatedFormat sanity
        const auto w2fSum = MakeExtensibleWfx(
            48000, 2, 32, 32, 8, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 22);
        CHECK(FormatWaveFormatSummary(nullptr) == "<null WAVEFORMATEX>");
        CHECK(FormatWaveFormatSummary(&w2fSum.Format).find("EXTENSIBLE(IEEE_FLOAT)") != std::string::npos);
        CHECK(DescribeValidatedFormat(ValidatedCaptureFormat{ 48000, 2, 16, 4, CaptureSampleType::Pcm16, false, 0 })
                  .find("48000 Hz / 2ch / PCM16") != std::string::npos);
    }
}

// ============================================================================
// Suite 2: Sample Conversion Tests
// ============================================================================

void TestSampleConversion() {
    std::printf("Running TestSampleConversion...\n");
    std::vector<int16_t> out;
    std::string err;

    ValidatedCaptureFormat pcm2{};
    {
        const WAVEFORMATEX w = MakePcmWfx(48000, 2, 16, 4);
        CHECK(ValidateCaptureWaveFormat(&w, pcm2, err));
    }

    // 2ch PCM16 bit-exact (preserving -32768 and 32767)
    {
        const std::vector<int16_t> in = { -32768, 32767, -1, 0, 1, -16384 };
        ConvertCaptureChunkToStereoInt16(pcm2, in.data(), 3, false, out);
        CHECK_EQ_U64(out.size(), 6u);
        for (size_t i = 0; i < in.size(); ++i) {
            CHECK_EQ_I32(out[i], in[i]);
        }
    }

    // Zero frame count
    {
        out = { 1, 2, 3 };
        ConvertCaptureChunkToStereoInt16(pcm2, nullptr, 0, false, out);
        CHECK(out.empty());
    }

    // SILENT flag overrides dirty non-zero input buffer -> exact zeros
    {
        const std::vector<int16_t> dirty = { -32768, 32767, 12345, -23456 };
        ConvertCaptureChunkToStereoInt16(pcm2, dirty.data(), 2, true, out);
        CHECK_EQ_U64(out.size(), 4u);
        for (int16_t s : out) {
            CHECK_EQ_I32(s, 0);
        }
    }

    // Null input pointer -> exact zeros
    {
        ConvertCaptureChunkToStereoInt16(pcm2, nullptr, 4, false, out);
        CHECK_EQ_U64(out.size(), 8u);
        for (int16_t s : out) {
            CHECK_EQ_I32(s, 0);
        }
    }

    // 1ch PCM16 -> stereo duplication (preserving -32768)
    {
        ValidatedCaptureFormat pcm1{};
        const WAVEFORMATEX w = MakePcmWfx(44100, 1, 16, 2);
        CHECK(ValidateCaptureWaveFormat(&w, pcm1, err));
        const std::vector<int16_t> monoIn = { -32768, 0, 1234, 32767 };
        ConvertCaptureChunkToStereoInt16(pcm1, monoIn.data(), 4, false, out);
        CHECK_EQ_U64(out.size(), 8u);
        CHECK_EQ_I32(out[0], -32768);
        CHECK_EQ_I32(out[1], -32768);
        CHECK_EQ_I32(out[2], 0);
        CHECK_EQ_I32(out[3], 0);
        CHECK_EQ_I32(out[4], 1234);
        CHECK_EQ_I32(out[5], 1234);
        CHECK_EQ_I32(out[6], 32767);
        CHECK_EQ_I32(out[7], 32767);
    }

    // IEEE Float32 edge cases: NaN, +/-Inf, +/-1e30f, +/-1.0f, +/-0.0f, subnormal
    {
        ValidatedCaptureFormat flt2{};
        const WAVEFORMATEX w = MakeFloatWfx(48000, 2, 32, 8);
        CHECK(ValidateCaptureWaveFormat(&w, flt2, err));

        const float qnan = std::numeric_limits<float>::quiet_NaN();
        const float infP = std::numeric_limits<float>::infinity();
        const float infN = -std::numeric_limits<float>::infinity();

        const std::vector<float> in = {
            qnan,   infP,       // frame 0 -> 0, 0
            infN,   1.0e30f,    // frame 1 -> 0, 32767
            -1.0e30f, 1.0f,     // frame 2 -> -32768, 32767
            -1.0f,  -0.0f,      // frame 3 -> -32768, 0
            0.5f,   -0.5f,      // frame 4 -> 16384, -16384
            1.0e-42f, -1.5f     // frame 5 -> 0, -32768
        };
        ConvertCaptureChunkToStereoInt16(flt2, in.data(), 6, false, out);
        CHECK_EQ_U64(out.size(), 12u);
        CHECK_EQ_I32(out[0], 0);
        CHECK_EQ_I32(out[1], 0);
        CHECK_EQ_I32(out[2], 0);
        CHECK_EQ_I32(out[3], 32767);
        CHECK_EQ_I32(out[4], -32768);
        CHECK_EQ_I32(out[5], 32767);
        CHECK_EQ_I32(out[6], -32768);
        CHECK_EQ_I32(out[7], 0);
        CHECK_EQ_I32(out[8], 16384);
        CHECK_EQ_I32(out[9], -16384);
        CHECK_EQ_I32(out[10], 0);
        CHECK_EQ_I32(out[11], -32768);

        for (int16_t s : out) {
            CHECK(s >= -32768 && s <= 32767);
        }
    }

    // IEEE Float32 1ch -> stereo duplication with silent vs active checks
    {
        ValidatedCaptureFormat flt1{};
        const WAVEFORMATEX w = MakeFloatWfx(48000, 1, 32, 4);
        CHECK(ValidateCaptureWaveFormat(&w, flt1, err));
        const std::vector<float> in = {
            -1.0f, std::numeric_limits<float>::quiet_NaN(), 1.0e20f
        };
        ConvertCaptureChunkToStereoInt16(flt1, in.data(), 3, false, out);
        CHECK_EQ_U64(out.size(), 6u);
        CHECK_EQ_I32(out[0], -32768);
        CHECK_EQ_I32(out[1], -32768);
        CHECK_EQ_I32(out[2], 0);
        CHECK_EQ_I32(out[3], 0);
        CHECK_EQ_I32(out[4], 32767);
        CHECK_EQ_I32(out[5], 32767);

        ConvertCaptureChunkToStereoInt16(flt1, in.data(), 3, true, out);
        for (int16_t s : out) {
            CHECK_EQ_I32(s, 0);
        }
    }
}

// ============================================================================
// Suite 3: MockAudioSessionAccess + SessionMuteJournal Tests
// ============================================================================

class MockAudioSessionAccess final : public IAudioSessionAccess {
public:
    std::vector<AudioSessionHandleInfo> sessions;
    int setCallCount = 0;
    int failOnSetCallNumber = 0; // 1-based; 0 = never fail
    bool failEnumerate = false;

    bool EnumerateSessions(std::vector<AudioSessionHandleInfo>& outSessions,
                           std::string& outError) override {
        if (failEnumerate) {
            outError = "Mock enumerate failure";
            return false;
        }
        outSessions = sessions;
        return true;
    }

    bool SetSessionMuteAndVolume(const SessionVolumeIdentity& target,
                                 bool mute,
                                 float masterVolume,
                                 std::string& outError) override {
        ++setCallCount;
        if (failOnSetCallNumber > 0 && setCallCount == failOnSetCallNumber) {
            outError = "Mock injected SetSessionMuteAndVolume failure";
            return false;
        }
        for (auto& s : sessions) {
            if (s.identity.endpointId == target.endpointId &&
                s.identity.sessionInstanceId == target.sessionInstanceId &&
                EqualsIgnoreCase(s.identity.exeFullPath, target.exeFullPath)) {
                s.state.mute = mute;
                s.state.masterVolume = masterVolume;
                return true;
            }
        }
        outError = "Mock target session not found";
        return false;
    }
};

AudioSessionHandleInfo MakeMockSession(const std::wstring& endpointId,
                                       const std::wstring& sessionInstanceId,
                                       const std::wstring& exeFullPath,
                                       uint32_t pid,
                                       bool mute,
                                       float volume,
                                       bool multiProc = false,
                                       bool sysSounds = false) {
    AudioSessionHandleInfo h;
    h.identity.endpointId = endpointId;
    h.identity.sessionInstanceId = sessionInstanceId;
    h.identity.exeFullPath = exeFullPath;
    h.identity.processId = pid;
    h.identity.isMultiProcessSession = multiProc;
    h.identity.isSystemSoundsSession = sysSounds;
    h.state.mute = mute;
    h.state.masterVolume = volume;
    h.sessionState = AudioSessionStateActive;
    return h;
}

void TestJsonEscapingAndJournalRoundtrip() {
    std::printf("Running TestJsonEscapingAndJournalRoundtrip...\n");
    std::string err;

    // 1. Escaping/unescaping complex Windows paths, quotes, control chars, UTF-8, surrogate pairs
    {
        const std::wstring complexPath =
            L"C:\\Users\\测试\"Quote\"\\AppData\\Local\\Temp\tline\n\\{0.0.0.00000000}.{ABC}\\Apple\"Music\".exe";
        const std::string utf8 = WideToUtf8(complexPath);
        const std::string escaped = EscapeJsonString(utf8);
        CHECK(escaped.find("\\\\") != std::string::npos);
        CHECK(escaped.find("\\\"") != std::string::npos);
        CHECK(escaped.find("\\t") != std::string::npos);
        CHECK(escaped.find("\\n") != std::string::npos);

        std::string roundtripUtf8;
        CHECK(UnescapeJsonString(escaped, roundtripUtf8, err));
        CHECK(roundtripUtf8 == utf8);
        CHECK(Utf8ToWide(roundtripUtf8) == complexPath);

        // Surrogate pair \uD83C\uDFB5 -> U+1F3B5 (MUSICAL NOTE)
        std::string noteUtf8;
        CHECK(UnescapeJsonString("\\uD83C\\uDFB5\\u4E2D\\u0007", noteUtf8, err));
        const std::wstring noteWide = Utf8ToWide(noteUtf8);
        CHECK_EQ_U64(noteWide.size(), 4u); // 2 UTF-16 code units + U+4E2D + U+0007
        CHECK(noteWide[2] == 0x4E2Du);
        CHECK(noteWide[3] == 0x0007u);

        // Unpaired surrogate must fail
        std::string badOut;
        CHECK(!UnescapeJsonString("\\uD800abc", badOut, err));
        CHECK(!UnescapeJsonString("dangling\\", badOut, err));
    }

    // 2. JournalDocument Serialize -> Deserialize lossless roundtrip
    {
        JournalDocument doc;
        doc.version = 1;
        doc.createdUtcIso = "2026-11-14T12:34:56.789Z";

        JournalSessionEntry e1;
        e1.endpointId = L"{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}";
        e1.sessionInstanceId = L"{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}|#%b{A}\\\"X\\\"|#1";
        e1.exeFullPath = L"C:\\Program Files\\WindowsApps\\AppleInc.AppleMusicWin_1.0\\苹果\\AppleMusic.exe";
        e1.recordedPid = 43210;
        e1.origMute = false;
        e1.origVolume = 0.375f;
        e1.appliedMute = true;
        e1.appliedVolume = 0.375f;
        doc.entries.push_back(e1);

        const std::string json = SessionMuteJournal::SerializeJournalJson(doc);
        JournalDocument parsed;
        CHECK(SessionMuteJournal::DeserializeJournalJson(json, parsed, err));
        CHECK(parsed.version == 1u);
        CHECK(parsed.createdUtcIso == doc.createdUtcIso);
        CHECK_EQ_U64(parsed.entries.size(), 1u);
        CHECK(parsed.entries[0].endpointId == e1.endpointId);
        CHECK(parsed.entries[0].sessionInstanceId == e1.sessionInstanceId);
        CHECK(parsed.entries[0].exeFullPath == e1.exeFullPath);
        CHECK(parsed.entries[0].recordedPid == e1.recordedPid);
        CHECK(parsed.entries[0].origMute == e1.origMute);
        CHECK(std::fabs(parsed.entries[0].origVolume - e1.origVolume) < 1e-6f);
        CHECK(parsed.entries[0].appliedMute == e1.appliedMute);
        CHECK(std::fabs(parsed.entries[0].appliedVolume - e1.appliedVolume) < 1e-6f);
    }

    // 3. Reject corrupt / unsupported JSON & invalid field bounds
    {
        JournalDocument badDoc;
        CHECK(!SessionMuteJournal::DeserializeJournalJson("{corrupt", badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson("{\"version\":2,\"entries\":[]}", badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson("{\"version\":4294967297,\"entries\":[]}", badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson("{\"version\":1.5,\"entries\":[]}", badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson("{\"version\":1,\"entries\":[{}]}", badDoc, err));

        auto makeEntryJson = [](const char* ep,
                                const char* siid,
                                const char* exe,
                                const char* origVol,
                                const char* appliedVol) {
            return std::string("{\"version\":1,\"entries\":[{") +
                   "\"endpointId\":\"" + ep + "\"," +
                   "\"sessionInstanceId\":\"" + siid + "\"," +
                   "\"exeFullPath\":\"" + exe + "\"," +
                   "\"origMute\":false," +
                   "\"origVolume\":" + origVol + "," +
                   "\"appliedMute\":true," +
                   "\"appliedVolume\":" + appliedVol +
                   "}]}";
        };

        // Valid boundary volumes 0.0 and 1.0 must parse
        CHECK(SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "C:\\\\App.exe", "0.0", "1.0"), badDoc, err));

        // Out-of-range origVolume (-0.1, 1.5, 1e100)
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "C:\\\\App.exe", "-0.1", "0.5"), badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "C:\\\\App.exe", "1.5", "0.5"), badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "C:\\\\App.exe", "1e100", "0.5"), badDoc, err));

        // Out-of-range appliedVolume (-0.1, 1.5, 1e100)
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "C:\\\\App.exe", "0.5", "-0.1"), badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "C:\\\\App.exe", "0.5", "1.5"), badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "C:\\\\App.exe", "0.5", "1e100"), badDoc, err));

        // Empty endpointId / sessionInstanceId / exeFullPath
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("", "{siid}", "C:\\\\App.exe", "0.5", "0.5"), badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "", "C:\\\\App.exe", "0.5", "0.5"), badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "", "0.5", "0.5"), badDoc, err));

        // Embedded NUL (\u0000) in endpointId / sessionInstanceId / exeFullPath
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}\\u0000tail", "{siid}", "C:\\\\App.exe", "0.5", "0.5"), badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}\\u0000tail", "C:\\\\App.exe", "0.5", "0.5"), badDoc, err));
        CHECK(!SessionMuteJournal::DeserializeJournalJson(
            makeEntryJson("{ep}", "{siid}", "C:\\\\App.exe\\u0000.txt", "0.5", "0.5"), badDoc, err));
    }
}

void TestSessionMuteJournalStateMatrix() {
    std::printf("Running TestSessionMuteJournalStateMatrix...\n");
    TempDirGuard tmpDir;
    std::string err;

    const std::wstring kEp1 = L"{0.0.0.00000000}.{aaaaaaaa-1111-2222-3333-444444444444}";
    const std::wstring kInstApple = L"{ep}|AppleMusic|#1";
    const std::wstring kExeApple = L"C:\\Program Files\\WindowsApps\\AppleInc\\AppleMusic.exe";

    const std::wstring kInstEdge = L"{ep}|msedge|#2";
    const std::wstring kExeEdge = L"C:\\Program Files (x86)\\Microsoft\\Edge\\msedge.exe";

    // Case 1: Normal apply + restore + idempotent second restore
    {
        const std::wstring jpath = tmpDir.ChildPath(L"case1_normal.json");
        MockAudioSessionAccess mock;
        mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeApple, 1001, false, 0.42f));
        mock.sessions.push_back(MakeMockSession(kEp1, kInstEdge, kExeEdge, 2002, false, 0.85f));

        JournalDocument appliedDoc;
        CHECK(SessionMuteJournal::BeginApplyMute(
            mock, jpath, mock.sessions[0].identity, true, 0.42f, appliedDoc, err));
        CHECK(FileExistsW(jpath));
        CHECK(mock.sessions[0].state.mute == true);
        CHECK(std::fabs(mock.sessions[0].state.masterVolume - 0.42f) < 1e-6f);
        // Unrelated session untouched
        CHECK(mock.sessions[1].state.mute == false);
        CHECK(std::fabs(mock.sessions[1].state.masterVolume - 0.85f) < 1e-6f);

        JournalRestoreSummary summary;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, summary));
        CHECK(summary.journalExisted);
        CHECK(summary.journalParsed);
        CHECK(summary.journalRemovedOrArchived);
        CHECK(!FileExistsW(jpath));
        CHECK_EQ_U64(summary.items.size(), 1u);
        CHECK(summary.items[0].status == RestoreStatus::Restored);
        CHECK(mock.sessions[0].state.mute == false);
        CHECK(std::fabs(mock.sessions[0].state.masterVolume - 0.42f) < 1e-6f);

        // Idempotent second call when journal no longer exists
        JournalRestoreSummary second;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, second));
        CHECK(!second.journalExisted);
    }

    // Case 2: Crash-leftover journal recovery on restart (PID changed, case-variant exeFullPath)
    {
        const std::wstring jpath = tmpDir.ChildPath(L"case2_crash_restart.json");
        JournalDocument leftover;
        leftover.version = 1;
        leftover.createdUtcIso = "2026-11-14T00:00:00.000Z";
        JournalSessionEntry e;
        e.endpointId = kEp1;
        e.sessionInstanceId = kInstApple;
        e.exeFullPath = L"c:\\program files\\windowsapps\\appleinc\\APPLEMUSIC.EXE";
        e.recordedPid = 1111; // Old pre-crash PID
        e.origMute = false;
        e.origVolume = 0.75f;
        e.appliedMute = true;
        e.appliedVolume = 0.50f;
        leftover.entries.push_back(e);

        CHECK(WriteTextFileAtomic(jpath, SessionMuteJournal::SerializeJournalJson(leftover), err));
        CHECK(FileExistsW(jpath));

        MockAudioSessionAccess mock;
        // Live session has DIFFERENT PID 9999, but same endpoint + sessionInstanceId + case-variant path
        mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeApple, 9999, true, 0.50f));

        JournalRestoreSummary summary;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, summary));
        CHECK(summary.journalExisted && summary.journalParsed && summary.journalRemovedOrArchived);
        CHECK(!FileExistsW(jpath));
        CHECK_EQ_U64(summary.items.size(), 1u);
        CHECK(summary.items[0].status == RestoreStatus::Restored);
        CHECK(mock.sessions[0].state.mute == false);
        CHECK(std::fabs(mock.sessions[0].state.masterVolume - 0.75f) < 1e-6f);
    }

    // Case 3: User manually unmuted while active -> SkippedUserModified (do NOT overwrite!)
    {
        const std::wstring jpath = tmpDir.ChildPath(L"case3_user_unmute.json");
        MockAudioSessionAccess mock;
        mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeApple, 1001, false, 0.60f));

        JournalDocument appliedDoc;
        CHECK(SessionMuteJournal::BeginApplyMute(
            mock, jpath, mock.sessions[0].identity, true, 0.60f, appliedDoc, err));

        // User manually unmutes in Windows volume mixer
        mock.sessions[0].state.mute = false;
        const int callsBeforeRestore = mock.setCallCount;

        JournalRestoreSummary summary;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, summary));
        CHECK(summary.journalRemovedOrArchived);
        CHECK(!FileExistsW(jpath));
        CHECK_EQ_U64(summary.items.size(), 1u);
        CHECK(summary.items[0].status == RestoreStatus::SkippedUserModified);
        CHECK_EQ_I32(mock.setCallCount, callsBeforeRestore);
        CHECK(mock.sessions[0].state.mute == false);
    }

    // Case 4: User manually adjusted volume > 1e-3f -> SkippedUserModified,
    //         vs minor <= 1e-3f float drift -> Restored
    {
        const std::wstring jpath = tmpDir.ChildPath(L"case4a_user_vol.json");
        MockAudioSessionAccess mock;
        mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeApple, 1001, false, 0.80f));

        JournalDocument appliedDoc;
        CHECK(SessionMuteJournal::BeginApplyMute(
            mock, jpath, mock.sessions[0].identity, true, 0.60f, appliedDoc, err));

        // User drags slider by 0.002 (> 1e-3f)
        mock.sessions[0].state.masterVolume = 0.602f;
        const int callsBeforeRestore = mock.setCallCount;

        JournalRestoreSummary summary;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, summary));
        CHECK_EQ_U64(summary.items.size(), 1u);
        CHECK(summary.items[0].status == RestoreStatus::SkippedUserModified);
        CHECK_EQ_I32(mock.setCallCount, callsBeforeRestore);
        CHECK(std::fabs(mock.sessions[0].state.masterVolume - 0.602f) < 1e-6f);

        // Subcase 4b: tiny float quantization drift (0.0002f <= 1e-3f) still restores
        const std::wstring jpathTol = tmpDir.ChildPath(L"case4b_tol.json");
        mock.sessions[0].state.mute = false;
        mock.sessions[0].state.masterVolume = 0.80f;
        CHECK(SessionMuteJournal::BeginApplyMute(
            mock, jpathTol, mock.sessions[0].identity, true, 0.60f, appliedDoc, err));
        mock.sessions[0].state.masterVolume = 0.6002f;

        JournalRestoreSummary summaryTol;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpathTol, summaryTol));
        CHECK_EQ_U64(summaryTol.items.size(), 1u);
        CHECK(summaryTol.items[0].status == RestoreStatus::Restored);
        CHECK(mock.sessions[0].state.mute == false);
        CHECK(std::fabs(mock.sessions[0].state.masterVolume - 0.80f) < 1e-6f);
    }

    // Case 5: Session disappeared -> SkippedSessionGone
    {
        const std::wstring jpath = tmpDir.ChildPath(L"case5_gone.json");
        MockAudioSessionAccess mock;
        mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeApple, 1001, false, 0.70f));

        JournalDocument appliedDoc;
        CHECK(SessionMuteJournal::BeginApplyMute(
            mock, jpath, mock.sessions[0].identity, true, 0.70f, appliedDoc, err));

        // Process exited and Windows removed the session
        mock.sessions.clear();
        const int callsBeforeRestore = mock.setCallCount;

        JournalRestoreSummary summary;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, summary));
        CHECK(summary.journalRemovedOrArchived);
        CHECK(!FileExistsW(jpath));
        CHECK_EQ_U64(summary.items.size(), 1u);
        CHECK(summary.items[0].status == RestoreStatus::SkippedSessionGone);
        CHECK_EQ_I32(mock.setCallCount, callsBeforeRestore);
    }

    // Case 6: Same PID reused, but different exeFullPath or different sessionInstanceId or endpointId
    //         -> MUST refuse mismatch (SkippedSessionGone)
    {
        const std::wstring jpath = tmpDir.ChildPath(L"case6_pid_collision.json");
        JournalDocument leftover;
        leftover.version = 1;
        JournalSessionEntry e;
        e.endpointId = kEp1;
        e.sessionInstanceId = kInstApple;
        e.exeFullPath = kExeApple;
        e.recordedPid = 4242;
        e.origMute = false;
        e.origVolume = 0.90f;
        e.appliedMute = true;
        e.appliedVolume = 0.90f;
        leftover.entries.push_back(e);

        // 6a: Same PID + same endpoint + same instanceId, DIFFERENT exeFullPath
        {
            CHECK(WriteTextFileAtomic(jpath, SessionMuteJournal::SerializeJournalJson(leftover), err));
            MockAudioSessionAccess mock;
            mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeEdge, 4242, true, 0.90f));
            JournalRestoreSummary summary;
            CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, summary));
            CHECK_EQ_U64(summary.items.size(), 1u);
            CHECK(summary.items[0].status == RestoreStatus::SkippedSessionGone);
            CHECK_EQ_I32(mock.setCallCount, 0);
            CHECK(mock.sessions[0].state.mute == true);
        }

        // 6b: Same PID + same endpoint + same exeFullPath, DIFFERENT sessionInstanceId
        {
            CHECK(WriteTextFileAtomic(jpath, SessionMuteJournal::SerializeJournalJson(leftover), err));
            MockAudioSessionAccess mock;
            mock.sessions.push_back(MakeMockSession(kEp1, L"{ep}|AppleMusic|#NEW", kExeApple, 4242, true, 0.90f));
            JournalRestoreSummary summary;
            CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, summary));
            CHECK_EQ_U64(summary.items.size(), 1u);
            CHECK(summary.items[0].status == RestoreStatus::SkippedSessionGone);
            CHECK_EQ_I32(mock.setCallCount, 0);
            CHECK(mock.sessions[0].state.mute == true);
        }

        // 6c: Same PID + same instanceId + same exeFullPath, DIFFERENT endpointId
        {
            CHECK(WriteTextFileAtomic(jpath, SessionMuteJournal::SerializeJournalJson(leftover), err));
            MockAudioSessionAccess mock;
            mock.sessions.push_back(MakeMockSession(L"{other-ep}", kInstApple, kExeApple, 4242, true, 0.90f));
            JournalRestoreSummary summary;
            CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpath, summary));
            CHECK_EQ_U64(summary.items.size(), 1u);
            CHECK(summary.items[0].status == RestoreStatus::SkippedSessionGone);
            CHECK_EQ_I32(mock.setCallCount, 0);
            CHECK(mock.sessions[0].state.mute == true);
        }
    }

    // Case 7: Safety refusals (multi-process, system sounds, incomplete identity, rollback on partial fail)
    {
        const std::wstring jpathRefuse = tmpDir.ChildPath(L"case7_refuse.json");
        MockAudioSessionAccess mock;
        mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeApple, 1001, false, 0.5f, true, false));
        mock.sessions.push_back(MakeMockSession(kEp1, kInstEdge, kExeEdge, 1002, false, 0.5f, false, true));

        JournalDocument doc;
        CHECK(!SessionMuteJournal::BeginApplyMute(
            mock, jpathRefuse, mock.sessions[0].identity, true, 0.5f, doc, err));
        CHECK(!FileExistsW(jpathRefuse));
        CHECK(!SessionMuteJournal::BeginApplyMute(
            mock, jpathRefuse, mock.sessions[1].identity, true, 0.5f, doc, err));
        CHECK(!FileExistsW(jpathRefuse));

        // Partial SetSessionMuteAndVolume failure rolls back earlier entry and clears journal
        mock.sessions.clear();
        mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeApple, 1001, false, 0.55f));
        mock.sessions.push_back(MakeMockSession(kEp1, L"{ep}|AppleMusic|#2", kExeApple, 1002, false, 0.65f));
        mock.failOnSetCallNumber = 2; // #1 succeeds, #2 fails -> rollback #1 via #3
        const std::vector<SessionVolumeIdentity> targets = {
            mock.sessions[0].identity, mock.sessions[1].identity
        };
        CHECK(!SessionMuteJournal::BeginApplyMute(
            mock, jpathRefuse, targets, true, nullptr, doc, err));
        CHECK(!FileExistsW(jpathRefuse));
        CHECK(mock.sessions[0].state.mute == false);
        CHECK(std::fabs(mock.sessions[0].state.masterVolume - 0.55f) < 1e-6f);

        // If RestoreFromJournal encounters FailedWrite, journal MUST persist on disk for retry
        mock.failOnSetCallNumber = 0;
        CHECK(SessionMuteJournal::BeginApplyMute(
            mock, jpathRefuse, { mock.sessions[0].identity }, true, nullptr, doc, err));
        CHECK(FileExistsW(jpathRefuse));
        mock.failOnSetCallNumber = mock.setCallCount + 1;
        JournalRestoreSummary failSummary;
        CHECK(!SessionMuteJournal::RestoreFromJournal(mock, jpathRefuse, failSummary));
        CHECK(FileExistsW(jpathRefuse));
        CHECK_EQ_U64(failSummary.items.size(), 1u);
        CHECK(failSummary.items[0].status == RestoreStatus::FailedWrite);

        // Retry succeeds cleanly
        mock.failOnSetCallNumber = 0;
        JournalRestoreSummary retrySummary;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpathRefuse, retrySummary));
        CHECK(!FileExistsW(jpathRefuse));
        CHECK_EQ_U64(retrySummary.items.size(), 1u);
        CHECK(retrySummary.items[0].status == RestoreStatus::Restored);
        CHECK(mock.sessions[0].state.mute == false);
    }

    // Case 8: Corrupt journal file preserves file and reports !journalParsed
    {
        const std::wstring jpathCorrupt = tmpDir.ChildPath(L"case8_corrupt.json");
        CHECK(WriteTextFileAtomic(jpathCorrupt, "{invalid json", err));
        MockAudioSessionAccess mock;
        JournalRestoreSummary summary;
        CHECK(!SessionMuteJournal::RestoreFromJournal(mock, jpathCorrupt, summary));
        CHECK(summary.journalExisted);
        CHECK(!summary.journalParsed);
        CHECK(!summary.journalRemovedOrArchived);
        CHECK(FileExistsW(jpathCorrupt));
    }

    // Case 9: BeginApplyMute refuses to overwrite an unrecovered existing journal file
    {
        const std::wstring jpathExisting = tmpDir.ChildPath(L"case9_existing.json");
        JournalDocument preCrashDoc;
        preCrashDoc.version = 1;
        preCrashDoc.createdUtcIso = "2026-11-14T00:00:00.000Z";
        JournalSessionEntry preEntry;
        preEntry.endpointId = kEp1;
        preEntry.sessionInstanceId = kInstApple;
        preEntry.exeFullPath = kExeApple;
        preEntry.recordedPid = 1234;
        preEntry.origMute = false;
        preEntry.origVolume = 0.77f;
        preEntry.appliedMute = true;
        preEntry.appliedVolume = 0.77f;
        preCrashDoc.entries.push_back(preEntry);
        CHECK(WriteTextFileAtomic(jpathExisting, SessionMuteJournal::SerializeJournalJson(preCrashDoc), err));

        MockAudioSessionAccess mock;
        mock.sessions.push_back(MakeMockSession(kEp1, kInstApple, kExeApple, 5678, true, 0.77f));

        JournalDocument newDoc;
        CHECK(!SessionMuteJournal::BeginApplyMute(
            mock, jpathExisting, mock.sessions[0].identity, true, 0.25f, newDoc, err));
        CHECK(!err.empty());
        CHECK_EQ_I32(mock.setCallCount, 0);
        CHECK(FileExistsW(jpathExisting));

        // Verify original pre-crash journal content was preserved intact
        std::string rawExisting;
        CHECK(ReadTextFileUtf8(jpathExisting, rawExisting, err));
        JournalDocument readBack;
        CHECK(SessionMuteJournal::DeserializeJournalJson(rawExisting, readBack, err));
        CHECK_EQ_U64(readBack.entries.size(), 1u);
        CHECK(std::fabs(readBack.entries[0].origVolume - 0.77f) < 1e-6f);

        // Recovering first cleans journal, allowing subsequent BeginApplyMute to succeed
        JournalRestoreSummary recSummary;
        CHECK(SessionMuteJournal::RestoreFromJournal(mock, jpathExisting, recSummary));
        CHECK(!FileExistsW(jpathExisting));
        CHECK(mock.sessions[0].state.mute == false);
        CHECK(SessionMuteJournal::BeginApplyMute(
            mock, jpathExisting, mock.sessions[0].identity, true, 0.50f, newDoc, err));
        CHECK(FileExistsW(jpathExisting));
        CHECK(mock.sessions[0].state.mute == true);
    }
}

// ============================================================================
// Suite 4: Pure Process Tree & Matching Helpers
// ============================================================================

void TestProcessTreeHelpers() {
    std::printf("Running TestProcessTreeHelpers...\n");
    std::vector<ProcessEntry> procs = {
        { 10, 1, L"explorer.exe", L"C:\\Windows\\explorer.exe" },
        { 100, 10, L"AppleMusic.exe", L"C:\\Apps\\AppleMusic.exe" },
        { 101, 100, L"WebView2.exe", L"C:\\Apps\\WebView2.exe" },
        { 102, 101, L"AudioRenderer.exe", L"C:\\Apps\\AudioRenderer.exe" },
        { 200, 10, L"msedge.exe", L"C:\\Apps\\msedge.exe" },
    };

    const auto applePids = FindPidsByExeName(procs, L"applemusic.exe");
    CHECK_EQ_U64(applePids.size(), 1u);
    CHECK(applePids[0] == 100u);

    const auto tree = CollectProcessTreePids(procs, 100u);
    CHECK_EQ_U64(tree.size(), 3u);
    CHECK(std::find(tree.begin(), tree.end(), 100u) != tree.end());
    CHECK(std::find(tree.begin(), tree.end(), 101u) != tree.end());
    CHECK(std::find(tree.begin(), tree.end(), 102u) != tree.end());
    CHECK(std::find(tree.begin(), tree.end(), 200u) == tree.end());

    // Cycle safety
    std::vector<ProcessEntry> cyclic = {
        { 50, 51, L"a.exe", L"" },
        { 51, 50, L"b.exe", L"" },
    };
    const auto cycTree = CollectProcessTreePids(cyclic, 50u);
    CHECK_EQ_U64(cycTree.size(), 2u);
}

} // namespace

int wmain() {
    std::printf("=== Stage D Phase D0 Pure-Offline Selftest ===\n");
    TestFormatValidation();
    TestSampleConversion();
    TestJsonEscapingAndJournalRoundtrip();
    TestSessionMuteJournalStateMatrix();
    TestProcessTreeHelpers();

    std::printf("\nSummary: totalChecks=%d, failedChecks=%d\n", g_totalChecks, g_failedChecks);
    if (g_failedChecks != 0) {
        std::fprintf(stderr, "STAGE D OFFLINE SELFTEST FAILED!\n");
        return 1;
    }
    std::printf("STAGE D OFFLINE SELFTEST PASSED!\n");
    return 0;
}
