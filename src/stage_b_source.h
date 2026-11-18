#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#include "stage_c_ipc.h"

namespace probe {

enum class StageBMode : uint8_t {
    Off = 0,
    Synth = 1,
    Wav = 2,
    Ipc = 3,
};

enum class StageBInitStatus : uint8_t {
    DisabledDefaultOff = 0,        // INI missing or [StageB] Mode absent -> safe default off
    DisabledIniOff = 1,            // Mode=off explicitly configured
    DisabledInvalidMode = 2,       // Unrecognized Mode string -> safe fallback off
    SynthReady = 3,                // B1 deterministic synth active
    WavReady = 4,                  // B2 in-memory 48kHz/2ch/16-bit WAV validated & active
    FallbackWavMissing = 5,        // Mode=wav but file could not be opened -> passthrough
    FallbackWavBadFormat = 6,      // Mode=wav but RIFF/PCM/48000Hz/2ch/16-bit check failed -> passthrough
    FallbackWavLengthMismatch = 7, // Mode=wav but PCM byte length != ExpectedPcmBytes -> passthrough
    IpcReady = 8,                  // Stage C cross-process IPC consumer mode configured & active
};

struct StageBConfigSnapshot {
    StageBMode requestedMode = StageBMode::Off;
    StageBMode effectiveMode = StageBMode::Off;
    StageBInitStatus status = StageBInitStatus::DisabledDefaultOff;
    std::wstring iniPath;
    std::wstring wavPath;
    uint64_t expectedPcmBytes = 8647104ULL;
    uint64_t loadedWavPcmBytes = 0;
    StageCIpcConfig ipcConfig;
};

// Frozen singleton managing startup configuration, preloaded read-only WAV memory buffer,
// and Stage C IPC configuration dispatch.
// Thread-safe lazy initialization outside DllMain loader lock; zero disk I/O on GetNextSample hot path.
class StageBManager {
public:
    static constexpr uint32_t kExpectedSampleRate = 48000;
    static constexpr uint16_t kExpectedChannels = 2;
    static constexpr uint16_t kExpectedBitsPerSample = 16;
    static constexpr uint16_t kExpectedBlockAlign = 4;
    static constexpr uint64_t kDefaultTargetPcmBytes = 8647104ULL; // 2,161,776 stereo frames
    static constexpr const wchar_t* kTargetFileName = L"IV_Probe_Target_45s.mp3";

    static StageBManager& Instance();

    // Called in DllMain(DLL_PROCESS_ATTACH) ONLY to record module directory path (no file I/O).
    void SetModuleHandle(HMODULE hModule);

    // Called once lazily when the first Reader is constructed; loads INI (WAV into RAM if Mode=wav,
    // or configures StageCIpcManager if Mode=ipc) and emits "StageB_ConfigLoaded" JSONL event.
    void EnsureInitialized();

    // Called in DllMain(DLL_PROCESS_DETACH) when dynamically unloaded (lpReserved == nullptr) for clean selftest cycles.
    void Shutdown();

    const StageBConfigSnapshot& GetConfig() const;

    // Case-insensitive filename leaf check against "IV_Probe_Target_45s.mp3" (supports '\\' and '/').
    static bool IsTargetTrackPath(const wchar_t* fullOrRelPath);

    // Mode-aware target track matcher:
    //  - For Mode=synth / Mode=wav: matches "IV_Probe_Target_45s.mp3" only.
    //  - For Mode=ipc: matches case-insensitively against [StageC] TargetFiles list
    //    (defaulting to "IV_Probe_Target_45s.mp3" if TargetFiles is omitted/empty).
    bool MatchesActiveTargetTrack(const wchar_t* fullOrRelPath) const;

    // Stateless, deterministic frame-indexed B1 synthesizer (48kHz, 2ch interleaved int16_t).
    static void RenderSynthFrames(uint64_t startFrame, uint32_t frameCount, int16_t* outInterleavedLR);

    // B2 in-memory WAV frame copy (zero disk I/O).
    bool CopyWavFrames(uint64_t startFrame, uint32_t frameCount, int16_t* outInterleavedLR) const;

    static const char* ModeToString(StageBMode mode);
    static const char* StatusToString(StageBInitStatus status);

private:
    StageBManager() = default;
    ~StageBManager() = default;

    void InitializeImpl();
    StageBInitStatus LoadAndValidateWav(const std::wstring& path, uint64_t expectedBytes);

    std::wstring moduleDir_;
    StageBConfigSnapshot config_{};
    std::vector<int16_t> wavSamples_; // Interleaved L, R samples (size = loadedWavPcmBytes / 2)
    CRITICAL_SECTION initCs_{};
    bool csReady_ = false;
    volatile LONG initialized_ = 0;
};

} // namespace probe
