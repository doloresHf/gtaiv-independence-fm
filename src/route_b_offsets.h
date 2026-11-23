#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace routeb {

// ============================================================================
// GTA IV: The Complete Edition (1.2.0.59, x86) Verified Struct Offsets & RVAs
// ============================================================================

inline constexpr uint32_t kDefaultPeImageBase          = 0x00400000u;

// audRadioStation offsets (sizeof slot = 0xBD0, 2 alternating slots at +0x0 and +0xBD0)
inline constexpr uint32_t kOffStationSlotStride        = 0x00000BD0u;
inline constexpr uint32_t kOffStationActiveSlotIdx     = 0x00001917u; // uint8_t (0 or 1)
inline constexpr uint32_t kOffStationTuned             = 0x0000191Bu; // uint8_t (!= 0 when tuned)
inline constexpr uint32_t kOffStationCrossfading       = 0x0000191Cu; // uint8_t
inline constexpr uint32_t kOffStationAllowNonUser      = 0x0000191Fu; // uint8_t
inline constexpr uint32_t kOffStationPausedFlag        = 0x00001927u; // uint8_t
inline constexpr uint32_t kOffStationIsUserStation     = 0x00001930u; // uint8_t (!= 0 for Independence FM)

// audRadioSlot offsets (within audRadioStation + slotIdx * 0xBD0)
inline constexpr uint32_t kOffSlotSound                = 0x00000964u; // void*
inline constexpr uint32_t kOffSlotWaveSlot             = 0x0000098Cu; // void* (audWaveSlot)
inline constexpr uint32_t kOffSlotPlayTimeMs           = 0x00000990u; // int32_t
inline constexpr uint32_t kOffSlotDurationMs           = 0x00000994u; // int32_t
inline constexpr uint32_t kOffSlotOutroMs              = 0x00000BBCu; // int32_t
inline constexpr uint32_t kOffSlotCategory             = 0x00000BC0u; // uint8_t (2 == UserTrack)
inline constexpr uint32_t kOffSlotState                = 0x00000BC8u; // uint8_t
inline constexpr uint32_t kOffSlotActive               = 0x00000BCAu; // uint8_t
inline constexpr uint32_t kOffSlotIsUserTrack          = 0x00000BCCu; // uint8_t
inline constexpr uint8_t  kSlotCategoryUserTrack       = 2u;

// audWaveSlot offsets
inline constexpr uint32_t kOffWaveSlotDecoder          = 0x00000094u; // void* (audUserTrackDecoder*)
inline constexpr uint32_t kOffWaveSlotWidePath         = 0x00000098u; // const wchar_t*

// audUserTrackDecoder offsets
inline constexpr uint32_t kOffDecoderAsyncFlag         = 0x00000030u; // uint8_t (1 = playback worker, 0 = scan)

// Function RVAs relative to GTAIV.exe ImageBase (0x00400000)
inline constexpr uint32_t kRvaAudRadioStationUpdate    = 0x00547020u; // 0x00947020
inline constexpr uint32_t kRvaAudRadioSlotPlay         = 0x00776920u; // 0x00B76920
inline constexpr uint32_t kRvaAudRadioSlotStop         = 0x00776C10u; // 0x00B76C10
inline constexpr uint32_t kRvaUserTrackFillBlocks      = 0x00483050u; // 0x00883050
inline constexpr uint32_t kRvaSkipForward              = 0x00545FE0u; // 0x00945FE0
inline constexpr uint32_t kRvaSkipBackward             = 0x00546490u; // 0x00946490
inline constexpr uint32_t kRvaGetUserRadioMode         = 0x004E1640u; // 0x008E1640
inline constexpr uint32_t kRvaAudWaveSlotOpenUser      = 0x004928A0u; // 0x008928A0

// Read-only Global RVAs relative to GTAIV.exe ImageBase (0x00400000)
inline constexpr uint32_t kRvaGlobalRadioMode          = 0x00D60ECCu; // 0x01160ECC (0=Radio, 1=Seq, 2=Shuffle)
inline constexpr uint32_t kRvaGlobalPause12845C8       = 0x00E845C8u; // 0x012845C8
inline constexpr uint32_t kRvaGlobalPause12845C9       = 0x00E845C9u; // 0x012845C9
inline constexpr uint32_t kRvaGlobalAudioMute115DBFC   = 0x00D5DBFCu; // 0x0115DBFC
inline constexpr uint32_t kRvaGlobalAudioTimeMs        = 0x00D735B4u; // 0x011735B4

enum class TargetId : size_t {
    AudRadioStationUpdate = 0,
    AudRadioSlotPlay      = 1,
    AudRadioSlotStop      = 2,
    UserTrackFillBlocks   = 3,
    SkipForward           = 4,
    SkipBackward          = 5,
    GetUserRadioMode      = 6,
    AudWaveSlotOpenUser   = 7,
    Count                 = 8,
};

struct SignatureSpec {
    TargetId id;
    const char* name;
    uint32_t rva;
    uint32_t patternLen;
    uint8_t pattern[20];
    const char* mask;
};

struct ResolvedTargetEntry {
    const char* name = "";
    uint32_t rva = 0u;
    uintptr_t address = 0u;
    bool matched = false;
};

struct ResolvedTargets {
    bool allMatched = false;
    ResolvedTargetEntry entries[static_cast<size_t>(TargetId::Count)]{};

    uintptr_t globalRadioMode        = 0u;
    uintptr_t globalPause12845C8     = 0u;
    uintptr_t globalPause12845C9     = 0u;
    uintptr_t globalAudioMute115DBFC = 0u;
    uintptr_t globalAudioTimeMs      = 0u;

    uintptr_t AddressOf(TargetId id) const noexcept {
        const size_t idx = static_cast<size_t>(id);
        return (idx < static_cast<size_t>(TargetId::Count)) ? entries[idx].address : 0u;
    }
};

// Returns PE SizeOfImage for a loaded module, or 0 on invalid headers.
size_t GetModuleImageSize(HMODULE hModule) noexcept;

// Returns the table of 8 verified GTA IV 1.2.0.59 prologue signatures.
const SignatureSpec* GetGtaIvSignatureSpecs(size_t* outCount = nullptr) noexcept;

// Matches 'len' bytes at 'mem' against 'pattern' using 'mask' ('x' = exact, '?' = wildcard).
bool MatchMaskedPattern(const uint8_t* mem,
                        const uint8_t* pattern,
                        const char* mask,
                        size_t len) noexcept;

// ASLR-safe RVA + signature + global bounds resolver.
// Guarantees out.allMatched == true ONLY when every signature matches inside [0, moduleImageSize).
bool ResolveGtaIvTargets(uintptr_t moduleBase,
                         size_t moduleImageSize,
                         ResolvedTargets& out) noexcept;

} // namespace routeb
