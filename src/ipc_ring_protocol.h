#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace stagec {

// ============================================================================
// Compile-Time Constants & Prime FrameCode Bijection Parameters
// ============================================================================

inline constexpr uint32_t kIpcMagic             = 0x31435649u; // 'IVC1' little-endian
inline constexpr uint32_t kIpcVersion           = 1u;
inline constexpr uint32_t kSampleRate           = 48000u;
inline constexpr uint32_t kChannels             = 2u;
inline constexpr uint32_t kBitsPerSample        = 16u;
inline constexpr uint32_t kBytesPerFrame        = 4u;          // 2ch * int16_t
inline constexpr uint32_t kRingCapacityFrames   = 1u << 20;    // 1,048,576 stereo frames (~21.845s capacity)
inline constexpr uint32_t kRingFrameMask        = kRingCapacityFrames - 1u;
inline constexpr uint32_t kRingPayloadBytes     = kRingCapacityFrames * kBytesPerFrame; // 4,194,304 B (4 MiB)
inline constexpr uint32_t kHeaderBytes          = 256u;
inline constexpr uint32_t kTotalSharedBytes     = kHeaderBytes + kRingPayloadBytes;     // 4,194,560 B (0x00400100)
inline constexpr uint32_t kPreloadBurstSamples  = 221u;        // GTA IV verified Burst 1 sample count (254,064 frames = 5.293s)
inline constexpr uint32_t kGuardFrames          = 48000u;      // 1.0s pre-copy safety guard against imminent wrap

inline constexpr const wchar_t* kDefaultMappingName = L"Local\\GTAIV_StageC_IpcRing_v1";
inline constexpr const wchar_t* kProducerMutexSuffix = L"_ProducerMutex";

// 29-bit prime modulus (2^29 - 3 = 536,870,909):
//  - Strictly < 2^29 (536,870,912), coprime to kRingCapacityFrames (2^20).
//  - Non-repeating period at 48,000 Hz = 11,184.81 seconds (~3.107 hours).
//  - Encodes bijectively into stereo (L, R) with L in [-16384, +16383] and R in [-16384, -1] U [+1, +16384].
//  - R is NEVER 0, so digital silence (0, 0) NEVER collides with any valid FrameCode frame!
inline constexpr uint64_t kFrameCodeMod         = 536870909ULL;
inline constexpr uint64_t kFrameCodeGenSalt     = 1000003ULL;

constexpr bool IsPrimeConstexpr(uint64_t n) {
    if (n < 2ULL) return false;
    if ((n % 2ULL) == 0ULL) return n == 2ULL;
    for (uint64_t d = 3ULL; d * d <= n; d += 2ULL) {
        if ((n % d) == 0ULL) return false;
    }
    return true;
}
static_assert(kFrameCodeMod < (1ULL << 29), "kFrameCodeMod must fit in 29 bits");
static_assert(IsPrimeConstexpr(kFrameCodeMod), "kFrameCodeMod must be prime");

enum class ProducerState : uint32_t {
    Uninit    = 0u,
    Starting  = 1u,
    Streaming = 2u,
    Paused    = 3u,
    Stopping  = 4u,
    Stopped   = 5u,
};

enum class ProducerPattern : uint32_t {
    Tone      = 0u, // Audible clean dual-frequency stereo chord + 5s chime
    FrameCode = 1u, // 29-bit prime bijective (frameIndex, sessionGeneration) verifier
};

// ============================================================================
// Cross-Bitness Fixed-Layout Shared Memory Header (256 Bytes = 4 Cache Lines)
// Rules:
//  - Fixed-width integer types ONLY (zero pointers, size_t, bool, or std::atomic).
//  - Every 64-bit field is naturally 8-byte aligned for x86 LOCK CMPXCHG8B atomicity.
//  - session_generation protocol:
//      0          = Uninitialized mapping
//      Odd (>=1)  = Producer holds mutex and is re-initializing header/ring (consumers MUST NOT read)
//      Even (>=2) = Ready session; monotonically increments across restarts (never reused/zeroed)
// ============================================================================

