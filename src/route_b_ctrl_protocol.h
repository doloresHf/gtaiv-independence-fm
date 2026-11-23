#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace routeb {

// ============================================================================
// Route B Cross-Bitness Control Shared Memory Protocol (512 Bytes = 8 CLs)
// Rules:
//  - Fixed-width integer types ONLY (zero pointers, size_t, bool, std::atomic).
//  - Every 64-bit field is naturally 8-byte aligned for x86 LOCK CMPXCHG8B.
//  - Strict single-writer ownership per cache-line group:
//      * CL 0     [0x000..0x03F]: Static header initialized once by creator.
//      * CL 1..3  [0x040..0x0FF]: Written ONLY by 32-bit IVRadioBridge.asi.
//      * CL 4..7  [0x100..0x1FF]: Written ONLY by 64-bit iv_apple_music_radio.exe.
// ============================================================================

inline constexpr uint32_t kCtrlMagic   = 0x31425249u; // 'IRB1' little-endian
inline constexpr uint32_t kCtrlVersion = 1u;
inline constexpr uint32_t kCtrlBytes   = 512u;

inline constexpr const wchar_t* kDefaultCtrlMappingName = L"Local\\GTAIV_RadioBridge_Ctrl_v1";

inline constexpr uint32_t kCtrlFlagObserveOnly = (1u << 0);

// Diagnostic pause-bit mask recorded in RadioBridgeCtrlBlock::raw_pause_flags_mask
inline constexpr uint32_t kPauseBit12845C8        = (1u << 0); // GTAIV.exe+0xE845C8 != 0
inline constexpr uint32_t kPauseBit12845C9        = (1u << 1); // GTAIV.exe+0xE845C9 != 0
inline constexpr uint32_t kPauseBit115DBFC        = (1u << 2); // GTAIV.exe+0xD5DBFC != 0
inline constexpr uint32_t kPauseBitStation1927    = (1u << 3); // audRadioStation+0x1927 != 0
inline constexpr uint32_t kPauseBitTimeInMsFrozen = (1u << 4); // timeInMs stalled >= 100ms while tuned

enum class BridgeState : uint32_t {
    Inactive        = 0u, // Non-Radio mode / non-Apple Music source / toggle off
    PlayingSong     = 1u, // Playing user music track in Radio mode
    DrainingSongEnd = 2u, // SMTC observed natural track end; draining game tail to trigger DJ
    PausedForDj     = 3u, // Native DJ Gary / commercial / Weazel News active; AM paused
    PreRollingForDj = 4u, // DJ remaining <= 5.4s with preloaded user slot; AM silently pre-rolling
    PausedForEsc    = 5u, // In-game ESC pause menu active; AM synced paused
    SkippingTrack   = 6u, // Processing in-game B / N key track skip
};

struct alignas(64) RadioBridgeCtrlBlock {
    // --- Cache Line 0 [0x000..0x03F]: Static Header (64 B) ---
    uint32_t magic;                     // 0x000: kCtrlMagic ('IRB1')
    uint32_t version;                   // 0x004: kCtrlVersion (1)
    uint32_t total_bytes;               // 0x008: kCtrlBytes (512)
    uint32_t flags;                     // 0x00C: kCtrlFlagObserveOnly etc.
    uint64_t qpc_freq;                  // 0x010: QueryPerformanceFrequency()
    uint64_t reserved_cl0[5];           // 0x018..0x03F: Reserved (0)

