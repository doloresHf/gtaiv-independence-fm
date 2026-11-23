#include "route_b_offsets.h"

#include <cstring>

namespace routeb {

namespace {

constexpr SignatureSpec kGtaIvSpecs[static_cast<size_t>(TargetId::Count)] = {
    {
        TargetId::AudRadioStationUpdate,
        "AudRadioStation_Update",
        kRvaAudRadioStationUpdate,
        20u,
        { 0x56, 0x57, 0x8B, 0xF9, 0x80, 0xBF, 0x1B, 0x19, 0x00, 0x00, 0x00, 0x75, 0x07, 0xC6, 0x87, 0x1F, 0x19, 0x00, 0x00, 0x01 },
        "xxxxxxxxxxxxxxxxxxxx"
    },
    {
        TargetId::AudRadioSlotPlay,
        "AudRadioSlot_Play",
        kRvaAudRadioSlotPlay,
        20u,
        { 0x56, 0x8B, 0xF1, 0x80, 0xBE, 0xC8, 0x0B, 0x00, 0x00, 0x00, 0x75, 0x6F, 0x83, 0xBE, 0x64, 0x09, 0x00, 0x00, 0x00, 0x74 },
        "xxxxxxxxxxxxxxxxxxxx"
    },
    {
        TargetId::AudRadioSlotStop,
        "AudRadioSlot_Stop",
        kRvaAudRadioSlotStop,
        20u,
        { 0x53, 0x8B, 0xD9, 0x8B, 0x8B, 0x64, 0x09, 0x00, 0x00, 0x85, 0xC9, 0x74, 0x07, 0x6A, 0x00, 0xE8, 0x2C, 0xAC, 0xD1, 0xFF },
        "xxxxxxxxxxxxxxxxxxxx"
    },
    {
        TargetId::UserTrackFillBlocks,
        "UserTrack_FillBlocks",
        kRvaUserTrackFillBlocks,
        20u,
        { 0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x6E, 0x0C, 0x85, 0xDB, 0x0F, 0x84, 0xCD, 0x00 },
        "xxxxxxxxxxxxxxxxxxxx"
    },
    {
        TargetId::SkipForward,
        "SkipForward",
        kRvaSkipForward,
        20u,
        { 0x56, 0x8B, 0xF1, 0x80, 0xBE, 0x30, 0x19, 0x00, 0x00, 0x00, 0x74, 0x41, 0x80, 0xBE, 0x29, 0x19, 0x00, 0x00, 0x00, 0x75 },
        "xxxxxxxxxxxxxxxxxxxx"
    },
    {
        TargetId::SkipBackward,
        "SkipBackward",
        kRvaSkipBackward,
        20u,
        { 0x57, 0x8B, 0xF9, 0x80, 0xBF, 0x30, 0x19, 0x00, 0x00, 0x00, 0x0F, 0x84, 0xAC, 0x01, 0x00, 0x00, 0x80, 0xBF, 0x29, 0x19 },
        "xxxxxxxxxxxxxxxxxxxx"
    },
    {
        TargetId::GetUserRadioMode,
        "GetUserRadioMode",
        kRvaGetUserRadioMode,
        16u,
        { 0xA1, 0xCC, 0x0E, 0x16, 0x01, 0x33, 0xD2, 0x83, 0x79, 0x08, 0x02, 0x0F, 0x4E, 0xC2, 0xC3, 0xCC, 0, 0, 0, 0 },
        "x????xxxxxxxxxxx"
    },
    {
        TargetId::AudWaveSlotOpenUser,
        "AudWaveSlot_OpenUser",
        kRvaAudWaveSlotOpenUser,
        20u,
        { 0x53, 0x8B, 0xD9, 0x57, 0xFF, 0x73, 0x38, 0xE8, 0x04, 0x14, 0xB7, 0xFF, 0x83, 0xC4, 0x04, 0x83, 0xBB, 0x98, 0x00, 0x00 },
        "xxxxxxxxxxxxxxxxxxxx"
    },
};

bool IsRvaRangeWithinImage(size_t imageSize, uint32_t rva, size_t spanBytes) noexcept {
    const size_t start = static_cast<size_t>(rva);
    if (start > imageSize) {
        return false;
    }
    return spanBytes <= (imageSize - start);
}

} // namespace

size_t GetModuleImageSize(HMODULE hModule) noexcept {
    if (!hModule) {
        return 0u;
    }
    const auto* base = reinterpret_cast<const uint8_t*>(hModule);
    const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000L) {
        return 0u;
    }
    const auto* nt32 = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt32->Signature != IMAGE_NT_SIGNATURE) {
        return 0u;
    }
    if (nt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return static_cast<size_t>(nt32->OptionalHeader.SizeOfImage);
    }
    if (nt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const auto* nt64 = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return static_cast<size_t>(nt64->OptionalHeader.SizeOfImage);
    }
    return 0u;
}