struct alignas(64) StageCIpcRingHeader {
    // --- Cache Line 0 [0x00..0x3F]: Session Identity & Static Format ---
    uint32_t magic;                     // 0x00: kIpcMagic ('IVC1')
    uint32_t version;                   // 0x04: kIpcVersion (1)
    uint32_t header_bytes;              // 0x08: 256
    uint32_t total_bytes;               // 0x0C: 4,194,560
    uint32_t sample_rate;               // 0x10: 48000
    uint32_t channels;                  // 0x14: 2
    uint32_t bits_per_sample;           // 0x18: 16
    uint32_t capacity_frames;           // 0x1C: 1,048,576
    uint64_t session_generation;        // 0x20: 0=uninit, odd=init-in-progress, even>=2=ready
    uint64_t session_id;                // 0x28: Unique producer run identifier
    uint64_t qpc_freq;                  // 0x30: QueryPerformanceFrequency()
    uint32_t producer_pid;              // 0x38: Producer Win32 PID
    uint32_t producer_arch;             // 0x3C: 64 or 32

    // --- Cache Line 1 [0x40..0x7F]: Producer Hot State (Written ONLY by Producer) ---
    uint64_t write_reserve_frame;       // 0x40: Published BEFORE writing PCM slots [w, w+N)
    uint64_t write_commit_frame;        // 0x48: Published AFTER writing PCM slots [w, w+N)
    int64_t  last_heartbeat_qpc;        // 0x50: Monotonic QPC timestamp updated each tick
    int64_t  start_qpc;                 // 0x58: Stream start QPC timestamp
    uint64_t producer_tick_count;       // 0x60: Monotonic producer tick counter
    uint32_t producer_state;            // 0x68: ProducerState underlying uint32_t
    uint32_t pattern_mode;              // 0x6C: ProducerPattern underlying uint32_t
    uint64_t reserved_cl1[2];           // 0x70..0x7F: Reserved (0)

    // --- Cache Line 2 [0x80..0xBF]: Consumer Telemetry (Written ONLY by Consumer) ---
    uint64_t consumer_read_frame;       // 0x80: Current liveReadFrame cursor
    int64_t  consumer_heartbeat_qpc;    // 0x88: Consumer last active pull QPC
    uint64_t watermark_frames;          // 0x90: Latest observed (commit - liveReadFrame)
    uint64_t underrun_events;           // 0x98: Count of active pulls with insufficient committed frames
    uint64_t underrun_frames;           // 0xA0: Total zero-padded frames emitted on underruns
    uint64_t overrun_events;            // 0xA8: Count of lag snaps when producer laps/approaches reader
    uint64_t overrun_skipped_frames;    // 0xB0: Total source frames skipped on overrun snaps
    uint32_t consumer_pid;              // 0xB8: Consumer Win32 PID
    uint32_t consumer_active_readers;   // 0xBC: Count of open IPC playback readers

    // --- Cache Line 3 [0xC0..0xFF]: Conflict, Dual-Slot & Disconnect Audit Counters ---
    uint64_t overwrite_conflict_events; // 0xC0: Post-copy reserveAfter/generation conflict detections
    uint64_t undecided_silence_frames;  // 0xC8: Frames silenced during seq <= 221 undecided preload
    uint64_t preload_resync_events;     // 0xD0: Controlled handoffs when next reader enters seq == 222
    uint64_t preload_skipped_frames;    // 0xD8: Source frames elapsed between primary EOF and next seq == 222
    uint64_t silence_disconnect_frames; // 0xE0: Frames silenced while producer offline/timed out
    uint64_t session_attach_count;      // 0xE8: Producer session generations attached by consumer
    uint64_t last_frame_age_us;         // 0xF0: Estimated age (us) of most recently delivered live frame
    uint64_t reserved_cl3;              // 0xF8..0xFF: Reserved (0)
};

struct StageCIpcSharedBlock {
    StageCIpcRingHeader header;
    int16_t pcm_samples[kRingCapacityFrames * kChannels]; // Interleaved L, R starting at byte offset 256
};

// ============================================================================
// Mandatory Compile-Time Cross-Bitness Layout & Alignment Assertions
// ============================================================================