    // --- Cache Lines 1-3 [0x040..0x0FF]: Game -> Host (192 B, Written ONLY by Win32 ASI) ---
    int64_t  game_heartbeat_qpc;        // 0x040: Updated every AudRadioStation_Update for user station
    uint64_t game_playhead_frame;       // 0x048: Estimated speaker playhead ring frame
    uint64_t game_decoded_head_frame;   // 0x050: Latest FillBlocks pull end ring frame
    uint64_t preroll_fill_count;        // 0x058: Successful Play()-time Block 0+1 fills
    uint32_t game_pid;                  // 0x060: GTAIV.exe PID
    uint32_t radio_mode;                // 0x064: 0=Radio, 1=Sequential, 2=Shuffle, 0xFFFFFFFF=unknown
    uint32_t station_tuned;             // 0x068: audRadioStation+0x191B (1=Independence FM active/tuned)
    uint32_t esc_paused;                // 0x06C: 1=ESC pause menu active (NEVER set on car exit / switch station)
    uint32_t active_slot_active;        // 0x070: [activeSlot + 0xBCA]
    uint32_t active_slot_is_user;       // 0x074: [activeSlot + 0xBC0] == 2 && [activeSlot + 0xBCC] != 0
    int32_t  active_slot_play_ms;       // 0x078: [activeSlot + 0x990]
    int32_t  active_slot_duration_ms;   // 0x07C: [activeSlot + 0x994]
    int32_t  active_slot_outro_ms;      // 0x080: [activeSlot + 0xBBC]
    uint32_t standby_slot_active;       // 0x084: [standbySlot + 0xBCA]
    uint32_t standby_slot_is_user;      // 0x088: [standbySlot + 0xBC0] == 2 && [standbySlot + 0xBCC] != 0
    uint32_t standby_preloaded_ready;   // 0x08C: 1=Block 0+1 recorded & ready for Play() prefill
    uint32_t active_slot_index;         // 0x090: [station + 0x1917] (0 or 1)
    uint32_t slot_transition_seq;       // 0x094: Incremented on every RadioSlot::Play
    uint32_t pending_skip_seq;          // 0x098: Incremented when B/N triggers during active user track
    int32_t  pending_skip_dir;          // 0x09C: +1 = N (Next), -1 = B (Prev)
    uint32_t last_ack_trigger_dj_seq;   // 0x0A0: Latest Host request_trigger_dj_seq applied by Game
    uint32_t block_frames_total;        // 0x0A4: 2 * framesPerBlock (~254,064 frames = 5.293s)
    uint32_t raw_pause_flags_mask;      // 0x0A8: kPauseBit* diagnostic mask
    uint32_t game_time_ms;              // 0x0AC: Latest timeInMs passed to AudRadioStation_Update
    uint32_t user_slot_play_seq;        // 0x0B0: Incremented ONLY when user track slot starts in Play()
    uint32_t station_crossfading;       // 0x0B4: [station + 0x191C] (1 during slot crossfade window)
    uint64_t reserved_game[9];          // 0x0B8..0x0FF: Reserved (0)

    // --- Cache Lines 4-7 [0x100..0x1FF]: Host -> Game (256 B, Written ONLY by x64 Host) ---
    int64_t  host_heartbeat_qpc;        // 0x100: Updated each coordinator tick
    int64_t  smtc_position_ms;          // 0x108: Reported Apple Music timeline position (ms)
    int64_t  smtc_duration_ms;          // 0x110: Reported Apple Music track duration (ms)
    uint64_t song_end_commit_frame;     // 0x118: Ring frame where outgoing song audio tail ended
    uint64_t preroll_start_frame;       // 0x120: Ring frame where incoming song 0:00.0 audio begins
    uint64_t skip_audio_ready_frame;    // 0x128: Ring frame where skipped track audio begins
    uint32_t host_pid;                  // 0x130: Host launcher PID
    uint32_t bridge_enabled;            // 0x134: 1=Apple Music source + Route B bridge enabled
    uint32_t bridge_state;              // 0x138: BridgeState underlying uint32_t
    uint32_t smtc_connected;            // 0x13C: 1=Bound to AppleInc.AppleMusicWin SMTC session
    uint32_t smtc_playback_status;      // 0x140: WinRT GlobalSystemMediaTransportControlsSessionPlaybackStatus
    uint32_t smtc_track_seq;            // 0x144: Incremented on track change
    uint32_t request_trigger_dj_seq;    // 0x148: Incremented to request dynamic durationMs shortening
    uint32_t request_skip_ack_seq;      // 0x14C: Matches pending_skip_seq once SMTC skip completed
    uint32_t preroll_active;            // 0x150: 1=Preroll active / ready at preroll_start_frame
    uint32_t audio_gate_muted;          // 0x154: 1=Host timeline audio gate muted to prevent track bleed
    uint32_t esc_splice_seq;            // 0x158: Incremented when ESC pause silence hole [start, end) published
    uint32_t reserved_host_u32;         // 0x15C: Reserved (0)
    uint64_t esc_splice_start_frame;    // 0x160: Ring frame where ESC pause silence hole begins
    uint64_t esc_splice_end_frame;      // 0x168: Ring frame where ESC resume audible audio begins
    uint64_t reserved_host[18];         // 0x170..0x1FF: Reserved (0)
};

// ============================================================================
// Mandatory Cross-Bitness Layout & Alignment Static Assertions
// ============================================================================

static_assert(sizeof(LONG) == 4, "Windows LONG must be 32-bit");
static_assert(sizeof(LONGLONG) == 8, "Windows LONGLONG must be 64-bit");
static_assert(sizeof(RadioBridgeCtrlBlock) == kCtrlBytes, "RadioBridgeCtrlBlock must be 512 bytes");
static_assert(alignof(RadioBridgeCtrlBlock) == 64, "RadioBridgeCtrlBlock must be 64-byte aligned");

// CL 0
static_assert(offsetof(RadioBridgeCtrlBlock, magic) == 0x000, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, version) == 0x004, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, total_bytes) == 0x008, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, flags) == 0x00C, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, qpc_freq) == 0x010, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, reserved_cl0) == 0x018, "offset mismatch");