const SignatureSpec* GetGtaIvSignatureSpecs(size_t* outCount) noexcept {
    if (outCount) {
        *outCount = static_cast<size_t>(TargetId::Count);
    }
    return kGtaIvSpecs;
}

bool MatchMaskedPattern(const uint8_t* mem,
                        const uint8_t* pattern,
                        const char* mask,
                        size_t len) noexcept {
    if (!mem || !pattern || !mask || len == 0u) {
        return false;
    }
    for (size_t i = 0u; i < len; ++i) {
        if (mask[i] == '\0') {
            return false;
        }
        if (mask[i] == '?') {
            continue;
        }
        if (mem[i] != pattern[i]) {
            return false;
        }
    }
    return mask[len] == '\0';
}

bool ResolveGtaIvTargets(uintptr_t moduleBase,
                         size_t moduleImageSize,
                         ResolvedTargets& out) noexcept {
    out = ResolvedTargets{};
    if (moduleBase == 0u || moduleImageSize == 0u) {
        return false;
    }

    const auto* basePtr = reinterpret_cast<const uint8_t*>(moduleBase);
    bool ok = true;

    for (size_t i = 0u; i < static_cast<size_t>(TargetId::Count); ++i) {
        const SignatureSpec& spec = kGtaIvSpecs[i];
        ResolvedTargetEntry& dst  = out.entries[i];
        dst.name = spec.name;
        dst.rva  = spec.rva;

        if (!IsRvaRangeWithinImage(moduleImageSize, spec.rva, spec.patternLen)) {
            dst.address = 0u;
            dst.matched = false;
            ok = false;
            continue;
        }

        const uint8_t* targetMem = basePtr + spec.rva;
        const bool match = MatchMaskedPattern(targetMem, spec.pattern, spec.mask, spec.patternLen);
        dst.matched = match;
        dst.address = match ? (moduleBase + static_cast<uintptr_t>(spec.rva)) : 0u;
        if (!match) {
            ok = false;
        }
    }

    const bool globalsValid =
        IsRvaRangeWithinImage(moduleImageSize, kRvaGlobalRadioMode, sizeof(uint32_t)) &&
        IsRvaRangeWithinImage(moduleImageSize, kRvaGlobalPause12845C8, sizeof(uint8_t)) &&
        IsRvaRangeWithinImage(moduleImageSize, kRvaGlobalPause12845C9, sizeof(uint8_t)) &&
        IsRvaRangeWithinImage(moduleImageSize, kRvaGlobalAudioMute115DBFC, sizeof(uint8_t)) &&
        IsRvaRangeWithinImage(moduleImageSize, kRvaGlobalAudioTimeMs, sizeof(uint32_t));

    if (globalsValid) {
        out.globalRadioMode        = moduleBase + kRvaGlobalRadioMode;
        out.globalPause12845C8     = moduleBase + kRvaGlobalPause12845C8;
        out.globalPause12845C9     = moduleBase + kRvaGlobalPause12845C9;
        out.globalAudioMute115DBFC = moduleBase + kRvaGlobalAudioMute115DBFC;
        out.globalAudioTimeMs      = moduleBase + kRvaGlobalAudioTimeMs;
    } else {
        ok = false;
    }

    out.allMatched = ok;
    return ok;
}

} // namespace routeb