static_assert(sizeof(StageCIpcRingHeader) == 256, "StageCIpcRingHeader must be exactly 256 bytes");
static_assert(alignof(StageCIpcRingHeader) == 64, "StageCIpcRingHeader must be 64-byte cache-line aligned");
static_assert(sizeof(StageCIpcSharedBlock) == kTotalSharedBytes, "StageCIpcSharedBlock size mismatch");
static_assert(offsetof(StageCIpcSharedBlock, header) == 0, "header offset must be 0");
static_assert(offsetof(StageCIpcSharedBlock, pcm_samples) == 256, "pcm_samples offset must be 256");

// Cache Line 0 offsets
static_assert(offsetof(StageCIpcRingHeader, magic) == 0x00, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, version) == 0x04, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, header_bytes) == 0x08, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, total_bytes) == 0x0C, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, sample_rate) == 0x10, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, channels) == 0x14, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, bits_per_sample) == 0x18, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, capacity_frames) == 0x1C, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, session_generation) == 0x20, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, session_id) == 0x28, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, qpc_freq) == 0x30, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, producer_pid) == 0x38, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, producer_arch) == 0x3C, "offset mismatch");

// Cache Line 1 offsets
static_assert(offsetof(StageCIpcRingHeader, write_reserve_frame) == 0x40, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, write_commit_frame) == 0x48, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, last_heartbeat_qpc) == 0x50, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, start_qpc) == 0x58, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, producer_tick_count) == 0x60, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, producer_state) == 0x68, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, pattern_mode) == 0x6C, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, reserved_cl1) == 0x70, "offset mismatch");

// Cache Line 2 offsets
static_assert(offsetof(StageCIpcRingHeader, consumer_read_frame) == 0x80, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, consumer_heartbeat_qpc) == 0x88, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, watermark_frames) == 0x90, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, underrun_events) == 0x98, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, underrun_frames) == 0xA0, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, overrun_events) == 0xA8, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, overrun_skipped_frames) == 0xB0, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, consumer_pid) == 0xB8, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, consumer_active_readers) == 0xBC, "offset mismatch");

// Cache Line 3 offsets
static_assert(offsetof(StageCIpcRingHeader, overwrite_conflict_events) == 0xC0, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, undecided_silence_frames) == 0xC8, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, preload_resync_events) == 0xD0, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, preload_skipped_frames) == 0xD8, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, silence_disconnect_frames) == 0xE0, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, session_attach_count) == 0xE8, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, last_frame_age_us) == 0xF0, "offset mismatch");
static_assert(offsetof(StageCIpcRingHeader, reserved_cl3) == 0xF8, "offset mismatch");

// 8-byte alignment check on all 64-bit atomic fields
static_assert((offsetof(StageCIpcRingHeader, session_generation) % 8) == 0, "64-bit align");
static_assert((offsetof(StageCIpcRingHeader, write_reserve_frame) % 8) == 0, "64-bit align");
static_assert((offsetof(StageCIpcRingHeader, write_commit_frame) % 8) == 0, "64-bit align");
static_assert((offsetof(StageCIpcRingHeader, last_heartbeat_qpc) % 8) == 0, "64-bit align");
static_assert((offsetof(StageCIpcRingHeader, consumer_read_frame) % 8) == 0, "64-bit align");

// ============================================================================
// Cross-Bitness Full-Fence Hardware Atomic Helpers (x86 LOCK CMPXCHG8B & x64)
// ============================================================================

inline uint32_t AtomicLoad32(const uint32_t* addr) noexcept {
    auto* p = reinterpret_cast<volatile LONG*>(const_cast<uint32_t*>(addr));
    return static_cast<uint32_t>(InterlockedCompareExchange(p, 0L, 0L));
}

inline void AtomicStore32(uint32_t* addr, uint32_t val) noexcept {
    auto* p = reinterpret_cast<volatile LONG*>(addr);
    InterlockedExchange(p, static_cast<LONG>(val));
}

inline uint64_t AtomicLoadU64(const uint64_t* addr) noexcept {
    auto* p = reinterpret_cast<volatile LONGLONG*>(const_cast<uint64_t*>(addr));
    return static_cast<uint64_t>(InterlockedCompareExchange64(p, 0LL, 0LL));
}