// CL 1..3 (Game -> Host)
static_assert(offsetof(RadioBridgeCtrlBlock, game_heartbeat_qpc) == 0x040, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, game_playhead_frame) == 0x048, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, game_decoded_head_frame) == 0x050, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, preroll_fill_count) == 0x058, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, game_pid) == 0x060, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, radio_mode) == 0x064, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, station_tuned) == 0x068, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, esc_paused) == 0x06C, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, active_slot_active) == 0x070, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, active_slot_is_user) == 0x074, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, active_slot_play_ms) == 0x078, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, active_slot_duration_ms) == 0x07C, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, active_slot_outro_ms) == 0x080, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, standby_slot_active) == 0x084, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, standby_slot_is_user) == 0x088, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, standby_preloaded_ready) == 0x08C, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, active_slot_index) == 0x090, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, slot_transition_seq) == 0x094, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, pending_skip_seq) == 0x098, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, pending_skip_dir) == 0x09C, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, last_ack_trigger_dj_seq) == 0x0A0, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, block_frames_total) == 0x0A4, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, raw_pause_flags_mask) == 0x0A8, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, game_time_ms) == 0x0AC, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, user_slot_play_seq) == 0x0B0, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, station_crossfading) == 0x0B4, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, reserved_game) == 0x0B8, "offset mismatch");

// CL 4..7 (Host -> Game)
static_assert(offsetof(RadioBridgeCtrlBlock, host_heartbeat_qpc) == 0x100, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, smtc_position_ms) == 0x108, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, smtc_duration_ms) == 0x110, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, song_end_commit_frame) == 0x118, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, preroll_start_frame) == 0x120, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, skip_audio_ready_frame) == 0x128, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, host_pid) == 0x130, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, bridge_enabled) == 0x134, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, bridge_state) == 0x138, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, smtc_connected) == 0x13C, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, smtc_playback_status) == 0x140, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, smtc_track_seq) == 0x144, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, request_trigger_dj_seq) == 0x148, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, request_skip_ack_seq) == 0x14C, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, preroll_active) == 0x150, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, audio_gate_muted) == 0x154, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, esc_splice_seq) == 0x158, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, reserved_host_u32) == 0x15C, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, esc_splice_start_frame) == 0x160, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, esc_splice_end_frame) == 0x168, "offset mismatch");
static_assert(offsetof(RadioBridgeCtrlBlock, reserved_host) == 0x170, "offset mismatch");

// 8-byte alignment assertions for all 64-bit atomic fields
static_assert((offsetof(RadioBridgeCtrlBlock, qpc_freq) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, game_heartbeat_qpc) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, game_playhead_frame) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, game_decoded_head_frame) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, preroll_fill_count) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, host_heartbeat_qpc) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, smtc_position_ms) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, smtc_duration_ms) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, song_end_commit_frame) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, preroll_start_frame) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, skip_audio_ready_frame) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, esc_splice_start_frame) % 8) == 0, "64-bit align");
static_assert((offsetof(RadioBridgeCtrlBlock, esc_splice_end_frame) % 8) == 0, "64-bit align");

// ============================================================================
// Cross-Bitness Full-Fence Hardware Atomic Helpers
// ============================================================================

inline uint32_t AtomicLoad32(const uint32_t* addr) noexcept {
    auto* p = reinterpret_cast<volatile LONG*>(const_cast<uint32_t*>(addr));
    return static_cast<uint32_t>(InterlockedCompareExchange(p, 0L, 0L));
}

inline void AtomicStore32(uint32_t* addr, uint32_t val) noexcept {
    auto* p = reinterpret_cast<volatile LONG*>(addr);
    InterlockedExchange(p, static_cast<LONG>(val));
}

inline int32_t AtomicLoadI32(const int32_t* addr) noexcept {
    auto* p = reinterpret_cast<volatile LONG*>(const_cast<int32_t*>(addr));
    return static_cast<int32_t>(InterlockedCompareExchange(p, 0L, 0L));
}

inline void AtomicStoreI32(int32_t* addr, int32_t val) noexcept {
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

inline int64_t AtomicLoadI64(const int64_t* addr) noexcept {
    auto* p = reinterpret_cast<volatile LONGLONG*>(const_cast<int64_t*>(addr));
    return static_cast<int64_t>(InterlockedCompareExchange64(p, 0LL, 0LL));
}

inline void AtomicStoreI64(int64_t* addr, int64_t val) noexcept {
    auto* p = reinterpret_cast<volatile LONGLONG*>(addr);
    InterlockedExchange64(p, static_cast<LONGLONG>(val));
}

} // namespace routeb