inline void AtomicStoreU64(uint64_t* addr, uint64_t val) noexcept {
    auto* p = reinterpret_cast<volatile LONGLONG*>(addr);
    InterlockedExchange64(p, static_cast<LONGLONG>(val));
}

inline uint64_t AtomicAddU64(uint64_t* addr, uint64_t delta) noexcept {
    auto* p = reinterpret_cast<volatile LONGLONG*>(addr);
    return static_cast<uint64_t>(InterlockedExchangeAdd64(p, static_cast<LONGLONG>(delta))) + delta;
}

inline int64_t AtomicLoadI64(const int64_t* addr) noexcept {
    auto* p = reinterpret_cast<volatile LONGLONG*>(const_cast<int64_t*>(addr));
    return static_cast<int64_t>(InterlockedCompareExchange64(p, 0LL, 0LL));
}

inline void AtomicStoreI64(int64_t* addr, int64_t val) noexcept {
    auto* p = reinterpret_cast<volatile LONGLONG*>(addr);
    InterlockedExchange64(p, static_cast<LONGLONG>(val));
}

// Monotonically computes the next odd initialization generation from any prior generation:
//   0 -> 1 (ready = 2)
//   2 -> 3 (ready = 4)
//   3 (crashed mid-init) -> 5 (ready = 6)
constexpr uint64_t NextOddInitGeneration(uint64_t prevGen) noexcept {
    return ((prevGen & 1ULL) == 0ULL) ? (prevGen + 1ULL) : (prevGen + 2ULL);
}

constexpr bool IsReadyEvenGeneration(uint64_t gen) noexcept {
    return (gen >= 2ULL) && ((gen & 1ULL) == 0ULL);
}

// ============================================================================
// 29-Bit Prime Bijective FrameCode Encoder / Decoder
//   - Zero collision with digital silence (0, 0) because outR is strictly non-zero.
//   - Built-in parity check between lo15 bit 0 and sign(outR) catches L/R tearing.
// ============================================================================

inline void EncodeFrameCode(uint64_t frameIndex, uint64_t sessionGeneration, int16_t& outL, int16_t& outR) noexcept {
    const uint64_t salt = ((sessionGeneration % kFrameCodeMod) * kFrameCodeGenSalt) % kFrameCodeMod;
    const uint32_t code = static_cast<uint32_t>(((frameIndex % kFrameCodeMod) + salt) % kFrameCodeMod);
    const uint32_t lo15 = code & 0x7FFFu;
    const uint32_t hi14 = (code >> 15) & 0x3FFFu;
    outL = static_cast<int16_t>(static_cast<int32_t>(lo15) - 16384);
    const int32_t magR = static_cast<int32_t>(hi14) + 1; // [1 .. 16384], never 0
    outR = static_cast<int16_t>((lo15 & 1u) ? magR : -magR);
}

inline bool IsExactSilenceFrame(int16_t inL, int16_t inR) noexcept {
    return (inL == 0) && (inR == 0);
}

inline bool DecodeFrameCode(int16_t inL, int16_t inR, uint64_t sessionGeneration, uint64_t& outFrameMod) noexcept {
    if (inL < -16384 || inL > 16383) return false;
    if (inR == 0 || inR < -16384 || inR > 16384) return false;

    const uint32_t lo15 = static_cast<uint32_t>(static_cast<int32_t>(inL) + 16384);
    const bool expectPosR = (lo15 & 1u) != 0u;
    if (expectPosR != (inR > 0)) return false; // Parity mismatch (corrupted/faded/torn frame)

    const uint32_t absR = (inR > 0)
        ? static_cast<uint32_t>(inR)
        : static_cast<uint32_t>(-static_cast<int32_t>(inR));
    const uint32_t hi14 = absR - 1u;
    const uint64_t code = (static_cast<uint64_t>(hi14) << 15) | static_cast<uint64_t>(lo15);
    if (code >= kFrameCodeMod) return false;

    const uint64_t salt = ((sessionGeneration % kFrameCodeMod) * kFrameCodeGenSalt) % kFrameCodeMod;
    outFrameMod = (code + kFrameCodeMod - salt) % kFrameCodeMod;
    return true;
}

} // namespace stagec
